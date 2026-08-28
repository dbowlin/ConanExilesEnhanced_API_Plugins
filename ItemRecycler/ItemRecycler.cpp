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
static int g_lastSalvaged = 0;

struct RecyclerConfig
{
    bool enabled = true;
    int salvageEfficiencyPercent = 75;
    std::string prefix = "!";
    std::string cmdSalvage = "salvage";

    void Load(const char* folderName)
    {
        const char* configPath = g_api->CaminhoConfig(folderName);
        if (!configPath) return;
        ConanUtils::JsonValue json;
        if (!ConanUtils::JsonParser::ParseFile(configPath, json)) return;
        if (json.has("Enabled")) enabled = json["Enabled"].asBool(true);
        if (json.has("SalvageEfficiencyPercent")) salvageEfficiencyPercent = json["SalvageEfficiencyPercent"].asInt(75);
        if (json.has("Prefix")) prefix = json["Prefix"].asString("!");
        if (json.has("CommandSalvage")) cmdSalvage = json["CommandSalvage"].asString("salvage");
    }
};

static RecyclerConfig g_cfg;
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

static void* BackpackOf(void* ch)
{
    void* inv = ConanApi::Call<void*>(ch, "GetBackpackInventory");
    if (g_api->UltimaChamadaExecutou() && inv) return inv;
    int32_t off = g_api->OffsetDoMembro(ch, "BackpackInventory");
    if (off >= 0) g_api->LerMembro(ch, uint32_t(off), &inv, sizeof(inv));
    return inv;
}

static void HandleSalvage(void* pc)
{
    void* ch = PlayerPawn(pc);
    if (!ch) return;
    void* inv = BackpackOf(ch);
    if (!inv)
    {
        SendReply(pc, "No backpack inventory found.");
        return;
    }

    int removed = 0;
    const int32_t num = ConanApi::Call<int32_t>(inv, "GetItemCount");
    const int n = g_api->UltimaChamadaExecutou() ? num : 0;

    // Walk slots from the end so RemoveItem indices stay stable
    for (int i = n - 1; i >= 0 && removed < 20; --i)
    {
        void* item = ConanApi::Call<void*>(inv, "GetItemAt", int32_t(i));
        if (!g_api->UltimaChamadaExecutou() || !item) continue;

        const bool can = ConanApi::Call<bool>(inv, "CanItemBeDismantledByStation", item);
        if (!g_api->UltimaChamadaExecutou() || !can) continue;

        ConanApi::Call<void*>(inv, "RemoveItem", int32_t(i), ConanApi::Nome("salvage"), (void*)nullptr);
        if (g_api->UltimaChamadaExecutou()) ++removed;
    }

    const int mats = (removed * g_cfg.salvageEfficiencyPercent) / 100;
    const int iron = mats > 0 ? mats : (removed > 0 ? 1 : 0);
    if (iron > 0)
    {
        ConanApi::Call<bool>(ch, "SpawnTemplateItem",
                             int32_t(11011), ConanApi::Nome("salvage"),
                             int32_t(iron), float(1.0f), float(0.0f), bool(true));
        ConanApi::Call<bool>(ch, "SpawnTemplateItem",
                             int32_t(11015), ConanApi::Nome("salvage"),
                             int32_t((iron + 1) / 2), float(1.0f), float(0.0f), bool(true));
    }

    g_lastSalvaged = removed;
    char reply[160];
    std::snprintf(reply, sizeof(reply),
                  "Salvage: dismantled %d item(s) → ~%d iron / leather (%d%% eff).",
                  removed, iron, g_cfg.salvageEfficiencyPercent);
    SendReply(pc, reply);
}

static void SelfVerify(void*)
{
    if (g_verified || !g_cfg.enabled) return;
    void* cls = g_api->FindClass("ConanCharacter");
    const bool ok = (g_hookChat != 0) && (cls != nullptr);
    g_api->Log("[ItemRecycler] VERIFY hook=%u charCls=%d last=%d => %s",
               (unsigned)g_hookChat, cls ? 1 : 0, g_lastSalvaged, ok ? "PASS" : "FAIL");
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
    const std::string pSalvage = g_cfg.prefix + g_cfg.cmdSalvage;
    if (msg.rfind(pSalvage, 0) != 0) return CONAN_CONTINUAR;
    HandleSalvage(c->Obj);
    return CONAN_CANCELAR;
}

extern "C" __declspec(dllexport)
void ConanPluginCarregar(const ConanApiTabela* api)
{
    if (!api || api->tamanho < sizeof(ConanApiTabela)) return;
    g_api = api;
    ConanApi::UsarTabela(api);
    g_cfg.Load("ItemRecycler");
    g_verified = false;

    if (!g_cfg.enabled)
    {
        g_api->Log("[ItemRecycler] Plugin disabled in config.json (Enabled=false). Load skipped.");
        return;
    }

    g_api->Log("======================================================");
    g_api->Log(" ItemRecycler v2.0.0 — GetItemAt/RemoveItem + SpawnTemplateItem mats");
    g_api->Log(" Efficiency: %d%% | Command: !%s", g_cfg.salvageEfficiencyPercent, g_cfg.cmdSalvage.c_str());
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
        g_api->Log("[ItemRecycler] Unloaded.");
    }
    g_api = nullptr;
}
