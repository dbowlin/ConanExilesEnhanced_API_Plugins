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
static uint32_t g_hookCanBePlaced = 0;
static uint32_t g_hookCanBePlacedInternal = 0;
static uint32_t g_hookPreCanBePlaced = 0;
static uint32_t g_flagTaskId = 0;
static uint32_t g_verifyId = 0;
static bool g_verified = false;

// ECanBePlacedResult::Success
static const uint8_t kPlaceSuccess = 0;

struct PlacementConfig
{
    bool enabled = true;
    bool relaxSlopeLimits = true;
    bool allowOverlapPlacing = true;
    bool allowWaterPlacing = true;

    void Load(const char* folderName)
    {
        const char* configPath = g_api->CaminhoConfig(folderName);
        if (!configPath) return;
        ConanUtils::JsonValue json;
        if (!ConanUtils::JsonParser::ParseFile(configPath, json)) return;
        if (json.has("Enabled")) enabled = json["Enabled"].asBool(true);
        if (json.has("RelaxSlopeLimits")) relaxSlopeLimits = json["RelaxSlopeLimits"].asBool(true);
        if (json.has("AllowOverlapPlacing")) allowOverlapPlacing = json["AllowOverlapPlacing"].asBool(true);
        if (json.has("AllowWaterPlacing")) allowWaterPlacing = json["AllowWaterPlacing"].asBool(true);
    }
};

static PlacementConfig g_cfg;
static void* g_placeClasses[5];
static int g_placeClassCount = 0;
static ScanUtil::IncrementalScan g_placeScan;
static const int kFlagsPerTick = 8;

static bool IsDefaultObject(void* obj)
{
    if (!obj) return true;
    char name[256] = {0};
    if (!g_api->NomeDoObjeto(obj, name, sizeof(name))) return false;
    return std::strncmp(name, "Default__", 9) == 0;
}

static void SetBoolMember(void* obj, const char* name, bool value)
{
    if (!obj) return;
    const int32_t off = g_api->OffsetDoMembro(obj, name);
    if (off < 0) return;
    uint8_t b = 0;
    g_api->LerMembro(obj, uint32_t(off), &b, sizeof(b));
    if (value) b |= 0x01;
    else b = uint8_t(b & ~0x01);
    g_api->EscreverMembro(obj, uint32_t(off), &b, sizeof(b));
}

static void ForcePlaceSuccess(ConanChamada* c)
{
    if (!g_cfg.enabled || !c) return;
    if (g_api->DefinirRetorno)
        g_api->DefinirRetorno(c, &kPlaceSuccess, sizeof(kPlaceSuccess));
}

static void AfterCanBePlaced(ConanChamada* c)
{
    ForcePlaceSuccess(c);
}

static void AfterCanBePlacedInternal(ConanChamada* c)
{
    ForcePlaceSuccess(c);
}

static void AfterPreCanBePlaced(ConanChamada* c)
{
    ForcePlaceSuccess(c);
}

extern "C" ConanAcao OnCanBePlaced(ConanChamada* c)
{
    if (!g_cfg.enabled || !c) return CONAN_CONTINUAR;
    // Let the game run, then force Success in the after-hook.
    // Also flip common allow flags on the piece being validated.
    if (c->Obj && !IsDefaultObject(c->Obj))
    {
        if (g_cfg.allowOverlapPlacing)
            SetBoolMember(c->Obj, "CanPlaceAnywhere", true);
        if (g_cfg.allowWaterPlacing)
            SetBoolMember(c->Obj, "CanPlaceOnWater", true);
        if (g_cfg.relaxSlopeLimits)
            SetBoolMember(c->Obj, "bIgnoreSplineLimit", true);
    }
    return CONAN_CONTINUAR;
}

static void CachePlaceClasses()
{
    if (g_placeClassCount > 0) return;
    const char* names[] = {
        "BP_BuildingBase_C", "BuildingBase", "BuildableBase",
        "BP_Master_Placeables_C", "PlaceableBase", nullptr
    };
    for (int i = 0; names[i]; ++i)
    {
        void* cls = g_api->FindClass(names[i]);
        if (cls && g_placeClassCount < 5) g_placeClasses[g_placeClassCount++] = cls;
    }
}

static void ApplyFlagsTo(void* obj)
{
    if (g_cfg.allowOverlapPlacing)
        SetBoolMember(obj, "CanPlaceAnywhere", true);
    if (g_cfg.allowWaterPlacing)
        SetBoolMember(obj, "CanPlaceOnWater", true);
    if (g_cfg.relaxSlopeLimits)
        SetBoolMember(obj, "bIgnoreSplineLimit", true);
}

static void ApplyPlacementFlags(void*)
{
    if (!g_cfg.enabled) return;
    CachePlaceClasses();
    if (g_placeClassCount == 0) return;

    g_placeScan.Tick(g_api, kFlagsPerTick,
        [&](void* obj) { return ScanUtil::MatchesAnyClass(g_api, obj, g_placeClasses, g_placeClassCount); },
        [&](void* obj) { ApplyFlagsTo(obj); return true; });
}

static void SelfVerify(void*)
{
    if (g_verified || !g_cfg.enabled) return;

    // Hooks-only: never Call CanBePlaced on CDOs (crashes).
    const bool ok = (g_hookCanBePlaced != 0) || (g_hookCanBePlacedInternal != 0) || (g_hookPreCanBePlaced != 0);
    g_api->Log("[PlacementUnblocker] VERIFY hookPlace=%u hookInternal=%u hookPre=%u => %s",
               (unsigned)g_hookCanBePlaced, (unsigned)g_hookCanBePlacedInternal,
               (unsigned)g_hookPreCanBePlaced, ok ? "PASS" : "FAIL");
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
    g_cfg.Load("PlacementUnblocker");
    g_verified = false;

    if (!g_cfg.enabled)
    {
        g_api->Log("[PlacementUnblocker] Plugin disabled in config.json (Enabled=false). Load skipped.");
        return;
    }

    g_api->Log("======================================================");
    g_api->Log(" PlacementUnblocker v2.1.0 — incremental placement flags");
    g_api->Log(" Overlap: %s | Slopes: %s | Water: %s",
               g_cfg.allowOverlapPlacing ? "Allowed" : "Vanilla",
               g_cfg.relaxSlopeLimits ? "Relaxed" : "Vanilla",
               g_cfg.allowWaterPlacing ? "Allowed" : "Vanilla");
    g_api->Log("======================================================");

    g_hookCanBePlaced = g_api->HookProcessEvent("CanBePlaced", OnCanBePlaced, AfterCanBePlaced, 100);
    g_hookCanBePlacedInternal = g_api->HookProcessEvent("CanBePlaced_Internal", OnCanBePlaced, AfterCanBePlacedInternal, 100);
    g_hookPreCanBePlaced = g_api->HookProcessEvent("PreCanBePlacedSuccess", OnCanBePlaced, AfterPreCanBePlaced, 100);
    g_flagTaskId = g_api->AgendarNaThreadDoJogo(ApplyPlacementFlags, 15, nullptr, 1);
    g_verifyId = g_api->AgendarNaThreadDoJogo(SelfVerify, 3, nullptr, 1);
}

extern "C" __declspec(dllexport)
void ConanPluginDescarregar(void)
{
    if (g_api)
    {
        if (g_hookCanBePlaced) g_api->RemoverHook(g_hookCanBePlaced);
        if (g_hookCanBePlacedInternal) g_api->RemoverHook(g_hookCanBePlacedInternal);
        if (g_hookPreCanBePlaced) g_api->RemoverHook(g_hookPreCanBePlaced);
        if (g_flagTaskId) g_api->CancelarAgendamento(g_flagTaskId);
        if (g_verifyId) g_api->CancelarAgendamento(g_verifyId);
        g_api->Log("[PlacementUnblocker] Unloaded.");
    }
    g_api = nullptr;
}
