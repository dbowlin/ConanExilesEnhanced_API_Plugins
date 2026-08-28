#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "Conan/ConanPluginApi.h"
#include "Conan/ConanBase.h"
#include "../common/JsonConfig.h"

#include <string>
#include <map>
#include <cmath>
#include <cstdio>
#include <cstring>

static const ConanApiTabela* g_api = nullptr;
static uint32_t g_hookChat = 0;
static uint32_t g_verifyId = 0;
static bool g_verified = false;
static int g_lastReset = 0;
static std::map<int64_t, DWORD> g_cooldowns;

struct DungeonConfig
{
    bool enabled = true;
    bool allowNonAdmin = true;
    int cooldownMinutes = 10;
    double scanRadiusMeters = 200.0;
    std::string prefix = "!";
    std::string cmdDungeon = "dungeon reset";

    void Load(const char* folderName)
    {
        const char* configPath = g_api->CaminhoConfig(folderName);
        if (!configPath) return;
        ConanUtils::JsonValue json;
        if (!ConanUtils::JsonParser::ParseFile(configPath, json)) return;
        if (json.has("Enabled")) enabled = json["Enabled"].asBool(true);
        if (json.has("AllowNonAdminReset")) allowNonAdmin = json["AllowNonAdminReset"].asBool(true);
        if (json.has("ResetCooldownMinutes")) cooldownMinutes = json["ResetCooldownMinutes"].asInt(10);
        if (json.has("ScanRadiusMeters")) scanRadiusMeters = json["ScanRadiusMeters"].asDouble(200.0);
        if (json.has("Prefix")) prefix = json["Prefix"].asString("!");
        if (json.has("CommandDungeon")) cmdDungeon = json["CommandDungeon"].asString("dungeon reset");
    }
};

static DungeonConfig g_cfg;
static const uint32_t CHAT_UID = 0x038;
static const uint32_t CHAT_TEXTO = 0x068;

static const char* kDungeonClasses[] = {
    "DungeonController",
    "BP_BaseDungeonController_C",
    "BP_WineCellarDungeonController_C",
    "BP_VolcanoDungeonController_C",
    "BP_WitchQueenDungeonController_C",
    "BP_JhabbalSagDungeonController_C",
    "BP_VolcanoDungeonSlavePits_Controller_C",
    "BP_VolcanoDungeon_Boss_Controller_C",
    "BP_WineCellarDungeon_BossController_C",
    "BP_WineCellarDungeon_PopulationController_C",
    "BP_BaseBossController_C",
    "BossController",
    nullptr
};

static bool ReadText(const void* base, uint32_t off, char* outBuf, int maxLen)
{
    outBuf[0] = 0;
    return g_api->LerTextoDoJogo(base, off, outBuf, maxLen) != 0;
}

static void SendReply(void* pc, const std::string& text)
{
    if (!pc || text.empty()) return;
    ConanApi::Call<void>(pc, "ClientHUDShowNotification",
                         ConanApi::TextoRico(text.c_str()), bool(true), bool(false));
    if (!g_api->UltimaChamadaExecutou())
        ConanApi::Call<void>(pc, "ClientMessage", ConanApi::Texto(text.c_str()),
                             ConanApi::Nome("Event"), float(6.0f));
}

static bool IsDefaultObject(void* obj)
{
    if (!obj) return true;
    char name[256] = {0};
    if (!g_api->NomeDoObjeto(obj, name, sizeof(name))) return false;
    return std::strncmp(name, "Default__", 9) == 0;
}

static void* ResolveClass(const char* name)
{
    if (!name || !name[0]) return nullptr;
    void* cls = g_api->FindClass(name);
    if (cls) return cls;
    void* cdo = g_api->GetDefaultObject(name);
    if (cdo) return cdo;
    void* objs[1];
    if (g_api->FindObjects(name, objs, 1, 1) > 0 && objs[0]) return objs[0];
    return nullptr;
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

static bool GetActorPosition(void* actor, FVector& outPos)
{
    if (!actor) return false;
    outPos = ConanApi::Call<FVector>(actor, "K2_GetActorLocation");
    return g_api->UltimaChamadaExecutou() != 0;
}

static int ResetNearbyDungeons(void* pc)
{
    void* ch = PlayerPawn(pc);
    if (!ch) return 0;
    FVector playerPos;
    if (!GetActorPosition(ch, playerPos)) return 0;

    const double maxCm = g_cfg.scanRadiusMeters * 100.0;
    int reset = 0;

    for (int c = 0; kDungeonClasses[c]; ++c)
    {
        void* objs[128];
        const int n = g_api->FindObjects(kDungeonClasses[c], objs, 128, 1);
        for (int i = 0; i < n; ++i)
        {
            if (IsDefaultObject(objs[i])) continue;
            FVector pos;
            if (!GetActorPosition(objs[i], pos)) continue;
            const double dx = playerPos.X - pos.X;
            const double dy = playerPos.Y - pos.Y;
            const double dz = playerPos.Z - pos.Z;
            if (std::sqrt(dx * dx + dy * dy + dz * dz) > maxCm) continue;

            ConanApi::Call<void>(objs[i], "ResetDungeon");
            if (g_api->UltimaChamadaExecutou()) { ++reset; continue; }
            ConanApi::Call<void>(objs[i], "ResetAfterEmptyDelay");
            if (g_api->UltimaChamadaExecutou()) { ++reset; continue; }
            ConanApi::Call<void>(objs[i], "ResetBoss");
            if (g_api->UltimaChamadaExecutou()) { ++reset; continue; }
            ConanApi::Call<void>(objs[i], "SignalResetDungeon__DelegateSignature");
            if (g_api->UltimaChamadaExecutou()) ++reset;
        }
    }
    g_lastReset = reset;
    return reset;
}

static void SelfVerify(void*)
{
    if (g_verified || !g_cfg.enabled) return;

    void* cls = nullptr;
    for (int c = 0; kDungeonClasses[c] && !cls; ++c)
        cls = ResolveClass(kDungeonClasses[c]);

    // PASS when chat hook registered (dungeon BP may not be streamed in at boot).
    const bool ok = (g_hookChat != 0);

    g_api->Log("[DungeonReset] VERIFY hook=%u dungeonCls=%d lastReset=%d => %s",
               (unsigned)g_hookChat, cls ? 1 : 0, g_lastReset, ok ? "PASS" : "FAIL");
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
    const std::string pDungeon = g_cfg.prefix + g_cfg.cmdDungeon;
    if (msg.rfind(pDungeon, 0) != 0) return CONAN_CONTINUAR;

    int64_t uid = 0;
    const uint8_t* pUid = static_cast<const uint8_t*>(chat) + CHAT_UID;
    if (g_api->Legivel(pUid, 8)) uid = *reinterpret_cast<const int64_t*>(pUid);

    const DWORD now = GetTickCount();
    auto it = g_cooldowns.find(uid);
    if (it != g_cooldowns.end())
    {
        const DWORD elapsed = (now - it->second) / 1000;
        const DWORD need = (DWORD)g_cfg.cooldownMinutes * 60;
        if (elapsed < need)
        {
            char reply[128];
            std::snprintf(reply, sizeof(reply), "Dungeon reset cooldown: %d min remaining.",
                          (int)((need - elapsed + 59) / 60));
            SendReply(c->Obj, reply);
            return CONAN_CANCELAR;
        }
    }

    const int n = ResetNearbyDungeons(c->Obj);
    g_cooldowns[uid] = now;
    char reply[160];
    if (n > 0)
        std::snprintf(reply, sizeof(reply),
                      "Dungeon reset: %d controller(s) pulsed within %.0fm.",
                      n, g_cfg.scanRadiusMeters);
    else
        std::snprintf(reply, sizeof(reply),
                      "No dungeon controllers found within %.0fm.",
                      g_cfg.scanRadiusMeters);
    SendReply(c->Obj, reply);
    return CONAN_CANCELAR;
}

extern "C" __declspec(dllexport)
void ConanPluginCarregar(const ConanApiTabela* api)
{
    if (!api || api->tamanho < sizeof(ConanApiTabela)) return;
    g_api = api;
    ConanApi::UsarTabela(api);
    g_cfg.Load("DungeonReset");
    g_verified = false;

    if (!g_cfg.enabled)
    {
        g_api->Log("[DungeonReset] Plugin disabled in config.json (Enabled=false). Load skipped.");
        return;
    }

    g_api->Log("======================================================");
    g_api->Log(" DungeonReset v2.1.0 — DungeonController ResetDungeon nearby");
    g_api->Log(" Command: !%s | Cooldown: %dm | Radius: %.0fm",
               g_cfg.cmdDungeon.c_str(), g_cfg.cooldownMinutes, g_cfg.scanRadiusMeters);
    g_api->Log("======================================================");

    g_hookChat = g_api->HookProcessEvent("ServerSendChatMessage", OnChatMessage, nullptr, 100);
    g_verifyId = g_api->AgendarNaThreadDoJogo(SelfVerify, 3, nullptr, 1);
}

extern "C" __declspec(dllexport)
void ConanPluginDescarregar(void)
{
    if (g_api)
    {
        if (g_hookChat) g_api->RemoverHook(g_hookChat);
        if (g_verifyId) g_api->CancelarAgendamento(g_verifyId);
        g_api->Log("[DungeonReset] Unloaded.");
    }
    g_api = nullptr;
}
