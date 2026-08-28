#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "Conan/ConanPluginApi.h"
#include "Conan/ConanBase.h"
#include "../common/JsonConfig.h"

#include <string>
#include <map>
#include <cstdio>
#include <cstring>

static const ConanApiTabela* g_api = nullptr;
static uint32_t g_hookChat = 0;
static uint32_t g_hookDamage = 0;
static uint32_t g_verifyId = 0;
static bool g_verified = false;

struct DpsConfig
{
    bool enabled = true;
    bool trackFollowerDamage = true;
    std::string prefix = "!";
    std::string cmdDps = "dps";

    void Load(const char* folderName)
    {
        const char* configPath = g_api->CaminhoConfig(folderName);
        if (!configPath) return;
        ConanUtils::JsonValue json;
        if (!ConanUtils::JsonParser::ParseFile(configPath, json)) return;
        if (json.has("Enabled")) enabled = json["Enabled"].asBool(true);
        if (json.has("TrackFollowerDamage")) trackFollowerDamage = json["TrackFollowerDamage"].asBool(true);
        if (json.has("Prefix")) prefix = json["Prefix"].asString("!");
        if (json.has("CommandDps")) cmdDps = json["CommandDps"].asString("dps");
    }
};

static DpsConfig g_cfg;
static std::map<int64_t, double> g_damageDealt;
static const uint32_t CHAT_UID = 0x038;
static const uint32_t CHAT_TEXTO = 0x068;

static bool ReadText(const void* base, uint32_t off, char* outBuf, int maxLen)
{
    outBuf[0] = 0;
    return g_api->LerTextoDoJogo(base, off, outBuf, maxLen) != 0;
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

static void* ResolveCharacter(void* actorOrCtrl)
{
    if (!actorOrCtrl) return nullptr;
    if (g_api->DescendeDe(actorOrCtrl, "ConanCharacter") ||
        g_api->DescendeDe(actorOrCtrl, "BaseNPCChar"))
        return actorOrCtrl;
    void* ch = nullptr;
    int32_t off = g_api->OffsetDoMembro(actorOrCtrl, "Character");
    if (off >= 0) g_api->LerMembro(actorOrCtrl, uint32_t(off), &ch, sizeof(ch));
    if (!ch)
    {
        off = g_api->OffsetDoMembro(actorOrCtrl, "Pawn");
        if (off >= 0) g_api->LerMembro(actorOrCtrl, uint32_t(off), &ch, sizeof(ch));
    }
    return ch;
}

static int64_t IdOf(void* character)
{
    if (!character) return 0;
    int64_t id = 0;
    int32_t off = g_api->OffsetDoMembro(character, "OwningPlayerId");
    if (off < 0) off = g_api->OffsetDoMembro(character, "PlayerId");
    if (off < 0) off = g_api->OffsetDoMembro(character, "OwnerPlayerId");
    if (off >= 0) g_api->LerMembro(character, uint32_t(off), &id, sizeof(id));
    if (id == 0) id = reinterpret_cast<int64_t>(character);
    return id;
}

extern "C" ConanAcao OnReceiveAnyDamage(ConanChamada* c)
{
    if (!g_cfg.enabled || !c || !c->Parms || c->ParmsSize < 0x20) return CONAN_CONTINUAR;
    float dmg = 0.0f;
    std::memcpy(&dmg, c->Parms, sizeof(float));
    if (dmg <= 0.0f) return CONAN_CONTINUAR;

    const uint8_t* p = static_cast<const uint8_t*>(c->Parms);
    void* instigatedBy = nullptr;
    void* damageCauser = nullptr;
    std::memcpy(&instigatedBy, p + 0x10, sizeof(void*));
    std::memcpy(&damageCauser, p + 0x18, sizeof(void*));

    void* attacker = ResolveCharacter(instigatedBy);
    if (!attacker) attacker = ResolveCharacter(damageCauser);
    if (!attacker) return CONAN_CONTINUAR;

    if (!g_api->DescendeDe(attacker, "ConanCharacter") &&
        !g_api->DescendeDe(attacker, "BaseNPCChar"))
        return CONAN_CONTINUAR;

    if (!g_cfg.trackFollowerDamage && g_api->DescendeDe(attacker, "BaseNPCChar"))
    {
        int64_t oid = 0;
        int32_t off = g_api->OffsetDoMembro(attacker, "OwningPlayerId");
        if (off >= 0) g_api->LerMembro(attacker, uint32_t(off), &oid, sizeof(oid));
        if (oid != 0) return CONAN_CONTINUAR; // skip thralls when disabled — actually trackFollower means INCLUDE
    }

    const int64_t id = IdOf(attacker);
    g_damageDealt[id] += double(dmg);
    return CONAN_CONTINUAR;
}

static void SelfVerify(void*)
{
    if (g_verified || !g_cfg.enabled) return;
    const bool ok = (g_hookChat != 0) && (g_hookDamage != 0);
    g_api->Log("[DamageMeter] VERIFY hookChat=%u hookDmg=%u tracked=%zu => %s",
               (unsigned)g_hookChat, (unsigned)g_hookDamage, g_damageDealt.size(),
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
    const std::string pDps = g_cfg.prefix + g_cfg.cmdDps;
    if (msg.rfind(pDps, 0) != 0) return CONAN_CONTINUAR;

    int64_t uid = 0;
    const uint8_t* pUid = static_cast<const uint8_t*>(chat) + CHAT_UID;
    if (g_api->Legivel(pUid, 8)) uid = *reinterpret_cast<const int64_t*>(pUid);

    double total = 0.0;
    if (uid != 0 && g_damageDealt.count(uid)) total = g_damageDealt[uid];
    else
    {
        for (auto& kv : g_damageDealt) total += kv.second;
    }

    char reply[128];
    std::snprintf(reply, sizeof(reply), "Session damage tracked: %.0f (entries=%zu).",
                  total, g_damageDealt.size());
    SendReply(c->Obj, reply);
    return CONAN_CANCELAR;
}

extern "C" __declspec(dllexport)
void ConanPluginCarregar(const ConanApiTabela* api)
{
    if (!api || api->tamanho < sizeof(ConanApiTabela)) return;
    g_api = api;
    ConanApi::UsarTabela(api);
    g_cfg.Load("DamageMeter");
    g_verified = false;
    g_damageDealt.clear();

    if (!g_cfg.enabled)
    {
        g_api->Log("[DamageMeter] Plugin disabled in config.json (Enabled=false). Load skipped.");
        return;
    }

    g_api->Log("======================================================");
    g_api->Log(" DamageMeter v2.0.0 — ReceiveAnyDamage tracker");
    g_api->Log(" Command: !%s", g_cfg.cmdDps.c_str());
    g_api->Log("======================================================");

    g_hookChat = g_api->HookProcessEvent("ServerSendChatMessage", OnChatMessage, nullptr, 100);
    g_hookDamage = g_api->HookProcessEvent("ReceiveAnyDamage", OnReceiveAnyDamage, nullptr, 40);
    g_verifyId = g_api->AgendarNaThreadDoJogo(SelfVerify, 3, nullptr, 1);
}

extern "C" __declspec(dllexport)
void ConanPluginDescarregar(void)
{
    if (g_api)
    {
        if (g_hookChat) g_api->RemoverHook(g_hookChat);
        if (g_hookDamage) g_api->RemoverHook(g_hookDamage);
        if (g_verifyId) g_api->CancelarAgendamento(g_verifyId);
        g_api->Log("[DamageMeter] Unloaded.");
    }
    g_api = nullptr;
}
