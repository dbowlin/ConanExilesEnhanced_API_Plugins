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

static const ConanApiTabela* g_api = nullptr;
static uint32_t g_hookChat = 0;
static uint32_t g_hookDeath = 0;
static uint32_t g_verifyId = 0;
static bool g_verified = false;
static int g_lastRecovered = 0;

struct RecoveryConfig
{
    bool enabled = true;
    int cooldownSeconds = 30;
    int costGoldCoins = 0;
    double absorbRadiusMeters = 5000.0;
    std::string prefix = "!";
    std::string cmdRecover = "recover";

    void Load(const char* folderName)
    {
        const char* configPath = g_api->CaminhoConfig(folderName);
        if (!configPath) return;
        ConanUtils::JsonValue json;
        if (!ConanUtils::JsonParser::ParseFile(configPath, json)) return;
        if (json.has("Enabled")) enabled = json["Enabled"].asBool(true);
        if (json.has("CooldownSeconds")) cooldownSeconds = json["CooldownSeconds"].asInt(30);
        if (json.has("CostGoldCoins")) costGoldCoins = json["CostGoldCoins"].asInt(0);
        if (json.has("AbsorbRadiusMeters")) absorbRadiusMeters = json["AbsorbRadiusMeters"].asDouble(5000.0);
        if (json.has("Prefix")) prefix = json["Prefix"].asString("!");
        if (json.has("CommandRecover")) cmdRecover = json["CommandRecover"].asString("recover");
    }
};

static RecoveryConfig g_cfg;
static void* g_lootClasses[2];
static int g_lootClassCount = 0;
static std::map<int64_t, DWORD> g_lastRecover;
static std::map<int64_t, FVector> g_deathPos;
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

static void CacheLootClasses()
{
    if (g_lootClassCount > 0) return;
    void* cls = g_api->FindClass("LootContainer");
    if (cls) g_lootClasses[g_lootClassCount++] = cls;
    cls = g_api->FindClass("BP_LootContainer_C");
    if (cls && g_lootClassCount < 2) g_lootClasses[g_lootClassCount++] = cls;
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

static bool TeleportActor(void* actor, FVector dest)
{
    FRotator rot{ 0.0, 0.0, 0.0 };
    if (ConanApi::Call<bool>(actor, "K2_TeleportTo", dest, rot) && g_api->UltimaChamadaExecutou())
        return true;
    return ConanApi::Call<bool>(actor, "TeleportTo", dest, rot) && g_api->UltimaChamadaExecutou();
}

extern "C" ConanAcao OnCharacterDeath(ConanChamada* c)
{
    if (!g_cfg.enabled || !c || !c->Obj) return CONAN_CONTINUAR;
    void* dead = c->Obj;
    if (!g_api->DescendeDe(dead, "ConanCharacter")) return CONAN_CONTINUAR;

    int64_t uid = 0;
    int32_t off = g_api->OffsetDoMembro(dead, "CharacterUniqueID");
    if (off < 0) off = g_api->OffsetDoMembro(dead, "PlayerId");
    if (off >= 0) g_api->LerMembro(dead, uint32_t(off), &uid, sizeof(uid));
    if (uid == 0) return CONAN_CONTINUAR;

    FVector pos;
    if (GetActorPosition(dead, pos))
        g_deathPos[uid] = pos;
    return CONAN_CONTINUAR;
}

static int TeleportNearbyLoot(void* charPawn, const FVector& playerPos, const FVector* preferNear)
{
    int moved = 0;
    CacheLootClasses();
    if (g_lootClassCount == 0) return 0;

    const double maxCm = g_cfg.absorbRadiusMeters * 100.0;
    FVector drop = playerPos;
    drop.Z += 15.0;

    ScanUtil::ForEachOfClasses(g_api, g_lootClasses, g_lootClassCount, [&](void* bag) {
        if (IsDefaultObject(bag)) return;
        FVector bagPos;
        if (!GetActorPosition(bag, bagPos)) return;

        double refX = preferNear ? preferNear->X : playerPos.X;
        double refY = preferNear ? preferNear->Y : playerPos.Y;
        double refZ = preferNear ? preferNear->Z : playerPos.Z;
        const double dx = bagPos.X - refX;
        const double dy = bagPos.Y - refY;
        const double dz = bagPos.Z - refZ;
        if (std::sqrt(dx * dx + dy * dy + dz * dz) > maxCm) return;

        if (TeleportActor(bag, drop))
            ++moved;
    });

    // Also try AbsorbLootBagsOfClass (pulls into inventory when class resolves)
    void* lootCls = g_api->FindClass("LootContainer");
    if (!lootCls) lootCls = g_api->FindClass("BP_LootContainer_C");
    if (lootCls)
    {
        ConanApi::Call<void>(charPawn, "AbsorbLootBagsOfClass",
                             float(float(g_cfg.absorbRadiusMeters * 100.0)), lootCls);
        if (g_api->UltimaChamadaExecutou() && moved == 0) moved = 1;
    }
    return moved;
}

static void HandleRecover(void* pc, int64_t uid)
{
    const DWORD now = GetTickCount();
    auto itCd = g_lastRecover.find(uid);
    if (itCd != g_lastRecover.end())
    {
        const DWORD elapsed = (now - itCd->second) / 1000;
        if (elapsed < (DWORD)g_cfg.cooldownSeconds)
        {
            char msg[128];
            std::snprintf(msg, sizeof(msg), "Please wait %d second(s) before recovering again.",
                          g_cfg.cooldownSeconds - (int)elapsed);
            SendReply(pc, msg);
            return;
        }
    }

    void* ch = PlayerPawn(pc);
    if (!ch) { SendReply(pc, "Error: Player character not found."); return; }
    FVector playerPos;
    if (!GetActorPosition(ch, playerPos)) { SendReply(pc, "Error: Could not read location."); return; }

    const FVector* prefer = nullptr;
    auto itPos = g_deathPos.find(uid);
    if (itPos != g_deathPos.end()) prefer = &itPos->second;

    const int n = TeleportNearbyLoot(ch, playerPos, prefer);
    g_lastRecovered = n;
    if (n > 0)
    {
        g_lastRecover[uid] = now;
        char reply[128];
        std::snprintf(reply, sizeof(reply), "Recovered %d loot bag(s) to your feet.", n);
        SendReply(pc, reply);
    }
    else
        SendReply(pc, "No nearby death loot bags found to recover.");
}

static void SelfVerify(void*)
{
    if (g_verified || !g_cfg.enabled) return;
    void* cls = g_api->FindClass("LootContainer");
    if (!cls) cls = g_api->FindClass("BP_LootContainer_C");
    const bool ok = (g_hookChat != 0) && (g_hookDeath != 0) && (cls != nullptr);
    g_api->Log("[CorpseRecovery] VERIFY hookChat=%u hookDeath=%u lootCls=%d last=%d => %s",
               (unsigned)g_hookChat, (unsigned)g_hookDeath, cls ? 1 : 0, g_lastRecovered,
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
    const std::string pRecover = g_cfg.prefix + g_cfg.cmdRecover;
    if (msg.rfind(pRecover, 0) != 0) return CONAN_CONTINUAR;

    int64_t uid = 0;
    const uint8_t* pUid = static_cast<const uint8_t*>(chat) + CHAT_UID;
    if (g_api->Legivel(pUid, 8)) uid = *reinterpret_cast<const int64_t*>(pUid);
    HandleRecover(c->Obj, uid);
    return CONAN_CANCELAR;
}

extern "C" __declspec(dllexport)
void ConanPluginCarregar(const ConanApiTabela* api)
{
    if (!api || api->tamanho < sizeof(ConanApiTabela)) return;
    g_api = api;
    ConanApi::UsarTabela(api);
    g_cfg.Load("CorpseRecovery");
    g_verified = false;

    if (!g_cfg.enabled)
    {
        g_api->Log("[CorpseRecovery] Plugin disabled in config.json (Enabled=false). Load skipped.");
        return;
    }

    g_api->Log("======================================================");
    g_api->Log(" CorpseRecovery v2.1.0 — LootContainer teleport + AbsorbLootBagsOfClass");
    g_api->Log(" Command: !%s | Cooldown: %ds", g_cfg.cmdRecover.c_str(), g_cfg.cooldownSeconds);
    g_api->Log("======================================================");

    g_hookChat = g_api->HookProcessEvent("ServerSendChatMessage", OnChatMessage, nullptr, 100);
    g_hookDeath = g_api->HookProcessEvent("OnDeath", OnCharacterDeath, nullptr, 100);
    g_verifyId = g_api->AgendarNaThreadDoJogo(SelfVerify, 3, nullptr, 1);
}

extern "C" __declspec(dllexport)
void ConanPluginDescarregar(void)
{
    if (g_api)
    {
        if (g_hookChat) g_api->RemoverHook(g_hookChat);
        if (g_hookDeath) g_api->RemoverHook(g_hookDeath);
        if (g_verifyId) g_api->CancelarAgendamento(g_verifyId);
        g_api->Log("[CorpseRecovery] Unloaded.");
    }
    g_api = nullptr;
}
