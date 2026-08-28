#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "Conan/ConanPluginApi.h"
#include "Conan/ConanBase.h"
#include "../common/JsonConfig.h"

#include <string>
#include <vector>
#include <map>
#include <cstdio>
#include <cstring>

static const ConanApiTabela* g_api = nullptr;
static uint32_t g_hookChat = 0;
static uint32_t g_verifyId = 0;
static bool g_verified = false;
static int g_lastDelivered = 0;

struct KitItem
{
    int templateId = 0;
    int quantity = 1;
};

struct KitDef
{
    std::string key;
    std::string name;
    int requiredLevel = 0;
    std::vector<KitItem> items;
};

struct KitConfig
{
    bool enabled = true;
    int cooldownHours = 24;
    std::string prefix = "!";
    std::string cmdKit = "kit";
    std::vector<KitDef> kits;

    void LoadDefaults()
    {
        kits.clear();
        KitDef starter;
        starter.key = "starter";
        starter.name = "Iron Adventurer Kit";
        starter.requiredLevel = 0;
        starter.items = {
            {50495,1},{52001,1},{52002,1},{52003,1},{52004,1},{52005,1},
            {51403,1},{53612,50},{51039,1},{51002,1},{51309,1},{51012,1},
            {41036,1},{13540,20},{41032,1}
        };
        kits.push_back(starter);
    }

    void LoadKitsFile(const char* path)
    {
        if (!path) return;
        ConanUtils::JsonValue json;
        if (!ConanUtils::JsonParser::ParseFile(path, json)) return;
        if (json.type != ConanUtils::JsonType::Array) return;

        std::vector<KitDef> loaded;
        for (const auto& entry : json.arrVal)
        {
            if (entry.type != ConanUtils::JsonType::Object) continue;
            KitDef kit;
            kit.key = entry.has("key") ? entry["key"].asString("") : "";
            kit.name = entry.has("name") ? entry["name"].asString(kit.key) : kit.key;
            if (entry.has("requiredLevel")) kit.requiredLevel = entry["requiredLevel"].asInt(0);
            if (kit.key.empty()) continue;
            if (entry.has("items") && entry["items"].type == ConanUtils::JsonType::Array)
            {
                for (const auto& it : entry["items"].arrVal)
                {
                    KitItem item;
                    item.templateId = it.has("templateId") ? it["templateId"].asInt(0) : 0;
                    item.quantity = it.has("quantity") ? it["quantity"].asInt(1) : 1;
                    if (item.templateId > 0 && item.quantity > 0)
                        kit.items.push_back(item);
                }
            }
            if (!kit.items.empty()) loaded.push_back(kit);
        }
        if (!loaded.empty()) kits = std::move(loaded);
    }

    void Load(const char* folderName)
    {
        LoadDefaults();
        const char* configPath = g_api->CaminhoConfig(folderName);
        if (configPath)
        {
            ConanUtils::JsonValue json;
            if (ConanUtils::JsonParser::ParseFile(configPath, json))
            {
                if (json.has("Enabled")) enabled = json["Enabled"].asBool(true);
                if (json.has("StarterKitCooldownHours")) cooldownHours = json["StarterKitCooldownHours"].asInt(24);
                if (json.has("Prefix")) prefix = json["Prefix"].asString("!");
                if (json.has("CommandKit")) cmdKit = json["CommandKit"].asString("kit");
            }
        }
        const char* kitsPath = g_api->CaminhoDados(folderName, "kits.json");
        LoadKitsFile(kitsPath);
    }

    const KitDef* Find(const std::string& key) const
    {
        for (const auto& k : kits)
            if (k.key == key) return &k;
        return kits.empty() ? nullptr : &kits[0];
    }
};

static KitConfig g_cfg;
static std::map<int64_t, DWORD> g_lastClaim;
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

static void* ProgressionOf(void* ch)
{
    if (!ch) return nullptr;
    void* prog = nullptr;
    int32_t off = g_api->OffsetDoMembro(ch, "BP_ProgressionSystem");
    if (off < 0) off = g_api->OffsetDoMembro(ch, "ProgressionSystem");
    if (off >= 0) g_api->LerMembro(ch, uint32_t(off), &prog, sizeof(prog));
    return prog;
}

static int PlayerLevel(void* pc)
{
    void* ch = PlayerPawn(pc);
    if (!ch) return 0;
    void* prog = ProgressionOf(ch);
    if (!prog) return 0;
    int32_t level = 0;
    ConanApi::CallSaida<void>(prog, "GetLevel", ConanApi::ParaFora(level));
    if (!g_api->UltimaChamadaExecutou()) return 0;
    return level;
}

static int DeliverKit(void* pc, const KitDef& kit)
{
    void* ch = PlayerPawn(pc);
    if (!ch) return 0;
    int ok = 0;
    for (const auto& item : kit.items)
    {
        const bool delivered = ConanApi::Call<bool>(
            ch, "SpawnTemplateItem",
            int32_t(item.templateId), ConanApi::Nome("kit"),
            int32_t(item.quantity), float(1.0f), float(0.0f), bool(true));
        if (g_api->UltimaChamadaExecutou() && delivered) ++ok;
    }
    g_lastDelivered = ok;
    return ok;
}

static void SelfVerify(void*)
{
    if (g_verified || !g_cfg.enabled) return;
    void* cls = g_api->FindClass("ConanCharacter");
    const bool ok = (g_hookChat != 0) && (cls != nullptr) && !g_cfg.kits.empty();
    g_api->Log("[StarterKits] VERIFY hook=%u kits=%zu last=%d => %s",
               (unsigned)g_hookChat, g_cfg.kits.size(), g_lastDelivered, ok ? "PASS" : "FAIL");
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
    const std::string pKit = g_cfg.prefix + g_cfg.cmdKit;
    if (msg.rfind(pKit, 0) != 0) return CONAN_CONTINUAR;

    int64_t uid = 0;
    const uint8_t* pUid = static_cast<const uint8_t*>(chat) + CHAT_UID;
    if (g_api->Legivel(pUid, 8)) uid = *reinterpret_cast<const int64_t*>(pUid);

    std::string arg;
    if (msg.size() > pKit.size())
    {
        arg = msg.substr(pKit.size());
        while (!arg.empty() && arg.front() == ' ') arg.erase(0, 1);
    }
    if (arg.empty()) arg = "starter";

    if (arg == "list" || arg == "help")
    {
        std::string list = "Kits: ";
        for (const auto& k : g_cfg.kits)
        {
            list += k.key;
            if (k.requiredLevel > 0)
            {
                char buf[24];
                std::snprintf(buf, sizeof(buf), "(lv%d)", k.requiredLevel);
                list += buf;
            }
            list += " ";
        }
        SendReply(c->Obj, list);
        return CONAN_CANCELAR;
    }

    const KitDef* kit = nullptr;
    for (const auto& k : g_cfg.kits)
        if (k.key == arg) { kit = &k; break; }
    if (!kit)
    {
        SendReply(c->Obj, "Unknown kit. Use !kit list");
        return CONAN_CANCELAR;
    }

    if (kit->requiredLevel > 0)
    {
        const int level = PlayerLevel(c->Obj);
        if (level < kit->requiredLevel)
        {
            char msg[128];
            std::snprintf(msg, sizeof(msg), "%s requires level %d (you are level %d).",
                          kit->name.c_str(), kit->requiredLevel, level);
            SendReply(c->Obj, msg);
            return CONAN_CANCELAR;
        }
    }

    if (g_cfg.cooldownHours > 0 && uid != 0)
    {
        const DWORD now = GetTickCount();
        auto it = g_lastClaim.find(uid);
        if (it != g_lastClaim.end())
        {
            const DWORD elapsedH = (now - it->second) / (1000u * 3600u);
            if (elapsedH < (DWORD)g_cfg.cooldownHours)
            {
                char cd[128];
                std::snprintf(cd, sizeof(cd), "Kit cooldown: wait %d more hour(s).",
                              g_cfg.cooldownHours - (int)elapsedH);
                SendReply(c->Obj, cd);
                return CONAN_CANCELAR;
            }
        }
        g_lastClaim[uid] = now;
    }

    const int n = DeliverKit(c->Obj, *kit);
    char reply[160];
    std::snprintf(reply, sizeof(reply), "%s delivered (%d/%zu items via SpawnTemplateItem).",
                  kit->name.c_str(), n, kit->items.size());
    SendReply(c->Obj, reply);
    return CONAN_CANCELAR;
}

extern "C" __declspec(dllexport)
void ConanPluginCarregar(const ConanApiTabela* api)
{
    if (!api || api->tamanho < sizeof(ConanApiTabela)) return;
    g_api = api;
    ConanApi::UsarTabela(api);
    g_cfg.Load("StarterKits");
    g_verified = false;

    if (!g_cfg.enabled)
    {
        g_api->Log("[StarterKits] Plugin disabled in config.json (Enabled=false). Load skipped.");
        return;
    }

    g_api->Log("======================================================");
    g_api->Log(" StarterKits v2.1.0 — kits.json with level-gated steel/hardened kits");
    g_api->Log(" Command: !%s [starter|steel|hardened|list] | kits=%zu",
               g_cfg.cmdKit.c_str(), g_cfg.kits.size());
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
        g_api->Log("[StarterKits] Unloaded.");
    }
    g_api = nullptr;
}
