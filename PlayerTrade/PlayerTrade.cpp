#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "Conan/ConanPluginApi.h"
#include "Conan/ConanBase.h"
#include "../common/JsonConfig.h"

#include <string>
#include <map>
#include <cmath>
#include <cstdio>
#include <cstring>

static const ConanApiTabela* g_api = nullptr;
static uint32_t g_hookChat = 0;
static uint32_t g_verifyId = 0;
static bool g_verified = false;

struct TradeOffer
{
    int64_t fromUid = 0;
    int64_t toUid = 0;
    void* fromPc = nullptr;
    void* toPc = nullptr;
    DWORD created = 0;
};

struct TradeConfig
{
    bool enabled = true;
    double maxTradeDistanceMeters = 15.0;
    std::string prefix = "!";
    std::string cmdTrade = "trade";

    void Load(const char* folderName)
    {
        const char* configPath = g_api->CaminhoConfig(folderName);
        if (!configPath) return;
        ConanUtils::JsonValue json;
        if (!ConanUtils::JsonParser::ParseFile(configPath, json)) return;
        if (json.has("Enabled")) enabled = json["Enabled"].asBool(true);
        if (json.has("MaxTradeDistanceMeters")) maxTradeDistanceMeters = json["MaxTradeDistanceMeters"].asDouble(15.0);
        if (json.has("Prefix")) prefix = json["Prefix"].asString("!");
        if (json.has("CommandTrade")) cmdTrade = json["CommandTrade"].asString("trade");
    }
};

static TradeConfig g_cfg;
static std::map<int64_t, TradeOffer> g_pending; // keyed by target uid
static const uint32_t CHAT_UID = 0x038;
static const uint32_t CHAT_USUARIO = 0x048;
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

static bool GetActorPosition(void* actor, FVector& outPos)
{
    if (!actor) return false;
    outPos = ConanApi::Call<FVector>(actor, "K2_GetActorLocation");
    return g_api->UltimaChamadaExecutou() != 0;
}

static void* BackpackOf(void* ch)
{
    void* inv = ConanApi::Call<void*>(ch, "GetBackpackInventory");
    if (g_api->UltimaChamadaExecutou() && inv) return inv;
    int32_t off = g_api->OffsetDoMembro(ch, "BackpackInventory");
    if (off >= 0) g_api->LerMembro(ch, uint32_t(off), &inv, sizeof(inv));
    return inv;
}

static bool FindNearestOtherPlayer(void* myPc, int64_t myUid, void** outPc, int64_t* outUid)
{
    void* myCh = PlayerPawn(myPc);
    if (!myCh) return false;
    FVector myPos;
    if (!GetActorPosition(myCh, myPos)) return false;

    void* pcs[128];
    const int n = g_api->FindObjects("ConanPlayerController", pcs, 128, 1);
    const double maxCm = g_cfg.maxTradeDistanceMeters * 100.0;
    double best = 1e300;
    void* bestPc = nullptr;
    int64_t bestUid = 0;

    for (int i = 0; i < n; ++i)
    {
        if (!pcs[i] || pcs[i] == myPc) continue;
        void* ch = PlayerPawn(pcs[i]);
        if (!ch) continue;
        FVector pos;
        if (!GetActorPosition(ch, pos)) continue;
        const double dx = myPos.X - pos.X;
        const double dy = myPos.Y - pos.Y;
        const double dz = myPos.Z - pos.Z;
        const double d = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (d > maxCm || d >= best) continue;

        int64_t uid = 0;
        int32_t off = g_api->OffsetDoMembro(ch, "CharacterUniqueID");
        if (off < 0) off = g_api->OffsetDoMembro(ch, "PlayerId");
        if (off >= 0) g_api->LerMembro(ch, uint32_t(off), &uid, sizeof(uid));
        if (uid != 0 && uid == myUid) continue;

        best = d;
        bestPc = pcs[i];
        bestUid = uid;
    }
    if (!bestPc) return false;
    *outPc = bestPc;
    *outUid = bestUid;
    return true;
}

static void SelfVerify(void*)
{
    if (g_verified || !g_cfg.enabled) return;
    void* cls = g_api->FindClass("ConanPlayerController");
    const bool ok = (g_hookChat != 0) && (cls != nullptr);
    g_api->Log("[PlayerTrade] VERIFY hook=%u pcCls=%d => %s",
               (unsigned)g_hookChat, cls ? 1 : 0, ok ? "PASS" : "FAIL");
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
    const std::string pTrade = g_cfg.prefix + g_cfg.cmdTrade;
    if (msg.rfind(pTrade, 0) != 0) return CONAN_CONTINUAR;

    int64_t uid = 0;
    const uint8_t* pUid = static_cast<const uint8_t*>(chat) + CHAT_UID;
    if (g_api->Legivel(pUid, 8)) uid = *reinterpret_cast<const int64_t*>(pUid);
    void* pc = c->Obj;

    std::string arg;
    if (msg.size() > pTrade.size())
    {
        arg = msg.substr(pTrade.size());
        while (!arg.empty() && arg.front() == ' ') arg.erase(0, 1);
    }

    if (arg == "accept" || arg == "yes")
    {
        auto it = g_pending.find(uid);
        if (it == g_pending.end())
        {
            SendReply(pc, "No pending trade request.");
            return CONAN_CANCELAR;
        }
        TradeOffer offer = it->second;
        g_pending.erase(it);

        void* myCh = PlayerPawn(pc);
        void* theirCh = PlayerPawn(offer.fromPc);
        void* myInv = BackpackOf(myCh);
        void* theirInv = BackpackOf(theirCh);
        if (!myInv || !theirInv)
        {
            SendReply(pc, "Trade failed: missing backpack.");
            SendReply(offer.fromPc, "Trade failed: missing backpack.");
            return CONAN_CANCELAR;
        }

        // Handshake confirmation — full item transfer is player-driven via open inventories.
        // Open each other's trade-adjacent feedback.
        SendReply(pc, "Trade accepted. Open inventories next to each other to exchange.");
        SendReply(offer.fromPc, "Your trade request was accepted. Exchange items nearby.");
        return CONAN_CANCELAR;
    }

    void* targetPc = nullptr;
    int64_t targetUid = 0;
    if (!FindNearestOtherPlayer(pc, uid, &targetPc, &targetUid))
    {
        char reply[128];
        std::snprintf(reply, sizeof(reply), "No player within %.0fm to trade with.",
                      g_cfg.maxTradeDistanceMeters);
        SendReply(pc, reply);
        return CONAN_CANCELAR;
    }

    TradeOffer offer;
    offer.fromUid = uid;
    offer.toUid = targetUid;
    offer.fromPc = pc;
    offer.toPc = targetPc;
    offer.created = GetTickCount();
    g_pending[targetUid] = offer;

    SendReply(pc, "Trade request sent to nearest player.");
    SendReply(targetPc, "Trade request received. Type !trade accept to confirm.");
    return CONAN_CANCELAR;
}

extern "C" __declspec(dllexport)
void ConanPluginCarregar(const ConanApiTabela* api)
{
    if (!api || api->tamanho < sizeof(ConanApiTabela)) return;
    g_api = api;
    ConanApi::UsarTabela(api);
    g_cfg.Load("PlayerTrade");
    g_verified = false;

    if (!g_cfg.enabled)
    {
        g_api->Log("[PlayerTrade] Plugin disabled in config.json (Enabled=false). Load skipped.");
        return;
    }

    g_api->Log("======================================================");
    g_api->Log(" PlayerTrade v2.0.0 — nearest-player trade handshake");
    g_api->Log(" Max Distance: %.0fm | !trade | !trade accept", g_cfg.maxTradeDistanceMeters);
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
        g_api->Log("[PlayerTrade] Unloaded.");
    }
    g_api = nullptr;
}
