#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "Conan/ConanPluginApi.h"
#include "Conan/ConanBase.h"
#include "../common/JsonConfig.h"
#include "../common/ObjectScan.h"

#include <string>
#include <map>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdint>

static const ConanApiTabela* g_api = nullptr;
static uint32_t g_hookChat = 0;
static uint32_t g_verifyId = 0;
static bool g_verified = false;
static int g_lastRescued = 0;

struct ThrallConfig
{
    bool enabled = true;
    bool allowRescueAll = true;
    int cooldownSeconds = 30;
    std::string prefix = "!";
    std::string cmdRescue = "rescue";
    std::string cmdList = "mythralls";

    void Load(const char* folderName)
    {
        const char* configPath = g_api->CaminhoConfig(folderName);
        if (!configPath) return;
        ConanUtils::JsonValue json;
        if (!ConanUtils::JsonParser::ParseFile(configPath, json)) return;
        if (json.has("Enabled")) enabled = json["Enabled"].asBool(true);
        if (json.has("AllowRescueAll")) allowRescueAll = json["AllowRescueAll"].asBool(true);
        if (json.has("CooldownSeconds")) cooldownSeconds = json["CooldownSeconds"].asInt(30);
        if (json.has("Prefix")) prefix = json["Prefix"].asString("!");
        if (json.has("CommandRescue")) cmdRescue = json["CommandRescue"].asString("rescue");
        if (json.has("CommandList")) cmdList = json["CommandList"].asString("mythralls");
    }
};

static ThrallConfig g_cfg;
static void* g_npcClasses[2];
static int g_npcClassCount = 0;
static std::map<int64_t, DWORD> g_lastCommands;
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

static bool GetActorPosition(void* actor, FVector& outPos)
{
    if (!actor) return false;
    outPos = ConanApi::Call<FVector>(actor, "K2_GetActorLocation");
    return g_api->UltimaChamadaExecutou() != 0;
}

static bool TeleportActor(void* actor, const FVector& destination)
{
    if (!actor) return false;
    FRotator rot{ 0.0, 0.0, 0.0 };
    if (ConanApi::Call<bool>(actor, "K2_TeleportTo", destination, rot) && g_api->UltimaChamadaExecutou())
        return true;
    return ConanApi::Call<bool>(actor, "TeleportTo", destination, rot) && g_api->UltimaChamadaExecutou();
}

static void CacheNpcClasses()
{
    if (g_npcClassCount > 0) return;
    void* cls = g_api->FindClass("BaseNPCChar");
    if (cls) g_npcClasses[g_npcClassCount++] = cls;
    cls = g_api->FindClass("ConanCharacter");
    if (cls && g_npcClassCount < 2) g_npcClasses[g_npcClassCount++] = cls;
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

static int RescueFollowers(void* pc, int64_t playerUid, const std::string& nameFilter, bool rescueAll, bool listOnly)
{
    void* charPawn = PlayerPawn(pc);
    if (!charPawn)
    {
        SendReply(pc, "Error: Character not found in world.");
        return 0;
    }

    FVector playerPos{};
    if (!GetActorPosition(charPawn, playerPos))
    {
        SendReply(pc, "Error: Unable to retrieve player coordinates.");
        return 0;
    }

    CacheNpcClasses();
    if (g_npcClassCount == 0) return 0;

    int rescuedCount = 0;
    int listed = 0;
    char nameBuf[256];

    ScanUtil::ForEachOfClasses(g_api, g_npcClasses, g_npcClassCount, [&](void* npc) {
        if (!npc || npc == charPawn || IsDefaultObject(npc)) return;

        int64_t ownerId = 0;
        int32_t offOwner = g_api->OffsetDoMembro(npc, "OwningPlayerId");
        if (offOwner < 0) offOwner = g_api->OffsetDoMembro(npc, "OwnerPlayerId");
        if (offOwner >= 0) g_api->LerMembro(npc, uint32_t(offOwner), &ownerId, sizeof(ownerId));
        if (ownerId == 0 || (playerUid != 0 && ownerId != playerUid)) return;

        nameBuf[0] = 0;
        g_api->NomeDoObjeto(npc, nameBuf, sizeof(nameBuf));

        if (!rescueAll && !nameFilter.empty() && nameFilter != "all")
        {
            if (std::string(nameBuf).find(nameFilter) == std::string::npos)
                return;
        }

        if (listOnly)
        {
            ++listed;
            return;
        }

        FVector spawnPos = playerPos;
        const double angle = (rescuedCount * 45.0) * (3.14159265 / 180.0);
        spawnPos.X += std::cos(angle) * (150.0 + (rescuedCount / 8) * 100.0);
        spawnPos.Y += std::sin(angle) * (150.0 + (rescuedCount / 8) * 100.0);
        spawnPos.Z += 20.0;

        if (TeleportActor(npc, spawnPos))
        {
            ++rescuedCount;
            g_api->Log("[ThrallRescue] Follower %s teleported via K2_TeleportTo.", nameBuf);
        }
    });

    g_lastRescued = rescuedCount;
    return listOnly ? listed : rescuedCount;
}

static void SelfVerify(void*)
{
    if (g_verified || !g_cfg.enabled) return;
    void* cls = g_api->FindClass("BaseNPCChar");
    if (!cls) cls = g_api->FindClass("ConanCharacter");
    const bool ok = (g_hookChat != 0) && (cls != nullptr);
    g_api->Log("[ThrallRescue] VERIFY hook=%u npcCls=%d last=%d => %s",
               (unsigned)g_hookChat, cls ? 1 : 0, g_lastRescued, ok ? "PASS" : "FAIL");
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
    const std::string prefixRescue = g_cfg.prefix + g_cfg.cmdRescue;
    const std::string prefixList = g_cfg.prefix + g_cfg.cmdList;
    const bool isRescue = (msg.rfind(prefixRescue, 0) == 0);
    const bool isList = (msg.rfind(prefixList, 0) == 0);
    if (!isRescue && !isList) return CONAN_CONTINUAR;

    int64_t uid = 0;
    const uint8_t* pUid = static_cast<const uint8_t*>(chat) + CHAT_UID;
    if (g_api->Legivel(pUid, 8)) uid = *reinterpret_cast<const int64_t*>(pUid);
    void* pc = c->Obj;

    if (isRescue)
    {
        const DWORD now = GetTickCount();
        auto it = g_lastCommands.find(uid);
        if (it != g_lastCommands.end())
        {
            const DWORD elapsed = (now - it->second) / 1000;
            if (elapsed < (DWORD)g_cfg.cooldownSeconds)
            {
                char msgCooldown[128];
                std::snprintf(msgCooldown, sizeof(msgCooldown),
                              "Please wait %d second(s) before rescuing again.",
                              g_cfg.cooldownSeconds - (int)elapsed);
                SendReply(pc, msgCooldown);
                return CONAN_CANCELAR;
            }
        }

        std::string argument;
        if (msg.size() > prefixRescue.size())
        {
            argument = msg.substr(prefixRescue.size());
            while (!argument.empty() && argument.front() == ' ') argument.erase(0, 1);
        }
        const bool all = (argument.empty() || argument == "all");
        if (all && !g_cfg.allowRescueAll && !argument.empty())
        {
            SendReply(pc, "The '!rescue all' command is disabled on this server.");
            return CONAN_CANCELAR;
        }

        const int count = RescueFollowers(pc, uid, argument, all, false);
        char reply[128];
        if (count > 0)
        {
            g_lastCommands[uid] = now;
            std::snprintf(reply, sizeof(reply), "Rescued %d thrall(s) via K2_TeleportTo.", count);
        }
        else
            std::snprintf(reply, sizeof(reply), "No matching followers found.");
        SendReply(pc, reply);
        return CONAN_CANCELAR;
    }

    if (isList)
    {
        const int count = RescueFollowers(pc, uid, "", true, true);
        char reply[96];
        std::snprintf(reply, sizeof(reply), "You own %d active thrall(s) in the world.", count);
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
    g_cfg.Load("ThrallRescue");
    g_verified = false;

    if (!g_cfg.enabled)
    {
        g_api->Log("[ThrallRescue] Plugin disabled in config.json (Enabled=false). Load skipped.");
        return;
    }

    g_api->Log("======================================================");
    g_api->Log(" ThrallRescue v2.1.0 — K2_TeleportTo owned followers");
    g_api->Log(" Commands: !%s [name|all] | !%s | Cooldown: %ds",
               g_cfg.cmdRescue.c_str(), g_cfg.cmdList.c_str(), g_cfg.cooldownSeconds);
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
        g_api->Log("[ThrallRescue] Unloaded.");
    }
    g_api = nullptr;
}
