#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "Conan/ConanPluginApi.h"
#include "Conan/ConanBase.h"
#include "../common/JsonConfig.h"

#include <string>
#include <vector>
#include <map>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <algorithm>

static const ConanApiTabela* g_api = nullptr;
static uint32_t g_hookChat = 0;
static uint32_t g_verifyId = 0;
static bool g_verified = false;

struct RadarConfig
{
    bool enabled = true;
    bool showMapGrid = true;
    std::string prefix = "!";
    std::string cmdWhere = "where";
    std::string cmdPing = "ping";

    void Load(const char* folderName)
    {
        const char* configPath = g_api->CaminhoConfig(folderName);
        if (configPath)
        {
            ConanUtils::JsonValue json;
            if (ConanUtils::JsonParser::ParseFile(configPath, json))
            {
                if (json.has("Enabled")) enabled = json["Enabled"].asBool(true);
                if (json.has("ShowMapGrid")) showMapGrid = json["ShowMapGrid"].asBool(true);
                if (json.has("Prefix")) prefix = json["Prefix"].asString("!");
                if (json.has("CommandWhere")) cmdWhere = json["CommandWhere"].asString("where");
                if (json.has("CommandPing")) cmdPing = json["CommandPing"].asString("ping");
            }
        }
    }
};

static RadarConfig g_cfg;

static const uint32_t CHAT_UID     = 0x038;
static const uint32_t CHAT_TEXTO   = 0x068;

static bool ReadText(const void* base, uint32_t off, char* outBuf, int maxLen)
{
    outBuf[0] = 0;
    return g_api->LerTextoDoJogo(base, off, outBuf, maxLen) != 0;
}

static void SendReply(void* playerController, const std::string& text)
{
    if (!playerController || text.empty()) return;

    ConanApi::Call<void>(playerController, "ClientHUDShowNotification",
                         ConanApi::TextoRico(text.c_str()),
                         bool(true),
                         bool(false));

    if (!g_api->UltimaChamadaExecutou())
    {
        ConanApi::Call<void>(playerController, "ClientMessage",
                             ConanApi::Texto(text.c_str()),
                             ConanApi::Nome("Event"),
                             float(6.0f));
    }
}

static bool GetActorPosition(void* actor, FVector& outPos)
{
    if (!actor) return false;
    outPos = ConanApi::Call<FVector>(actor, "K2_GetActorLocation");
    return g_api->UltimaChamadaExecutou();
}

static std::string CalculateMapGrid(double x, double y)
{
    double minX = -320000.0, maxX = 380000.0;
    double minY = -320000.0, maxY = 380000.0;

    int col = (int)(((x - minX) / (maxX - minX)) * 14.0);
    int row = (int)(((y - minY) / (maxY - minY)) * 14.0);

    col = (std::max)(0, (std::min)(13, col));
    row = (std::max)(0, (std::min)(13, row));

    char letter = (char)('A' + col);
    int num = row + 1;

    char buf[16];
    std::snprintf(buf, sizeof(buf), "%c%d", letter, num);
    return std::string(buf);
}

static void HandleWhere(void* pc, int64_t uid)
{
    void* charPawn = nullptr;
    int32_t offChar = g_api->OffsetDoMembro(pc, "Character");
    if (offChar >= 0) g_api->LerMembro(pc, offChar, &charPawn, sizeof(void*));
    if (!charPawn)
    {
        int32_t offPawn = g_api->OffsetDoMembro(pc, "Pawn");
        if (offPawn >= 0) g_api->LerMembro(pc, offPawn, &charPawn, sizeof(void*));
    }
    if (!charPawn) return;

    FVector playerPos;
    if (!GetActorPosition(charPawn, playerPos)) return;

    std::string myGrid = CalculateMapGrid(playerPos.X, playerPos.Y);
    char reply[256];
    std::snprintf(reply, sizeof(reply), "Your Location: Grid [%s] • Coord: [%.0f, %.0f, %.0f]",
                  myGrid.c_str(), playerPos.X, playerPos.Y, playerPos.Z);
    SendReply(pc, reply);
}

static void SelfVerify(void*)
{
    if (g_verified || !g_cfg.enabled) return;
    const bool ok = (g_hookChat != 0);
    g_api->Log("[ClanRadar] VERIFY hook=%u => %s", (unsigned)g_hookChat, ok ? "PASS" : "FAIL");
    if (ok)
    {
        g_verified = true;
        if (g_verifyId) { g_api->CancelarAgendamento(g_verifyId); g_verifyId = 0; }
    }
}

extern "C" ConanAcao OnChatMessage(ConanChamada* c)
{
    if (!c || !c->Parms || c->ParmsSize < 0x80) return CONAN_CONTINUAR;
    const void* chat = c->Parms;

    char text[512] = {0};
    ReadText(chat, CHAT_TEXTO, text, sizeof(text));

    if (text[0] != '!' && text[0] != '/') return CONAN_CONTINUAR;

    std::string msg(text);
    std::string pWhere = g_cfg.prefix + g_cfg.cmdWhere;
    std::string pPing  = g_cfg.prefix + g_cfg.cmdPing;

    int64_t uid = 0;
    const uint8_t* pUid = static_cast<const uint8_t*>(chat) + CHAT_UID;
    if (g_api->Legivel(pUid, 8)) uid = *reinterpret_cast<const int64_t*>(pUid);

    void* pc = c->Obj;

    if (msg.rfind(pWhere, 0) == 0 || msg.rfind(pPing, 0) == 0)
    {
        HandleWhere(pc, uid);
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

    g_cfg.Load("ClanRadar");
    g_verified = false;

    if (!g_cfg.enabled)
    {
        g_api->Log("[ClanRadar] Plugin disabled in config.json (Enabled=false). Load skipped.");
        return;
    }

    g_api->Log("======================================================");
    g_api->Log(" ClanRadar v2.0.0 — K2_GetActorLocation grid locator");
    g_api->Log(" Commands: !%s | !%s", g_cfg.cmdWhere.c_str(), g_cfg.cmdPing.c_str());
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
        g_api->Log("[ClanRadar] Unloaded.");
    }
    g_api = nullptr;
}
