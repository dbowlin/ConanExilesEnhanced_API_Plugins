#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "Conan/ConanPluginApi.h"
#include "Conan/ConanBase.h"
#include "../common/JsonConfig.h"

#include <string>
#include <map>
#include <fstream>
#include <cstdio>
#include <cstring>

static const ConanApiTabela* g_api = nullptr;
static uint32_t g_hookChat = 0;
static uint32_t g_verifyId = 0;
static bool g_verified = false;
static int g_lastTpOk = 0;

struct WaypointPos
{
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

struct WaypointConfig
{
    bool enabled = true;
    int maxHomes = 5;
    int homeCooldownSeconds = 30;
    std::string prefix = "!";
    std::string cmdHome = "home";
    std::string cmdSetHome = "sethome";
    std::string cmdDelHome = "delhome";
    std::string cmdHomes = "homes";

    void Load(const char* folderName)
    {
        const char* configPath = g_api->CaminhoConfig(folderName);
        if (!configPath) return;
        ConanUtils::JsonValue json;
        if (!ConanUtils::JsonParser::ParseFile(configPath, json)) return;
        if (json.has("Enabled")) enabled = json["Enabled"].asBool(true);
        if (json.has("MaxHomes")) maxHomes = json["MaxHomes"].asInt(5);
        if (json.has("HomeCooldownSeconds")) homeCooldownSeconds = json["HomeCooldownSeconds"].asInt(30);
        if (json.has("Prefix")) prefix = json["Prefix"].asString("!");
        if (json.has("CommandHome")) cmdHome = json["CommandHome"].asString("home");
        if (json.has("CommandSetHome")) cmdSetHome = json["CommandSetHome"].asString("sethome");
        if (json.has("CommandDelHome")) cmdDelHome = json["CommandDelHome"].asString("delhome");
        if (json.has("CommandHomes")) cmdHomes = json["CommandHomes"].asString("homes");
    }
};

static WaypointConfig g_cfg;
static std::map<int64_t, std::map<std::string, WaypointPos>> g_playerHomes;
static std::map<int64_t, DWORD> g_cooldowns;
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

static void SaveHomes()
{
    const char* filepath = g_api->CaminhoDados("Waypoints", "homes.json");
    if (!filepath) return;
    std::ofstream file(filepath);
    if (!file.is_open()) return;
    file << "{\n";
    bool firstP = true;
    for (const auto& pairP : g_playerHomes)
    {
        if (!firstP) file << ",\n";
        firstP = false;
        file << "  \"" << pairP.first << "\": {\n";
        bool firstH = true;
        for (const auto& pairH : pairP.second)
        {
            if (!firstH) file << ",\n";
            firstH = false;
            file << "    \"" << pairH.first << "\": ["
                 << pairH.second.x << ", " << pairH.second.y << ", " << pairH.second.z << "]";
        }
        file << "\n  }";
    }
    file << "\n}\n";
}

static void LoadHomes()
{
    const char* filepath = g_api->CaminhoDados("Waypoints", "homes.json");
    if (!filepath) return;
    ConanUtils::JsonValue json;
    if (!ConanUtils::JsonParser::ParseFile(filepath, json) || json.type != ConanUtils::JsonType::Object)
        return;
    g_playerHomes.clear();
    for (const auto& p : json.objVal)
    {
        int64_t uid = 0;
        try { uid = std::stoll(p.first); } catch (...) { continue; }
        if (p.second.type != ConanUtils::JsonType::Object) continue;
        for (const auto& h : p.second.objVal)
        {
            if (h.second.type != ConanUtils::JsonType::Array || h.second.arrVal.size() < 3) continue;
            WaypointPos pos;
            pos.x = h.second.arrVal[0].asDouble();
            pos.y = h.second.arrVal[1].asDouble();
            pos.z = h.second.arrVal[2].asDouble();
            g_playerHomes[uid][h.first] = pos;
        }
    }
}

static bool GetCharPosition(void* pc, FVector& outPos, void** outChar = nullptr)
{
    void* charPawn = nullptr;
    int32_t off = g_api->OffsetDoMembro(pc, "Character");
    if (off >= 0) g_api->LerMembro(pc, uint32_t(off), &charPawn, sizeof(charPawn));
    if (!charPawn)
    {
        off = g_api->OffsetDoMembro(pc, "Pawn");
        if (off >= 0) g_api->LerMembro(pc, uint32_t(off), &charPawn, sizeof(charPawn));
    }
    if (!charPawn) return false;
    if (outChar) *outChar = charPawn;
    outPos = ConanApi::Call<FVector>(charPawn, "K2_GetActorLocation");
    return g_api->UltimaChamadaExecutou() != 0;
}

static bool TeleportChar(void* pc, const FVector& dest)
{
    void* charPawn = nullptr;
    FVector currentPos;
    if (!GetCharPosition(pc, currentPos, &charPawn)) return false;
    FRotator rot{ 0.0, 0.0, 0.0 };
    if (ConanApi::Call<bool>(charPawn, "K2_TeleportTo", dest, rot) && g_api->UltimaChamadaExecutou())
    {
        g_lastTpOk = 1;
        return true;
    }
    const bool ok = ConanApi::Call<bool>(charPawn, "TeleportTo", dest, rot);
    if (g_api->UltimaChamadaExecutou() && ok) { g_lastTpOk = 1; return true; }
    return false;
}

static void HandleSetHome(void* pc, int64_t uid, const std::string& name)
{
    const std::string homeName = name.empty() ? "default" : name;
    auto& homes = g_playerHomes[uid];
    if (homes.size() >= (size_t)g_cfg.maxHomes && homes.find(homeName) == homes.end())
    {
        char msg[128];
        std::snprintf(msg, sizeof(msg), "Limit reached! Maximum of %d waypoints allowed.", g_cfg.maxHomes);
        SendReply(pc, msg);
        return;
    }
    FVector pos;
    if (!GetCharPosition(pc, pos))
    {
        SendReply(pc, "Error: Unable to read your current coordinates.");
        return;
    }
    homes[homeName] = WaypointPos{ pos.X, pos.Y, pos.Z };
    SaveHomes();
    char msgSuccess[128];
    std::snprintf(msgSuccess, sizeof(msgSuccess), "Waypoint '%s' saved.", homeName.c_str());
    SendReply(pc, msgSuccess);
}

static void HandleHome(void* pc, int64_t uid, const std::string& name)
{
    const std::string homeName = name.empty() ? "default" : name;
    auto itPlayer = g_playerHomes.find(uid);
    if (itPlayer == g_playerHomes.end() || itPlayer->second.find(homeName) == itPlayer->second.end())
    {
        char msgErr[128];
        std::snprintf(msgErr, sizeof(msgErr), "Waypoint '%s' not found. Type !homes.", homeName.c_str());
        SendReply(pc, msgErr);
        return;
    }
    const DWORD now = GetTickCount();
    auto itCd = g_cooldowns.find(uid);
    if (itCd != g_cooldowns.end())
    {
        const DWORD elapsed = (now - itCd->second) / 1000;
        if (elapsed < (DWORD)g_cfg.homeCooldownSeconds)
        {
            char msgCd[128];
            std::snprintf(msgCd, sizeof(msgCd), "Please wait %d second(s).",
                          g_cfg.homeCooldownSeconds - (int)elapsed);
            SendReply(pc, msgCd);
            return;
        }
    }
    const auto& wPos = itPlayer->second[homeName];
    FVector dest{ wPos.x, wPos.y, wPos.z + 10.0 };
    if (TeleportChar(pc, dest))
    {
        g_cooldowns[uid] = now;
        char msgTp[128];
        std::snprintf(msgTp, sizeof(msgTp), "Teleported to '%s' (K2_TeleportTo).", homeName.c_str());
        SendReply(pc, msgTp);
    }
    else
        SendReply(pc, "Teleport failed.");
}

static void HandleDelHome(void* pc, int64_t uid, const std::string& name)
{
    const std::string homeName = name.empty() ? "default" : name;
    auto it = g_playerHomes.find(uid);
    if (it != g_playerHomes.end() && it->second.erase(homeName) > 0)
    {
        SaveHomes();
        char msg[128];
        std::snprintf(msg, sizeof(msg), "Waypoint '%s' deleted.", homeName.c_str());
        SendReply(pc, msg);
    }
    else
        SendReply(pc, "Waypoint not found.");
}

static void HandleListHomes(void* pc, int64_t uid)
{
    auto it = g_playerHomes.find(uid);
    if (it == g_playerHomes.end() || it->second.empty())
    {
        SendReply(pc, "No saved waypoints. Use: !sethome [name]");
        return;
    }
    std::string list = "Your waypoints: ";
    for (const auto& pair : it->second) list += "[" + pair.first + "] ";
    SendReply(pc, list);
}

static void SelfVerify(void*)
{
    if (g_verified || !g_cfg.enabled) return;
    void* cls = g_api->FindClass("ConanCharacter");
    const bool ok = (g_hookChat != 0) && (cls != nullptr);
    g_api->Log("[Waypoints] VERIFY hook=%u charCls=%d lastTp=%d => %s",
               (unsigned)g_hookChat, cls ? 1 : 0, g_lastTpOk, ok ? "PASS" : "FAIL");
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
    const std::string pSetHome = g_cfg.prefix + g_cfg.cmdSetHome;
    const std::string pDelHome = g_cfg.prefix + g_cfg.cmdDelHome;
    const std::string pHomes = g_cfg.prefix + g_cfg.cmdHomes;
    const std::string pHome = g_cfg.prefix + g_cfg.cmdHome;

    int64_t uid = 0;
    const uint8_t* pUid = static_cast<const uint8_t*>(chat) + CHAT_UID;
    if (g_api->Legivel(pUid, 8)) uid = *reinterpret_cast<const int64_t*>(pUid);
    void* pc = c->Obj;

    auto extractArg = [](const std::string& str, const std::string& pref) -> std::string {
        if (str.size() <= pref.size()) return "";
        std::string arg = str.substr(pref.size());
        while (!arg.empty() && arg.front() == ' ') arg.erase(0, 1);
        while (!arg.empty() && arg.back() == ' ') arg.pop_back();
        return arg;
    };

    if (msg.rfind(pSetHome, 0) == 0) { HandleSetHome(pc, uid, extractArg(msg, pSetHome)); return CONAN_CANCELAR; }
    if (msg.rfind(pDelHome, 0) == 0) { HandleDelHome(pc, uid, extractArg(msg, pDelHome)); return CONAN_CANCELAR; }
    if (msg.rfind(pHomes, 0) == 0) { HandleListHomes(pc, uid); return CONAN_CANCELAR; }
    if (msg.rfind(pHome, 0) == 0) { HandleHome(pc, uid, extractArg(msg, pHome)); return CONAN_CANCELAR; }
    return CONAN_CONTINUAR;
}

extern "C" __declspec(dllexport)
void ConanPluginCarregar(const ConanApiTabela* api)
{
    if (!api || api->tamanho < sizeof(ConanApiTabela)) return;
    g_api = api;
    ConanApi::UsarTabela(api);
    g_cfg.Load("Waypoints");
    g_verified = false;

    if (!g_cfg.enabled)
    {
        g_api->Log("[Waypoints] Plugin disabled in config.json (Enabled=false). Load skipped.");
        return;
    }

    LoadHomes();

    g_api->Log("======================================================");
    g_api->Log(" Waypoints v2.0.0 — K2_TeleportTo homes");
    g_api->Log(" Commands: !%s | !%s | !%s | Max: %d",
               g_cfg.cmdSetHome.c_str(), g_cfg.cmdHome.c_str(), g_cfg.cmdHomes.c_str(), g_cfg.maxHomes);
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
        SaveHomes();
        g_api->Log("[Waypoints] Unloaded.");
    }
    g_api = nullptr;
}
