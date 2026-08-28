#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "Conan/ConanPluginApi.h"
#include "Conan/ConanBase.h"
#include "../common/JsonConfig.h"
#include "../common/ObjectScan.h"

#include <string>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>

static const ConanApiTabela* g_api = nullptr;
static uint32_t g_hookChat = 0;
static uint32_t g_verifyId = 0;
static bool g_verified = false;
static int g_lastMoved = 0;

struct PullerConfig
{
    bool enabled = true;
    double pullRadiusMeters = 75.0;
    bool enableAutoDeposit = true;
    std::string prefix = "!";
    std::string cmdPull = "pull";
    std::string cmdDeposit = "deposit";

    void Load(const char* folderName)
    {
        const char* configPath = g_api->CaminhoConfig(folderName);
        if (!configPath) return;
        ConanUtils::JsonValue json;
        if (!ConanUtils::JsonParser::ParseFile(configPath, json)) return;
        if (json.has("Enabled")) enabled = json["Enabled"].asBool(true);
        if (json.has("PullRadiusMeters")) pullRadiusMeters = json["PullRadiusMeters"].asDouble(75.0);
        if (json.has("EnableAutoDeposit")) enableAutoDeposit = json["EnableAutoDeposit"].asBool(true);
        if (json.has("Prefix")) prefix = json["Prefix"].asString("!");
        if (json.has("CommandPull")) cmdPull = json["CommandPull"].asString("pull");
        if (json.has("CommandDeposit")) cmdDeposit = json["CommandDeposit"].asString("deposit");
    }
};

static PullerConfig g_cfg;
static void* g_containerClasses[2];
static int g_containerClassCount = 0;
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

static void CacheContainerClasses()
{
    if (g_containerClassCount > 0) return;
    void* cls = g_api->FindClass("BaseContainer");
    if (cls) g_containerClasses[g_containerClassCount++] = cls;
    cls = g_api->FindClass("BP_PL_Chest_C");
    if (cls && g_containerClassCount < 2) g_containerClasses[g_containerClassCount++] = cls;
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
    if (!ch) return nullptr;
    void* inv = ConanApi::Call<void*>(ch, "GetBackpackInventory");
    if (g_api->UltimaChamadaExecutou() && inv) return inv;
    int32_t off = g_api->OffsetDoMembro(ch, "BackpackInventory");
    if (off >= 0) g_api->LerMembro(ch, uint32_t(off), &inv, sizeof(inv));
    return inv;
}

static void* ContainerInventory(void* box)
{
    if (!box) return nullptr;
    void* inv = ConanApi::Call<void*>(box, "GetInventory");
    if (g_api->UltimaChamadaExecutou() && inv) return inv;
    int32_t off = g_api->OffsetDoMembro(box, "m_Inventory");
    if (off >= 0) g_api->LerMembro(box, uint32_t(off), &inv, sizeof(inv));
    if (!inv)
    {
        off = g_api->OffsetDoMembro(box, "Inventory");
        if (off >= 0) g_api->LerMembro(box, uint32_t(off), &inv, sizeof(inv));
    }
    return inv;
}

static bool GetActorPosition(void* actor, FVector& outPos)
{
    if (!actor) return false;
    outPos = ConanApi::Call<FVector>(actor, "K2_GetActorLocation");
    return g_api->UltimaChamadaExecutou() != 0;
}

static int ParseTemplateId(const std::string& query)
{
    if (query.empty()) return 0;
    char* end = nullptr;
    long v = std::strtol(query.c_str(), &end, 10);
    if (end && *end == 0 && v > 0) return (int)v;

    // Common aliases
    if (query == "iron" || query == "ironbar") return 11011;
    if (query == "steel" || query == "steelbar") return 11012;
    if (query == "wood") return 10001;
    if (query == "stone") return 10011;
    if (query == "hide" || query == "leather") return 11015;
    if (query == "fiber") return 10021;
    return 0;
}

static int PullFromNearby(void* backpack, const FVector& playerPos, int templateId, int amount)
{
    CacheContainerClasses();
    if (g_containerClassCount == 0) return 0;

    const double maxCm = g_cfg.pullRadiusMeters * 100.0;
    int moved = 0;
    int remaining = amount > 0 ? amount : 999999;

    ScanUtil::ForEachOfClasses(g_api, g_containerClasses, g_containerClassCount, [&](void* box) {
        if (remaining <= 0) return;
        if (IsDefaultObject(box)) return;
        FVector boxPos;
        if (!GetActorPosition(box, boxPos)) return;
        const double dx = playerPos.X - boxPos.X;
        const double dy = playerPos.Y - boxPos.Y;
        const double dz = playerPos.Z - boxPos.Z;
        if (std::sqrt(dx * dx + dy * dy + dz * dz) > maxCm) return;

        void* src = ContainerInventory(box);
        if (!src || IsDefaultObject(src)) return;

        const int32_t n = ConanApi::Call<int32_t>(
            src, "MoveItemsByTemplateId",
            int32_t(templateId), int32_t(remaining), bool(false),
            backpack, bool(true));
        if (g_api->UltimaChamadaExecutou() && n > 0)
        {
            moved += n;
            remaining -= n;
        }
    });
    return moved;
}

static int DepositAllNearby(void* backpack, const FVector& playerPos)
{
    CacheContainerClasses();
    if (g_containerClassCount == 0) return 0;

    const double maxCm = g_cfg.pullRadiusMeters * 100.0;
    void* nearestInv = nullptr;
    double nearestDist = 1e18;

    ScanUtil::ForEachOfClasses(g_api, g_containerClasses, g_containerClassCount, [&](void* box) {
        if (IsDefaultObject(box)) return;
        FVector boxPos;
        if (!GetActorPosition(box, boxPos)) return;
        const double dx = playerPos.X - boxPos.X;
        const double dy = playerPos.Y - boxPos.Y;
        const double dz = playerPos.Z - boxPos.Z;
        const double dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (dist > maxCm) return;
        void* inv = ContainerInventory(box);
        if (!inv || IsDefaultObject(inv)) return;
        if (dist < nearestDist) { nearestDist = dist; nearestInv = inv; }
    });
    if (!nearestInv || !backpack) return 0;

    const int32_t count = ConanApi::Call<int32_t>(backpack, "GetItemCount");
    const int n = g_api->UltimaChamadaExecutou() ? count : 0;
    int moved = 0;
    for (int i = n - 1; i >= 0; --i)
    {
        void* item = ConanApi::Call<void*>(backpack, "GetItemAt", int32_t(i));
        if (!g_api->UltimaChamadaExecutou() || !item) continue;
        const int32_t tid = ConanApi::Call<int32_t>(item, "GetTemplateId");
        if (!g_api->UltimaChamadaExecutou() || tid <= 0) continue;
        const int32_t qty = ConanApi::Call<int32_t>(
            backpack, "MoveItemsByTemplateId",
            int32_t(tid), int32_t(999999), bool(true),
            nearestInv, bool(true));
        if (g_api->UltimaChamadaExecutou() && qty > 0) moved += qty;
    }
    return moved;
}

static void HandlePull(void* pc, const std::string& query, int amount)
{
    const int tid = ParseTemplateId(query);
    if (tid <= 0)
    {
        SendReply(pc, "Usage: !pull <templateId|iron|wood|stone> [amount]");
        return;
    }
    void* ch = PlayerPawn(pc);
    if (!ch) { SendReply(pc, "Character not found."); return; }
    void* backpack = BackpackOf(ch);
    if (!backpack) { SendReply(pc, "Backpack not found."); return; }
    FVector pos;
    if (!GetActorPosition(ch, pos)) { SendReply(pc, "Could not read position."); return; }

    const int moved = PullFromNearby(backpack, pos, tid, amount);
    g_lastMoved = moved;
    char reply[160];
    std::snprintf(reply, sizeof(reply),
                  "Pulled %d of template %d from nearby chests (%.0fm).",
                  moved, tid, g_cfg.pullRadiusMeters);
    SendReply(pc, reply);
}

static void HandleDeposit(void* pc)
{
    if (!g_cfg.enableAutoDeposit)
    {
        SendReply(pc, "Auto-deposit is disabled.");
        return;
    }
    void* ch = PlayerPawn(pc);
    if (!ch) return;
    void* backpack = BackpackOf(ch);
    FVector pos;
    if (!backpack || !GetActorPosition(ch, pos)) return;
    const int moved = DepositAllNearby(backpack, pos);
    g_lastMoved = moved;
    char reply[128];
    std::snprintf(reply, sizeof(reply), "Deposited %d item stack(s) into nearest chest.", moved);
    SendReply(pc, reply);
}

static void SelfVerify(void*)
{
    if (g_verified || !g_cfg.enabled) return;
    void* invCls = g_api->FindClass("ItemInventory");
    const bool ok = (g_hookChat != 0) && (invCls != nullptr);
    g_api->Log("[StoragePuller] VERIFY hook=%u invCls=%d lastMoved=%d => %s",
               (unsigned)g_hookChat, invCls ? 1 : 0, g_lastMoved, ok ? "PASS" : "FAIL");
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
    const std::string pPull = g_cfg.prefix + g_cfg.cmdPull;
    const std::string pDeposit = g_cfg.prefix + g_cfg.cmdDeposit;

    if (msg.rfind(pPull, 0) == 0)
    {
        std::string arg;
        int amount = 100;
        if (msg.size() > pPull.size())
        {
            arg = msg.substr(pPull.size());
            while (!arg.empty() && arg.front() == ' ') arg.erase(0, 1);
            size_t sp = arg.find(' ');
            if (sp != std::string::npos)
            {
                std::string sAmt = arg.substr(sp + 1);
                arg = arg.substr(0, sp);
                while (!sAmt.empty() && sAmt.front() == ' ') sAmt.erase(0, 1);
                if (!sAmt.empty()) amount = std::atoi(sAmt.c_str());
            }
        }
        HandlePull(c->Obj, arg, amount);
        return CONAN_CANCELAR;
    }
    if (msg.rfind(pDeposit, 0) == 0)
    {
        HandleDeposit(c->Obj);
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
    g_cfg.Load("StoragePuller");
    g_verified = false;

    if (!g_cfg.enabled)
    {
        g_api->Log("[StoragePuller] Plugin disabled in config.json (Enabled=false). Load skipped.");
        return;
    }

    g_api->Log("======================================================");
    g_api->Log(" StoragePuller v2.1.0 — MoveItemsByTemplateId chest <-> backpack");
    g_api->Log(" Radius: %.0fm | Commands: !pull, !deposit", g_cfg.pullRadiusMeters);
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
        g_api->Log("[StoragePuller] Unloaded.");
    }
    g_api = nullptr;
}
