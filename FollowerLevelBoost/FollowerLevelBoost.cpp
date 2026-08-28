#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "Conan/ConanPluginApi.h"
#include "Conan/ConanBase.h"
#include "../common/JsonConfig.h"
#include "../common/ObjectScan.h"

#include <string>
#include <cstdio>
#include <cstring>

static const ConanApiTabela* g_api = nullptr;
static uint32_t g_hookXp = 0;
static uint32_t g_hookBoss = 0;
static uint32_t g_verifyId = 0;
static bool g_verified = false;
static int g_boosted = 0;
static int g_bossBonus = 0;

struct LevelBoostConfig
{
    bool enabled = true;
    double followerXpMultiplier = 3.0;
    int bossKillBonusXp = 5000;

    void Load(const char* folderName)
    {
        const char* configPath = g_api->CaminhoConfig(folderName);
        if (!configPath) return;
        ConanUtils::JsonValue json;
        if (!ConanUtils::JsonParser::ParseFile(configPath, json)) return;
        if (json.has("Enabled")) enabled = json["Enabled"].asBool(true);
        if (json.has("FollowerXpMultiplier")) followerXpMultiplier = json["FollowerXpMultiplier"].asDouble(3.0);
        if (json.has("BossKillBonusXp")) bossKillBonusXp = json["BossKillBonusXp"].asInt(5000);
    }
};

static LevelBoostConfig g_cfg;
static void* g_npcClasses[2];
static int g_npcClassCount = 0;

static void CacheNpcClasses()
{
    if (g_npcClassCount > 0) return;
    void* cls = g_api->FindClass("BaseNPCChar");
    if (cls) g_npcClasses[g_npcClassCount++] = cls;
    cls = g_api->FindClass("PersistentHumanoidNPC_Companion_C");
    if (cls && g_npcClassCount < 2) g_npcClasses[g_npcClassCount++] = cls;
}

static bool IsDefaultObject(void* obj)
{
    if (!obj) return true;
    char name[256] = {0};
    if (!g_api->NomeDoObjeto(obj, name, sizeof(name))) return false;
    return std::strncmp(name, "Default__", 9) == 0;
}

static int64_t ReadOwnerId(void* npc)
{
    int64_t ownerId = 0;
    int32_t off = g_api->OffsetDoMembro(npc, "OwningPlayerId");
    if (off < 0) off = g_api->OffsetDoMembro(npc, "OwnerPlayerId");
    if (off >= 0) g_api->LerMembro(npc, uint32_t(off), &ownerId, sizeof(ownerId));
    return ownerId;
}

static void* OwnerCharacterOfProgression(void* prog)
{
    if (!prog) return nullptr;
    void* owner = nullptr;
    int32_t off = g_api->OffsetDoMembro(prog, "ownerCharacter");
    if (off < 0) off = g_api->OffsetDoMembro(prog, "OwnerCharacter");
    if (off >= 0) g_api->LerMembro(prog, uint32_t(off), &owner, sizeof(owner));
    return owner;
}

static void* ProgressionOf(void* npc)
{
    if (!npc) return nullptr;
    void* prog = nullptr;
    int32_t off = g_api->OffsetDoMembro(npc, "NPCProgressionComponent");
    if (off < 0) off = g_api->OffsetDoMembro(npc, "NPCProgressionSystem");
    if (off >= 0) g_api->LerMembro(npc, uint32_t(off), &prog, sizeof(prog));
    if (prog && !IsDefaultObject(prog)) return prog;

    // Component may already be the progression system
    if (g_api->DescendeDe(npc, "NPCProgressionSystem") ||
        g_api->DescendeDe(npc, "BP_NPCProgressionSystem_C"))
        return npc;
    return nullptr;
}

static bool IsOwnedFollower(void* target)
{
    if (!target || IsDefaultObject(target)) return false;
    void* ch = target;
    if (g_api->DescendeDe(target, "NPCProgressionSystem") ||
        g_api->DescendeDe(target, "BP_NPCProgressionSystem_C"))
    {
        ch = OwnerCharacterOfProgression(target);
        if (!ch) return false;
    }
    if (!(g_api->DescendeDe(ch, "BaseNPCChar") ||
          g_api->DescendeDe(ch, "ConanCharacter") ||
          g_api->DescendeDe(ch, "PersistentHumanoidNPC_Companion_C")))
        return false;
    return ReadOwnerId(ch) != 0;
}

// SDK: NPCProgressionSystem::GiveExperiencePoints(int32 Num)
extern "C" ConanAcao OnGiveExperiencePoints(ConanChamada* c)
{
    if (!g_cfg.enabled || !c || !c->Parms || !c->Obj) return CONAN_CONTINUAR;
    if (c->ParmsSize < sizeof(int32_t)) return CONAN_CONTINUAR;
    if (!IsOwnedFollower(c->Obj)) return CONAN_CONTINUAR;

    int32_t* pXp = reinterpret_cast<int32_t*>(c->Parms);
    if (pXp && *pXp > 0)
    {
        const double scaled = static_cast<double>(*pXp) * g_cfg.followerXpMultiplier;
        *pXp = static_cast<int32_t>(scaled);
        if (*pXp < 1) *pXp = 1;
        ++g_boosted;
    }
    return CONAN_CONTINUAR;
}

// Fallback: SlotNPCExperienceReceived(pet, experience, nextLevelExperience)
extern "C" ConanAcao OnSlotNpcExperience(ConanChamada* c)
{
    if (!g_cfg.enabled || !c || !c->Parms || c->ParmsSize < 16) return CONAN_CONTINUAR;
    const uint8_t* p = static_cast<const uint8_t*>(c->Parms);
    void* pet = nullptr;
    std::memcpy(&pet, p, sizeof(void*));
    if (!pet || !IsOwnedFollower(pet)) return CONAN_CONTINUAR;

    int32_t* pXp = reinterpret_cast<int32_t*>(const_cast<uint8_t*>(p + 8));
    if (pXp && *pXp > 0)
    {
        const double scaled = static_cast<double>(*pXp) * g_cfg.followerXpMultiplier;
        *pXp = static_cast<int32_t>(scaled);
        if (*pXp < 1) *pXp = 1;
        ++g_boosted;
    }
    return CONAN_CONTINUAR;
}

static void GrantBossBonusToFollowers(void* killerHint)
{
    if (g_cfg.bossKillBonusXp <= 0) return;

    int64_t ownerFilter = 0;
    if (killerHint && !IsDefaultObject(killerHint))
    {
        ownerFilter = ReadOwnerId(killerHint);
        if (ownerFilter == 0 && g_api->DescendeDe(killerHint, "ConanCharacter"))
        {
            int32_t off = g_api->OffsetDoMembro(killerHint, "PlayerId");
            if (off < 0) off = g_api->OffsetDoMembro(killerHint, "UniqueID");
            if (off >= 0) g_api->LerMembro(killerHint, uint32_t(off), &ownerFilter, sizeof(ownerFilter));
        }
    }

    CacheNpcClasses();
    if (g_npcClassCount == 0) return;

    ScanUtil::ForEachOfClasses(g_api, g_npcClasses, g_npcClassCount, [&](void* npc) {
        if (IsDefaultObject(npc)) return;
        const int64_t oid = ReadOwnerId(npc);
        if (oid == 0) return;
        if (ownerFilter != 0 && oid != ownerFilter) return;

        void* prog = ProgressionOf(npc);
        if (prog)
        {
            ConanApi::Call<void>(prog, "GiveExperiencePoints", int32_t(g_cfg.bossKillBonusXp));
            if (g_api->UltimaChamadaExecutou()) { ++g_bossBonus; return; }
        }
        ConanApi::Call<void>(npc, "GiveExperiencePoints", int32_t(g_cfg.bossKillBonusXp));
        if (g_api->UltimaChamadaExecutou()) ++g_bossBonus;
    });
}

// SDK: BossKilled(Character, killer) on event BPs; OnBossDefeated on boss controllers
extern "C" ConanAcao OnBossKilled(ConanChamada* c)
{
    if (!g_cfg.enabled || !c) return CONAN_CONTINUAR;

    void* killer = nullptr;
    if (c->Parms && c->ParmsSize >= 16)
    {
        const uint8_t* p = static_cast<const uint8_t*>(c->Parms);
        std::memcpy(&killer, p + 8, sizeof(void*));
    }
    GrantBossBonusToFollowers(killer);
    return CONAN_CONTINUAR;
}

extern "C" ConanAcao OnBossDefeated(ConanChamada* c)
{
    if (!g_cfg.enabled || !c) return CONAN_CONTINUAR;
    GrantBossBonusToFollowers(nullptr);
    return CONAN_CONTINUAR;
}

static uint32_t TryHook(const char* name, ConanFnAntes fn)
{
    if (!name || !fn) return 0;
    const uint32_t id = g_api->HookProcessEvent(name, fn, nullptr, 100);
    if (id) g_api->Log("[FollowerLevelBoost] hooked %s id=%u", name, (unsigned)id);
    return id;
}

static void SelfVerify(void*)
{
    if (g_verified || !g_cfg.enabled) return;
    // PASS when at least one hook registered.
    const bool ok = (g_hookXp != 0) || (g_hookBoss != 0);
    g_api->Log("[FollowerLevelBoost] VERIFY hookXp=%u hookBoss=%u boosted=%d bossBonus=%d => %s",
               (unsigned)g_hookXp, (unsigned)g_hookBoss, g_boosted, g_bossBonus,
               ok ? "PASS" : "FAIL");
    if (ok)
    {
        g_verified = true;
        if (g_verifyId) { g_api->CancelarAgendamento(g_verifyId); g_verifyId = 0; }
    }
}

extern "C" __declspec(dllexport)
void ConanPluginCarregar(const ConanApiTabela* api)
{
    if (!api || api->tamanho < sizeof(ConanApiTabela)) return;
    g_api = api;
    ConanApi::UsarTabela(api);
    g_cfg.Load("FollowerLevelBoost");
    g_verified = false;

    if (!g_cfg.enabled)
    {
        g_api->Log("[FollowerLevelBoost] Plugin disabled in config.json (Enabled=false). Load skipped.");
        return;
    }

    g_api->Log("======================================================");
    g_api->Log(" FollowerLevelBoost v2.2.0 — GiveExperiencePoints + OnBossDefeated");
    g_api->Log(" Follower XP Multiplier: %.1fx | Boss Bonus: +%d XP",
               g_cfg.followerXpMultiplier, g_cfg.bossKillBonusXp);
    g_api->Log("======================================================");

    // Real SDK name is GiveExperiencePoints on NPCProgressionSystem (AddExperience does not exist).
    g_hookXp = TryHook("GiveExperiencePoints", OnGiveExperiencePoints);
    if (!g_hookXp) g_hookXp = TryHook("SlotNPCExperienceReceived", OnSlotNpcExperience);

    g_hookBoss = TryHook("OnBossDefeated", OnBossDefeated);
    if (!g_hookBoss) g_hookBoss = TryHook("BossKilled", OnBossKilled);
    if (!g_hookBoss) g_hookBoss = TryHook("OnBossDefeated__DelegateSignature", OnBossDefeated);

    g_verifyId = g_api->AgendarNaThreadDoJogo(SelfVerify, 3, nullptr, 1);
}

extern "C" __declspec(dllexport)
void ConanPluginDescarregar(void)
{
    if (g_api)
    {
        if (g_hookXp) g_api->RemoverHook(g_hookXp);
        if (g_hookBoss) g_api->RemoverHook(g_hookBoss);
        if (g_verifyId) g_api->CancelarAgendamento(g_verifyId);
        g_api->Log("[FollowerLevelBoost] Unloaded.");
    }
    g_api = nullptr;
}
