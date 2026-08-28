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
static uint32_t g_verifyId = 0;
static bool g_verified = false;
static int g_lastFed = 0;
static int g_lastSpawnOk = 0;

struct FeedConfig
{
    bool enabled = true;
    int defaultFoodTemplateId = 18210;
    int foodQuantityPerThrall = 20;
    std::string prefix = "!";
    std::string cmdFeed = "feed";

    void Load(const char* folderName)
    {
        const char* configPath = g_api->CaminhoConfig(folderName);
        if (!configPath) return;
        ConanUtils::JsonValue json;
        if (!ConanUtils::JsonParser::ParseFile(configPath, json)) return;
        if (json.has("Enabled")) enabled = json["Enabled"].asBool(true);
        if (json.has("DefaultFoodTemplateId")) defaultFoodTemplateId = json["DefaultFoodTemplateId"].asInt(18210);
        if (json.has("FoodQuantityPerThrall")) foodQuantityPerThrall = json["FoodQuantityPerThrall"].asInt(20);
        if (json.has("Prefix")) prefix = json["Prefix"].asString("!");
        if (json.has("CommandFeed")) cmdFeed = json["CommandFeed"].asString("feed");
    }
};

static FeedConfig g_cfg;
static void* g_npcClasses[2];
static int g_npcClassCount = 0;
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

static bool GiveFood(void* npc, int templateId, int qty)
{
    const bool ok = ConanApi::Call<bool>(
        npc, "SpawnTemplateItem",
        int32_t(templateId), ConanApi::Nome("diet"),
        int32_t(qty), float(1.0f), float(0.0f), bool(true));
    if (g_api->UltimaChamadaExecutou() && ok)
    {
        g_lastSpawnOk = 1;
        return true;
    }

    // Fallback: AddItemTemplate on backpack inventory
    void* inv = nullptr;
    ConanApi::CallSaida<void>(npc, "GetBackpackInventory", ConanApi::ParaFora(inv));
    if (!inv)
    {
        int32_t off = g_api->OffsetDoMembro(npc, "BackpackInventory");
        if (off >= 0) g_api->LerMembro(npc, uint32_t(off), &inv, sizeof(inv));
    }
    if (!inv || IsDefaultObject(inv)) return false;

    const int32_t added = ConanApi::Call<int32_t>(
        inv, "AddItemTemplate",
        int32_t(templateId), int32_t(-1), ConanApi::Nome("diet"),
        int32_t(qty), bool(false), float(1.0f), float(0.0f));
    if (g_api->UltimaChamadaExecutou() && added >= 0)
    {
        g_lastSpawnOk = 1;
        return true;
    }
    return false;
}

static void CacheNpcClasses()
{
    if (g_npcClassCount > 0) return;
    void* cls = g_api->FindClass("BaseNPCChar");
    if (cls) g_npcClasses[g_npcClassCount++] = cls;
    cls = g_api->FindClass("ConanCharacter");
    if (cls && g_npcClassCount < 2) g_npcClasses[g_npcClassCount++] = cls;
}

static void HandleFeed(void* pc, int64_t uid, const std::string& foodType)
{
    int foodId = g_cfg.defaultFoodTemplateId;
    if (foodType == "pork" || foodType == "agility") foodId = 18218;
    else if (foodType == "feast" || foodType == "vitality") foodId = 18260;
    else if (foodType == "gruel" || foodType == "strength") foodId = 18210;

    CacheNpcClasses();
    int fed = 0;
    ScanUtil::ForEachOfClasses(g_api, g_npcClasses, g_npcClassCount, [&](void* npc) {
        int64_t ownerId = 0;
        int32_t off = g_api->OffsetDoMembro(npc, "OwningPlayerId");
        if (off < 0) off = g_api->OffsetDoMembro(npc, "OwnerPlayerId");
        if (off >= 0) g_api->LerMembro(npc, uint32_t(off), &ownerId, sizeof(ownerId));
        if (ownerId == 0 || (uid != 0 && ownerId != uid)) return;
        if (GiveFood(npc, foodId, g_cfg.foodQuantityPerThrall)) ++fed;
    });

    g_lastFed = fed;
    char reply[128];
    std::snprintf(reply, sizeof(reply), "Diet food (id %d) delivered to %d follower(s).", foodId, fed);
    SendReply(pc, reply);
}

static void SelfVerify(void*)
{
    if (g_verified || !g_cfg.enabled) return;
    void* cls = g_api->FindClass("ConanCharacter");
    const bool ok = (g_hookChat != 0) && (cls != nullptr);
    g_api->Log("[FeedingManager] VERIFY hook=%u charCls=%d lastFed=%d spawnOk=%d => %s",
               (unsigned)g_hookChat, cls ? 1 : 0, g_lastFed, g_lastSpawnOk,
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
    const std::string pFeed = g_cfg.prefix + g_cfg.cmdFeed;
    if (msg.rfind(pFeed, 0) != 0) return CONAN_CONTINUAR;

    int64_t uid = 0;
    const uint8_t* pUid = static_cast<const uint8_t*>(chat) + CHAT_UID;
    if (g_api->Legivel(pUid, 8)) uid = *reinterpret_cast<const int64_t*>(pUid);

    std::string arg;
    if (msg.size() > pFeed.size())
    {
        arg = msg.substr(pFeed.size());
        while (!arg.empty() && arg.front() == ' ') arg.erase(0, 1);
    }
    HandleFeed(c->Obj, uid, arg);
    return CONAN_CANCELAR;
}

extern "C" __declspec(dllexport)
void ConanPluginCarregar(const ConanApiTabela* api)
{
    if (!api || api->tamanho < sizeof(ConanApiTabela)) return;
    g_api = api;
    ConanApi::UsarTabela(api);
    g_cfg.Load("FeedingManager");
    g_verified = false;

    if (!g_cfg.enabled)
    {
        g_api->Log("[FeedingManager] Plugin disabled in config.json (Enabled=false). Load skipped.");
        return;
    }

    g_api->Log("======================================================");
    g_api->Log(" FeedingManager v2.0.0 — SpawnTemplateItem / AddItemTemplate diet");
    g_api->Log(" Command: !%s [gruel|pork|feast] | Qty: %d",
               g_cfg.cmdFeed.c_str(), g_cfg.foodQuantityPerThrall);
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
        g_api->Log("[FeedingManager] Unloaded.");
    }
    g_api = nullptr;
}
