#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "Conan/ConanPluginApi.h"
#include "Conan/ConanBase.h"
#include "../common/JsonConfig.h"

#include <string>
#include <vector>
#include <cstdio>
#include <cstring>

static const ConanApiTabela* g_api = nullptr;
static uint32_t g_hookChat = 0;
static uint32_t g_tipTaskId = 0;
static uint32_t g_verifyId = 0;
static bool g_verified = false;
static size_t g_tipIndex = 0;

struct AnnouncerConfig
{
    bool enabled = true;
    int intervalMinutes = 20;
    std::string prefix = "!";
    std::string cmdBroadcast = "broadcast";
    std::vector<std::string> tips;

    void Load(const char* folderName)
    {
        tips.clear();
        const char* configPath = g_api->CaminhoConfig(folderName);
        if (configPath)
        {
            ConanUtils::JsonValue json;
            if (ConanUtils::JsonParser::ParseFile(configPath, json))
            {
                if (json.has("Enabled")) enabled = json["Enabled"].asBool(true);
                if (json.has("IntervalMinutes")) intervalMinutes = json["IntervalMinutes"].asInt(20);
                if (json.has("Prefix")) prefix = json["Prefix"].asString("!");
                if (json.has("CommandBroadcast")) cmdBroadcast = json["CommandBroadcast"].asString("broadcast");
                if (json.has("Tips") && json["Tips"].type == ConanUtils::JsonType::Array)
                {
                    for (const auto& t : json["Tips"].arrVal)
                    {
                        const std::string s = t.asString("");
                        if (!s.empty()) tips.push_back(s);
                    }
                }
            }
        }
        if (tips.empty())
        {
            tips.push_back("Welcome — type !broadcast <msg> (admins) or check Discord for rules.");
            tips.push_back("Tip: Use building mode carefully near slopes; PlacementUnblocker may be enabled.");
            tips.push_back("Tip: Feed and gear your thralls — FollowerLevelBoost speeds their XP.");
            tips.push_back("Server tip: Corpse recovery and area loot commands may be available.");
        }
        if (intervalMinutes < 1) intervalMinutes = 1;
    }
};

static AnnouncerConfig g_cfg;

static const uint32_t CHAT_TEXTO = 0x068;

static bool ReadText(const void* base, uint32_t off, char* outBuf, int maxLen)
{
    outBuf[0] = 0;
    return g_api->LerTextoDoJogo(base, off, outBuf, maxLen) != 0;
}

static void BroadcastAll(const std::string& msg)
{
    if (msg.empty()) return;
    if (g_api->MensagemParaTodos)
    {
        g_api->MensagemParaTodos(msg.c_str());
        return;
    }

    void* pcs[128];
    int total = g_api->FindObjects("PlayerController", pcs, 128, 1);
    for (int i = 0; i < total; ++i)
    {
        void* pc = pcs[i];
        if (!pc) continue;
        ConanApi::Call<void>(pc, "ClientHUDShowNotification",
                             ConanApi::TextoRico(msg.c_str()), bool(true), bool(false));
    }
}

static void RotateTip(void*)
{
    if (!g_cfg.enabled || g_cfg.tips.empty()) return;
    if (g_tipIndex >= g_cfg.tips.size()) g_tipIndex = 0;
    const std::string& tip = g_cfg.tips[g_tipIndex++];
    BroadcastAll(tip);
    g_api->Log("[ServerAnnouncer] tip broadcast (%zu/%zu)", g_tipIndex, g_cfg.tips.size());
}

static void SelfVerify(void*)
{
    if (g_verified || !g_cfg.enabled) return;

    int callOk = 0;
    if (g_api->MensagemParaTodos)
    {
        // Empty / no-op style: API resolves; avoid spamming players on verify.
        // Prefer a dry resolve check via function pointer + tip task scheduled.
        callOk = 1;
    }
    if (!callOk)
    {
        void* pcs[8];
        const int n = g_api->FindObjects("PlayerController", pcs, 8, 1);
        if (n > 0 && pcs[0])
        {
            ConanApi::Call<void>(pcs[0], "ClientHUDShowNotification",
                                 ConanApi::TextoRico(" "), bool(true), bool(false));
            if (g_api->UltimaChamadaExecutou()) callOk = 1;
        }
    }

    const bool ok = (g_hookChat != 0) && (g_tipTaskId != 0) && (callOk != 0) && !g_cfg.tips.empty();
    g_api->Log("[ServerAnnouncer] VERIFY hook=%u tipTask=%u tips=%zu msgApi=%d callOk=%d => %s",
               (unsigned)g_hookChat, (unsigned)g_tipTaskId, g_cfg.tips.size(),
               g_api->MensagemParaTodos ? 1 : 0, callOk,
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
    const std::string pBroadcast = g_cfg.prefix + g_cfg.cmdBroadcast;

    if (msg.rfind(pBroadcast, 0) == 0)
    {
        std::string body;
        if (msg.size() > pBroadcast.size())
        {
            body = msg.substr(pBroadcast.size());
            while (!body.empty() && body.front() == ' ') body.erase(0, 1);
        }
        if (!body.empty())
            BroadcastAll("[ANNOUNCEMENT] " + body);
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
    g_cfg.Load("ServerAnnouncer");
    g_verified = false;
    g_tipIndex = 0;

    if (!g_cfg.enabled)
    {
        g_api->Log("[ServerAnnouncer] Plugin disabled in config.json (Enabled=false). Load skipped.");
        return;
    }

    g_api->Log("======================================================");
    g_api->Log(" ServerAnnouncer v2.0.0 — Scheduled tips + !broadcast");
    g_api->Log(" Interval: %dm | Tips: %zu | Command: !%s <message>",
               g_cfg.intervalMinutes, g_cfg.tips.size(), g_cfg.cmdBroadcast.c_str());
    g_api->Log("======================================================");

    g_hookChat = g_api->HookProcessEvent("ServerSendChatMessage", OnChatMessage, nullptr, 100);
    const uint32_t tipSecs = uint32_t(g_cfg.intervalMinutes) * 60u;
    g_tipTaskId = g_api->AgendarNaThreadDoJogo(RotateTip, tipSecs, nullptr, 1);
    g_verifyId = g_api->AgendarNaThreadDoJogo(SelfVerify, 3, nullptr, 1);
}

extern "C" __declspec(dllexport)
void ConanPluginDescarregar(void)
{
    if (g_api)
    {
        if (g_hookChat) g_api->RemoverHook(g_hookChat);
        if (g_tipTaskId) g_api->CancelarAgendamento(g_tipTaskId);
        if (g_verifyId) g_api->CancelarAgendamento(g_verifyId);
        g_api->Log("[ServerAnnouncer] Unloaded.");
    }
    g_api = nullptr;
}
