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
static int g_lastRepaired = 0;

struct RepairConfig
{
    bool enabled = true;
    double repairRadiusMeters = 50.0;
    bool requireMaterialsInChests = false;
    std::string prefix = "!";
    std::string cmdRepair = "repair";
    std::string cmdBaseInfo = "baseinfo";

    void Load(const char* folderName)
    {
        const char* configPath = g_api->CaminhoConfig(folderName);
        if (!configPath) return;
        ConanUtils::JsonValue json;
        if (!ConanUtils::JsonParser::ParseFile(configPath, json)) return;
        if (json.has("Enabled")) enabled = json["Enabled"].asBool(true);
        if (json.has("RepairRadiusMeters")) repairRadiusMeters = json["RepairRadiusMeters"].asDouble(50.0);
        if (json.has("RequireMaterialsInChests")) requireMaterialsInChests = json["RequireMaterialsInChests"].asBool(false);
        if (json.has("Prefix")) prefix = json["Prefix"].asString("!");
        if (json.has("CommandRepair")) cmdRepair = json["CommandRepair"].asString("repair");
        if (json.has("CommandBaseInfo")) cmdBaseInfo = json["CommandBaseInfo"].asString("baseinfo");
    }
};

static RepairConfig g_cfg;
static void* g_buildClasses[2];
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
    void* cls = g_api->FindClass("BuildableBase");
    if (cls) g_buildClasses[g_buildClassCount++] = cls;
    cls = g_api->FindClass("BuildingBase");
    if (cls && g_buildClassCount < 2) g_buildClasses[g_buildClassCount++] = cls;
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

static bool RepairPiece(void* piece)
{
    if (!piece || IsDefaultObject(piece)) return false;

    int32_t maxHp = 0;
    int32_t curHp = 0;
    int32_t offMax = g_api->OffsetDoMembro(piece, "MaxHealth");
    int32_t offCur = g_api->OffsetDoMembro(piece, "CurrentHealth");
    if (offMax >= 0) g_api->LerMembro(piece, uint32_t(offMax), &maxHp, sizeof(maxHp));
    if (offCur >= 0) g_api->LerMembro(piece, uint32_t(offCur), &curHp, sizeof(curHp));
    if (maxHp <= 0) return false;
    if (curHp >= maxHp) return false;

    ConanApi::Call<void>(piece, "SetCurrentHealth", maxHp);
    if (g_api->UltimaChamadaExecutou()) return true;

    // Fallback: write member directly
    if (offCur >= 0 && g_api->EscreverMembro(piece, uint32_t(offCur), &maxHp, sizeof(maxHp)) > 0)
        return true;
    return false;
}

static void HandleRepair(void* pc)
{
    void* charPawn = PlayerPawn(pc);
    if (!charPawn) return;

    FVector playerPos;
    if (!GetActorPosition(charPawn, playerPos)) return;

    CacheBuildClasses();
    if (g_buildClassCount == 0) return;

    int repairedCount = 0;
    const double maxDistCm = g_cfg.repairRadiusMeters * 100.0;

    ScanUtil::ForEachOfClasses(g_api, g_buildClasses, g_buildClassCount, [&](void* piece) {
        if (IsDefaultObject(piece)) return;
        FVector pos;
        if (!GetActorPosition(piece, pos)) return;
        const double dx = playerPos.X - pos.X;
        const double dy = playerPos.Y - pos.Y;
        const double dz = playerPos.Z - pos.Z;
        if (std::sqrt(dx * dx + dy * dy + dz * dz) > maxDistCm) return;
        if (RepairPiece(piece)) ++repairedCount;
    });

    g_lastRepaired = repairedCount;
    char reply[128];
    std::snprintf(reply, sizeof(reply),
                  "Repair complete: %d base structure piece(s) restored to 100%% health.",
                  repairedCount);
    SendReply(pc, reply);
}

static void HandleBaseInfo(void* pc)
{
    void* charPawn = PlayerPawn(pc);
    if (!charPawn) return;
    FVector playerPos;
    if (!GetActorPosition(charPawn, playerPos)) return;

    CacheBuildClasses();
    if (g_buildClassCount == 0) return;

    int pieceCount = 0;
    int damaged = 0;
    const double maxDistCm = g_cfg.repairRadiusMeters * 100.0;
    ScanUtil::ForEachOfClasses(g_api, g_buildClasses, g_buildClassCount, [&](void* piece) {
        if (IsDefaultObject(piece)) return;
        FVector pos;
        if (!GetActorPosition(piece, pos)) return;
        const double dx = playerPos.X - pos.X;
        const double dy = playerPos.Y - pos.Y;
        const double dz = playerPos.Z - pos.Z;
        if (std::sqrt(dx * dx + dy * dy + dz * dz) > maxDistCm) return;
        ++pieceCount;
        int32_t maxHp = 0, curHp = 0;
        int32_t offMax = g_api->OffsetDoMembro(piece, "MaxHealth");
        int32_t offCur = g_api->OffsetDoMembro(piece, "CurrentHealth");
        if (offMax >= 0) g_api->LerMembro(piece, uint32_t(offMax), &maxHp, sizeof(maxHp));
        if (offCur >= 0) g_api->LerMembro(piece, uint32_t(offCur), &curHp, sizeof(curHp));
        if (maxHp > 0 && curHp < maxHp) ++damaged;
    });

    char reply[160];
    std::snprintf(reply, sizeof(reply),
                  "Base Scan: %d pieces within %.0fm (%d damaged).",
                  pieceCount, g_cfg.repairRadiusMeters, damaged);
    SendReply(pc, reply);
}

static void SelfVerify(void*)
{
    if (g_verified || !g_cfg.enabled) return;
    void* cls = g_api->FindClass("BuildableBase");
    if (!cls) cls = g_api->FindClass("BuildingBase");
    const bool ok = (g_hookChat != 0) && (cls != nullptr);
    g_api->Log("[BaseAutoRepair] VERIFY hook=%u buildCls=%d lastRepaired=%d => %s",
               (unsigned)g_hookChat, cls ? 1 : 0, g_lastRepaired, ok ? "PASS" : "FAIL");
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
    const std::string pRepair = g_cfg.prefix + g_cfg.cmdRepair;
    const std::string pBaseInfo = g_cfg.prefix + g_cfg.cmdBaseInfo;
    void* pc = c->Obj;

    if (msg.rfind(pRepair, 0) == 0) { HandleRepair(pc); return CONAN_CANCELAR; }
    if (msg.rfind(pBaseInfo, 0) == 0) { HandleBaseInfo(pc); return CONAN_CANCELAR; }
    return CONAN_CONTINUAR;
}

extern "C" __declspec(dllexport)
void ConanPluginCarregar(const ConanApiTabela* api)
{
    if (!api || api->tamanho < sizeof(ConanApiTabela)) return;
    g_api = api;
    ConanApi::UsarTabela(api);
    g_cfg.Load("BaseAutoRepair");
    g_verified = false;

    if (!g_cfg.enabled)
    {
        g_api->Log("[BaseAutoRepair] Plugin disabled in config.json (Enabled=false). Load skipped.");
        return;
    }

    g_api->Log("======================================================");
    g_api->Log(" BaseAutoRepair v2.1.0 — SetCurrentHealth(MaxHealth)");
    g_api->Log(" Commands: !repair | !baseinfo | Radius: %.0fm", g_cfg.repairRadiusMeters);
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
        g_api->Log("[BaseAutoRepair] Unloaded.");
    }
    g_api = nullptr;
}
