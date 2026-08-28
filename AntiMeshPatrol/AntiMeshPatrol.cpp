#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "Conan/ConanPluginApi.h"
#include "Conan/ConanBase.h"
#include "../common/JsonConfig.h"
#include "../common/ObjectScan.h"

#include <cstdio>
#include <cstring>

static const ConanApiTabela* g_api = nullptr;
static uint32_t g_taskId = 0;
static int g_tick = 0;
static int g_locOk = 0;
static int g_rescues = 0;

struct AntiMeshConfig
{
    bool enabled = true;
    double minSafeZ = -40000.0;
    double rescueZ = 1000.0;
    int intervalSeconds = 3;
    bool playersOnly = true;

    void Load(const char* folderName)
    {
        const char* configPath = g_api->CaminhoConfig(folderName);
        if (!configPath) return;
        ConanUtils::JsonValue json;
        if (!ConanUtils::JsonParser::ParseFile(configPath, json)) return;
        if (json.has("Enabled")) enabled = json["Enabled"].asBool(true);
        if (json.has("MinSafeZCoordinate")) minSafeZ = json["MinSafeZCoordinate"].asDouble(-40000.0);
        if (json.has("RescueZCoordinate")) rescueZ = json["RescueZCoordinate"].asDouble(1000.0);
        if (json.has("AutoRescueIntervalSeconds")) intervalSeconds = json["AutoRescueIntervalSeconds"].asInt(3);
        if (json.has("PlayersOnly")) playersOnly = json["PlayersOnly"].asBool(true);
    }
};

static AntiMeshConfig g_cfg;
static void* g_playerClasses[2];
static int g_playerClassCount = 0;
static ScanUtil::IncrementalScan g_playerScan;
static const int kRescuePerTick = 4;

static bool IsDefaultObject(void* obj)
{
    if (!obj) return true;
    char name[256] = {0};
    if (!g_api->NomeDoObjeto(obj, name, sizeof(name))) return false;
    return std::strncmp(name, "Default__", 9) == 0;
}

static bool IsPlayerCharacter(void* ch)
{
    // Player pawns typically have a non-null PlayerState / Controller that is ConanPlayerController
    const int32_t offPs = g_api->OffsetDoMembro(ch, "PlayerState");
    if (offPs < 0) return false;
    void* ps = nullptr;
    if (g_api->LerMembro(ch, uint32_t(offPs), &ps, sizeof(ps)) <= 0) return false;
    return ps && g_api->Legivel(ps, 8) && !IsDefaultObject(ps);
}

static void CachePlayerClasses()
{
    if (g_playerClassCount > 0) return;
    void* cls = g_api->FindClass("ConanPlayerCharacter");
    if (cls) g_playerClasses[g_playerClassCount++] = cls;
    cls = g_api->FindClass("ConanCharacter");
    if (cls && g_playerClassCount < 2) g_playerClasses[g_playerClassCount++] = cls;
}

static bool RescueIfUnderMesh(void* ch)
{
    if (g_cfg.playersOnly && !IsPlayerCharacter(ch)) return false;
    FVector pos = ConanApi::Call<FVector>(ch, "K2_GetActorLocation");
    if (!g_api->UltimaChamadaExecutou()) return false;
    ++g_locOk;
    if (pos.Z >= g_cfg.minSafeZ) return false;

    FVector dest{ pos.X, pos.Y, g_cfg.rescueZ };
    FRotator rot{ 0.0, 0.0, 0.0 };
    const bool ok = ConanApi::Call<bool>(ch, "K2_TeleportTo", dest, rot);
    if (!g_api->UltimaChamadaExecutou() || !ok)
    {
        g_api->Log("[AntiMeshPatrol] K2_TeleportTo failed for %p at Z=%.0f", ch, pos.Z);
        return false;
    }
    ++g_rescues;
    g_api->Log("[AntiMeshPatrol] Rescued actor %p from Z=%.0f -> %.0f", ch, pos.Z, g_cfg.rescueZ);
    return true;
}

static void PatrolCoordinates(void*)
{
    ++g_tick;
    CachePlayerClasses();
    if (g_playerClassCount == 0) return;

    int scanned = 0;
    g_playerScan.Tick(g_api, kRescuePerTick,
        [&](void* ch) {
            ++scanned;
            return true;
        },
        [&](void* ch) { return RescueIfUnderMesh(ch); });

    if (g_tick == 1 || (g_tick % 20) == 0)
    {
        g_api->Log("[AntiMeshPatrol] tick=%d scanned=%d rescues=%d minZ=%.0f",
                   g_tick, scanned, g_rescues, g_cfg.minSafeZ);
    }
}

extern "C" __declspec(dllexport)
void ConanPluginCarregar(const ConanApiTabela* api)
{
    if (!api || api->tamanho < sizeof(ConanApiTabela)) return;
    g_api = api;
    ConanApi::UsarTabela(api);

    g_cfg.Load("AntiMeshPatrol");
    g_tick = 0;
    g_locOk = 0;
    g_rescues = 0;

    if (!g_cfg.enabled)
    {
        g_api->Log("[AntiMeshPatrol] disabled");
        return;
    }

    if (g_cfg.intervalSeconds < 1) g_cfg.intervalSeconds = 1;

    g_api->Log("======================================================");
    g_api->Log(" AntiMeshPatrol v2.1.0 — incremental under-mesh rescue");
    g_api->Log(" MinSafeZ=%.0f RescueZ=%.0f Interval=%ds PlayersOnly=%d",
               g_cfg.minSafeZ, g_cfg.rescueZ, g_cfg.intervalSeconds,
               g_cfg.playersOnly ? 1 : 0);
    g_api->Log("======================================================");

    g_taskId = g_api->AgendarNaThreadDoJogo(PatrolCoordinates, (uint32_t)g_cfg.intervalSeconds, nullptr, 1);
}

extern "C" __declspec(dllexport)
void ConanPluginDescarregar(void)
{
    if (g_api && g_taskId) g_api->CancelarAgendamento(g_taskId);
    g_api = nullptr;
}
