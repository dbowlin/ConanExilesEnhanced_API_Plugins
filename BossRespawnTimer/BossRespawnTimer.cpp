#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "Conan/ConanPluginApi.h"
#include "Conan/ConanBase.h"
#include "../common/JsonConfig.h"
#include "../common/ObjectScan.h"

#include <string>
#include <cstdio>
#include <cstring>

static const ConanApiTabela* g_api = nullptr;
static uint32_t g_hookChat = 0;
static uint32_t g_tickId = 0;
static uint32_t g_verifyId = 0;
static bool g_verified = false;
static int g_lastReset = 0;
static int g_timersPatched = 0;

struct BossTimerConfig
{
    bool enabled = true;
    int respawnMinutes = 3;
    std::string prefix = "!";
    std::string cmdTimer = "bosstimer";
    std::string cmdRespawn = "respawnbosses";

    void Load(const char* folderName)
    {
        const char* configPath = g_api->CaminhoConfig(folderName);
        if (!configPath) return;
        ConanUtils::JsonValue json;
        if (!ConanUtils::JsonParser::ParseFile(configPath, json)) return;
        if (json.has("Enabled")) enabled = json["Enabled"].asBool(true);
        if (json.has("BossRespawnMinutes")) respawnMinutes = json["BossRespawnMinutes"].asInt(3);
        if (json.has("Prefix")) prefix = json["Prefix"].asString("!");
        if (json.has("CommandBossTimer")) cmdTimer = json["CommandBossTimer"].asString("bosstimer");
        if (json.has("CommandRespawnBosses")) cmdRespawn = json["CommandRespawnBosses"].asString("respawnbosses");
    }
};

static BossTimerConfig g_cfg;
static void* g_bossClasses[3];
static int g_bossClassCount = 0;
static ScanUtil::IncrementalScan g_bossScan;
static const int kPatchPerTick = 4;
static const uint32_t CHAT_TEXTO = 0x068;

static bool ReadText(const void* base, uint32_t off, char* outBuf, int maxLen)
{
    outBuf[0] = 0;
    return g_api->LerTextoDoJogo(base, off, outBuf, maxLen) != 0;
}

static bool IsDefaultObject(void* obj)
{
    if (!obj) return true;
    char name[256] = {0};
    if (!g_api->NomeDoObjeto(obj, name, sizeof(name))) return false;
    return std::strncmp(name, "Default__", 9) == 0;
}

static void SendReply(void* playerController, const std::string& text)
{
    if (!playerController || text.empty()) return;
    ConanApi::Call<void>(playerController, "ClientHUDShowNotification",
                         ConanApi::TextoRico(text.c_str()), bool(true), bool(false));
    if (!g_api->UltimaChamadaExecutou())
    {
        ConanApi::Call<void>(playerController, "ClientMessage",
                             ConanApi::Texto(text.c_str()), ConanApi::Nome("Event"), float(6.0f));
    }
}

static void CacheBossClasses()
{
    if (g_bossClassCount > 0) return;
    const char* names[] = { "BossController", "BP_BaseBossController_C", "BP_DagonHead_C", nullptr };
    for (int i = 0; names[i]; ++i)
    {
        void* cls = g_api->FindClass(names[i]);
        if (cls && g_bossClassCount < 3) g_bossClasses[g_bossClassCount++] = cls;
    }
}

static int PatchBossTimerFields(void* obj)
{
    if (!obj || ScanUtil::IsDefaultObject(g_api, obj)) return 0;

    int patched = 0;
    const double seconds = double(g_cfg.respawnMinutes) * 60.0;

    int32_t off = g_api->OffsetDoMembro(obj, "RespawnTime");
    if (off >= 0)
    {
        double v = seconds;
        if (g_api->EscreverMembro(obj, uint32_t(off), &v, sizeof(v)) > 0)
            ++patched;
    }
    off = g_api->OffsetDoMembro(obj, "EncounterResetTimer");
    if (off >= 0)
    {
        double v = seconds;
        if (g_api->EscreverMembro(obj, uint32_t(off), &v, sizeof(v)) > 0)
            ++patched;
    }
    off = g_api->OffsetDoMembro(obj, "RespawnTimer");
    if (off >= 0)
    {
        double v = seconds;
        (void)v;
    }
    return patched;
}

static int PatchRespawnTimers()
{
    CacheBossClasses();
    if (g_bossClassCount == 0) return 0;

    int patched = 0;
    ScanUtil::ForEachOfClasses(g_api, g_bossClasses, g_bossClassCount, [&](void* obj) {
        patched += PatchBossTimerFields(obj);
    });
    g_timersPatched = patched;
    return patched;
}

static int ForceRespawnBosses()
{
    CacheBossClasses();
    if (g_bossClassCount == 0) return 0;

    int reset = 0;
    ScanUtil::ForEachOfClasses(g_api, g_bossClasses, g_bossClassCount, [&](void* obj) {
        if (ScanUtil::IsDefaultObject(g_api, obj)) return;
        ConanApi::Call<void>(obj, "ResetBoss");
        if (g_api->UltimaChamadaExecutou()) { ++reset; return; }
        ConanApi::Call<void>(obj, "BossSpawner");
        if (g_api->UltimaChamadaExecutou()) ++reset;
    });
    g_lastReset = reset;
    return reset;
}

static void TickPatch(void*)
{
    if (!g_cfg.enabled) return;
    CacheBossClasses();
    if (g_bossClassCount == 0) return;

    g_bossScan.Tick(g_api, kPatchPerTick,
        [&](void* obj) {
            return ScanUtil::MatchesAnyClass(g_api, obj, g_bossClasses, g_bossClassCount);
        },
        [&](void* obj) {
            const int n = PatchBossTimerFields(obj);
            if (n > 0) g_timersPatched += n;
            return n > 0;
        });
}

static void SelfVerify(void*)
{
    if (g_verified || !g_cfg.enabled) return;
    void* cls = g_api->FindClass("BP_BaseBossController_C");
    if (!cls) cls = g_api->FindClass("BossController");
    const bool ok = (g_hookChat != 0) && (cls != nullptr);
    g_api->Log("[BossRespawnTimer] VERIFY hook=%u bossCls=%d patched=%d lastReset=%d => %s",
               (unsigned)g_hookChat, cls ? 1 : 0, g_timersPatched, g_lastReset,
               ok ? "PASS" : "FAIL");
    if (ok)
    {
        g_verified = true;
        if (g_verifyId) { g_api->CancelarAgendamento(g_verifyId); g_verifyId = 0; }
    }
}

extern "C" ConanAcao OnChatMessage(ConanChamada* c)
{
    if (!g_cfg.enabled) return CONAN_CONTINUAR;
    if (!c || !c->Parms || c->ParmsSize < 0x80) return CONAN_CONTINUAR;
    const void* chat = c->Parms;
    char text[512] = {0};
    ReadText(chat, CHAT_TEXTO, text, sizeof(text));
    if (text[0] != '!' && text[0] != '/') return CONAN_CONTINUAR;

    std::string msg(text);
    const std::string pTimer = g_cfg.prefix + g_cfg.cmdTimer;
    const std::string pRespawn = g_cfg.prefix + g_cfg.cmdRespawn;
    void* pc = c->Obj;

    if (msg.rfind(pTimer, 0) == 0)
    {
        const int n = PatchRespawnTimers();
        char reply[160];
        std::snprintf(reply, sizeof(reply),
                      "Boss respawn target: %d min (patched %d timer field(s)).",
                      g_cfg.respawnMinutes, n);
        SendReply(pc, reply);
        return CONAN_CANCELAR;
    }

    if (msg.rfind(pRespawn, 0) == 0)
    {
        const int n = ForceRespawnBosses();
        char reply[128];
        std::snprintf(reply, sizeof(reply), "Boss reset pulse: %d controller(s) ResetBoss/BossSpawner.", n);
        SendReply(pc, reply);
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
    g_cfg.Load("BossRespawnTimer");
    g_verified = false;

    if (!g_cfg.enabled)
    {
        g_api->Log("[BossRespawnTimer] Plugin disabled in config.json (Enabled=false). Load skipped.");
        return;
    }

    g_api->Log("======================================================");
    g_api->Log(" BossRespawnTimer v2.1.0 — RespawnTime + ResetBoss");
    g_api->Log(" Respawn Delay: %d minutes | Commands: !%s | !%s",
               g_cfg.respawnMinutes, g_cfg.cmdTimer.c_str(), g_cfg.cmdRespawn.c_str());
    g_api->Log("======================================================");

    g_hookChat = g_api->HookProcessEvent("ServerSendChatMessage", OnChatMessage, nullptr, 100);
    g_tickId = g_api->AgendarNaThreadDoJogo(TickPatch, 60, nullptr, 1);
    g_verifyId = g_api->AgendarNaThreadDoJogo(SelfVerify, 3, nullptr, 1);
}

extern "C" __declspec(dllexport)
void ConanPluginDescarregar(void)
{
    if (g_api)
    {
        if (g_hookChat) g_api->RemoverHook(g_hookChat);
        if (g_tickId) g_api->CancelarAgendamento(g_tickId);
        if (g_verifyId) g_api->CancelarAgendamento(g_verifyId);
        g_api->Log("[BossRespawnTimer] Unloaded.");
    }
    g_api = nullptr;
}
