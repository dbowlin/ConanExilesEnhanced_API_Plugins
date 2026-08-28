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
static int g_lastCopied = 0;

struct WardrobeConfig
{
    bool enabled = true;
    double maxRangeMeters = 25.0;
    std::string prefix = "!";
    std::string cmdUniform = "uniform";

    void Load(const char* folderName)
    {
        const char* configPath = g_api->CaminhoConfig(folderName);
        if (!configPath) return;
        ConanUtils::JsonValue json;
        if (!ConanUtils::JsonParser::ParseFile(configPath, json)) return;
        if (json.has("Enabled")) enabled = json["Enabled"].asBool(true);
        if (json.has("MaxRangeMeters")) maxRangeMeters = json["MaxRangeMeters"].asDouble(25.0);
        if (json.has("Prefix")) prefix = json["Prefix"].asString("!");
        if (json.has("CommandUniform")) cmdUniform = json["CommandUniform"].asString("uniform");
    }
};

static WardrobeConfig g_cfg;
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

static void CacheNpcClasses()
{
    if (g_npcClassCount > 0) return;
    void* cls = g_api->FindClass("BaseNPCChar");
    if (cls) g_npcClasses[g_npcClassCount++] = cls;
    cls = g_api->FindClass("ConanCharacter");
    if (cls && g_npcClassCount < 2) g_npcClasses[g_npcClassCount++] = cls;
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

static void* EquipmentInv(void* character)
{
    if (!character) return nullptr;
    void* inv = nullptr;
    ConanApi::CallSaida<void>(character, "GetEquipmentInventory", ConanApi::ParaFora(inv));
    if (g_api->UltimaChamadaExecutou() && inv) return inv;
    inv = ConanApi::Call<void*>(character, "GetEquipmentInventoryNative");
    if (g_api->UltimaChamadaExecutou() && inv) return inv;
    int32_t off = g_api->OffsetDoMembro(character, "EquipmentInventory");
    if (off >= 0) g_api->LerMembro(character, uint32_t(off), &inv, sizeof(inv));
    if (!inv)
    {
        off = g_api->OffsetDoMembro(character, "m_EquipmentInventory");
        if (off >= 0) g_api->LerMembro(character, uint32_t(off), &inv, sizeof(inv));
    }
    return inv;
}

static int CopyEquipment(void* srcEq, void* dstEq)
{
    if (!srcEq || !dstEq || srcEq == dstEq) return 0;
    const int32_t count = ConanApi::Call<int32_t>(srcEq, "GetItemCount");
    const int n = g_api->UltimaChamadaExecutou() ? count : 0;
    int moved = 0;
    for (int i = 0; i < n; ++i)
    {
        void* item = ConanApi::Call<void*>(srcEq, "GetItemAt", int32_t(i));
        if (!g_api->UltimaChamadaExecutou() || !item) continue;
        const int32_t tid = ConanApi::Call<int32_t>(item, "GetTemplateId");
        if (!g_api->UltimaChamadaExecutou() || tid <= 0) continue;

        // Duplicate onto target by moving a copy of the template (spawn on thrall then move)
        // Prefer MoveItem of a spawned stack when available; otherwise MoveItemsByTemplateId
        // from a temporary backpack is not available — spawn via owner character is handled below.
        const bool ok = ConanApi::Call<bool>(
            srcEq, "MoveItem",
            int32_t(i), dstEq, int32_t(-1), int32_t(1), bool(true));
        if (g_api->UltimaChamadaExecutou() && ok)
        {
            ++moved;
            continue;
        }
        const int32_t nMoved = ConanApi::Call<int32_t>(
            srcEq, "MoveItemsByTemplateId",
            int32_t(tid), int32_t(1), bool(false),
            dstEq, bool(true));
        if (g_api->UltimaChamadaExecutou() && nMoved > 0) ++moved;
    }
    return moved;
}

static int ApplyUniform(void* pc, int64_t uid)
{
    void* player = PlayerPawn(pc);
    if (!player) return 0;
    FVector playerPos;
    if (!GetActorPosition(player, playerPos)) return 0;
    void* playerEq = EquipmentInv(player);
    if (!playerEq)
    {
        SendReply(pc, "Player equipment inventory not found.");
        return 0;
    }

    CacheNpcClasses();
    if (g_npcClassCount == 0) return 0;

    const double maxCm = g_cfg.maxRangeMeters * 100.0;
    int applied = 0;

    ScanUtil::ForEachOfClasses(g_api, g_npcClasses, g_npcClassCount, [&](void* npc) {
        if (!npc || npc == player || IsDefaultObject(npc)) return;

        int64_t ownerId = 0;
        int32_t off = g_api->OffsetDoMembro(npc, "OwningPlayerId");
        if (off < 0) off = g_api->OffsetDoMembro(npc, "OwnerPlayerId");
        if (off >= 0) g_api->LerMembro(npc, uint32_t(off), &ownerId, sizeof(ownerId));
        if (ownerId == 0 || (uid != 0 && ownerId != uid)) return;

        FVector pos;
        if (!GetActorPosition(npc, pos)) return;
        const double dx = playerPos.X - pos.X;
        const double dy = playerPos.Y - pos.Y;
        const double dz = playerPos.Z - pos.Z;
        if (std::sqrt(dx * dx + dy * dy + dz * dz) > maxCm) return;

        void* eq = EquipmentInv(npc);
        if (!eq || IsDefaultObject(eq)) return;

        // Prefer spawning matching templates onto thrall (non-destructive copy of worn look)
        const int32_t count = ConanApi::Call<int32_t>(playerEq, "GetItemCount");
        const int n = g_api->UltimaChamadaExecutou() ? count : 0;
        int local = 0;
        for (int s = 0; s < n; ++s)
        {
            void* item = ConanApi::Call<void*>(playerEq, "GetItemAt", int32_t(s));
            if (!g_api->UltimaChamadaExecutou() || !item) continue;
            const int32_t tid = ConanApi::Call<int32_t>(item, "GetTemplateId");
            if (!g_api->UltimaChamadaExecutou() || tid <= 0) continue;
            const bool ok = ConanApi::Call<bool>(
                npc, "SpawnTemplateItem",
                int32_t(tid), ConanApi::Nome("uniform"),
                int32_t(1), float(1.0f), float(0.0f), bool(false));
            if (g_api->UltimaChamadaExecutou() && ok) ++local;
        }
        if (local == 0)
            local = CopyEquipment(playerEq, eq);
        if (local > 0) ++applied;
    });
    g_lastCopied = applied;
    return applied;
}

static void SelfVerify(void*)
{
    if (g_verified || !g_cfg.enabled) return;
    void* cls = g_api->FindClass("EquipmentInventory");
    const bool ok = (g_hookChat != 0);
    g_api->Log("[ThrallWardrobe] VERIFY hook=%u eqCls=%d last=%d => %s",
               (unsigned)g_hookChat, cls ? 1 : 0, g_lastCopied, ok ? "PASS" : "FAIL");
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
    const std::string pUniform = g_cfg.prefix + g_cfg.cmdUniform;
    if (msg.rfind(pUniform, 0) != 0) return CONAN_CONTINUAR;

    int64_t uid = 0;
    const uint8_t* pUid = static_cast<const uint8_t*>(chat) + CHAT_UID;
    if (g_api->Legivel(pUid, 8)) uid = *reinterpret_cast<const int64_t*>(pUid);

    const int n = ApplyUniform(c->Obj, uid);
    char reply[128];
    std::snprintf(reply, sizeof(reply),
                  n > 0 ? "Uniform applied to %d nearby thrall(s) (SpawnTemplateItem/MoveItem)."
                        : "No owned thralls in range with equipment inventory.",
                  n);
    SendReply(c->Obj, reply);
    return CONAN_CANCELAR;
}

extern "C" __declspec(dllexport)
void ConanPluginCarregar(const ConanApiTabela* api)
{
    if (!api || api->tamanho < sizeof(ConanApiTabela)) return;
    g_api = api;
    ConanApi::UsarTabela(api);
    g_cfg.Load("ThrallWardrobe");
    g_verified = false;

    if (!g_cfg.enabled)
    {
        g_api->Log("[ThrallWardrobe] Plugin disabled in config.json (Enabled=false). Load skipped.");
        return;
    }

    g_api->Log("======================================================");
    g_api->Log(" ThrallWardrobe v2.1.0 — GetEquipmentInventory + SpawnTemplateItem");
    g_api->Log(" Command: !%s | Range: %.0fm", g_cfg.cmdUniform.c_str(), g_cfg.maxRangeMeters);
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
        g_api->Log("[ThrallWardrobe] Unloaded.");
    }
    g_api = nullptr;
}
