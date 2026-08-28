#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "Conan/ConanPluginApi.h"
#include "Conan/ConanBase.h"
#include "../common/JsonConfig.h"

#include <string>
#include <unordered_map>
#include <cstdio>
#include <cstring>
#include <algorithm>

static const ConanApiTabela* g_api = nullptr;
static uint32_t g_taskId = 0;
static uint32_t g_hookChat = 0;
static uint32_t g_verifyId = 0;
static bool g_verified = false;
static int g_totalSorted = 0;

static const uint8_t kSortByName = 0;
static const uint8_t kSortByWeight = 1;
static const uint8_t kSortByType = 2;
static const uint8_t kSortByAge = 3;
static const uint8_t kInvPlaceable = 4;
static const uint8_t kInvCraftArtisan = 8;
static const uint8_t kInvDismantler = 14;
static const uint8_t kStatStack = 1;
static const uint8_t kStatWeight = 5;
static const uint32_t CHAT_TEXTO = 0x068;

struct SortConfig
{
    bool enabled = true;
    std::string sortMethod = "ByType";
    int intervalSeconds = 30;
    int containersPerTick = 6;
    bool includeCrafting = true;
    bool sortOnItemAdded = true;
    std::string prefix = "!";
    std::string cmdSort = "sort";

    void Load(const char* folder)
    {
        const char* path = g_api->CaminhoConfig(folder);
        if (!path) return;
        ConanUtils::JsonValue json;
        if (!ConanUtils::JsonParser::ParseFile(path, json)) return;
        if (json.has("Enabled")) enabled = json["Enabled"].asBool(true);
        if (json.has("SortMethod")) sortMethod = json["SortMethod"].asString("ByType");
        if (json.has("AutoSortIntervalSeconds")) intervalSeconds = json["AutoSortIntervalSeconds"].asInt(30);
        if (json.has("ContainersPerTick")) containersPerTick = json["ContainersPerTick"].asInt(6);
        if (json.has("IncludeCraftingInventories"))
            includeCrafting = json["IncludeCraftingInventories"].asBool(true);
        if (json.has("SortOnItemAdded")) sortOnItemAdded = json["SortOnItemAdded"].asBool(true);
        if (json.has("Prefix")) prefix = json["Prefix"].asString("!");
        if (json.has("CommandSort")) cmdSort = json["CommandSort"].asString("sort");
        if (intervalSeconds < 5) intervalSeconds = 5;
        if (containersPerTick < 1) containersPerTick = 1;
        if (containersPerTick > 3) containersPerTick = 3;
    }
};

static const int kIndicesPerTick = 4096;
static const int kMaxSortPerTick = 2;
static const int kPlaceableScanBudget = 2048;

static SortConfig g_cfg;
static std::unordered_map<void*, uint32_t> g_lastFingerprint;
static int g_scanCursor = 0;
static void* g_invClass = nullptr;
static void* g_placeableClass = nullptr;

static bool CallOk()
{
    return g_api->UltimaChamadaExecutou() != 0;
}

static bool IsDefaultObject(void* obj)
{
    if (!obj) return true;
    char name[256] = {0};
    if (!g_api->NomeDoObjeto(obj, name, sizeof(name))) return false;
    return std::strncmp(name, "Default__", 9) == 0;
}

static uint8_t ParseSortMethod(const std::string& method)
{
    if (method == "ByName" || method == "Name") return kSortByName;
    if (method == "ByWeight" || method == "Weight" || method == "HeaviestFirst") return kSortByWeight;
    if (method == "ByAge" || method == "Age") return kSortByAge;
    return kSortByType;
}

static uint8_t EngineSortType(uint8_t internalSort)
{
    if (internalSort == kSortByName) return kSortByName;
    if (internalSort == kSortByWeight) return kSortByWeight;
    return kSortByType;
}

static uint8_t InventoryTypeOf(void* inv)
{
    uint8_t t = 0;
    int32_t off = g_api->OffsetDoMembro(inv, "inventoryType");
    if (off >= 0) g_api->LerMembro(inv, uint32_t(off), &t, sizeof(t));
    return t;
}

static bool ClassIsOrExtends(void* objClass, void* targetClass)
{
    if (!objClass || !targetClass) return false;
    void* cur = objClass;
    for (int depth = 0; depth < 32 && cur; ++depth)
    {
        if (cur == targetClass) return true;
        void* super = nullptr;
        const int32_t off = g_api->OffsetDoMembro(cur, "SuperStruct");
        if (off < 0) break;
        g_api->LerMembro(cur, uint32_t(off), &super, sizeof(super));
        cur = super;
    }
    return false;
}

static bool IsClassObject(void* obj, void* targetClass)
{
    if (!obj || !targetClass) return false;
    void* objClass = nullptr;
    g_api->LerMembro(obj, 0x10, &objClass, sizeof(objClass));
    return objClass && ClassIsOrExtends(objClass, targetClass);
}

static void CacheClasses()
{
    g_invClass = g_api->FindClass("ItemInventory");
    g_placeableClass = g_api->FindClass("PlaceableInventory");
}

static bool IsInventoryObject(void* obj)
{
    return g_invClass && IsClassObject(obj, g_invClass);
}

static bool IsTargetContainer(void* inv)
{
    if (!inv || IsDefaultObject(inv)) return false;
    const uint8_t t = InventoryTypeOf(inv);
    if (t == kInvPlaceable || t == kInvDismantler) return true;
    if (g_cfg.includeCrafting && t == kInvCraftArtisan) return true;
    return false;
}

static void* AnyPlayerController()
{
    void* pcs[8];
    const int n = g_api->FindObjects("ConanPlayerController", pcs, 8, 1);
    for (int i = 0; i < n; ++i)
        if (!IsDefaultObject(pcs[i])) return pcs[i];
    return nullptr;
}

static bool WriteRequestedSort(void* inv, uint8_t sortType)
{
    int32_t off = g_api->OffsetDoMembro(inv, "m_RequestedSortType");
    if (off < 0) return false;
    return g_api->EscreverMembro(inv, uint32_t(off), &sortType, sizeof(sortType)) > 0;
}

static int32_t TemplateOf(void* item)
{
    if (!item) return 0;
    int32_t id = ConanApi::Call<int32_t>(item, "GetVisualItemTemplateId");
    if (CallOk() && id > 0) return id;
    id = 0;
    int32_t off = g_api->OffsetDoMembro(item, "TemplateId");
    if (off >= 0) g_api->LerMembro(item, uint32_t(off), &id, sizeof(id));
    return id;
}

static int32_t StackOf(void* item)
{
    int32_t n = ConanApi::Call<int32_t>(item, "GetIntStat", kStatStack);
    return CallOk() ? n : 1;
}

static float WeightOf(void* item)
{
    float w = ConanApi::Call<float>(item, "GetFloatStat", kStatWeight);
    return CallOk() ? w : 0.0f;
}

static void ItemNameOf(void* item, char* buf, int len)
{
    buf[0] = 0;
    if (!item) return;
    ConanApi::CallSaida<void>(item, "GetItemName", ConanApi::ParaRetornoTextoRico(buf, len));
    if (buf[0]) return;
    g_api->NomeDoObjeto(item, buf, len);
}

struct SlotEntry
{
    int32_t index = -1;
    int32_t templateId = 0;
    int32_t stack = 0;
    float weight = 0.0f;
    float age = 0.0f;
    uint8_t category = 0;
    char name[96];
};

static int CollectSlots(void* inv, SlotEntry* slots, int maxSlots)
{
    int32_t capacity = 0;
    int32_t off = g_api->OffsetDoMembro(inv, "MaxItemCount");
    if (off >= 0) g_api->LerMembro(inv, uint32_t(off), &capacity, sizeof(capacity));
    if (capacity <= 0 || capacity > maxSlots) capacity = maxSlots;

    int filled = 0;
    for (int i = 0; i < capacity; ++i)
    {
        void* item = ConanApi::Call<void*>(inv, "GetItem", int32_t(i));
        if (!CallOk() || !item) continue;
        SlotEntry& s = slots[filled++];
        s.index = int32_t(i);
        s.templateId = TemplateOf(item);
        s.stack = StackOf(item);
        s.weight = WeightOf(item);
        s.age = ConanApi::Call<float>(item, "GetFloatStat", uint8_t(8));
        if (!CallOk()) s.age = float(i);
        s.category = ConanApi::Call<uint8_t>(item, "GetGUICategory");
        if (!CallOk()) s.category = 0;
        ItemNameOf(item, s.name, sizeof(s.name));
    }
    return filled;
}

static bool CompareSlots(const SlotEntry& a, const SlotEntry& b, uint8_t sortType)
{
    if (sortType == kSortByName)
        return std::strcmp(a.name, b.name) < 0;
    if (sortType == kSortByWeight)
    {
        if (a.weight != b.weight) return a.weight > b.weight;
        return a.templateId < b.templateId;
    }
    if (sortType == kSortByAge)
    {
        if (a.age != b.age) return a.age < b.age;
        return a.templateId < b.templateId;
    }
    if (a.category != b.category) return a.category < b.category;
    if (a.templateId != b.templateId) return a.templateId < b.templateId;
    return std::strcmp(a.name, b.name) < 0;
}

static uint32_t Fingerprint(void* inv)
{
    const int32_t count = ConanApi::Call<int32_t>(inv, "GetItemCount");
    uint32_t h = uint32_t(count < 0 ? 0 : count);
    const int limit = count < 0 ? 0 : (count > 48 ? 48 : count);
    for (int i = 0; i < limit; ++i)
    {
        void* item = ConanApi::Call<void*>(inv, "GetItem", int32_t(i));
        if (!CallOk() || !item) continue;
        h = h * 31u + uint32_t(TemplateOf(item));
        h = h * 31u + uint32_t(StackOf(item));
    }
    return h;
}

static bool ManualSort(void* inv, uint8_t sortType)
{
    SlotEntry slots[512];
    int filled = CollectSlots(inv, slots, 512);
    if (filled < 2) return false;

    std::sort(slots, slots + filled, [sortType](const SlotEntry& a, const SlotEntry& b) {
        return CompareSlots(a, b, sortType);
    });

    bool moved = false;
    for (int pass = 0; pass < filled && pass < 8; ++pass)
    {
        filled = CollectSlots(inv, slots, 512);
        if (filled < 2) break;

        int misplaced = -1;
        for (int i = 0; i < filled; ++i)
        {
            if (slots[i].index != i) { misplaced = i; break; }
        }
        if (misplaced < 0) break;

        std::sort(slots, slots + filled, [sortType](const SlotEntry& a, const SlotEntry& b) {
            return CompareSlots(a, b, sortType);
        });

        const int32_t src = slots[misplaced].index;
        const bool ok = ConanApi::Call<bool>(inv, "MoveItem",
            src, inv, int32_t(misplaced), int32_t(-1), bool(false));
        if (!CallOk() || !ok) break;
        moved = true;
    }
    return moved;
}

static bool SortInventory(void* inv, uint8_t sortType, bool force)
{
    if (!IsTargetContainer(inv)) return false;
    const uint32_t fp = Fingerprint(inv);
    if (!force)
    {
        auto it = g_lastFingerprint.find(inv);
        if (it != g_lastFingerprint.end() && it->second == fp) return false;
    }

    WriteRequestedSort(inv, EngineSortType(sortType));
    void* pc = AnyPlayerController();
    if (pc)
    {
        ConanApi::Call<void>(pc, "ServerSetSortType", inv, EngineSortType(sortType));
        if (CallOk())
        {
            g_lastFingerprint[inv] = fp;
            return true;
        }
    }

    if (ManualSort(inv, sortType))
    {
        g_lastFingerprint[inv] = Fingerprint(inv);
        return true;
    }
    return false;
}

static int SortBatch(int budget, bool force)
{
    if (!g_invClass) return 0;
    const int total = g_api->NumObjects();
    if (total <= 0) return 0;

    const uint8_t sortType = ParseSortMethod(g_cfg.sortMethod);
    int sorted = 0;
    int checked = 0;
    while (checked < kIndicesPerTick && sorted < budget)
    {
        if (g_scanCursor >= total) g_scanCursor = 0;
        void* inv = g_api->GetObjectByIndex(g_scanCursor++);
        ++checked;
        if (!inv || IsDefaultObject(inv)) continue;
        if (!IsInventoryObject(inv)) continue;
        if (!IsTargetContainer(inv)) continue;
        if (SortInventory(inv, sortType, force)) ++sorted;
    }
    return sorted;
}

static void TickSort(void*)
{
    if (!g_cfg.enabled) return;
    g_cfg.Load("ContainerAutoSort");
    const int budget = g_cfg.containersPerTick < kMaxSortPerTick
        ? g_cfg.containersPerTick : kMaxSortPerTick;
    const int n = SortBatch(budget, false);
    if (n > 0)
    {
        g_totalSorted += n;
        g_api->Log("[ContainerAutoSort] sorted %d containers (%s), total=%d",
                   n, g_cfg.sortMethod.c_str(), g_totalSorted);
    }
}

static void SelfVerify(void*)
{
    if (g_verified || !g_cfg.enabled) return;
    void* invCls = g_api->FindClass("ItemInventory");
    void* pcCls = g_api->FindClass("ConanPlayerController");
    const bool ok = (g_taskId != 0) && invCls && pcCls;
    g_api->Log("[ContainerAutoSort] VERIFY task=%u invCls=%d pcCls=%d method=%s => %s",
               (unsigned)g_taskId, invCls ? 1 : 0, pcCls ? 1 : 0,
               g_cfg.sortMethod.c_str(), ok ? "PASS" : "FAIL");
    if (ok)
    {
        g_verified = true;
        if (g_verifyId) { g_api->CancelarAgendamento(g_verifyId); g_verifyId = 0; }
    }
}

static void* PlayerPawn(void* pc)
{
    void* ch = nullptr;
    int32_t off = g_api->OffsetDoMembro(pc, "Character");
    if (off >= 0) g_api->LerMembro(pc, uint32_t(off), &ch, sizeof(ch));
    if (!ch)
    {
        off = g_api->OffsetDoMembro(pc, "Pawn");
        if (off >= 0) g_api->LerMembro(pc, uint32_t(off), &ch, sizeof(ch));
    }
    return ch;
}

static void SendReply(void* pc, const std::string& text)
{
    if (!pc || text.empty()) return;
    ConanApi::Call<void>(pc, "ClientHUDShowNotification",
                         ConanApi::TextoRico(text.c_str()), bool(true), bool(false));
    if (!CallOk())
        ConanApi::Call<void>(pc, "ClientMessage", ConanApi::Texto(text.c_str()),
                             ConanApi::Nome("Event"), float(6.0f));
}

static void* NearestContainer(void* pawn, double radiusM)
{
    if (!pawn || !g_placeableClass) return nullptr;
    FVector pos = ConanApi::Call<FVector>(pawn, "K2_GetActorLocation");
    if (!CallOk()) return nullptr;

    const int total = g_api->NumObjects();
    if (total <= 0) return nullptr;

    void* best = nullptr;
    double bestD = radiusM * radiusM * 4.0;
    int checked = 0;
    int cursor = 0;
    while (checked < kPlaceableScanBudget && cursor < total)
    {
        void* box = g_api->GetObjectByIndex(cursor++);
        ++checked;
        if (!box || IsDefaultObject(box)) continue;
        if (!IsClassObject(box, g_placeableClass)) continue;

        FVector bp = ConanApi::Call<FVector>(box, "K2_GetActorLocation");
        if (!CallOk()) continue;
        const double dx = double(pos.X - bp.X);
        const double dy = double(pos.Y - bp.Y);
        const double dz = double(pos.Z - bp.Z);
        const double d2 = dx * dx + dy * dy + dz * dz;
        if (d2 < bestD) { bestD = d2; best = box; }
    }
    if (!best) return nullptr;
    void* inv = ConanApi::Call<void*>(best, "GetInventory");
    if (CallOk() && inv) return inv;
    int32_t off = g_api->OffsetDoMembro(best, "m_Inventory");
    if (off >= 0) g_api->LerMembro(best, uint32_t(off), &inv, sizeof(inv));
    return inv;
}

static bool ReadChatText(ConanChamada* c, char* buf, int len)
{
    buf[0] = 0;
    if (!c || !c->Parms || c->ParmsSize < int(CHAT_TEXTO + 16)) return false;
    return g_api->LerTextoDoJogo(c->Parms, CHAT_TEXTO, buf, len) != 0;
}

extern "C" ConanAcao ContainerAutoSort_OnChat(ConanChamada* c)
{
    if (!g_cfg.enabled || !c || !c->Obj) return CONAN_CONTINUAR;
    g_cfg.Load("ContainerAutoSort");

    char msg[256] = {0};
    if (!ReadChatText(c, msg, sizeof(msg))) return CONAN_CONTINUAR;
    const std::string text = msg;
    if (text.size() < g_cfg.prefix.size() + g_cfg.cmdSort.size()) return CONAN_CONTINUAR;
    if (text.compare(0, g_cfg.prefix.size(), g_cfg.prefix) != 0) return CONAN_CONTINUAR;

    std::string cmd = text.substr(g_cfg.prefix.size());
    while (!cmd.empty() && cmd[0] == ' ') cmd.erase(0, 1);
    std::string arg;
    const size_t sp = cmd.find(' ');
    if (sp != std::string::npos) { arg = cmd.substr(sp + 1); cmd = cmd.substr(0, sp); }
    if (cmd != g_cfg.cmdSort) return CONAN_CONTINUAR;

    if (!arg.empty())
    {
        for (auto& ch : arg)
            if (ch >= 'a' && ch <= 'z') ch = char(ch - 'a' + 'A');
        g_cfg.sortMethod = arg;
    }

    void* pawn = PlayerPawn(c->Obj);
    void* inv = NearestContainer(pawn, 8.0);
    if (!inv)
    {
        SendReply(c->Obj, "No container nearby to sort.");
        return CONAN_CANCELAR;
    }

    const uint8_t sortType = ParseSortMethod(g_cfg.sortMethod);
    const bool ok = SortInventory(inv, sortType, true);
    SendReply(c->Obj, ok ? ("Sorted nearby container by " + g_cfg.sortMethod + ".")
                         : "Could not sort nearby container.");
    return CONAN_CANCELAR;
}

extern "C" __declspec(dllexport)
void ConanPluginCarregar(const ConanApiTabela* api)
{
    if (!api || api->tamanho < sizeof(ConanApiTabela)) return;
    g_api = api;
    ConanApi::UsarTabela(api);
    g_cfg.Load("ContainerAutoSort");
    g_verified = false;
    g_lastFingerprint.clear();
    g_scanCursor = 0;
    CacheClasses();

    if (!g_cfg.enabled)
    {
        g_api->Log("[ContainerAutoSort] disabled in config.json");
        return;
    }

    g_api->Log("======================================================");
    g_api->Log(" ContainerAutoSort v1.1.0 — incremental scan, no FindObjects");
    g_api->Log(" Method: %s | interval: %ds | batch: %d (max %d)",
               g_cfg.sortMethod.c_str(), g_cfg.intervalSeconds,
               g_cfg.containersPerTick, kMaxSortPerTick);
    g_api->Log(" Commands: %s%s [ByType|ByName|ByWeight|ByAge]",
               g_cfg.prefix.c_str(), g_cfg.cmdSort.c_str());
    g_api->Log("======================================================");

    g_hookChat = g_api->HookProcessEvent("ServerSendChatMessage", ContainerAutoSort_OnChat, nullptr, 80);
    g_taskId = g_api->AgendarNaThreadDoJogo(TickSort, uint32_t(g_cfg.intervalSeconds), nullptr, 1);
    g_verifyId = g_api->AgendarNaThreadDoJogo(SelfVerify, 3, nullptr, 1);
}

extern "C" __declspec(dllexport)
void ConanPluginDescarregar(void)
{
    if (g_api)
    {
        if (g_hookChat) g_api->RemoverHook(g_hookChat);
        if (g_taskId) g_api->CancelarAgendamento(g_taskId);
        if (g_verifyId) g_api->CancelarAgendamento(g_verifyId);
        g_api->Log("[ContainerAutoSort] Unloaded.");
    }
    g_api = nullptr;
}
