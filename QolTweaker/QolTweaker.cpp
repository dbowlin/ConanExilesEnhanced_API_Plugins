#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "Conan/ConanPluginApi.h"
#include "Conan/ConanBase.h"
#include "../common/JsonConfig.h"
#include "../common/ObjectScan.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <unordered_set>

static const ConanApiTabela* g_api = nullptr;
static uint32_t g_taskId = 0;
static uint32_t g_invTaskId = 0;
static bool g_tableDone = false;
static int g_tick = 0;

// FItemTableRow offsets (ConanStructs.h / static_assert)
static constexpr uint32_t OFF_MAX_STACK = 288;
static constexpr uint32_t OFF_WEIGHT = 384;
static constexpr uint32_t OFF_BURN_TIME = 896;
static constexpr uint32_t OFF_CONTAINER_SIZE = 904;
static constexpr uint32_t ITEM_ROW_SIZE = 1048;

// UDataTable::RowMap is not reflected; measured live at +0x30
static constexpr uint32_t OFF_ROW_MAP = 0x30;

struct QolConfig
{
    bool enabled = true;
    int stackMultiplier = 100;
    int maxStackCap = 10000;
    double weightMultiplier = 0.5;
    double spoilTimeMultiplier = 5.0;
    double fuelBurnMultiplier = 10.0;
    bool expandContainers = true;
    int containerSlotBonus = 200;

    void Load(const char* folderName)
    {
        const char* configPath = g_api->CaminhoConfig(folderName);
        if (!configPath) return;
        ConanUtils::JsonValue json;
        if (!ConanUtils::JsonParser::ParseFile(configPath, json)) return;
        if (json.has("Enabled")) enabled = json["Enabled"].asBool(true);
        if (json.has("StackMultiplier")) stackMultiplier = json["StackMultiplier"].asInt(100);
        if (json.has("MaxStackCap")) maxStackCap = json["MaxStackCap"].asInt(10000);
        if (json.has("WeightMultiplier")) weightMultiplier = json["WeightMultiplier"].asDouble(0.5);
        if (json.has("SpoilTimeMultiplier")) spoilTimeMultiplier = json["SpoilTimeMultiplier"].asDouble(5.0);
        if (json.has("FuelBurnMultiplier")) fuelBurnMultiplier = json["FuelBurnMultiplier"].asDouble(10.0);
        if (json.has("ExpandContainers")) expandContainers = json["ExpandContainers"].asBool(true);
        if (json.has("ContainerSlotBonus")) containerSlotBonus = json["ContainerSlotBonus"].asInt(200);
    }
};

static QolConfig g_cfg;
static std::unordered_set<void*> g_invPatched;
static void* g_dataTableClass = nullptr;
static void* g_itemTable = nullptr;
static int g_tableScanCursor = 0;
static void* g_invClass = nullptr;
static ScanUtil::IncrementalScan g_invScan;
static const int kInvPatchPerTick = 12;

struct FNameRaw { int32_t index; int32_t number; };

static void* FindItemTable()
{
    if (g_itemTable) return g_itemTable;
    if (!g_dataTableClass) g_dataTableClass = g_api->FindClass("DataTable");
    if (!g_dataTableClass) return nullptr;

    const int total = g_api->NumObjects();
    int checked = 0;
    while (checked < 4096)
    {
        if (g_tableScanCursor >= total) g_tableScanCursor = 0;
        void* obj = g_api->GetObjectByIndex(g_tableScanCursor++);
        ++checked;
        if (!obj || ScanUtil::IsDefaultObject(g_api, obj)) continue;
        if (!ScanUtil::IsClassObject(g_api, obj, g_dataTableClass)) continue;
        if (ScanUtil::ObjectNameIs(g_api, obj, "ItemTable"))
        {
            g_itemTable = obj;
            return obj;
        }
    }
    return nullptr;
}

static int ClampStack(int v)
{
    if (v < 1) v = 1;
    if (v > g_cfg.maxStackCap) v = g_cfg.maxStackCap;
    return v;
}

static void PatchItemTable()
{
    void* itemTable = FindItemTable();
    if (!itemTable)
    {
        g_api->Log("[QolTweaker] ItemTable not found yet.");
        return;
    }

    unsigned char mapHdr[16] = {};
    if (g_api->LerMembro(itemTable, OFF_ROW_MAP, mapHdr, 16) <= 0)
    {
        g_api->Log("[QolTweaker] Failed reading RowMap header.");
        return;
    }

    void* data = nullptr;
    int32_t num = 0;
    int32_t max = 0;
    std::memcpy(&data, mapHdr, 8);
    std::memcpy(&num, mapHdr + 8, 4);
    std::memcpy(&max, mapHdr + 12, 4);

    if (!data || num <= 0 || max <= 0 || max > 100000 || !g_api->Legivel(data, (size_t)max * 16))
    {
        g_api->Log("[QolTweaker] RowMap looks invalid data=%p num=%d max=%d", data, num, max);
        return;
    }

    int stackTouched = 0, containerTouched = 0, weightTouched = 0, burnTouched = 0;
    const int limit = max; // walk capacity; skip empty slots

    for (int i = 0; i < limit; ++i)
    {
        FNameRaw key{};
        void* row = nullptr;
        std::memcpy(&key, (char*)data + (size_t)i * 16, 8);
        std::memcpy(&row, (char*)data + (size_t)i * 16 + 8, 8);
        if (!row || key.index == 0) continue;
        if (!g_api->Legivel(row, ITEM_ROW_SIZE)) continue;

        int32_t maxStack = 0;
        std::memcpy(&maxStack, (char*)row + OFF_MAX_STACK, 4);
        if (maxStack > 1)
        {
            const int32_t neu = ClampStack(maxStack * g_cfg.stackMultiplier);
            if (neu != maxStack)
            {
                std::memcpy((char*)row + OFF_MAX_STACK, &neu, 4);
                ++stackTouched;
            }
        }

        if (g_cfg.expandContainers)
        {
            int32_t cont = 0;
            std::memcpy(&cont, (char*)row + OFF_CONTAINER_SIZE, 4);
            if (cont > 0)
            {
                const int32_t neu = cont + g_cfg.containerSlotBonus;
                std::memcpy((char*)row + OFF_CONTAINER_SIZE, &neu, 4);
                ++containerTouched;
            }
        }

        if (g_cfg.weightMultiplier > 0.0 && g_cfg.weightMultiplier != 1.0)
        {
            float w = 0.0f;
            std::memcpy(&w, (char*)row + OFF_WEIGHT, 4);
            if (w > 0.0f)
            {
                w = (float)(w * g_cfg.weightMultiplier);
                std::memcpy((char*)row + OFF_WEIGHT, &w, 4);
                ++weightTouched;
            }
        }

        if (g_cfg.fuelBurnMultiplier > 0.0 && g_cfg.fuelBurnMultiplier != 1.0)
        {
            float burn = 0.0f;
            std::memcpy(&burn, (char*)row + OFF_BURN_TIME, 4);
            if (burn > 0.0f)
            {
                burn = (float)(burn * g_cfg.fuelBurnMultiplier);
                std::memcpy((char*)row + OFF_BURN_TIME, &burn, 4);
                ++burnTouched;
            }
        }
    }

    g_tableDone = true;
    g_api->Log("[QolTweaker] ItemTable patched rows~%d: stacks=%d containers=%d weight=%d burn=%d (x%d stack, +%d slots)",
               num, stackTouched, containerTouched, weightTouched, burnTouched,
               g_cfg.stackMultiplier, g_cfg.containerSlotBonus);
}

static bool PatchOneInventory(void* inv, int& stackClamp, int& slots, int& perish)
{
    const int32_t offClamp = g_api->OffsetDoMembro(inv, "ClampMaxStackSize");
    if (offClamp >= 0)
    {
        const int32_t want = g_cfg.maxStackCap;
        g_api->EscreverMembro(inv, (uint32_t)offClamp, &want, 4);
        ++stackClamp;
    }

    if (g_cfg.spoilTimeMultiplier > 0.0 && g_cfg.spoilTimeMultiplier != 1.0)
    {
        const int32_t offP = g_api->OffsetDoMembro(inv, "PerishModifier");
        if (offP >= 0)
        {
            float rate = (float)(1.0 / g_cfg.spoilTimeMultiplier);
            g_api->EscreverMembro(inv, (uint32_t)offP, &rate, 4);
            ++perish;
        }
    }

    if (!g_cfg.expandContainers || g_invPatched.count(inv)) return true;

    const int32_t offMax = g_api->OffsetDoMembro(inv, "MaxItemCount");
    if (offMax < 0) return true;
    int32_t cur = 0;
    g_api->LerMembro(inv, (uint32_t)offMax, &cur, 4);
    if (cur <= 0) { g_invPatched.insert(inv); return true; }

    const int32_t want = cur + g_cfg.containerSlotBonus;
    ConanApi::Call<void>(inv, "SetMaxItemCount", want);
    if (!g_api->UltimaChamadaExecutou())
        g_api->EscreverMembro(inv, (uint32_t)offMax, &want, 4);
    ConanApi::Call<void>(inv, "GrowItemList", want);
    g_invPatched.insert(inv);
    ++slots;
    return true;
}

static void PatchInventories()
{
    if (!g_invClass) g_invClass = g_api->FindClass("ItemInventory");
    if (!g_invClass) return;

    int stackClamp = 0, slots = 0, perish = 0;
    g_invScan.Tick(g_api, kInvPatchPerTick,
        [&](void* obj) { return ScanUtil::IsClassObject(g_api, obj, g_invClass); },
        [&](void* inv) {
            PatchOneInventory(inv, stackClamp, slots, perish);
            return true;
        });

    if ((g_tick % 15) == 1)
        g_api->Log("[QolTweaker] inventories: clamp=%d slotExpand=%d perish=%d tracked=%zu",
                   stackClamp, slots, perish, g_invPatched.size());
}

static void TickTable(void*)
{
    g_cfg.Load("QolTweaker");
    if (!g_cfg.enabled) return;
    if (!g_tableDone) PatchItemTable();
}

static void TickInv(void*)
{
    ++g_tick;
    g_cfg.Load("QolTweaker");
    if (!g_cfg.enabled) return;
    if (!g_tableDone) PatchItemTable();
    PatchInventories();
}

extern "C" __declspec(dllexport)
void ConanPluginCarregar(const ConanApiTabela* api)
{
    if (!api || api->tamanho < sizeof(ConanApiTabela)) return;
    g_api = api;
    ConanApi::UsarTabela(api);
    g_cfg.Load("QolTweaker");
    g_tableDone = false;
    g_invPatched.clear();
    g_tick = 0;

    if (!g_cfg.enabled)
    {
        g_api->Log("[QolTweaker] disabled in config.json");
        return;
    }

    g_api->Log("======================================================");
    g_api->Log(" QolTweaker v2.1.0 — incremental ItemTable + ItemInventory scan");
    g_api->Log(" Stacks: x%d (cap %d) | Containers: +%d | Spoil: %.1fx | Weight: %.2fx",
               g_cfg.stackMultiplier, g_cfg.maxStackCap,
               g_cfg.containerSlotBonus, g_cfg.spoilTimeMultiplier, g_cfg.weightMultiplier);
    g_api->Log("======================================================");

    g_taskId = g_api->AgendarNaThreadDoJogo(TickTable, 5, nullptr, 0);
    g_invTaskId = g_api->AgendarNaThreadDoJogo(TickInv, 3, nullptr, 1);
}

extern "C" __declspec(dllexport)
void ConanPluginDescarregar(void)
{
    if (g_api)
    {
        if (g_taskId) g_api->CancelarAgendamento(g_taskId);
        if (g_invTaskId) g_api->CancelarAgendamento(g_invTaskId);
    }
    g_api = nullptr;
}
