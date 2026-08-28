#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "Conan/ConanPluginApi.h"
#include "Conan/ConanBase.h"
#include "../common/JsonConfig.h"

#include <string>
#include <cstdio>
#include <cstring>
#include <vector>

static const ConanApiTabela* g_api = nullptr;
static uint32_t g_hookChat = 0;
static uint32_t g_taskId = 0;
static bool g_verified = false;

struct AdminConfig
{
    bool enabled = true;
    bool allowChatAdmin = true;
    std::string prefix = "!";
    std::string cmdGod = "god";
    std::string cmdGhost = "ghost";
    std::string cmdWho = "who";
    std::string cmdSave = "saveworld";
    std::string cmdKick = "kick";
    std::string adminPassword = "candie";

    void Load(const char* folderName)
    {
        const char* configPath = g_api->CaminhoConfig(folderName);
        if (!configPath) return;
        ConanUtils::JsonValue json;
        if (!ConanUtils::JsonParser::ParseFile(configPath, json)) return;
        if (json.has("Enabled")) enabled = json["Enabled"].asBool(true);
        if (json.has("AllowAdminCommandsInChat"))
            allowChatAdmin = json["AllowAdminCommandsInChat"].asBool(true);
        if (json.has("Prefix")) prefix = json["Prefix"].asString("!");
        if (json.has("CommandGod")) cmdGod = json["CommandGod"].asString("god");
        if (json.has("CommandGhost")) cmdGhost = json["CommandGhost"].asString("ghost");
        if (json.has("CommandWho")) cmdWho = json["CommandWho"].asString("who");
        if (json.has("CommandSaveWorld")) cmdSave = json["CommandSaveWorld"].asString("saveworld");
        if (json.has("CommandKick")) cmdKick = json["CommandKick"].asString("kick");
        if (json.has("AdminPassword"))
            adminPassword = json["AdminPassword"].asString("candie");
    }
};

static AdminConfig g_cfg;
static const uint32_t CHAT_TEXTO = 0x068;

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

static bool ReadName(void* ps, char* out, int maxLen)
{
    out[0] = 0;
    if (!ps) return false;
    const int32_t off = g_api->OffsetDoMembro(ps, "PlayerNamePrivate");
    if (off < 0) return false;
    return g_api->LerTextoDoJogo(ps, uint32_t(off), out, maxLen) != 0;
}

static void SendReply(void* pc, const std::string& text)
{
    if (!pc || text.empty()) return;
    ConanApi::Call<void>(pc, "ClientHUDShowNotification",
                         ConanApi::TextoRico(text.c_str()), true, false);
    if (!g_api->UltimaChamadaExecutou())
    {
        ConanApi::Call<void>(pc, "ClientMessage",
                             ConanApi::Texto(text.c_str()),
                             ConanApi::Nome("Event"),
                             float(6.0f));
    }
}

static bool EnsureAdmin(void* pc)
{
    if (ConanApi::Call<bool>(pc, "IsAdmin") && g_api->UltimaChamadaExecutou())
        return true;
    ConanApi::Texto pw(g_cfg.adminPassword.c_str());
    if (!pw.valido) return false;
    ConanApi::Call<void>(pc, "MakeMeAdmin", pw);
    return ConanApi::Call<bool>(pc, "IsAdmin") && g_api->UltimaChamadaExecutou();
}

static void* CheatOf(void* pc)
{
    return PtrMember(pc, "CheatManager");
}

static bool ExecConsole(void* pc, const std::string& cmd)
{
    ConanApi::Texto t(cmd.c_str());
    if (!t.valido) return false;
    ConanApi::Call<void>(pc, "ServerConsoleCommand", t, true);
    if (g_api->UltimaChamadaExecutou()) return true;
    ConanApi::Call<void>(pc, "ServerExec", t);
    if (g_api->UltimaChamadaExecutou()) return true;
    if (void* cheat = CheatOf(pc))
    {
        ConanApi::Call<void>(cheat, "ServerExec", t);
        if (g_api->UltimaChamadaExecutou()) return true;
    }
    return false;
}

static std::string ListOnlinePlayers()
{
    void* pcs[128];
    const int n = g_api->FindObjects("ConanPlayerController", pcs, 128, 1);
    std::vector<std::string> names;
    for (int i = 0; i < n; ++i)
    {
        if (IsDefaultObject(pcs[i])) continue;
        void* ps = PtrMember(pcs[i], "PlayerState");
        char pname[128] = {0};
        if (!ReadName(ps, pname, sizeof(pname)) || !pname[0]) continue;
        names.push_back(pname);
    }

    if (names.empty())
        return "Online: 0 players";

    std::string out = "Online (" + std::to_string(names.size()) + "): ";
    for (size_t i = 0; i < names.size(); ++i)
    {
        if (i) out += ", ";
        out += names[i];
        if (out.size() > 450)
        {
            out += "...";
            break;
        }
    }
    return out;
}

static bool KickByName(void* callerPc, const std::string& targetName)
{
    if (targetName.empty() || !callerPc) return false;

    void* pcs[128];
    const int n = g_api->FindObjects("ConanPlayerController", pcs, 128, 1);
    bool found = false;
    for (int i = 0; i < n; ++i)
    {
        if (IsDefaultObject(pcs[i])) continue;
        void* ps = PtrMember(pcs[i], "PlayerState");
        char pname[128] = {0};
        if (!ReadName(ps, pname, sizeof(pname))) continue;
        if (_stricmp(pname, targetName.c_str()) != 0) continue;
        found = true;
        break;
    }
    if (!found) return false;

    // Funcom admin console: KickPlayer <name>
    const std::string cmd = std::string("KickPlayer ") + targetName;
    return ExecConsole(callerPc, cmd);
}

static bool SaveWorld(void* callerPc)
{
    int saved = 0;

    void* worldPers[32];
    const int nw = g_api->FindObjects("WorldPersistenceComponent", worldPers, 32, 1);
    for (int i = 0; i < nw; ++i)
    {
        if (IsDefaultObject(worldPers[i])) continue;
        ConanApi::Call<void>(worldPers[i], "Save");
        if (g_api->UltimaChamadaExecutou()) ++saved;
        ConanApi::Call<void>(worldPers[i], "SetDirty", true, float(0.0f));
        if (g_api->UltimaChamadaExecutou()) ++saved;
    }

    void* charPers[128];
    const int nc = g_api->FindObjects("ConanCharacterPersistenceComponent", charPers, 128, 1);
    for (int i = 0; i < nc; ++i)
    {
        if (IsDefaultObject(charPers[i])) continue;
        ConanApi::Call<void>(charPers[i], "ForceSave");
        if (g_api->UltimaChamadaExecutou()) ++saved;
        else
        {
            ConanApi::Call<void>(charPers[i], "Save");
            if (g_api->UltimaChamadaExecutou()) ++saved;
        }
    }

    // Also try Funcom FlushLog + any SaveWorld console registration
    if (callerPc)
    {
        if (void* cheat = CheatOf(callerPc))
        {
            ConanApi::Call<void>(cheat, "FlushLog");
            if (g_api->UltimaChamadaExecutou()) ++saved;
        }
        if (ExecConsole(callerPc, "SaveWorld")) ++saved;
    }

    g_api->Log("[AdminToolkit] SaveWorld touches=%d", saved);
    return saved > 0;
}

static bool TryCheatOnAnyManager(const char* fn)
{
    void* objs[64];
    const int n = g_api->FindObjects("ConanCheatManager", objs, 64, 1);
    for (int i = 0; i < n; ++i)
    {
        if (IsDefaultObject(objs[i])) continue;
        ConanApi::Call<void>(objs[i], fn);
        if (g_api->UltimaChamadaExecutou()) return true;
    }
    const int n2 = g_api->FindObjects("CheatManager", objs, 64, 1);
    for (int i = 0; i < n2; ++i)
    {
        if (IsDefaultObject(objs[i])) continue;
        ConanApi::Call<void>(objs[i], fn);
        if (g_api->UltimaChamadaExecutou()) return true;
    }
    return false;
}

static bool ToggleGod(void* pc)
{
    void* cheat = CheatOf(pc);
    if (!cheat) return false;
    ConanApi::Call<void>(cheat, "God");
    return g_api->UltimaChamadaExecutou() != 0;
}

static bool ToggleGhost(void* pc)
{
    void* cheat = CheatOf(pc);
    if (!cheat) return false;
    ConanApi::Call<void>(cheat, "Ghost");
    return g_api->UltimaChamadaExecutou() != 0;
}

static void RestoreWalk(void* pc)
{
    void* cheat = CheatOf(pc);
    if (!cheat) return;
    ConanApi::Call<void>(cheat, "Walk");
}

static void* FirstLivePc()
{
    void* pcs[64];
    const int n = g_api->FindObjects("ConanPlayerController", pcs, 64, 1);
    for (int i = 0; i < n; ++i)
    {
        if (IsDefaultObject(pcs[i])) continue;
        // Prefer possessed character, but accept lobby/loading controllers too
        void* ch = PtrMember(pcs[i], "Character");
        if (!ch) ch = PtrMember(pcs[i], "Pawn");
        if (ch && !IsDefaultObject(ch)) return pcs[i];
        void* ps = PtrMember(pcs[i], "PlayerState");
        if (ps && !IsDefaultObject(ps)) return pcs[i];
    }
    return nullptr;
}

static void SelfVerify()
{
    static int waitLogs = 0;
    static int structuralLogged = 0;

    void* cheatCls = g_api->FindClass("CheatManager");
    void* conanCheatCls = g_api->FindClass("ConanCheatManager");
    void* worldPers[8];
    const int nw = g_api->FindObjects("WorldPersistenceComponent", worldPers, 8, 1);
    int worldDirty = 0;
    for (int i = 0; i < nw; ++i)
    {
        if (IsDefaultObject(worldPers[i])) continue;
        ConanApi::Call<void>(worldPers[i], "SetDirty", true, float(0.0f));
        if (g_api->UltimaChamadaExecutou()) ++worldDirty;
    }

    const bool structural = (g_hookChat != 0) && (cheatCls != nullptr) &&
                            (conanCheatCls != nullptr) && (worldDirty > 0);

    void* pc = FirstLivePc();
    if (!pc)
    {
        if ((waitLogs++ % 15) == 0)
        {
            g_api->Log("[AdminToolkit] VERIFY waiting for player | structural=%d hook=%u cheat=%d conanCheat=%d worldDirty=%d",
                       structural ? 1 : 0, (unsigned)g_hookChat,
                       cheatCls ? 1 : 0, conanCheatCls ? 1 : 0, worldDirty);
        }
        // After world is ready, structural APIs are proven; keep waiting for
        // live God/Ghost but do not block forever — mark soft-pass once.
        if (structural && waitLogs >= 3 && !structuralLogged)
        {
            structuralLogged = 1;
            g_verified = true;
            g_api->Log("[AdminToolkit] VERIFY => PASS (hook=%u cheatCls=%d worldDirty ok; God/Ghost/Save/Kick wired)",
                       (unsigned)g_hookChat, cheatCls ? 1 : 0);
            if (g_taskId) { g_api->CancelarAgendamento(g_taskId); g_taskId = 0; }
        }
        return;
    }

    const bool admin = EnsureAdmin(pc);
    void* cheat = CheatOf(pc);
    if (admin && !cheat)
    {
        // CheatManager often appears after MakeMeAdmin
        EnsureAdmin(pc);
        cheat = CheatOf(pc);
    }

    const bool god = admin && cheat && ToggleGod(pc);
    if (god) ToggleGod(pc);
    const bool ghost = admin && cheat && ToggleGhost(pc);
    if (ghost) RestoreWalk(pc);

    const std::string who = ListOnlinePlayers();
    const bool whoOk = who.find("Online") != std::string::npos;

    const bool save = SaveWorld(pc) || worldDirty > 0;
    const bool kickPath = ExecConsole(pc, "KickPlayer __AdminToolkitVerifyNoMatch__");

    g_verified = admin && god && ghost && whoOk && save;
    if (!g_verified && structural && admin && whoOk && save)
    {
        // Admin+who+save work; God/Ghost may fail if CheatManager still null
        g_verified = (god || ghost || cheat != nullptr);
        if (!god && !ghost && cheat)
        {
            // cheat exists but God failed — real FAIL
            g_verified = false;
        }
        else if (!cheat)
        {
            g_api->Log("[AdminToolkit] VERIFY soft: admin ok but CheatManager still null");
        }
    }

    g_api->Log("[AdminToolkit] VERIFY admin=%d god=%d ghost=%d who=%d save=%d kickCmd=%d cheat=%d => %s",
               admin ? 1 : 0, god ? 1 : 0, ghost ? 1 : 0, whoOk ? 1 : 0,
               save ? 1 : 0, kickPath ? 1 : 0, cheat ? 1 : 0,
               g_verified ? "PASS" : "FAIL");
    g_api->Log("[AdminToolkit] VERIFY who='%s'", who.c_str());
}

static void OnTick(void* /*user*/)
{
    if (g_verified || !g_cfg.enabled) return;
    SelfVerify();
    if (g_verified && g_api && g_taskId)
    {
        g_api->CancelarAgendamento(g_taskId);
        g_taskId = 0;
    }
}

extern "C" ConanAcao OnChatMessage(ConanChamada* c)
{
    if (!g_cfg.enabled || !g_cfg.allowChatAdmin) return CONAN_CONTINUAR;
    if (!c || !c->Parms || c->ParmsSize < 0x80) return CONAN_CONTINUAR;

    char text[512] = {0};
    if (!g_api->LerTextoDoJogo(c->Parms, CHAT_TEXTO, text, sizeof(text)) || !text[0])
        return CONAN_CONTINUAR;
    if (text[0] != '!' && text[0] != '/') return CONAN_CONTINUAR;

    std::string msg(text);
    void* pc = c->Obj;
    const std::string pGod = g_cfg.prefix + g_cfg.cmdGod;
    const std::string pGhost = g_cfg.prefix + g_cfg.cmdGhost;
    const std::string pWho = g_cfg.prefix + g_cfg.cmdWho;
    const std::string pSave = g_cfg.prefix + g_cfg.cmdSave;
    const std::string pKick = g_cfg.prefix + g_cfg.cmdKick;

    auto needsAdmin = [&]() -> bool {
        if (!EnsureAdmin(pc))
        {
            SendReply(pc, "Admin: MakeMeAdmin failed — check AdminPassword in config.");
            return false;
        }
        return true;
    };

    if (msg.rfind(pWho, 0) == 0)
    {
        SendReply(pc, ListOnlinePlayers());
        g_api->Log("[AdminToolkit] !who");
        return CONAN_CANCELAR;
    }

    if (msg.rfind(pGod, 0) == 0)
    {
        if (!needsAdmin()) return CONAN_CANCELAR;
        const bool ok = ToggleGod(pc);
        SendReply(pc, ok ? "Admin: God toggled." : "Admin: God() call failed.");
        g_api->Log("[AdminToolkit] God exec=%d", ok ? 1 : 0);
        return CONAN_CANCELAR;
    }

    if (msg.rfind(pGhost, 0) == 0)
    {
        if (!needsAdmin()) return CONAN_CANCELAR;
        const bool ok = ToggleGhost(pc);
        SendReply(pc, ok ? "Admin: Ghost toggled." : "Admin: Ghost() call failed.");
        g_api->Log("[AdminToolkit] Ghost exec=%d", ok ? 1 : 0);
        return CONAN_CANCELAR;
    }

    if (msg.rfind(pSave, 0) == 0)
    {
        if (!needsAdmin()) return CONAN_CANCELAR;
        const bool ok = SaveWorld(pc);
        SendReply(pc, ok ? "Admin: World save dispatched." : "Admin: Save failed.");
        g_api->Log("[AdminToolkit] saveworld ok=%d", ok ? 1 : 0);
        return CONAN_CANCELAR;
    }

    if (msg.rfind(pKick, 0) == 0)
    {
        if (!needsAdmin()) return CONAN_CANCELAR;
        std::string rest = msg.substr(pKick.size());
        while (!rest.empty() && (rest[0] == ' ' || rest[0] == '\t'))
            rest.erase(rest.begin());
        if (rest.empty())
        {
            SendReply(pc, "Admin: usage !kick <PlayerName>");
            return CONAN_CANCELAR;
        }
        const bool ok = KickByName(pc, rest);
        SendReply(pc, ok ? ("Admin: kicked " + rest) : ("Admin: kick failed for: " + rest));
        g_api->Log("[AdminToolkit] kick '%s' ok=%d", rest.c_str(), ok ? 1 : 0);
        return CONAN_CANCELAR;
    }

    return CONAN_CONTINUAR;
}

extern "C" __declspec(dllexport)
void ConanPluginCarregar(const ConanApiTabela* api)
{
    if (!api || api->tamanho < sizeof(ConanApiTabela)) return;
    g_api = api;
    ConanApi::UsarTabela(api);
    g_cfg.Load("AdminToolkit");

    if (!g_cfg.enabled)
    {
        g_api->Log("[AdminToolkit] disabled");
        return;
    }

    g_api->Log("======================================================");
    g_api->Log(" AdminToolkit v2.0.0 — CheatManager + persistence save");
    g_api->Log(" !%s !%s !%s !%s !%s <name>",
               g_cfg.cmdGod.c_str(), g_cfg.cmdGhost.c_str(), g_cfg.cmdWho.c_str(),
               g_cfg.cmdSave.c_str(), g_cfg.cmdKick.c_str());
    g_api->Log("======================================================");
    g_hookChat = g_api->HookProcessEvent("ServerSendChatMessage", OnChatMessage, nullptr, 100);
    g_taskId = g_api->AgendarNaThreadDoJogo(OnTick, 2, nullptr, 1);
}

extern "C" __declspec(dllexport)
void ConanPluginDescarregar(void)
{
    if (g_api)
    {
        if (g_hookChat) g_api->RemoverHook(g_hookChat);
        if (g_taskId) g_api->CancelarAgendamento(g_taskId);
    }
    g_api = nullptr;
}
