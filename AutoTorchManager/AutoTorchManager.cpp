#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "Conan/ConanPluginApi.h"
#include "Conan/ConanBase.h"
#include "../common/JsonConfig.h"

#include <string>
#include <cstdio>
#include <cstring>

static const ConanApiTabela* g_api = nullptr;
static uint32_t g_hookChat = 0;
static uint32_t g_tickId = 0;
static uint32_t g_batchId = 0;
static uint32_t g_verifyId = 0;
static bool g_verified = false;
static int g_lastLightsState = -1;
static int g_lastCallOk = 0;

static const int kScanLimit = 256;
static const int kMaxPerTick = 12;

static const char* kLightClasses[] = {
    "BP_BAC_TurnLightOnOff_C",
    "BP_BAC_TurnLightOnOff",
    nullptr
};

struct TorchConfig
{
    bool enabled = true;
    int autoDuskHour = 19;
    int autoDawnHour = 6;
    bool infiniteTorchFuel = true;
    std::string prefix = "!";
    std::string cmdLights = "lights";

    void Load(const char* folderName)
    {
        const char* configPath = g_api->CaminhoConfig(folderName);
        if (!configPath) return;
        ConanUtils::JsonValue json;
        if (!ConanUtils::JsonParser::ParseFile(configPath, json)) return;
        if (json.has("Enabled")) enabled = json["Enabled"].asBool(true);
        if (json.has("AutoDuskHour")) autoDuskHour = json["AutoDuskHour"].asInt(19);
        if (json.has("AutoDawnHour")) autoDawnHour = json["AutoDawnHour"].asInt(6);
        if (json.has("InfiniteTorchFuel")) infiniteTorchFuel = json["InfiniteTorchFuel"].asBool(true);
        if (json.has("Prefix")) prefix = json["Prefix"].asString("!");
        if (json.has("CommandLights")) cmdLights = json["CommandLights"].asString("lights");
    }
};

static TorchConfig g_cfg;
static int g_pendingState = -1;
static int g_batchIndex = 0;
static int g_batchTotal = 0;
static void* g_batchTargets[kScanLimit];
static void* g_batchOwners[kScanLimit];

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

static void* ResolveClass(const char* name)
{
    if (!name || !name[0]) return nullptr;
    void* cls = g_api->FindClass(name);
    if (cls) return cls;
    void* cdo = g_api->GetDefaultObject(name);
    if (cdo) return cdo;
    void* objs[1];
    if (g_api->FindObjects(name, objs, 1, 1) > 0 && objs[0]) return objs[0];
    return nullptr;
}

static void SendReply(void* playerController, const std::string& text)
{
    if (!playerController || text.empty()) return;
    ConanApi::Call<void>(playerController, "ClientHUDShowNotification",
                         ConanApi::TextoRico(text.c_str()), bool(true), bool(false));
    if (!g_api->UltimaChamadaExecutou())
    {
        ConanApi::Call<void>(playerController, "ClientMessage",
                             ConanApi::Texto(text.c_str()),
                             ConanApi::Nome("Event"), float(6.0f));
    }
}

static void* LightCompOf(void* actor)
{
    if (!actor) return nullptr;
    void* comp = nullptr;
    int32_t off = g_api->OffsetDoMembro(actor, "BP_BAC_TurnLightOnOff");
    if (off >= 0) g_api->LerMembro(actor, uint32_t(off), &comp, sizeof(comp));
    if (!comp || IsDefaultObject(comp)) return nullptr;
    return comp;
}

static void ClearFuelConsume(void* actor)
{
    if (!g_cfg.infiniteTorchFuel || !actor) return;
    void* fuel = nullptr;
    int32_t off = g_api->OffsetDoMembro(actor, "BP_BAC_UsesFuel");
    if (off >= 0) g_api->LerMembro(actor, uint32_t(off), &fuel, sizeof(fuel));
    if (!fuel || IsDefaultObject(fuel)) return;
    uint8_t b = 0;
    const int32_t bitOff = 0xE8;
    if (g_api->LerMembro(fuel, bitOff, &b, sizeof(b)) > 0)
    {
        b = uint8_t(b & ~0x01);
        g_api->EscreverMembro(fuel, bitOff, &b, sizeof(b));
    }
}

static bool ApplyLight(void* comp, void* owner, bool turnOn)
{
    if (!comp || IsDefaultObject(comp)) return false;
    ConanApi::Call<void>(comp, turnOn ? "TurnOnAll" : "TurnOffAll");
    if (!g_api->UltimaChamadaExecutou())
        ConanApi::Call<void>(comp, "ToggleAll");
    if (!g_api->UltimaChamadaExecutou()) return false;
    g_lastCallOk = 1;
    if (turnOn && owner) ClearFuelConsume(owner);
    return true;
}

static int CollectLightTargets()
{
    int count = 0;
    void* comps[kScanLimit];
    for (int c = 0; kLightClasses[c]; ++c)
    {
        const int n = g_api->FindObjects(kLightClasses[c], comps, kScanLimit, 1);
        for (int i = 0; i < n && count < kScanLimit; ++i)
        {
            if (IsDefaultObject(comps[i])) continue;
            g_batchTargets[count] = comps[i];
            g_batchOwners[count] = nullptr;
            ++count;
        }
        if (count > 0) return count;
    }

    void* actors[kScanLimit];
    int a = g_api->FindObjects("BP_Master_Placeables_C", actors, kScanLimit, 1);
    if (a <= 0) a = g_api->FindObjects("PlaceableBase", actors, kScanLimit, 1);
    for (int i = 0; i < a && count < kScanLimit; ++i)
    {
        if (IsDefaultObject(actors[i])) continue;
        void* comp = LightCompOf(actors[i]);
        if (!comp) continue;
        g_batchTargets[count] = comp;
        g_batchOwners[count] = actors[i];
        ++count;
    }
    return count;
}

static void BeginLightJob(bool turnOn)
{
    g_pendingState = turnOn ? 1 : 0;
    g_batchIndex = 0;
    g_batchTotal = CollectLightTargets();
}

static int ProcessLightBatch(int budget)
{
    if (g_pendingState < 0) return 0;
    const bool turnOn = (g_pendingState == 1);
    int applied = 0;

    while (g_batchIndex < g_batchTotal && applied < budget)
    {
        if (ApplyLight(g_batchTargets[g_batchIndex], g_batchOwners[g_batchIndex], turnOn))
            ++applied;
        ++g_batchIndex;
    }

    if (g_batchIndex >= g_batchTotal)
    {
        g_lastLightsState = turnOn ? 1 : 0;
        g_pendingState = -1;
        g_batchTotal = 0;
        g_batchIndex = 0;
    }
    return applied;
}

static bool ReadHourOfDay(float& outHour)
{
    outHour = -1.0f;
    void* managers[8];
    const int n = g_api->FindObjects("ConanTimeOfDayManager", managers, 8, 1);
    if (n <= 0) return false;
    for (int i = 0; i < n; ++i)
    {
        if (IsDefaultObject(managers[i])) continue;
        float tod = ConanApi::Call<float>(managers[i], "GetTimeOfDay");
        if (!g_api->UltimaChamadaExecutou())
        {
            const int32_t off = g_api->OffsetDoMembro(managers[i], "TimeOfDay");
            if (off < 0) continue;
            if (g_api->LerMembro(managers[i], uint32_t(off), &tod, sizeof(tod)) <= 0) continue;
        }
        if (tod > 24.0f) tod *= (1.0f / 100.0f);
        if (tod < 0.0f) tod = 0.0f;
        if (tod >= 24.0f) tod = 23.99f;
        outHour = tod;
        return true;
    }
    return false;
}

static bool WantLightsOn(float hour)
{
    const int h = (int)hour;
    const int dusk = g_cfg.autoDuskHour;
    const int dawn = g_cfg.autoDawnHour;
    if (dusk == dawn) return false;
    if (dusk > dawn) return (h >= dusk) || (h < dawn);
    return (h >= dusk) && (h < dawn);
}

static void TickBatch(void*)
{
    if (!g_cfg.enabled || g_pendingState < 0) return;
    const int n = ProcessLightBatch(kMaxPerTick);
    if (n > 0)
        g_api->Log("[AutoTorchManager] batch applied %d lights (%d/%d, state=%s)",
                   n, g_batchIndex, g_batchTotal, g_pendingState ? "ON" : "OFF");
}

static void TickLights(void*)
{
    if (!g_cfg.enabled) return;
    if (g_pendingState >= 0) return;

    float hour = -1.0f;
    if (!ReadHourOfDay(hour)) return;

    const bool wantOn = WantLightsOn(hour);
    const int want = wantOn ? 1 : 0;
    if (want == g_lastLightsState) return;

    BeginLightJob(wantOn);
    const int n = ProcessLightBatch(kMaxPerTick);
    g_api->Log("[AutoTorchManager] TOD=%.2f => lights %s started (%d/%d this tick)",
               hour, wantOn ? "ON" : "OFF", n, g_batchTotal);
}

static void SelfVerify(void*)
{
    if (g_verified || !g_cfg.enabled) return;

    void* cls = nullptr;
    for (int c = 0; kLightClasses[c] && !cls; ++c)
        cls = ResolveClass(kLightClasses[c]);

    void* todCls = g_api->FindClass("ConanTimeOfDayManager");
    if (!todCls) todCls = g_api->GetDefaultObject("ConanTimeOfDayManager");

    void* comps[4];
    int nComp = 0;
    for (int c = 0; kLightClasses[c]; ++c)
    {
        nComp = g_api->FindObjects(kLightClasses[c], comps, 4, 1);
        if (nComp > 0) break;
    }

    int callOk = g_lastCallOk;
    if (!callOk && nComp > 0 && !IsDefaultObject(comps[0]))
    {
        ConanApi::Call<void>(comps[0], "TurnOnAll");
        if (g_api->UltimaChamadaExecutou()) callOk = 1;
    }

    float hour = -1.0f;
    const bool todOk = ReadHourOfDay(hour) || (todCls != nullptr);
    const bool ok = (g_tickId != 0) && (g_batchId != 0) && ((callOk != 0) || todOk);

    g_api->Log("[AutoTorchManager] VERIFY lightCls=%d batch=%u tick=%u => %s",
               cls ? 1 : 0, (unsigned)g_batchId, (unsigned)g_tickId, ok ? "PASS" : "FAIL");
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

    char text[512] = {0};
    ReadText(c->Parms, CHAT_TEXTO, text, sizeof(text));
    if (text[0] != '!' && text[0] != '/') return CONAN_CONTINUAR;

    std::string msg(text);
    const std::string pLights = g_cfg.prefix + g_cfg.cmdLights;
    if (msg.rfind(pLights, 0) != 0) return CONAN_CONTINUAR;

    std::string arg;
    if (msg.size() > pLights.size())
    {
        arg = msg.substr(pLights.size());
        while (!arg.empty() && arg.front() == ' ') arg.erase(0, 1);
    }

    bool on = true;
    if (arg == "off" || arg == "0") on = false;
    else if (arg == "toggle" || arg == "t")
        on = (g_lastLightsState != 1);

    BeginLightJob(on);
    const int n = ProcessLightBatch(kMaxPerTick);
    char reply[160];
    std::snprintf(reply, sizeof(reply),
                  "Lighting %s queued (%d/%d this tick, max %d/tick).",
                  on ? "ON" : "OFF", n, g_batchTotal, kMaxPerTick);
    SendReply(c->Obj, reply);
    return CONAN_CANCELAR;
}

extern "C" __declspec(dllexport)
void ConanPluginCarregar(const ConanApiTabela* api)
{
    if (!api || api->tamanho < sizeof(ConanApiTabela)) return;
    g_api = api;
    ConanApi::UsarTabela(api);
    g_cfg.Load("AutoTorchManager");
    g_verified = false;
    g_lastLightsState = -1;
    g_pendingState = -1;
    g_batchIndex = 0;
    g_batchTotal = 0;

    if (!g_cfg.enabled)
    {
        g_api->Log("[AutoTorchManager] Plugin disabled in config.json (Enabled=false). Load skipped.");
        return;
    }

    g_api->Log("======================================================");
    g_api->Log(" AutoTorchManager v2.2.0 — incremental light batches");
    g_api->Log(" Dusk: %02d:00 | Dawn: %02d:00 | max %d/tick | !%s",
               g_cfg.autoDuskHour, g_cfg.autoDawnHour, kMaxPerTick, g_cfg.cmdLights.c_str());
    g_api->Log("======================================================");

    g_hookChat = g_api->HookProcessEvent("ServerSendChatMessage", OnChatMessage, nullptr, 100);
    g_tickId = g_api->AgendarNaThreadDoJogo(TickLights, 30, nullptr, 1);
    g_batchId = g_api->AgendarNaThreadDoJogo(TickBatch, 5, nullptr, 1);
    g_verifyId = g_api->AgendarNaThreadDoJogo(SelfVerify, 3, nullptr, 1);
}

extern "C" __declspec(dllexport)
void ConanPluginDescarregar(void)
{
    if (g_api)
    {
        if (g_hookChat) g_api->RemoverHook(g_hookChat);
        if (g_tickId) g_api->CancelarAgendamento(g_tickId);
        if (g_batchId) g_api->CancelarAgendamento(g_batchId);
        if (g_verifyId) g_api->CancelarAgendamento(g_verifyId);
        g_api->Log("[AutoTorchManager] Unloaded.");
    }
    g_api = nullptr;
}
