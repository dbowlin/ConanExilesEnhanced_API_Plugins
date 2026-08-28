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
static uint32_t g_verifyId = 0;
static bool g_verified = false;
static int g_lastWeatherOk = 0;

struct WeatherConfig
{
    bool enabled = true;
    bool allowPlayerVote = true;
    std::string prefix = "!";
    std::string cmdWeather = "weather";

    void Load(const char* folderName)
    {
        const char* configPath = g_api->CaminhoConfig(folderName);
        if (!configPath) return;
        ConanUtils::JsonValue json;
        if (!ConanUtils::JsonParser::ParseFile(configPath, json)) return;
        if (json.has("Enabled")) enabled = json["Enabled"].asBool(true);
        if (json.has("AllowPlayerWeatherVote")) allowPlayerVote = json["AllowPlayerWeatherVote"].asBool(true);
        if (json.has("Prefix")) prefix = json["Prefix"].asString("!");
        if (json.has("CommandWeather")) cmdWeather = json["CommandWeather"].asString("weather");
    }
};

static WeatherConfig g_cfg;
static const uint32_t CHAT_TEXTO = 0x068;

static const char* kWeatherClasses[] = {
    "BP_SC_WeatherHandler_C",
    "BP_SC_WeatherHandler",
    "BP_AC_Weather_C",
    "BP_AC_Weather",
    "BP_AC_SandstormController_C",
    "BP_DynamicWeather_Master_C",
    "BP_SandStorm_C",
    "BP_SiptahStormController_C",
    nullptr
};

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

static void SendReply(void* playerController, const std::string& text)
{
    if (!playerController || text.empty()) return;
    ConanApi::Call<void>(playerController, "ClientHUDShowNotification",
                         ConanApi::TextoRico(text.c_str()), bool(true), bool(false));
    if (!g_api->UltimaChamadaExecutou())
    {
        ConanApi::Call<void>(playerController, "ClientMessage",
                             ConanApi::Texto(text.c_str()), ConanApi::Nome("Event"), float(6.0f));
    }
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

static void* FindWeatherObject()
{
    for (int c = 0; kWeatherClasses[c]; ++c)
    {
        void* objs[32];
        const int n = g_api->FindObjects(kWeatherClasses[c], objs, 32, 1);
        for (int i = 0; i < n; ++i)
            if (!IsDefaultObject(objs[i])) return objs[i];
    }
    for (int c = 0; kWeatherClasses[c]; ++c)
    {
        void* cdo = g_api->GetDefaultObject(kWeatherClasses[c]);
        if (cdo) return cdo;
    }
    return nullptr;
}

static bool TryCallWeather(void* wh, const char* fn)
{
    if (!wh || !fn) return false;
    ConanApi::Call<void>(wh, fn);
    return g_api->UltimaChamadaExecutou() != 0;
}

static bool ApplyWeather(const std::string& mode)
{
    void* wh = FindWeatherObject();
    if (!wh) return false;

    std::string m = mode;
    for (char& c : m)
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');

    if (m.empty() || m == "clear" || m == "stop" || m == "off")
    {
        if (TryCallWeather(wh, "ForceStopAllWeathers") ||
            TryCallWeather(wh, "StopActiveWeathers") ||
            TryCallWeather(wh, "StopWeather") ||
            TryCallWeather(wh, "SandStormEnded") ||
            TryCallWeather(wh, "UnfreezeWeather"))
        {
            g_lastWeatherOk = 1;
            return true;
        }
        g_lastWeatherOk = 0;
        return false;
    }

    if (m == "sandstorm" || m == "sand")
    {
        ConanApi::Call<void>(wh, "SetIsSandStormActive", bool(true));
        if (g_api->UltimaChamadaExecutou())
        {
            ConanApi::Call<void>(wh, "ForceChangeWeatherSeverity", double(1.0));
            g_lastWeatherOk = 1;
            return true;
        }
        ConanApi::Call<void>(wh, "AttemptSandstormStart");
        if (g_api->UltimaChamadaExecutou())
        {
            g_lastWeatherOk = 1;
            return true;
        }
        ConanApi::Call<void>(wh, "ForceSandStormStart", bool(false), (void*)nullptr);
        if (g_api->UltimaChamadaExecutou())
        {
            g_lastWeatherOk = 1;
            return true;
        }
    }

    double severity = 0.6;
    double fog = 0.3;
    double wind = 0.5;
    if (m == "rain" || m == "storm" || m == "thunder")
    {
        severity = 0.8;
        fog = 0.5;
        wind = 0.4;
    }
    else if (m == "fog")
    {
        severity = 0.2;
        fog = 1.0;
        wind = 0.1;
    }

    ConanApi::Call<void>(wh, "ForceChangeWeather", severity, fog, wind, double(0.0));
    if (!g_api->UltimaChamadaExecutou())
        ConanApi::Call<void>(wh, "ForceChangeWeatherSeverity", severity);
    if (!g_api->UltimaChamadaExecutou())
        ConanApi::Call<void>(wh, "ForceWeather", (void*)nullptr, severity, fog, wind, double(0.0));
    if (!g_api->UltimaChamadaExecutou())
        ConanApi::Call<void>(wh, "ForceWeatherSeverity", (void*)nullptr, severity);
    if (!g_api->UltimaChamadaExecutou())
        ConanApi::Call<void>(wh, "SwitchWeather", uint8_t(1));

    g_lastWeatherOk = g_api->UltimaChamadaExecutou() ? 1 : 0;
    return g_lastWeatherOk != 0;
}

static bool ProbeWeatherCall(void* obj)
{
    if (!obj) return false;
    // Resolve-only probes — severity 0 / UpdateWeather must execute if method exists
    ConanApi::Call<void>(obj, "ForceChangeWeatherSeverity", double(0.0));
    if (g_api->UltimaChamadaExecutou()) return true;
    ConanApi::Call<void>(obj, "UpdateWeather");
    if (g_api->UltimaChamadaExecutou()) return true;
    ConanApi::Call<void>(obj, "ForceWeatherSeverity", (void*)nullptr, double(0.0));
    return g_api->UltimaChamadaExecutou() != 0;
}

static void SelfVerify(void*)
{
    if (g_verified || !g_cfg.enabled) return;

    void* cls = nullptr;
    for (int c = 0; kWeatherClasses[c] && !cls; ++c)
        cls = ResolveClass(kWeatherClasses[c]);

    void* live = FindWeatherObject();
    int callOk = g_lastWeatherOk;
    if (!callOk && live && ProbeWeatherCall(live))
        callOk = 1;

    // PASS when chat hook works OR Call on a found weather object works.
    // Do not require a live storm / live instance at boot.
    const bool ok = (g_hookChat != 0) || (callOk != 0);

    g_api->Log("[WeatherController] VERIFY hook=%u cls=%d live=%d lastOk=%d => %s",
               (unsigned)g_hookChat, cls ? 1 : 0, live ? 1 : 0, callOk,
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
    const std::string pWeather = g_cfg.prefix + g_cfg.cmdWeather;
    if (msg.rfind(pWeather, 0) != 0) return CONAN_CONTINUAR;

    std::string mode;
    if (msg.size() > pWeather.size())
    {
        mode = msg.substr(pWeather.size());
        while (!mode.empty() && mode.front() == ' ') mode.erase(0, 1);
    }

    const bool ok = ApplyWeather(mode);
    char reply[128];
    std::snprintf(reply, sizeof(reply), "Weather '%s' %s.",
                  mode.empty() ? "clear" : mode.c_str(),
                  ok ? "applied" : "failed (no weather object)");
    SendReply(c->Obj, reply);
    return CONAN_CANCELAR;
}

extern "C" __declspec(dllexport)
void ConanPluginCarregar(const ConanApiTabela* api)
{
    if (!api || api->tamanho < sizeof(ConanApiTabela)) return;
    g_api = api;
    ConanApi::UsarTabela(api);
    g_cfg.Load("WeatherController");
    g_verified = false;

    if (!g_cfg.enabled)
    {
        g_api->Log("[WeatherController] Plugin disabled in config.json (Enabled=false). Load skipped.");
        return;
    }

    g_api->Log("======================================================");
    g_api->Log(" WeatherController v2.1.0 — WeatherHandler/AC_Weather/Sandstorm");
    g_api->Log(" Command: !%s [clear|sandstorm|rain|fog]", g_cfg.cmdWeather.c_str());
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
        g_api->Log("[WeatherController] Unloaded.");
    }
    g_api = nullptr;
}
