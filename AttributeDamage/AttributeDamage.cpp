#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "Conan/ConanPluginApi.h"
#include "Conan/ConanBase.h"
#include "../common/JsonConfig.h"

#include <cstdio>
#include <cstring>

static const ConanApiTabela* g_api = nullptr;
static uint32_t g_hookAny = 0;
static uint32_t g_hookPoint = 0;
static uint32_t g_hookTake = 0;
static uint32_t g_verifyId = 0;
static bool g_verified = false;
static int g_scaledHits = 0;

struct AttrDmgConfig
{
    bool enabled = true;
    double strengthPctPerPoint = 5.0;
    double agilityPctPerPoint = 5.0;
    double offStatPctPerPoint = 0.5;
    double agilityWeaponMult = 2.5;
    bool includePerformer = true;

    void Load(const char* folderName)
    {
        const char* configPath = g_api->CaminhoConfig(folderName);
        if (!configPath) return;
        ConanUtils::JsonValue json;
        if (!ConanUtils::JsonParser::ParseFile(configPath, json)) return;
        if (json.has("Enabled")) enabled = json["Enabled"].asBool(true);
        if (json.has("StrengthDamagePercentPerPoint"))
            strengthPctPerPoint = json["StrengthDamagePercentPerPoint"].asDouble(5.0);
        if (json.has("AgilityDamagePercentPerPoint"))
            agilityPctPerPoint = json["AgilityDamagePercentPerPoint"].asDouble(5.0);
        if (json.has("OffStatDamagePercentPerPoint"))
            offStatPctPerPoint = json["OffStatDamagePercentPerPoint"].asDouble(0.5);
        if (json.has("AgilityWeaponDamageMultiplier"))
            agilityWeaponMult = json["AgilityWeaponDamageMultiplier"].asDouble(2.5);
        if (json.has("IncludePerformerWeapons"))
            includePerformer = json["IncludePerformerWeapons"].asBool(true);
    }
};

static AttrDmgConfig g_cfg;

static bool IsDefaultObject(void* obj)
{
    if (!obj) return true;
    char name[256] = {0};
    if (!g_api->NomeDoObjeto(obj, name, sizeof(name))) return false;
    return std::strncmp(name, "Default__", 9) == 0;
}

static void* PtrMember(void* obj, const char* name)
{
    if (!obj) return nullptr;
    const int32_t off = g_api->OffsetDoMembro(obj, name);
    if (off < 0) return nullptr;
    void* p = nullptr;
    if (g_api->LerMembro(obj, uint32_t(off), &p, sizeof(p)) <= 0) return nullptr;
    if (!p || !g_api->Legivel(p, 8)) return nullptr;
    return p;
}

static void* ResolveCharacter(void* actorOrCtrl)
{
    if (!actorOrCtrl || IsDefaultObject(actorOrCtrl)) return nullptr;
    if (g_api->DescendeDe(actorOrCtrl, "ConanCharacter") ||
        g_api->DescendeDe(actorOrCtrl, "BaseNPCChar"))
        return actorOrCtrl;

    void* ch = PtrMember(actorOrCtrl, "Character");
    if (!ch) ch = PtrMember(actorOrCtrl, "Pawn");
    if (!ch) ch = ConanApi::Call<void*>(actorOrCtrl, "K2_GetPawn");
    if (!ch || IsDefaultObject(ch)) return nullptr;
    return ch;
}

static float ReadAttr(void* character, const char* primary, const char* alt)
{
    if (!character) return 0.0f;
    void* attr = PtrMember(character, "AttributeSystem");
    if (!attr) attr = PtrMember(character, "BP_AttributeSystem");
    if (!attr) return 0.0f;

    // Prefer live Call by name (DLL used GetAttributeValue / GetStatValue)
    float v = ConanApi::Call<float>(attr, "GetAttributeValue", ConanApi::Nome(primary));
    if (g_api->UltimaChamadaExecutou()) return v;
    v = ConanApi::Call<float>(attr, "GetStatValue", ConanApi::Nome(primary));
    if (g_api->UltimaChamadaExecutou()) return v;
    if (alt)
    {
        v = ConanApi::Call<float>(attr, "GetAttributeValue", ConanApi::Nome(alt));
        if (g_api->UltimaChamadaExecutou()) return v;
        v = ConanApi::Call<float>(attr, "GetStatValue", ConanApi::Nome(alt));
        if (g_api->UltimaChamadaExecutou()) return v;
    }

    // Fallback: BonusDamage* are rates; AttributeStats not easily indexed — try member ints
    int32_t iv = 0;
    int32_t off = g_api->OffsetDoMembro(attr, primary);
    if (off < 0 && alt) off = g_api->OffsetDoMembro(attr, alt);
    if (off >= 0 && g_api->LerMembro(attr, uint32_t(off), &iv, sizeof(iv)) > 0)
        return float(iv);
    return 0.0f;
}

static void* CurrentWeapon(void* character)
{
    if (!character) return nullptr;
    const char* names[] = {
        "CurrentWeapon", "EquippedWeapon", "ActiveWeapon",
        "RightHandWeapon", "Weapon", nullptr
    };
    for (int i = 0; names[i]; ++i)
    {
        void* w = PtrMember(character, names[i]);
        if (w && !IsDefaultObject(w)) return w;
    }
    void* w = ConanApi::Call<void*>(character, "GetCurrentWeapon");
    if (g_api->UltimaChamadaExecutou() && w && !IsDefaultObject(w)) return w;
    w = ConanApi::Call<void*>(character, "GetEquippedWeapon");
    if (g_api->UltimaChamadaExecutou() && w && !IsDefaultObject(w)) return w;
    return nullptr;
}

static bool IsAgilityFamilyWeapon(void* weapon)
{
    if (!weapon) return false;
    char name[256] = {0};
    g_api->NomeDoObjeto(weapon, name, sizeof(name));
    // Lowercase-ish compare via simple substrings Funcom uses
    auto has = [&](const char* s) -> bool {
        return std::strstr(name, s) != nullptr;
    };
    if (has("Bow") || has("bow") || has("Crossbow") || has("crossbow")) return true;
    if (has("Dagger") || has("dagger") || has("Throwing") || has("throwing")) return true;
    if (has("Arrow") || has("Javelin") || has("javelin")) return true;
    if (g_cfg.includePerformer && (has("perform") || has("Perform") || has("Dance") || has("dance")))
        return true;
    return false;
}

static void ScaleDamageParm(ConanChamada* c)
{
    if (!g_cfg.enabled || !c || !c->Parms || c->ParmsSize < sizeof(float)) return;

    float* pDmg = reinterpret_cast<float*>(c->Parms);
    if (!pDmg || *pDmg <= 0.0f) return;

    // ReceiveAnyDamage: Damage@0, DamageType@8, InstigatedBy@16, DamageCauser@24
    void* attacker = nullptr;
    if (c->ParmsSize >= 0x20)
    {
        const uint8_t* p = static_cast<const uint8_t*>(c->Parms);
        void* instigatedBy = nullptr;
        void* damageCauser = nullptr;
        std::memcpy(&instigatedBy, p + 0x10, sizeof(void*));
        std::memcpy(&damageCauser, p + 0x18, sizeof(void*));
        void* candidates[] = { instigatedBy, damageCauser };
        for (void* cand : candidates)
        {
            void* ch = ResolveCharacter(cand);
            if (!ch) continue;
            if (g_api->DescendeDe(ch, "ConanCharacter") || g_api->DescendeDe(ch, "BaseNPCChar"))
            {
                attacker = ch;
                break;
            }
        }
    }
    if (!attacker && c->Obj)
    {
        void* last = PtrMember(c->Obj, "LastDamageCauser");
        if (!last) last = PtrMember(c->Obj, "LastAttacker");
        attacker = ResolveCharacter(last);
    }
    if (!attacker) return;

    const float str = ReadAttr(attacker, "AttributeStrength", "Strength");
    const float agi = ReadAttr(attacker, "AttributeAgility", "Agility");
    void* weapon = CurrentWeapon(attacker);
    const bool agiWep = IsAgilityFamilyWeapon(weapon);

    double mult = 1.0;
    if (agiWep)
    {
        mult += (agi * g_cfg.agilityPctPerPoint + str * g_cfg.offStatPctPerPoint) / 100.0;
        mult *= g_cfg.agilityWeaponMult;
    }
    else
    {
        mult += (str * g_cfg.strengthPctPerPoint + agi * g_cfg.offStatPctPerPoint) / 100.0;
    }

    if (mult < 0.01) mult = 0.01;
    *pDmg = float(*pDmg * mult);
    ++g_scaledHits;
}

extern "C" ConanAcao OnReceiveAnyDamage(ConanChamada* c)
{
    ScaleDamageParm(c);
    return CONAN_CONTINUAR;
}

extern "C" ConanAcao OnReceivePointDamage(ConanChamada* c)
{
    ScaleDamageParm(c);
    return CONAN_CONTINUAR;
}

extern "C" ConanAcao OnTakeDamage(ConanChamada* c)
{
    ScaleDamageParm(c);
    return CONAN_CONTINUAR;
}

static void SelfVerify(void*)
{
    if (g_verified || !g_cfg.enabled) return;
    const bool hooksOk = (g_hookAny != 0) || (g_hookPoint != 0) || (g_hookTake != 0);
    void* attrCls = g_api->FindClass("BP_AttributeSystem_C");
    if (!attrCls) attrCls = g_api->FindClass("AttributeSystem");
    const bool ok = hooksOk && (attrCls != nullptr);
    g_api->Log("[AttributeDamage] VERIFY hookAny=%u hookPoint=%u hookTake=%u attrCls=%d scaled=%d => %s",
               (unsigned)g_hookAny, (unsigned)g_hookPoint, (unsigned)g_hookTake,
               attrCls ? 1 : 0, g_scaledHits, ok ? "PASS" : "FAIL");
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
    g_cfg.Load("AttributeDamage");
    g_verified = false;
    g_scaledHits = 0;

    if (!g_cfg.enabled)
    {
        g_api->Log("[AttributeDamage] Plugin disabled in config.json (Enabled=false). Load skipped.");
        return;
    }

    g_api->Log("======================================================");
    g_api->Log(" AttributeDamage v2.0.0 — Strength / Agility Parity");
    g_api->Log(" STR: %.1f%%/pt | AGI: %.1f%%/pt | Off-stat: %.1f%%/pt | AGI family: %.2fx | Performer: %s",
               g_cfg.strengthPctPerPoint, g_cfg.agilityPctPerPoint, g_cfg.offStatPctPerPoint,
               g_cfg.agilityWeaponMult, g_cfg.includePerformer ? "yes" : "no");
    g_api->Log("======================================================");

    g_hookAny = g_api->HookProcessEvent("ReceiveAnyDamage", OnReceiveAnyDamage, nullptr, 50);
    g_hookPoint = g_api->HookProcessEvent("ReceivePointDamage", OnReceivePointDamage, nullptr, 50);
    g_hookTake = g_api->HookProcessEvent("TakeDamage", OnTakeDamage, nullptr, 50);
    g_verifyId = g_api->AgendarNaThreadDoJogo(SelfVerify, 3, nullptr, 1);
}

extern "C" __declspec(dllexport)
void ConanPluginDescarregar(void)
{
    if (g_api)
    {
        if (g_hookAny) g_api->RemoverHook(g_hookAny);
        if (g_hookPoint) g_api->RemoverHook(g_hookPoint);
        if (g_hookTake) g_api->RemoverHook(g_hookTake);
        if (g_verifyId) g_api->CancelarAgendamento(g_verifyId);
        g_api->Log("[AttributeDamage] Unloaded.");
    }
    g_api = nullptr;
}
