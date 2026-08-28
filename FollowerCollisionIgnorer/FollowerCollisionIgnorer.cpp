#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "Conan/ConanPluginApi.h"
#include "Conan/ConanBase.h"
#include "../common/JsonConfig.h"

#include <cstdio>
#include <cstring>

static const ConanApiTabela* g_api = nullptr;
static uint32_t g_taskId = 0;
static uint32_t g_verifyId = 0;
static uint32_t g_hookLogin = 0;
static bool g_verified = false;

static const uint8_t kChannelPawn = 2;
static const uint8_t kChannelVehicle = 6;
static const uint8_t kResponseIgnore = 0;

static const int kIndicesPerTick = 4096;
static const int kMaxPerTick = 4;
static const int kDoneCapacity = 512;
static const DWORD kLoginDeferMs = 15000;

struct CollisionConfig
{
    bool enabled = true;
    bool ignorePlayerCollision = true;
    bool ignoreMountedCollision = true;

    void Load(const char* folderName)
    {
        const char* configPath = g_api->CaminhoConfig(folderName);
        if (!configPath) return;
        ConanUtils::JsonValue json;
        if (!ConanUtils::JsonParser::ParseFile(configPath, json)) return;
        if (json.has("Enabled")) enabled = json["Enabled"].asBool(true);
        if (json.has("IgnorePlayerCollision"))
            ignorePlayerCollision = json["IgnorePlayerCollision"].asBool(true);
        if (json.has("IgnoreMountedCollision"))
            ignoreMountedCollision = json["IgnoreMountedCollision"].asBool(true);
    }
};

static CollisionConfig g_cfg;
static void* g_done[kDoneCapacity];
static int g_doneCount = 0;
static void* g_npcClasses[2];
static int g_npcClassCount = 0;
static int g_scanCursor = 0;
static volatile long g_deferUntilMs = 0;

static bool IsDefaultObject(void* obj)
{
    if (!obj) return true;
    char name[256] = {0};
    if (!g_api->NomeDoObjeto(obj, name, sizeof(name))) return false;
    return std::strncmp(name, "Default__", 9) == 0;
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

static bool IsNpcObject(void* obj)
{
    if (!obj || g_npcClassCount == 0) return false;
    void* objClass = nullptr;
    g_api->LerMembro(obj, 0x10, &objClass, sizeof(objClass));
    if (!objClass) return false;
    for (int i = 0; i < g_npcClassCount; ++i)
    {
        if (g_npcClasses[i] && ClassIsOrExtends(objClass, g_npcClasses[i]))
            return true;
    }
    return false;
}

static int64_t ReadOwnerId(void* npc)
{
    int64_t ownerId = 0;
    int32_t off = g_api->OffsetDoMembro(npc, "OwningPlayerId");
    if (off < 0) off = g_api->OffsetDoMembro(npc, "OwnerPlayerId");
    if (off >= 0) g_api->LerMembro(npc, uint32_t(off), &ownerId, sizeof(ownerId));
    return ownerId;
}

static void* CapsuleOf(void* npc)
{
    void* capsule = nullptr;
    int32_t off = g_api->OffsetDoMembro(npc, "CapsuleComponent");
    if (off >= 0) g_api->LerMembro(npc, uint32_t(off), &capsule, sizeof(capsule));
    if (!capsule || IsDefaultObject(capsule)) return nullptr;
    return capsule;
}

static bool AlreadyProcessed(void* npc)
{
    for (int i = 0; i < g_doneCount; ++i)
        if (g_done[i] == npc) return true;
    return false;
}

static void MarkProcessed(void* npc)
{
    if (AlreadyProcessed(npc)) return;
    if (g_doneCount >= kDoneCapacity)
    {
        g_doneCount = kDoneCapacity / 2;
        for (int i = 0; i < g_doneCount; ++i)
            g_done[i] = g_done[i + kDoneCapacity / 2];
    }
    g_done[g_doneCount++] = npc;
}

static bool IgnoreChannel(void* capsule, uint8_t channel)
{
    ConanApi::Call<void>(capsule, "SetCollisionResponseToChannel", channel, kResponseIgnore);
    return g_api->UltimaChamadaExecutou() != 0;
}

static bool ApplyCapsule(void* npc)
{
    void* capsule = CapsuleOf(npc);
    if (!capsule) return false;

    bool ok = false;
    if (g_cfg.ignorePlayerCollision && IgnoreChannel(capsule, kChannelPawn))
        ok = true;
    if (g_cfg.ignoreMountedCollision && IgnoreChannel(capsule, kChannelVehicle))
        ok = true;
    if (ok) MarkProcessed(npc);
    return ok;
}

static void CacheNpcClasses()
{
    g_npcClassCount = 0;
    void* cls = g_api->FindClass("BaseNPCChar");
    if (cls) g_npcClasses[g_npcClassCount++] = cls;
    cls = g_api->FindClass("ConanCharacter");
    if (cls && g_npcClassCount < 2) g_npcClasses[g_npcClassCount++] = cls;
}

static int ProcessIncrementalScan(int budget)
{
    const int total = g_api->NumObjects();
    if (total <= 0 || g_npcClassCount == 0) return 0;

    int applied = 0;
    int checked = 0;

    while (applied < budget && checked < kIndicesPerTick)
    {
        if (g_scanCursor >= total) g_scanCursor = 0;
        void* obj = g_api->GetObjectByIndex(g_scanCursor++);
        ++checked;
        if (!obj || IsDefaultObject(obj)) continue;
        if (!IsNpcObject(obj)) continue;
        if (ReadOwnerId(obj) == 0) continue;
        if (AlreadyProcessed(obj)) continue;
        if (ApplyCapsule(obj)) ++applied;
    }
    return applied;
}

static bool IsDeferred()
{
    const long until = InterlockedCompareExchange(&g_deferUntilMs, 0, 0);
    if (until <= 0) return false;
    return static_cast<long>(GetTickCount()) < until;
}

static void ApplyCollisionSettings(void*)
{
    if (!g_cfg.enabled || IsDeferred()) return;

    const int applied = ProcessIncrementalScan(kMaxPerTick);
    if (applied > 0)
        g_api->Log("[FollowerCollisionIgnorer] applied Ignore on %d followers", applied);
}

static void SelfVerify(void*)
{
    if (g_verified || !g_cfg.enabled) return;

    int callOk = 0;
    void* npcs[32];
    const int n = g_api->FindObjects("BaseNPCChar", npcs, 32, 1);
    for (int i = 0; i < n && !callOk; ++i)
    {
        if (IsDefaultObject(npcs[i])) continue;
        void* capsule = CapsuleOf(npcs[i]);
        if (!capsule) continue;
        if (IgnoreChannel(capsule, kChannelPawn)) callOk = 1;
    }

    const bool ok = (g_taskId != 0) && (callOk != 0);
    g_api->Log("[FollowerCollisionIgnorer] VERIFY task=%u npcCls=%d callOk=%d => %s",
               (unsigned)g_taskId, g_npcClassCount > 0 ? 1 : 0, callOk, ok ? "PASS" : "FAIL");
    if (ok)
    {
        g_verified = true;
        if (g_verifyId) { g_api->CancelarAgendamento(g_verifyId); g_verifyId = 0; }
    }
}

static void DeferAfterLogin()
{
    const DWORD until = GetTickCount() + kLoginDeferMs;
    InterlockedExchange(&g_deferUntilMs, static_cast<long>(until));
}

extern "C" ConanAcao FollowerCollisionIgnorer_OnLogin(ConanChamada* c)
{
    (void)c;
    DeferAfterLogin();
    return CONAN_CONTINUAR;
}

extern "C" __declspec(dllexport)
void ConanPluginCarregar(const ConanApiTabela* api)
{
    if (!api || api->tamanho < sizeof(ConanApiTabela)) return;
    g_api = api;
    ConanApi::UsarTabela(api);
    g_cfg.Load("FollowerCollisionIgnorer");
    g_verified = false;
    g_doneCount = 0;
    g_scanCursor = 0;
    CacheNpcClasses();

    if (!g_cfg.enabled)
    {
        g_api->Log("[FollowerCollisionIgnorer] Plugin disabled in config.json (Enabled=false). Load skipped.");
        return;
    }

    g_api->Log("======================================================");
    g_api->Log(" FollowerCollisionIgnorer v2.2.0 — cursor scan, no FindObjects");
    g_api->Log(" Player: %s | Mounted(Vehicle): %s | %d idx/tick, max %d apply",
               g_cfg.ignorePlayerCollision ? "Ignore" : "Vanilla",
               g_cfg.ignoreMountedCollision ? "Ignore" : "Vanilla",
               kIndicesPerTick, kMaxPerTick);
    g_api->Log(" PostLogin defer: %us | tick interval: 5s", (unsigned)(kLoginDeferMs / 1000));
    g_api->Log("======================================================");

    g_hookLogin = g_api->HookProcessEvent("K2_PostLogin", FollowerCollisionIgnorer_OnLogin, nullptr, 50);
    g_taskId = g_api->AgendarNaThreadDoJogo(ApplyCollisionSettings, 5, nullptr, 1);
    g_verifyId = g_api->AgendarNaThreadDoJogo(SelfVerify, 3, nullptr, 1);
}

extern "C" __declspec(dllexport)
void ConanPluginDescarregar(void)
{
    if (g_api)
    {
        if (g_hookLogin) g_api->RemoverHook(g_hookLogin);
        if (g_taskId) g_api->CancelarAgendamento(g_taskId);
        if (g_verifyId) g_api->CancelarAgendamento(g_verifyId);
        g_api->Log("[FollowerCollisionIgnorer] Unloaded.");
    }
    g_api = nullptr;
}
