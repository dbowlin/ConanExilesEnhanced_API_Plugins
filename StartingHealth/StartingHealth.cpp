#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "Conan/ConanPluginApi.h"
#include "Conan/ConanBase.h"
#include "../common/JsonConfig.h"
#include "../common/ObjectScan.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

static const ConanApiTabela* g_api = nullptr;
static uint32_t g_taskId = 0;
static int g_tick = 0;
static bool g_adminOk = false;

struct HealthConfig
{
    bool enabled = true;
    int baseStartingHealth = 250;
    double healthMultiplier = 4.0;
    char adminPassword[64] = "candie";

    void Load(const char* folderName)
    {
        const char* configPath = g_api->CaminhoConfig(folderName);
        if (!configPath) return;
        ConanUtils::JsonValue json;
        if (!ConanUtils::JsonParser::ParseFile(configPath, json)) return;
        if (json.has("Enabled")) enabled = json["Enabled"].asBool(true);
        if (json.has("BaseStartingHealth")) baseStartingHealth = json["BaseStartingHealth"].asInt(250);
        if (json.has("HealthMultiplier")) healthMultiplier = json["HealthMultiplier"].asDouble(4.0);
        if (json.has("AdminPassword"))
        {
            const std::string s = json["AdminPassword"].asString("candie");
            std::snprintf(adminPassword, sizeof(adminPassword), "%s", s.c_str());
        }
    }
};

static HealthConfig g_cfg;
static void* g_pcClasses[1];
static int g_pcClassCount = 0;
static ScanUtil::IncrementalScan g_pcScan;
static const int kApplyPerTick = 4;

static void CachePcClasses()
{
    if (g_pcClassCount > 0) return;
    void* cls = g_api->FindClass("ConanPlayerController");
    if (cls) g_pcClasses[g_pcClassCount++] = cls;
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

static void EnsureAdmin(void* pc)
{
    if (g_adminOk) return;
    if (ConanApi::Call<bool>(pc, "IsAdmin") && g_api->UltimaChamadaExecutou())
    {
        g_adminOk = true;
        return;
    }
    ConanApi::Texto pw(g_cfg.adminPassword);
    if (!pw.valido) return;
    ConanApi::Call<void>(pc, "MakeMeAdmin", pw);
    if (ConanApi::Call<bool>(pc, "IsAdmin"))
        g_adminOk = true;
}

// Vanilla: HP ≈ 200 * (1 + 0.1 * Vitality). Solve for Vitality given target HP.
static int VitalityPoints()
{
    const double want = (std::max)(1.0, (double)g_cfg.baseStartingHealth * g_cfg.healthMultiplier);
    // want = 200 * (1 + 0.1 * V)  =>  V = (want/200 - 1) / 0.1
    double v = (want / 200.0 - 1.0) / 0.1;
    if (v < 0.0) v = 0.0;
    if (v > 50.0) v = 50.0;
    return (int)(v + 0.5);
}

static void Apply(void* pc)
{
    void* ch = PtrMember(pc, "Character");
    if (!ch) ch = PtrMember(pc, "Pawn");
    if (!ch || IsDefaultObject(ch)) return;

    EnsureAdmin(pc);
    void* cheat = PtrMember(pc, "CheatManager");
    void* attr = PtrMember(ch, "AttributeSystem");

    if (attr) WriteFloat(attr, "BonusHealthPercentagePerAttribute", 0.1f);

    const int pts = VitalityPoints();
    if (cheat)
    {
        ConanApi::Texto n("AttributeHealth");
        if (n.valido)
            ConanApi::Call<void>(cheat, "SetStat", n, float(pts));
    }
    if (attr)
    {
        ConanApi::Call<void>(attr, "OnAttributeVitalityChanged", int32_t(pts));
        ConanApi::Call<void>(attr, "UpdateAttributes");
    }
}

static void Tick(void*)
{
    ++g_tick;
    g_cfg.Load("StartingHealth");
    if (!g_cfg.enabled) return;

    CachePcClasses();
    if (g_pcClassCount == 0) return;

    int applied = 0;
    g_pcScan.Tick(g_api, kApplyPerTick,
        [&](void* obj) {
            return ScanUtil::MatchesAnyClass(g_api, obj, g_pcClasses, g_pcClassCount);
        },
        [&](void* pc) {
            if (ScanUtil::IsDefaultObject(g_api, pc)) return false;
            Apply(pc);
            ++applied;
            return true;
        });

    if ((g_tick % 15) == 1)
        g_api->Log("[StartingHealth] tick=%d applied=%d vitality=%d wantHP=%.0f admin=%d",
                   g_tick, applied, VitalityPoints(),
                   g_cfg.baseStartingHealth * g_cfg.healthMultiplier, g_adminOk ? 1 : 0);
}

extern "C" __declspec(dllexport)
void ConanPluginCarregar(const ConanApiTabela* api)
{
    if (!api || api->tamanho < sizeof(ConanApiTabela)) return;
    g_api = api;
    ConanApi::UsarTabela(api);
    g_cfg.Load("StartingHealth");
    g_tick = 0;
    g_adminOk = false;
    g_api->Log("======================================================");
    g_api->Log(" StartingHealth v2.1.0 — SetStat AttributeHealth + OnAttributeVitalityChanged");
    g_api->Log("======================================================");
    g_taskId = g_api->AgendarNaThreadDoJogo(Tick, 2, nullptr, 1);
}

extern "C" __declspec(dllexport)
void ConanPluginDescarregar(void)
{
    if (g_api && g_taskId) g_api->CancelarAgendamento(g_taskId);
    g_api = nullptr;
}
