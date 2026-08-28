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
#include <algorithm>

static const ConanApiTabela* g_api = nullptr;
static uint32_t g_hookChat = 0;
static uint32_t g_hookDeath = 0;
static uint32_t g_verifyId = 0;
static bool g_verified = false;

struct DeathRecord
{
    FVector location{ 0.0, 0.0, 0.0 };
    DWORD timestamp = 0;
    std::string grid;
};

struct CorpseConfig
{
    bool enabled = true;
    bool allowTpToCorpse = false;
    std::string prefix = "!";
    std::string cmdCorpse = "corpse";
    std::string cmdTpCorpse = "tpcorpse";

    void Load(const char* folderName)
    {
        const char* configPath = g_api->CaminhoConfig(folderName);
        if (!configPath) return;
        ConanUtils::JsonValue json;
        if (!ConanUtils::JsonParser::ParseFile(configPath, json)) return;
        if (json.has("Enabled")) enabled = json["Enabled"].asBool(true);
        if (json.has("AllowTpToCorpse")) allowTpToCorpse = json["AllowTpToCorpse"].asBool(false);
        if (json.has("Prefix")) prefix = json["Prefix"].asString("!");
        if (json.has("CommandCorpse")) cmdCorpse = json["CommandCorpse"].asString("corpse");
        if (json.has("CommandTpCorpse")) cmdTpCorpse = json["CommandTpCorpse"].asString("tpcorpse");
    }
};

static CorpseConfig g_cfg;
static std::map<int64_t, DeathRecord> g_lastDeaths;
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

static std::string CalculateMapGrid(double x, double y)
{
    const double minX = -320000.0, maxX = 380000.0;
    const double minY = -320000.0, maxY = 380000.0;
    int col = (int)(((x - minX) / (maxX - minX)) * 14.0);
    int row = (int)(((y - minY) / (maxY - minY)) * 14.0);
    col = (std::max)(0, (std::min)(13, col));
    row = (std::max)(0, (std::min)(13, row));
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%c%d", (char)('A' + col), row + 1);
    return buf;
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

static bool GetCharPosition(void* pc, FVector& outPos, void** outChar = nullptr)
{
    void* ch = PlayerPawn(pc);
    if (!ch) return false;
    if (outChar) *outChar = ch;
    outPos = ConanApi::Call<FVector>(ch, "K2_GetActorLocation");
    return g_api->UltimaChamadaExecutou() != 0;
}

static bool TeleportActor(void* actor, FVector dest)
{
    if (!actor) return false;
    FRotator rot{ 0.0, 0.0, 0.0 };
    if (ConanApi::Call<bool>(actor, "K2_TeleportTo", dest, rot) && g_api->UltimaChamadaExecutou())
        return true;
    return ConanApi::Call<bool>(actor, "TeleportTo", dest, rot) && g_api->UltimaChamadaExecutou();
}

static bool TryDeathFromWorld(void* pc, FVector& outPos)
{
    void* comps[8];
    int n = g_api->FindObjects("BP_ConanWorldComposition_C", comps, 8, 1);
    if (n <= 0) n = g_api->FindObjects("ConanWorldComposition", comps, 8, 1);
    for (int i = 0; i < n; ++i)
    {
        if (!comps[i]) continue;
        outPos = ConanApi::Call<FVector>(comps[i], "TryGetPlayerDeathPosition", pc);
        if (g_api->UltimaChamadaExecutou() && (outPos.X != 0.0 || outPos.Y != 0.0 || outPos.Z != 0.0))
            return true;
    }
    return false;
}

static int64_t CharUid(void* deadChar)
{
    int64_t uid = 0;
    int32_t off = g_api->OffsetDoMembro(deadChar, "CharacterUniqueID");
    if (off < 0) off = g_api->OffsetDoMembro(deadChar, "PlayerId");
    if (off >= 0) g_api->LerMembro(deadChar, uint32_t(off), &uid, sizeof(uid));
    return uid;
}

extern "C" ConanAcao OnCharacterDeath(ConanChamada* c)
{
    if (!g_cfg.enabled || !c || !c->Obj) return CONAN_CONTINUAR;
    void* deadChar = c->Obj;
    if (!g_api->DescendeDe(deadChar, "ConanCharacter")) return CONAN_CONTINUAR;

    const int64_t uid = CharUid(deadChar);
    if (uid == 0) return CONAN_CONTINUAR;

    FVector pos = ConanApi::Call<FVector>(deadChar, "K2_GetActorLocation");
    if (!g_api->UltimaChamadaExecutou()) return CONAN_CONTINUAR;

    DeathRecord rec;
    rec.location = pos;
    rec.timestamp = GetTickCount();
    rec.grid = CalculateMapGrid(pos.X, pos.Y);
    g_lastDeaths[uid] = rec;
    g_api->Log("[CorpseLocator] Death UID %lld at [%.0f,%.0f,%.0f] grid %s",
               (long long)uid, pos.X, pos.Y, pos.Z, rec.grid.c_str());
    return CONAN_CONTINUAR;
}

static bool ResolveDeath(void* pc, int64_t uid, DeathRecord& out)
{
    auto it = g_lastDeaths.find(uid);
    if (it != g_lastDeaths.end()) { out = it->second; return true; }

    FVector pos;
    if (TryDeathFromWorld(pc, pos))
    {
        out.location = pos;
        out.timestamp = GetTickCount();
        out.grid = CalculateMapGrid(pos.X, pos.Y);
        g_lastDeaths[uid] = out;
        return true;
    }
    return false;
}

static void SelfVerify(void*)
{
    if (g_verified || !g_cfg.enabled) return;
    const bool ok = (g_hookChat != 0) && (g_hookDeath != 0);
    g_api->Log("[CorpseLocator] VERIFY hookChat=%u hookDeath=%u deaths=%zu => %s",
               (unsigned)g_hookChat, (unsigned)g_hookDeath, g_lastDeaths.size(),
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
    const std::string pCorpse = g_cfg.prefix + g_cfg.cmdCorpse;
    const std::string pTp = g_cfg.prefix + g_cfg.cmdTpCorpse;
    const bool isCorpse = msg.rfind(pCorpse, 0) == 0;
    const bool isTp = msg.rfind(pTp, 0) == 0;
    if (!isCorpse && !isTp) return CONAN_CONTINUAR;

    int64_t uid = 0;
    const uint8_t* pUid = static_cast<const uint8_t*>(chat) + CHAT_UID;
    if (g_api->Legivel(pUid, 8)) uid = *reinterpret_cast<const int64_t*>(pUid);
    void* pc = c->Obj;

    DeathRecord rec;
    if (!ResolveDeath(pc, uid, rec))
    {
        SendReply(pc, "No recent death recorded.");
        return CONAN_CANCELAR;
    }

    if (isCorpse)
    {
        FVector cur;
        double distM = 0.0;
        if (GetCharPosition(pc, cur))
        {
            const double dx = cur.X - rec.location.X;
            const double dy = cur.Y - rec.location.Y;
            const double dz = cur.Z - rec.location.Z;
            distM = std::sqrt(dx * dx + dy * dy + dz * dz) / 100.0;
        }
        char reply[256];
        std::snprintf(reply, sizeof(reply),
                      "Last death: Grid [%s]  •  Distance: %.0f m  •  Coord: [%.0f, %.0f, %.0f]",
                      rec.grid.c_str(), distM, rec.location.X, rec.location.Y, rec.location.Z);
        SendReply(pc, reply);
        return CONAN_CANCELAR;
    }

    if (!g_cfg.allowTpToCorpse)
    {
        SendReply(pc, "Teleport to corpse location is disabled on this server.");
        return CONAN_CANCELAR;
    }

    void* ch = nullptr;
    FVector cur;
    if (!GetCharPosition(pc, cur, &ch))
    {
        SendReply(pc, "Teleport failed.");
        return CONAN_CANCELAR;
    }
    FVector dest = rec.location;
    dest.Z += 20.0;
    SendReply(pc, TeleportActor(ch, dest) ? "Teleported to your corpse location!" : "Teleport failed.");
    return CONAN_CANCELAR;
}

extern "C" __declspec(dllexport)
void ConanPluginCarregar(const ConanApiTabela* api)
{
    if (!api || api->tamanho < sizeof(ConanApiTabela)) return;
    g_api = api;
    ConanApi::UsarTabela(api);
    g_cfg.Load("CorpseLocator");
    g_verified = false;

    if (!g_cfg.enabled)
    {
        g_api->Log("[CorpseLocator] Plugin disabled in config.json (Enabled=false). Load skipped.");
        return;
    }

    g_api->Log("======================================================");
    g_api->Log(" CorpseLocator v2.0.0 — OnDeath + TryGetPlayerDeathPosition");
    g_api->Log(" Commands: !%s | !%s (TP: %s)",
               g_cfg.cmdCorpse.c_str(), g_cfg.cmdTpCorpse.c_str(),
               g_cfg.allowTpToCorpse ? "Yes" : "No");
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
        g_api->Log("[CorpseLocator] Unloaded.");
    }
    g_api = nullptr;
}
