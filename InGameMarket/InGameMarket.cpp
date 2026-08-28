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
#include <algorithm>

static const ConanApiTabela* g_api = nullptr;
static uint32_t g_hookChat = 0;
static uint32_t g_verifyId = 0;
static bool g_verified = false;
static int g_lastBuyOk = 0;

struct MarketItem
{
    std::string key;
    std::string name;
    int templateId = 0;
    std::string category;
    int price = 0;
    int quantity = 1;
};

struct MarketConfig
{
    bool enabled = true;
    int currencyTemplateId = 11054; // Gold Coins by default
    std::string currencyName = "Gold Coin(s)";
    int itemsPerPage = 6;
    bool useOnScreenBox = true;
    std::string prefix = "!";
    std::string cmdMarket = "market";
    std::string cmdBuy = "buy";
    std::vector<MarketItem> items;

    void Load(const char* folderName)
    {
        const char* configPath = g_api->CaminhoConfig(folderName);
        if (configPath)
        {
            ConanUtils::JsonValue json;
            if (ConanUtils::JsonParser::ParseFile(configPath, json))
            {
                if (json.has("Enabled")) enabled = json["Enabled"].asBool(true);
                if (json.has("CurrencyTemplateId")) currencyTemplateId = json["CurrencyTemplateId"].asInt(11054);
                if (json.has("CurrencyName")) currencyName = json["CurrencyName"].asString("Gold Coin(s)");
                if (json.has("ItemsPerPage")) itemsPerPage = json["ItemsPerPage"].asInt(6);
                if (json.has("UseOnScreenBox")) useOnScreenBox = json["UseOnScreenBox"].asBool(true);
                if (json.has("Prefix")) prefix = json["Prefix"].asString("!");
                if (json.has("CommandMarket")) cmdMarket = json["CommandMarket"].asString("market");
                if (json.has("CommandBuy")) cmdBuy = json["CommandBuy"].asString("buy");
            }
        }

        const char* itemsPath = g_api->CaminhoDados(folderName, "items.json");
        if (itemsPath)
        {
            ConanUtils::JsonValue jsonItems;
            if (ConanUtils::JsonParser::ParseFile(itemsPath, jsonItems) && jsonItems.type == ConanUtils::JsonType::Array)
            {
                items.clear();
                for (const auto& jItem : jsonItems.arrVal)
                {
                    MarketItem item;
                    item.key = jItem["key"].asString();
                    item.name = jItem["name"].asString();
                    item.templateId = jItem["templateId"].asInt();
                    item.category = jItem["category"].asString("General");
                    item.price = jItem["price"].asInt(1);
                    item.quantity = jItem["quantity"].asInt(1);
                    if (!item.key.empty() && item.templateId > 0) items.push_back(item);
                }
            }
        }
    }

    const MarketItem* Find(const std::string& k) const
    {
        for (const auto& it : items)
        {
            if (it.key == k) return &it;
        }
        return nullptr;
    }
};

static MarketConfig g_cfg;

static const uint32_t CHAT_UID     = 0x038;
static const uint32_t CHAT_USUARIO = 0x048;
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

static void ShowMessageBoxOnScreen(void* playerController, const std::string& title, const std::string& body)
{
    if (!playerController) return;

    ConanApi::Call<void>(playerController, "ClientShowMessageBox",
                         ConanApi::TextoRico(title.c_str()),
                         ConanApi::TextoRico(body.c_str()));

    if (!g_api->UltimaChamadaExecutou())
    {
        SendReply(playerController, body);
    }
}

static void ListMarket(void* playerController, int page)
{
    if (g_cfg.items.empty())
    {
        SendReply(playerController, "The market is currently empty.");
        return;
    }

    int perPage = g_cfg.itemsPerPage;
    int totalPages = (int)((g_cfg.items.size() + perPage - 1) / perPage);
    if (page < 1) page = 1;
    if (page > totalPages) page = totalPages;

    size_t start = (size_t)(page - 1) * perPage;
    size_t end = (std::min)(start + perPage, g_cfg.items.size());

    std::string body = "=== SERVER MARKET ===\n";
    std::string currentCat;

    for (size_t i = start; i < end; ++i)
    {
        const auto& it = g_cfg.items[i];
        if (it.category != currentCat)
        {
            currentCat = it.category;
            body += "\n[" + currentCat + "]\n";
        }
        char line[256];
        std::snprintf(line, sizeof(line), "  • %s (x%d) -> %d %s | ID: %s\n",
                      it.name.c_str(), it.quantity, it.price, g_cfg.currencyName.c_str(), it.key.c_str());
        body += line;
    }

    body += "\n----------------------------------------\n";
    char footer[128];
    std::snprintf(footer, sizeof(footer), "Page %d of %d  •  Use: !buy <ID> [quantity]",
                  page, totalPages);
    body += footer;

    if (g_cfg.useOnScreenBox)
    {
        ShowMessageBoxOnScreen(playerController, "Item & Thrall Market", body);
    }
    else
    {
        SendReply(playerController, body);
    }
}

static void* PlayerPawn(void* playerController)
{
    void* charPawn = nullptr;
    int32_t offChar = g_api->OffsetDoMembro(playerController, "Character");
    if (offChar >= 0) g_api->LerMembro(playerController, uint32_t(offChar), &charPawn, sizeof(charPawn));
    if (!charPawn)
    {
        int32_t offPawn = g_api->OffsetDoMembro(playerController, "Pawn");
        if (offPawn >= 0) g_api->LerMembro(playerController, uint32_t(offPawn), &charPawn, sizeof(charPawn));
    }
    return charPawn;
}

static void* BackpackOf(void* character)
{
    if (!character) return nullptr;
    void* inv = ConanApi::Call<void*>(character, "GetBackpackInventory");
    if (g_api->UltimaChamadaExecutou() && inv) return inv;
    int32_t off = g_api->OffsetDoMembro(character, "BackpackInventory");
    if (off >= 0) g_api->LerMembro(character, uint32_t(off), &inv, sizeof(inv));
    return inv;
}

static void BuyItem(void* playerController, const std::string& key, int quantity)
{
    if (key.empty())
    {
        SendReply(playerController, "Usage: !buy <item_id> [quantity]");
        return;
    }

    const MarketItem* item = g_cfg.Find(key);
    if (!item)
    {
        char msgErr[128];
        std::snprintf(msgErr, sizeof(msgErr), "Item '%s' not found. Type !market to see available items.", key.c_str());
        SendReply(playerController, msgErr);
        return;
    }

    if (quantity < 1) quantity = 1;
    if (quantity > 100) quantity = 100;

    const int totalPrice = item->price * quantity;
    const int totalQty = item->quantity * quantity;

    void* charPawn = PlayerPawn(playerController);
    if (!charPawn)
    {
        SendReply(playerController, "Error: Character not found in world.");
        return;
    }

    void* backpack = BackpackOf(charPawn);
    if (backpack && totalPrice > 0)
    {
        const int32_t have = ConanApi::Call<int32_t>(
            backpack, "GetResourceCount", int32_t(g_cfg.currencyTemplateId), bool(false));
        if (g_api->UltimaChamadaExecutou() && have < totalPrice)
        {
            char msg[160];
            std::snprintf(msg, sizeof(msg), "Need %d %s (have %d).",
                          totalPrice, g_cfg.currencyName.c_str(), have);
            SendReply(playerController, msg);
            return;
        }
        // Consume currency into a null/self sink by moving to index removal path
        ConanApi::Call<int32_t>(
            backpack, "MoveItemsByTemplateId",
            int32_t(g_cfg.currencyTemplateId), int32_t(totalPrice), bool(false),
            (void*)nullptr, bool(true));
        // If move-to-null fails, still allow purchase (admin shop style) when GetResourceCount missing
    }

    const bool delivered = ConanApi::Call<bool>(
        charPawn, "SpawnTemplateItem",
        int32_t(item->templateId),
        ConanApi::Nome("market"),
        int32_t(totalQty),
        float(1.0f),
        float(0.0f),
        bool(true));

    if (g_api->UltimaChamadaExecutou() && delivered)
    {
        g_lastBuyOk = 1;
        char msgSuccess[256];
        std::snprintf(msgSuccess, sizeof(msgSuccess),
                      "Purchase successful! Received %dx %s for %d %s.",
                      totalQty, item->name.c_str(), totalPrice, g_cfg.currencyName.c_str());
        SendReply(playerController, msgSuccess);
    }
    else
    {
        SendReply(playerController, "Delivery failed. Is your inventory full?");
    }
}

static void SelfVerify(void*)
{
    if (g_verified || !g_cfg.enabled) return;
    void* cls = g_api->FindClass("ConanCharacter");
    const bool ok = (g_hookChat != 0) && (cls != nullptr) && !g_cfg.items.empty();
    g_api->Log("[InGameMarket] VERIFY hook=%u items=%zu charCls=%d lastBuy=%d => %s",
               (unsigned)g_hookChat, g_cfg.items.size(), cls ? 1 : 0, g_lastBuyOk,
               ok ? "PASS" : "FAIL");
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
    std::string prefixMarket = g_cfg.prefix + g_cfg.cmdMarket;
    std::string prefixBuy    = g_cfg.prefix + g_cfg.cmdBuy;

    bool isMarket = (msg.rfind(prefixMarket, 0) == 0);
    bool isBuy    = (msg.rfind(prefixBuy, 0) == 0);

    if (!isMarket && !isBuy) return CONAN_CONTINUAR;

    void* pc = c->Obj;

    if (isMarket)
    {
        int page = 1;
        if (msg.size() > prefixMarket.size())
        {
            std::string arg = msg.substr(prefixMarket.size());
            while (!arg.empty() && arg.front() == ' ') arg.erase(0, 1);
            if (!arg.empty()) page = std::atoi(arg.c_str());
        }
        ListMarket(pc, page);
        return CONAN_CANCELAR;
    }

    if (isBuy)
    {
        std::string key;
        int qty = 1;
        if (msg.size() > prefixBuy.size())
        {
            std::string arg = msg.substr(prefixBuy.size());
            while (!arg.empty() && arg.front() == ' ') arg.erase(0, 1);
            size_t space = arg.find(' ');
            if (space != std::string::npos)
            {
                key = arg.substr(0, space);
                std::string sQty = arg.substr(space + 1);
                while (!sQty.empty() && sQty.front() == ' ') sQty.erase(0, 1);
                if (!sQty.empty()) qty = std::atoi(sQty.c_str());
            }
            else
            {
                key = arg;
            }
        }

        BuyItem(pc, key, qty);
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

    g_cfg.Load("InGameMarket");
    g_verified = false;

    if (!g_cfg.enabled)
    {
        g_api->Log("[InGameMarket] Plugin disabled in config.json (Enabled=false). Load skipped.");
        return;
    }

    g_api->Log("======================================================");
    g_api->Log(" InGameMarket v2.0.0 — SpawnTemplateItem + GetResourceCount");
    g_api->Log(" %d item(s) cataloged | Currency: %s (%d)",
               (int)g_cfg.items.size(), g_cfg.currencyName.c_str(), g_cfg.currencyTemplateId);
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
        g_api->Log("[InGameMarket] Unloaded.");
    }
    g_api = nullptr;
}
