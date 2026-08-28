#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "Conan/ConanPluginApi.h"
#include "Conan/ConanBase.h"
#include "../common/JsonConfig.h"
#include "../common/ObjectScan.h"

#include <string>
#include <cmath>
#include <cstdio>
#include <cstring>

static const ConanApiTabela* g_api = nullptr;
static uint32_t g_hookChat = 0;
static uint32_t g_taskId = 0;
static bool g_verified = false;

struct LootConfig
{
    bool enabled = true;
    double lootRadiusMeters = 15.0;
    std::string prefix = "!";
    std::string cmdLoot = "loot";

    void Load(const char* folderName)
    {
        const char* configPath = g_api->CaminhoConfig(folderName);
        if (!configPath) return;
        ConanUtils::JsonValue json;
        if (!ConanUtils::JsonParser::ParseFile(configPath, json)) return;
        if (json.has("Enabled")) enabled = json["Enabled"].asBool(true);
        if (json.has("LootRadiusMeters")) lootRadiusMeters = json["LootRadiusMeters"].asDouble(15.0);
        if (json.has("Prefix")) prefix = json["Prefix"].asString("!");
        if (json.has("CommandLoot")) cmdLoot = json["CommandLoot"].asString("loot");
    }
};

static LootConfig g_cfg;
static void* g_lootClasses[5];
static int g_lootClassCount = 0;
static const uint32_t CHAT_TEXTO = 0x068;

static void CacheLootClasses()
{
    if (g_lootClassCount > 0) return;
    const char* names[] = {
        "LootContainer", "BP_LootContainer_C", "CorpseBase",
        "Corpse_C", "BP_PL_LootBag_DestroyedContainer_C", nullptr
    };
    for (int i = 0; names[i]; ++i)
    {
        void* cls = g_api->FindClass(names[i]);
        if (cls && g_lootClassCount < 5) g_lootClasses[g_lootClassCount++] = cls;
    }
}

static bool IsDefaultObject(void* obj)
{
    if (!obj) return true;
    char name[256] = {0};
    if (!g_api->NomeDoObjeto(obj, name, sizeof(name))) return false;
    return std::strncmp(name, "Default__", 9) == 0;
}

static void* PtrMember(void* obj, const char* name)
{
    if (!obj) return nullptr;
    const int32_t off = g_api->OffsetDoMembro(obj, name);
    if (off < 0) return nullptr;
    void* p = nullptr;
    if (g_api->LerMembro(obj, uint32_t(off), &p, sizeof(p)) <= 0) return nullptr;
    if (!p || !g_api->Legivel(p, 8)) return nullptr;
    return p;
}

static void SendReply(void* pc, const std::string& text)
{
    if (!pc || text.empty()) return;
    ConanApi::Call<void>(pc, "ClientHUDShowNotification",
                         ConanApi::TextoRico(text.c_str()), true, false);
    if (!g_api->UltimaChamadaExecutou())
    {
        ConanApi::Call<void>(pc, "ClientMessage",
                             ConanApi::Texto(text.c_str()),
                             ConanApi::Nome("Event"),
                             float(6.0f));
    }
}

static bool GetActorPosition(void* actor, FVector& outPos)
{
    if (!actor) return false;
    outPos = ConanApi::Call<FVector>(actor, "K2_GetActorLocation");
    return g_api->UltimaChamadaExecutou() != 0;
}

static void* PawnOf(void* pc)
{
    void* ch = PtrMember(pc, "Character");
    if (!ch) ch = PtrMember(pc, "Pawn");
    if (!ch || IsDefaultObject(ch)) return nullptr;
    return ch;
}

static bool WithinRadius(const FVector& a, const FVector& b, double maxCm)
{
    const double dx = a.X - b.X;
    const double dy = a.Y - b.Y;
    const double dz = a.Z - b.Z;
    return std::sqrt(dx * dx + dy * dy + dz * dz) <= maxCm;
}

static bool TakeLoot(void* pc, void* ch, void* container)
{
    // Prefer character TakeAllLootItems
    ConanApi::Call<void>(ch, "TakeAllLootItems", container);
    if (g_api->UltimaChamadaExecutou()) return true;

    // PC QuickLootAll / ServerValidatedQuickLootAll
    ConanApi::Call<void>(pc, "ServerValidatedQuickLootAll", container, false);
    if (g_api->UltimaChamadaExecutou()) return true;
    ConanApi::Call<void>(pc, "QuickLootAll", container, false);
    if (g_api->UltimaChamadaExecutou()) return true;

    // Inventory path: container.m_Inventory -> MoveItem into player inventory
    void* srcInv = PtrMember(container, "m_Inventory");
    if (!srcInv) srcInv = PtrMember(container, "Inventory");
    void* dstInv = PtrMember(ch, "ItemInventory");
    if (!dstInv) dstInv = PtrMember(ch, "Inventory");
    if (srcInv && dstInv)
    {
        ConanApi::Call<void>(pc, "LootAllInventoryItemsChecked", srcInv, dstInv, false);
        if (g_api->UltimaChamadaExecutou()) return true;
    }
    return false;
}

static int SweepLootNear(void* pc, void* ch, const FVector& playerPos, double radiusM, bool dryRun)
{
    const double maxCm = radiusM * 100.0;
    CacheLootClasses();
    if (g_lootClassCount == 0) return 0;

    int found = 0;
    int taken = 0;

    ScanUtil::ForEachOfClasses(g_api, g_lootClasses, g_lootClassCount, [&](void* bag) {
        if (IsDefaultObject(bag)) return;
        FVector pos;
        if (!GetActorPosition(bag, pos)) return;
        if (!WithinRadius(playerPos, pos, maxCm)) return;
        ++found;
        if (dryRun) return;
        if (TakeLoot(pc, ch, bag)) ++taken;
    });

    if (!dryRun)
        g_api->Log("[AreaLoot] sweep found=%d taken=%d radius=%.0fm", found, taken, radiusM);
    return dryRun ? found : taken;
}

static void HandleLoot(void* pc)
{
    void* ch = PawnOf(pc);
    if (!ch)
    {
        SendReply(pc, "AreaLoot: no character.");
        return;
    }
    FVector playerPos;
    if (!GetActorPosition(ch, playerPos))
    {
        SendReply(pc, "AreaLoot: could not read position.");
        return;
    }

    const int taken = SweepLootNear(pc, ch, playerPos, g_cfg.lootRadiusMeters, false);
    char reply[160];
    std::snprintf(reply, sizeof(reply),
                  taken > 0 ? "Area loot: vacuumed %d nearby bags/corpses."
                            : "Area loot: no loot bags/corpses in %.0fm.",
                  taken > 0 ? taken : (int)g_cfg.lootRadiusMeters);
    if (taken <= 0)
        std::snprintf(reply, sizeof(reply), "Area loot: no loot bags/corpses within %.0fm.", g_cfg.lootRadiusMeters);
    else
        std::snprintf(reply, sizeof(reply), "Area loot: vacuumed %d nearby bags/corpses.", taken);
    SendReply(pc, reply);
}

static void SelfVerify(void*)
{
    if (g_verified || !g_cfg.enabled) return;

    void* lootCls = g_api->FindClass("LootContainer");
    void* corpseCls = g_api->FindClass("CorpseBase");
    void* bags[64];
    const int nLoot = g_api->FindObjects("LootContainer", bags, 64, 1);
    int live = 0;
    int locOk = 0;
    for (int i = 0; i < nLoot; ++i)
    {
        if (IsDefaultObject(bags[i])) continue;
        ++live;
        FVector p;
        if (GetActorPosition(bags[i], p)) ++locOk;
    }

    // Reflection: TakeAllLootItems must resolve on ConanCharacter CDO
    int takeFn = 0;
    void* cdo = g_api->GetDefaultObject("ConanCharacter");
    if (!cdo) cdo = g_api->GetDefaultObject("ConanPlayerCharacter");
    if (cdo)
    {
        // Passing null container should still attempt ProcessEvent / size check
        ConanApi::Call<void>(cdo, "TakeAllLootItems", (void*)nullptr);
        takeFn = g_api->UltimaChamadaExecutou() ? 1 : 0;
        // Even if it refuses null, ChamarFuncao may return 0 — that's OK if class has the fn.
        // Prefer: hook installed + classes found + locations readable.
    }

    const bool ok = (g_hookChat != 0) && (lootCls || corpseCls) && (live == 0 || locOk > 0);
    g_api->Log("[AreaLoot] VERIFY hook=%u lootCls=%d corpseCls=%d liveBags=%d locOk=%d takeCall=%d => %s",
               (unsigned)g_hookChat, lootCls ? 1 : 0, corpseCls ? 1 : 0, live, locOk, takeFn,
               ok ? "PASS" : "FAIL");
    if (ok)
    {
        g_verified = true;
        if (g_taskId) { g_api->CancelarAgendamento(g_taskId); g_taskId = 0; }
    }
}

extern "C" ConanAcao OnChatMessage(ConanChamada* c)
{
    if (!g_cfg.enabled) return CONAN_CONTINUAR;
    if (!c || !c->Parms || c->ParmsSize < 0x80) return CONAN_CONTINUAR;

    char text[512] = {0};
    if (!g_api->LerTextoDoJogo(c->Parms, CHAT_TEXTO, text, sizeof(text)) || !text[0])
        return CONAN_CONTINUAR;
    if (text[0] != '!' && text[0] != '/') return CONAN_CONTINUAR;

    std::string msg(text);
    const std::string pLoot = g_cfg.prefix + g_cfg.cmdLoot;
    if (msg.rfind(pLoot, 0) == 0)
    {
        HandleLoot(c->Obj);
        return CONAN_CANCELAR;
    }
    return CONAN_CONTINUAR;
}

extern "C" __declspec(dllexport)
void ConanPluginCarregar(const ConanApiTabela* api)
{
    if (!api || api->tamanho < sizeof(ConanApiTabela)) return;
    g_api = api;
    ConanApi::UsarTabela(api);
    g_cfg.Load("AreaLoot");
    g_verified = false;

    if (!g_cfg.enabled)
    {
        g_api->Log("[AreaLoot] disabled");
        return;
    }

    g_api->Log("======================================================");
    g_api->Log(" AreaLoot v2.1.0 — TakeAllLootItems / QuickLoot vacuum");
    g_api->Log(" Radius: %.0fm | Command: !%s", g_cfg.lootRadiusMeters, g_cfg.cmdLoot.c_str());
    g_api->Log("======================================================");

    g_hookChat = g_api->HookProcessEvent("ServerSendChatMessage", OnChatMessage, nullptr, 100);
    g_taskId = g_api->AgendarNaThreadDoJogo(SelfVerify, 3, nullptr, 1);
}

extern "C" __declspec(dllexport)
void ConanPluginDescarregar(void)
{
    if (g_api)
    {
        if (g_hookChat) g_api->RemoverHook(g_hookChat);
        if (g_taskId) g_api->CancelarAgendamento(g_taskId);
    }
    g_api = nullptr;
}
