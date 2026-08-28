#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "Conan/ConanPluginApi.h"
#include "Conan/ConanBase.h"
#include "../common/JsonConfig.h"

#include <string>
#include <cstdio>
#include <cstring>

static const ConanApiTabela* g_api = nullptr;
static uint32_t g_hookChat = 0;
static uint32_t g_verifyId = 0;
static bool g_verified = false;
static int g_lastSetOk = 0;

struct SorceryConfig
{
    bool enabled = true;
    bool enableFastCasting = true;
    int maxCorruptionPercent = 50;
    std::string prefix = "!";
    std::string cmdCast = "cast";

    void Load(const char* folderName)
    {
        const char* configPath = g_api->CaminhoConfig(folderName);
        if (!configPath) return;
        ConanUtils::JsonValue json;
        if (!ConanUtils::JsonParser::ParseFile(configPath, json)) return;
        if (json.has("Enabled")) enabled = json["Enabled"].asBool(true);
        if (json.has("EnableFastCasting")) enableFastCasting = json["EnableFastCasting"].asBool(true);
        if (json.has("MaxCorruptionPercent")) maxCorruptionPercent = json["MaxCorruptionPercent"].asInt(50);
        if (json.has("Prefix")) prefix = json["Prefix"].asString("!");
        if (json.has("CommandCast")) cmdCast = json["CommandCast"].asString("cast");
    }
};

static SorceryConfig g_cfg;
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

static bool CapCorruption(void* ch)
{
    if (!ch) return false;
    const double maxVal = double(g_cfg.maxCorruptionPercent);
    ConanApi::Call<void>(ch, "SetCorruption", maxVal);
    if (g_api->UltimaChamadaExecutou())
    {
        g_lastSetOk = 1;
        return true;
    }
    // Try CheatManager SetStat AttributeCorruption
    void* cheat = nullptr;
    int32_t off = g_api->OffsetDoMembro(ch, "Controller");
    void* ctrl = nullptr;
    if (off >= 0) g_api->LerMembro(ch, uint32_t(off), &ctrl, sizeof(ctrl));
    if (ctrl)
    {
        off = g_api->OffsetDoMembro(ctrl, "CheatManager");
        if (off >= 0) g_api->LerMembro(ctrl, uint32_t(off), &cheat, sizeof(cheat));
    }
    if (cheat)
    {
        ConanApi::Call<void>(cheat, "SetStat", ConanApi::Texto("Corruption"), float(float(maxVal)));
        if (g_api->UltimaChamadaExecutou()) { g_lastSetOk = 1; return true; }
    }
    return false;
}

static void SelfVerify(void*)
{
    if (g_verified || !g_cfg.enabled) return;
    void* cls = g_api->FindClass("ConanCharacter");
    const bool ok = (g_hookChat != 0) && (cls != nullptr);
    g_api->Log("[SorceryHelper] VERIFY hook=%u charCls=%d setOk=%d => %s",
               (unsigned)g_hookChat, cls ? 1 : 0, g_lastSetOk, ok ? "PASS" : "FAIL");
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
    const std::string pCast = g_cfg.prefix + g_cfg.cmdCast;
    if (msg.rfind(pCast, 0) != 0) return CONAN_CONTINUAR;

    std::string spell;
    if (msg.size() > pCast.size())
    {
        spell = msg.substr(pCast.size());
        while (!spell.empty() && spell.front() == ' ') spell.erase(0, 1);
    }

    void* ch = PlayerPawn(c->Obj);
    const bool capped = CapCorruption(ch);
    char reply[160];
    std::snprintf(reply, sizeof(reply),
                  "Sorcery prep '%s': corruption capped to %d%% (%s).",
                  spell.empty() ? "ready" : spell.c_str(),
                  g_cfg.maxCorruptionPercent,
                  capped ? "SetCorruption ok" : "SetCorruption unavailable");
    SendReply(c->Obj, reply);
    return CONAN_CANCELAR;
}

extern "C" __declspec(dllexport)
void ConanPluginCarregar(const ConanApiTabela* api)
{
    if (!api || api->tamanho < sizeof(ConanApiTabela)) return;
    g_api = api;
    ConanApi::UsarTabela(api);
    g_cfg.Load("SorceryHelper");
    g_verified = false;

    if (!g_cfg.enabled)
    {
        g_api->Log("[SorceryHelper] Plugin disabled in config.json (Enabled=false). Load skipped.");
        return;
    }

    g_api->Log("======================================================");
    g_api->Log(" SorceryHelper v2.0.0 — SetCorruption cap before cast");
    g_api->Log(" Command: !%s | Max corruption: %d%%",
               g_cfg.cmdCast.c_str(), g_cfg.maxCorruptionPercent);
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
        g_api->Log("[SorceryHelper] Unloaded.");
    }
    g_api = nullptr;
}
