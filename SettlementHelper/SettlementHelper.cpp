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
static uint32_t g_verifyId = 0;
static bool g_verified = false;
static int g_lastUnstick = 0;
static int g_lastTavern = 0;

struct SettlementConfig
{
    bool enabled = true;
    double scanRadiusMeters = 50.0;
    std::string prefix = "!";
    std::string cmdUnstick = "unstick";
    std::string cmdTavern = "tavern reroll";

    void Load(const char* folderName)
    {
        const char* configPath = g_api->CaminhoConfig(folderName);
        if (!configPath) return;
        ConanUtils::JsonValue json;
        if (!ConanUtils::JsonParser::ParseFile(configPath, json)) return;
        if (json.has("Enabled")) enabled = json["Enabled"].asBool(true);
        if (json.has("ScanRadiusMeters")) scanRadiusMeters = json["ScanRadiusMeters"].asDouble(50.0);
        if (json.has("Prefix")) prefix = json["Prefix"].asString("!");
        if (json.has("CommandUnstick")) cmdUnstick = json["CommandUnstick"].asString("unstick");
        if (json.has("CommandTavern")) cmdTavern = json["CommandTavern"].asString("tavern reroll");
    }
};

static SettlementConfig g_cfg;
static void* g_npcClasses[2];
static int g_npcClassCount = 0;
static void* g_tavernClass = nullptr;
static const uint32_t CHAT_UID = 0x038;
static const uint32_t CHAT_TEXTO = 0x068;

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

static void CacheNpcClasses()
{
    if (g_npcClassCount > 0) return;
    void* cls = g_api->FindClass("BaseNPCChar");
    if (cls) g_npcClasses[g_npcClassCount++] = cls;
    cls = g_api->FindClass("ConanCharacter");
    if (cls && g_npcClassCount < 2) g_npcClasses[g_npcClassCount++] = cls;
}

static void CacheTavernClass()
{
    if (g_tavernClass) return;
    g_tavernClass = g_api->FindClass("BP_PL_TavernBar_C");
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

static bool TeleportActor(void* actor, FVector dest)
{
    FRotator rot{ 0.0, 0.0, 0.0 };
    if (ConanApi::Call<bool>(actor, "K2_TeleportTo", dest, rot) && g_api->UltimaChamadaExecutou())
        return true;
    return ConanApi::Call<bool>(actor, "TeleportTo", dest, rot) && g_api->UltimaChamadaExecutou();
}

static int HandleUnstick(void* pc, int64_t uid)
{
    void* charPawn = PlayerPawn(pc);
    if (!charPawn) return 0;
    FVector playerPos;
    if (!GetActorPosition(charPawn, playerPos)) return 0;

    CacheNpcClasses();
    if (g_npcClassCount == 0) return 0;
    int count = 0;
    const double maxCm = g_cfg.scanRadiusMeters * 100.0;

    ScanUtil::ForEachOfClasses(g_api, g_npcClasses, g_npcClassCount, [&](void* npc) {
        if (!npc || npc == charPawn || IsDefaultObject(npc)) return;
        int64_t ownerId = 0;
        int32_t off = g_api->OffsetDoMembro(npc, "OwningPlayerId");
        if (off < 0) off = g_api->OffsetDoMembro(npc, "OwnerPlayerId");
        if (off >= 0) g_api->LerMembro(npc, uint32_t(off), &ownerId, sizeof(ownerId));
        if (ownerId == 0 || (uid != 0 && ownerId != uid)) return;

        FVector npcPos;
        if (!GetActorPosition(npc, npcPos)) return;
        const double dx = playerPos.X - npcPos.X;
        const double dy = playerPos.Y - npcPos.Y;
        const double dz = playerPos.Z - npcPos.Z;
        if (std::sqrt(dx * dx + dy * dy + dz * dz) > maxCm) return;

        FVector lift = npcPos;
        lift.Z += 25.0;
        if (TeleportActor(npc, lift)) ++count;
    });
    g_lastUnstick = count;
    return count;
}

static int HandleTavernReroll(void* pc)
{
    void* charPawn = PlayerPawn(pc);
    FVector playerPos{};
    const bool havePos = charPawn && GetActorPosition(charPawn, playerPos);
    const double maxCm = g_cfg.scanRadiusMeters * 100.0;

    CacheTavernClass();
    if (!g_tavernClass) return 0;

    int touched = 0;
    ScanUtil::ForEachOfClass(g_api, g_tavernClass, [&](void* bar) {
        if (IsDefaultObject(bar)) return;
        if (havePos)
        {
            FVector pos;
            if (!GetActorPosition(bar, pos)) return;
            const double dx = playerPos.X - pos.X;
            const double dy = playerPos.Y - pos.Y;
            const double dz = playerPos.Z - pos.Z;
            if (std::sqrt(dx * dx + dy * dy + dz * dz) > maxCm * 4.0) return;
        }
        ConanApi::Call<void>(bar, "DismissRandomPatron");
        if (g_api->UltimaChamadaExecutou()) ++touched;
        ConanApi::Call<void>(bar, "InitializeSharedPatronInventory");
        if (g_api->UltimaChamadaExecutou()) ++touched;
        ConanApi::Call<void>(bar, "FindSharedPatronInventory");
        if (g_api->UltimaChamadaExecutou()) ++touched;
    });
    g_lastTavern = touched;
    return touched;
}

static void SelfVerify(void*)
{
    if (g_verified || !g_cfg.enabled) return;
    void* tavernCls = g_api->FindClass("BP_PL_TavernBar_C");
    const bool ok = (g_hookChat != 0);
    g_api->Log("[SettlementHelper] VERIFY hook=%u tavernCls=%d unstick=%d tavern=%d => %s",
               (unsigned)g_hookChat, tavernCls ? 1 : 0, g_lastUnstick, g_lastTavern,
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
    const std::string pUnstick = g_cfg.prefix + g_cfg.cmdUnstick;
    const std::string pTavern = g_cfg.prefix + g_cfg.cmdTavern;
    int64_t uid = 0;
    const uint8_t* pUid = static_cast<const uint8_t*>(chat) + CHAT_UID;
    if (g_api->Legivel(pUid, 8)) uid = *reinterpret_cast<const int64_t*>(pUid);
    void* pc = c->Obj;

    if (msg.rfind(pUnstick, 0) == 0)
    {
        const int n = HandleUnstick(pc, uid);
        char reply[128];
        std::snprintf(reply, sizeof(reply), "Unstuck %d owned thrall(s) via K2_TeleportTo.", n);
        SendReply(pc, reply);
        return CONAN_CANCELAR;
    }
    if (msg.rfind(pTavern, 0) == 0)
    {
        const int n = HandleTavernReroll(pc);
        char reply[128];
        std::snprintf(reply, sizeof(reply),
                      n > 0 ? "Tavern refresh pulsed (%d calls on BP_PL_TavernBar)."
                            : "No BP_PL_TavernBar found nearby.",
                      n);
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
    g_cfg.Load("SettlementHelper");
    g_verified = false;

    if (!g_cfg.enabled)
    {
        g_api->Log("[SettlementHelper] Plugin disabled in config.json (Enabled=false). Load skipped.");
        return;
    }

    g_api->Log("======================================================");
    g_api->Log(" SettlementHelper v2.1.0 — K2_TeleportTo unstick + TavernBar refresh");
    g_api->Log(" Commands: !unstick | !tavern reroll | Radius: %.0fm", g_cfg.scanRadiusMeters);
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
        g_api->Log("[SettlementHelper] Unloaded.");
    }
    g_api = nullptr;
}
