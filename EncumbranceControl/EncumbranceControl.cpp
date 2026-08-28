#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "Conan/ConanPluginApi.h"
#include "Conan/ConanBase.h"
#include "../common/JsonConfig.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

static const ConanApiTabela* g_api = nullptr;
static uint32_t g_taskId = 0;
static uint32_t g_hookLogin = 0;
static int g_tick = 0;
static volatile long g_applyPending = 0;

struct EncumbranceConfig
{
    bool enabled = true;
    bool encumbranceEnabled = true;
    double carryCapacityMultiplier = 10.0;
    double overloadPenaltyMultiplier = 0.0;
    char adminPassword[64] = "candie";

    void Load(const char* folderName)
    {
        const char* configPath = g_api->CaminhoConfig(folderName);
        if (!configPath) return;
        ConanUtils::JsonValue json;
        if (!ConanUtils::JsonParser::ParseFile(configPath, json)) return;
        if (json.has("Enabled")) enabled = json["Enabled"].asBool(true);
        if (json.has("EncumbranceEnabled")) encumbranceEnabled = json["EncumbranceEnabled"].asBool(true);
        if (json.has("CarryCapacityMultiplier")) carryCapacityMultiplier = json["CarryCapacityMultiplier"].asDouble(10.0);
        if (json.has("OverloadPenaltyMultiplier")) overloadPenaltyMultiplier = json["OverloadPenaltyMultiplier"].asDouble(0.0);
        if (json.has("AdminPassword"))
        {
            const std::string s = json["AdminPassword"].asString("candie");
            std::snprintf(adminPassword, sizeof(adminPassword), "%s", s.c_str());
        }
    }
};

struct ApplyResult
{
    bool applied = false;
    bool encSysOk = false;
    double readMax = 0.0;
};

static EncumbranceConfig g_cfg;
static const int kMaxPlayers = 32;
static const double kVanillaBaseCarry = 70.0;
static const double kUnlimitedCarry = 999999.0;

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

static bool IsDefaultObject(void* obj)
{
    if (!obj) return true;
    char name[256] = {0};
    if (!g_api->NomeDoObjeto(obj, name, sizeof(name))) return false;
    return std::strncmp(name, "Default__", 9) == 0;
}

static void WriteFloat(void* obj, const char* name, float v)
{
    const int32_t off = g_api->OffsetDoMembro(obj, name);
    if (off < 0) return;
    g_api->EscreverMembro(obj, uint32_t(off), &v, sizeof(v));
}

static void WriteDouble(void* obj, const char* name, double v)
{
    const int32_t off = g_api->OffsetDoMembro(obj, name);
    if (off < 0) return;
    g_api->EscreverMembro(obj, uint32_t(off), &v, sizeof(v));
}

static void EnsureAdmin(void* pc)
{
    if (ConanApi::Call<bool>(pc, "IsAdmin") && g_api->UltimaChamadaExecutou())
        return;
    ConanApi::Texto pw(g_cfg.adminPassword);
    if (!pw.valido) return;
    ConanApi::Call<void>(pc, "MakeMeAdmin", pw);
}

static void* EncumbranceOf(void* ch)
{
    void* enc = PtrMember(ch, "BP_EncumbranceSystem");
    if (!enc) enc = PtrMember(ch, "EncumbranceSystem");
    if (!enc) enc = PtrMember(ch, "Encumbrance System");
    return enc;
}

static double TargetCarryCapacity()
{
    if (!g_cfg.encumbranceEnabled) return kUnlimitedCarry;
    return kVanillaBaseCarry * (std::max)(1.0, g_cfg.carryCapacityMultiplier);
}

static void SetPenaltyMultiplier(void* pc, float value)
{
    ConanApi::Texto key("PlayerEncumbrancePenaltyMultiplier");
    if (!key.valido) return;
    ConanApi::Call<void>(pc, "SetServerSetting", key, value, true);
}

static int ExpertisePoints()
{
    if (!g_cfg.encumbranceEnabled) return 0;
    const double target = TargetCarryCapacity();
    const int pts = (int)((target - kVanillaBaseCarry) / 15.0 + 0.5);
    return (std::min)(50, (std::max)(0, pts));
}

static double ReadEncumbranceMax(void* enc)
{
    if (!enc) return 0.0;
    const double maxVal = ConanApi::Call<double>(enc, "GetEncumbranceMax");
    if (g_api->UltimaChamadaExecutou()) return maxVal;
    return 0.0;
}

static void WriteCarryBase(void* enc, double targetBase)
{
    WriteDouble(enc, "EncumbranceBase", targetBase);
    ConanApi::Call<void>(enc, "OnRep_EncumbranceBase");
    ConanApi::Call<void>(enc, "UpdateEncumbrance");
    ConanApi::Call<void>(enc, "HandleCharacterEncumbrance");
}

static ApplyResult ApplyCarry(void* pc, void* ch, double targetBase, float penalty)
{
    ApplyResult result;
    EnsureAdmin(pc);

    void* cheat = PtrMember(pc, "CheatManager");
    void* attr = PtrMember(ch, "AttributeSystem");
    void* enc = EncumbranceOf(ch);

    if (attr) WriteFloat(attr, "BonusEncumbrancePerAttribute", 15.0f);

    if (enc)
    {
        result.encSysOk = true;
        WriteCarryBase(enc, targetBase);
        result.readMax = ReadEncumbranceMax(enc);
    }

    const int pts = ExpertisePoints();
    if (cheat && pts > 0)
    {
        ConanApi::Texto n("AttributeEncumbrance");
        if (n.valido)
            ConanApi::Call<void>(cheat, "SetStat", n, float(pts));
    }
    if (attr && pts > 0)
    {
        ConanApi::Call<void>(attr, "OnAttributeEncumbranceChanged", int32_t(pts));
        ConanApi::Call<void>(attr, "OnAttributeChanged", ch, uint8_t(16), int32_t(pts), int32_t(pts));
        ConanApi::Call<void>(attr, "UpdateAttributes");
    }

    SetPenaltyMultiplier(pc, penalty);
    result.applied = true;
    return result;
}

static ApplyResult ApplyToPlayer(void* pc)
{
    ApplyResult empty;
    void* ch = PtrMember(pc, "Character");
    if (!ch) ch = PtrMember(pc, "Pawn");
    if (!ch || IsDefaultObject(ch)) return empty;

    const double target = TargetCarryCapacity();
    const float penalty = g_cfg.encumbranceEnabled
        ? ((g_cfg.overloadPenaltyMultiplier <= 0.0) ? 0.0f : float(g_cfg.overloadPenaltyMultiplier))
        : 0.0f;
    return ApplyCarry(pc, ch, target, penalty);
}

static int ApplyAllOnline(double& outReadMax, bool& outEncSysOk)
{
    void* pcs[kMaxPlayers];
    const int n = g_api->FindObjects("ConanPlayerController", pcs, kMaxPlayers, 1);
    int applied = 0;
    outReadMax = 0.0;
    outEncSysOk = false;

    for (int i = 0; i < n; ++i)
    {
        if (IsDefaultObject(pcs[i])) continue;
        const ApplyResult r = ApplyToPlayer(pcs[i]);
        if (!r.applied) continue;
        ++applied;
        if (r.encSysOk) outEncSysOk = true;
        if (r.readMax > outReadMax) outReadMax = r.readMax;
    }
    return applied;
}

static void Tick(void*)
{
    ++g_tick;
    g_cfg.Load("EncumbranceControl");
    if (!g_cfg.enabled) return;

    if (InterlockedExchange(&g_applyPending, 0))
        g_api->Log("[EncumbranceControl] PostLogin apply triggered");

    double readMax = 0.0;
    bool encSysOk = false;
    const int applied = ApplyAllOnline(readMax, encSysOk);

    if ((g_tick % 15) == 1)
    {
        g_api->Log("[EncumbranceControl] tick=%d applied=%d enc=%s enabled=%d wantCarry=%.0f readMax=%.0f",
                   g_tick, applied, encSysOk ? "ok" : "null",
                   g_cfg.encumbranceEnabled ? 1 : 0, TargetCarryCapacity(), readMax);
    }
}

extern "C" ConanAcao EncumbranceControl_OnLogin(ConanChamada* c)
{
    (void)c;
    InterlockedExchange(&g_applyPending, 1);
    return CONAN_CONTINUAR;
}

extern "C" __declspec(dllexport)
void ConanPluginCarregar(const ConanApiTabela* api)
{
    if (!api || api->tamanho < sizeof(ConanApiTabela)) return;
    g_api = api;
    ConanApi::UsarTabela(api);
    g_cfg.Load("EncumbranceControl");
    g_tick = 0;
    g_applyPending = 0;

    g_api->Log("======================================================");
    g_api->Log(" EncumbranceControl v2.3.0 — FindObjects + PostLogin");
    g_api->Log(" ON: EncumbranceBase = 70 x multiplier | OFF: unlimited carry");
    g_api->Log("======================================================");

    g_hookLogin = g_api->HookProcessEvent("K2_PostLogin", EncumbranceControl_OnLogin, nullptr, 50);
    g_taskId = g_api->AgendarNaThreadDoJogo(Tick, 2, nullptr, 1);
}

extern "C" __declspec(dllexport)
void ConanPluginDescarregar(void)
{
    if (g_api)
    {
        if (g_hookLogin) g_api->RemoverHook(g_hookLogin);
        if (g_taskId) g_api->CancelarAgendamento(g_taskId);
    }
    g_api = nullptr;
}
