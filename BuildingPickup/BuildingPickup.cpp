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

static const ConanApiTabela* g_api = nullptr;
static uint32_t g_hookChat = 0;
static uint32_t g_verifyId = 0;
static bool g_verified = false;
static int g_lastPickupOk = 0;

struct PickupConfig
{
    bool enabled = true;
    double pickupRadiusMeters = 10.0;
    bool allowPickupFullContainers = false;
    std::string prefix = "!";
    std::string cmdPickup = "pickup";

    void Load(const char* folderName)
    {
        const char* configPath = g_api->CaminhoConfig(folderName);
        if (!configPath) return;
        ConanUtils::JsonValue json;
        if (!ConanUtils::JsonParser::ParseFile(configPath, json)) return;
        if (json.has("Enabled")) enabled = json["Enabled"].asBool(true);
        if (json.has("PickupRadiusMeters")) pickupRadiusMeters = json["PickupRadiusMeters"].asDouble(10.0);
        if (json.has("AllowPickupFullContainers")) allowPickupFullContainers = json["AllowPickupFullContainers"].asBool(false);
        if (json.has("Prefix")) prefix = json["Prefix"].asString("!");
        if (json.has("CommandPickup")) cmdPickup = json["CommandPickup"].asString("pickup");
    }
};

static PickupConfig g_cfg;
static void* g_buildClasses[3];
static int g_buildClassCount = 0;
static const uint32_t CHAT_TEXTO = 0x068;

static bool ReadText(const void* base, uint32_t off, char* outBuf, int maxLen)
{
    outBuf[0] = 0;
    return g_api->LerTextoDoJogo(base, off, outBuf, maxLen) != 0;
}

static bool IsDefaultObject(void* obj)
{
    if (!obj) return true;
    char name[256] = {0};
    if (!g_api->NomeDoObjeto(obj, name, sizeof(name))) return false;
    return std::strncmp(name, "Default__", 9) == 0;
}

static void SendReply(void* playerController, const std::string& text)
{
    if (!playerController || text.empty()) return;
    ConanApi::Call<void>(playerController, "ClientHUDShowNotification",
                         ConanApi::TextoRico(text.c_str()), bool(true), bool(false));
    if (!g_api->UltimaChamadaExecutou())
    {
        ConanApi::Call<void>(playerController, "ClientMessage",
                             ConanApi::Texto(text.c_str()), ConanApi::Nome("Event"), float(6.0f));
    }
}

static bool GetActorPosition(void* actor, FVector& outPos)
{
    if (!actor) return false;
    outPos = ConanApi::Call<FVector>(actor, "K2_GetActorLocation");
    return g_api->UltimaChamadaExecutou() != 0;
}

static void CacheBuildClasses()
{
    if (g_buildClassCount > 0) return;
    const char* names[] = { "BuildableBase", "PlaceableBase", "BuildingBase", nullptr };
    for (int i = 0; names[i]; ++i)
    {
        void* cls = g_api->FindClass(names[i]);
        if (cls && g_buildClassCount < 3) g_buildClasses[g_buildClassCount++] = cls;
    }
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

static bool TryReturnToInventory(void* pc, void* piece)
{
    // PlayerController APIs
    ConanApi::Call<void>(pc, "ServerReturnPlaceableToInventory", piece);
    if (g_api->UltimaChamadaExecutou()) return true;

    ConanApi::Call<void>(pc, "ServerReturnBuildingToInventory", piece, (void*)nullptr, int32_t(0));
    if (g_api->UltimaChamadaExecutou()) return true;

    // Character interactable paths (spaces in BP names)
    void* ch = PlayerPawn(pc);
    if (ch)
    {
        ConanApi::Call<void>(ch, "Player Interactable ReturnToInventory", piece);
        if (g_api->UltimaChamadaExecutou()) return true;
        ConanApi::Call<void>(ch, "Player Interactable Dismantle", piece);
        if (g_api->UltimaChamadaExecutou()) return true;
        ConanApi::Call<void>(ch, "Server Interactable Dismantle", piece);
        if (g_api->UltimaChamadaExecutou()) return true;
    }
    return false;
}

static void HandlePickup(void* pc, double customRadius)
{
    void* charPawn = PlayerPawn(pc);
    if (!charPawn) return;
    FVector playerPos;
    if (!GetActorPosition(charPawn, playerPos)) return;

    const double radius = (customRadius > 0.0) ? customRadius : g_cfg.pickupRadiusMeters;
    const double maxDistCm = radius * 100.0;

    CacheBuildClasses();
    if (g_buildClassCount == 0) return;

    void* closest = nullptr;
    double closestDist = 1e300;
    ScanUtil::ForEachOfClasses(g_api, g_buildClasses, g_buildClassCount, [&](void* piece) {
        if (IsDefaultObject(piece)) return;
        FVector pos;
        if (!GetActorPosition(piece, pos)) return;
        const double dx = playerPos.X - pos.X;
        const double dy = playerPos.Y - pos.Y;
        const double dz = playerPos.Z - pos.Z;
        const double dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (dist <= maxDistCm && dist < closestDist)
        {
            closestDist = dist;
            closest = piece;
        }
    });

    if (!closest)
    {
        SendReply(pc, "No structure or placeable piece found in direct pickup range.");
        return;
    }

    if (TryReturnToInventory(pc, closest))
    {
        g_lastPickupOk = 1;
        SendReply(pc, "Structure piece picked up and returned to inventory.");
    }
    else
    {
        g_lastPickupOk = 0;
        SendReply(pc, "Pickup failed — no ReturnToInventory / Dismantle path executed.");
    }
}

static void SelfVerify(void*)
{
    if (g_verified || !g_cfg.enabled) return;
    void* cls = g_api->FindClass("BuildableBase");
    if (!cls) cls = g_api->FindClass("PlaceableBase");
    const bool ok = (g_hookChat != 0) && (cls != nullptr);
    g_api->Log("[BuildingPickup] VERIFY hook=%u buildCls=%d lastOk=%d => %s",
               (unsigned)g_hookChat, cls ? 1 : 0, g_lastPickupOk, ok ? "PASS" : "FAIL");
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
    const std::string pPickup = g_cfg.prefix + g_cfg.cmdPickup;
    if (msg.rfind(pPickup, 0) != 0) return CONAN_CONTINUAR;

    double rad = 0.0;
    if (msg.size() > pPickup.size())
    {
        std::string arg = msg.substr(pPickup.size());
        while (!arg.empty() && arg.front() == ' ') arg.erase(0, 1);
        if (!arg.empty()) rad = std::atof(arg.c_str());
    }
    HandlePickup(c->Obj, rad);
    return CONAN_CANCELAR;
}

extern "C" __declspec(dllexport)
void ConanPluginCarregar(const ConanApiTabela* api)
{
    if (!api || api->tamanho < sizeof(ConanApiTabela)) return;
    g_api = api;
    ConanApi::UsarTabela(api);
    g_cfg.Load("BuildingPickup");
    g_verified = false;

    if (!g_cfg.enabled)
    {
        g_api->Log("[BuildingPickup] Plugin disabled in config.json (Enabled=false). Load skipped.");
        return;
    }

    g_api->Log("======================================================");
    g_api->Log(" BuildingPickup v2.1.0 — ServerReturnPlaceableToInventory");
    g_api->Log(" Command: !%s [radius] | Max Range: %.0fm", g_cfg.cmdPickup.c_str(), g_cfg.pickupRadiusMeters);
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
        g_api->Log("[BuildingPickup] Unloaded.");
    }
    g_api = nullptr;
}
