#include "mods.h"
#include "module_load.h"
#include <stdio.h>
#include <shlwapi.h>
#include <string>
#include <vector>
#include <detours/detours.h>

ModsConfig g_modsConfig = {};
volatile int* g_gameState = nullptr;

static volatile LONG s_gameModsInstalled = 0;

static constexpr int STATE_IN_WORLD = 14;

BYTE* FindIdleCheck(HMODULE module) {
    // sub eax, [rbx+lastInput]; cmp eax, [limit]; jbe; mov r9d, ebp; xor r8d, r8d; mov edx, 901652
    return FindPattern(module, "2B 83 ?? ?? 00 00 3B 05 ?? ?? ?? ?? 76 ?? 44 8B CD 45 33 C0 BA 14 C2 0D 00");
}

bool IsInWorld() {
    if (!g_gameState) {
        return false;
    }
    static int lastLogged = -1;
    int state = *g_gameState;
    if (state != lastLogged) {
        lastLogged = state;
        ModsLog("game state %d", state);
    }
    return state == STATE_IN_WORLD;
}

static const Setting SETTINGS[] = {
    { L"ChatTime", L"Format", Setting::Int, &g_modsConfig.chatTimeFormat, 0, 0, 2 },
    { L"AntiAfk", L"Enabled", Setting::Bool, &g_modsConfig.antiAfk, 0 },
    { L"AntiAfk", L"NoSessionTimeout", Setting::Bool, &g_modsConfig.noSessionTimeout, 0 },
    { L"Ping", L"OverlayMode", Setting::Int, &g_modsConfig.pingOverlayMode, 0, 0, 2 },
    { L"Ping", L"Interval", Setting::Int, &g_modsConfig.pingInterval, 3000, 500, 60000 },
    // the start of the base line, one line below the frame rate of the DXVK HUD
    { L"Ping", L"X", Setting::Int, &g_modsConfig.pingX, 8, 0, 16384 },
    { L"Ping", L"Y", Setting::Int, &g_modsConfig.pingY, 48, 0, 16384 },
    { L"Ping", L"Scale", Setting::Float, &g_modsConfig.pingScale, 1, 0.25f, 4 },
    // the limit is a signed byte immediate in the client
    { L"Macros", L"Limit", Setting::Int, &g_modsConfig.macroLimit, 0, 0, 127 },
    { L"Buffs", L"TargetColumns", Setting::Int, &g_modsConfig.targetBuffColumns, 0, 0, 64 },
    { L"Buffs", L"OwnColumns", Setting::Int, &g_modsConfig.ownBuffColumns, 0, 0, 64 },
    { L"Stats", L"Enabled", Setting::Bool, &g_modsConfig.statPrecision, 0 },
    { L"QuestTargets", L"Enabled", Setting::Bool, &g_modsConfig.questTargets, 0 },
    { L"MoveReports", L"Window", Setting::Int, &g_modsConfig.moveReportWindow, 50, 0, 150 },
    { L"MoveReports", L"Distance", Setting::Float, &g_modsConfig.moveReportDistance, 0.5f, 0.1f, 1 },
    { L"MoveReports", L"TurnAngle", Setting::Float, &g_modsConfig.moveReportTurnAngle, 15, 1, 30 },
    { L"MoveReports", L"OthersSnapTime", Setting::Float, &g_modsConfig.othersSnapTime, 0.25f, 0.1f, 0.5f },
    { L"MoveReports", L"OthersHeightInAir", Setting::Bool, &g_modsConfig.othersHeightInAir, 1 },
    { L"MultiClient", L"Enabled", Setting::Bool, &g_modsConfig.multiClient, 0 },
    { L"Login", L"SkipAgreement", Setting::Bool, &g_modsConfig.skipAgreement, 0 },
    { L"Destroy", L"SkipTypedConfirm", Setting::Bool, &g_modsConfig.skipTypedConfirm, 0 },
    { L"Performance", L"LoadingMode", Setting::Int, &g_modsConfig.fastStart, FAST_START_OFF, FAST_START_OFF, FAST_START_FULL },
    { L"Performance", L"Mimalloc", Setting::Bool, &g_modsConfig.mimalloc, 0 },
    { L"UiScale", L"Fit", Setting::Bool, &g_modsConfig.uiScaleFit, 0 },
    { L"UiScale", L"LargeGlyphFrom", Setting::Int, &g_modsConfig.largeGlyphFrom, 33, 8, 64 },
    { L"SystemCfg", L"PlainText", Setting::Bool, &g_modsConfig.plainSystemCfg, 0 },
};

static void LoadConfig() {
    for (const Setting& setting : SETTINGS) {
        ReadSetting(setting);
    }
}

/// The idle check skips itself early on for some values of the global game state: mov eax, [state]; cmp eax, 10
static bool FindGameState(HMODULE game) {
    BYTE* check = FindIdleCheck(game);
    BYTE* function = check ? FunctionStart(check) : nullptr;
    for (BYTE* p = function; p && p < function + 0x40; p++) {
        if (p[0] == 0x8B && p[1] == 0x05 && p[6] == 0x83 && p[7] == 0xF8 && p[8] == 0x0A) {
            g_gameState = (volatile int*)ResolveRip(p + 2, p + 6);
            return true;
        }
    }
    return false;
}

/// Installs one mod in a scope of its own when mods.ini turns it on, for the summary in version.log.
template <typename... Args>
static void InstallIf(const char* id, bool enabled, void (*install)(Args...), Args... args) {
    if (!enabled) {
        ReportStatus(id, InstallStatus::Disabled);
        return;
    }
    InstallScope scope(id);
    install(args...);
}

static void InstallGameMods(HMODULE game) {
    if (s_gameModsInstalled) {
        return;
    }
    {
        InstallScope scope("Game.dll mods", true);
        {
            InstallScope state("game_state");
            FindGameState(game);
        }
        ModsLog("Game.dll at %p, game state at %p", game, g_gameState);
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        const ModsConfig& cfg = g_modsConfig;
        InstallIf("chat_time", cfg.chatTimeFormat != 0, InstallChatTime, game);
        InstallIf("timeouts", true, InstallTimeouts, game);
        InstallIf("ping", cfg.pingOverlayMode != 0, InstallPing, game);
        InstallIf("macro_limit", cfg.macroLimit != 0, InstallMacroLimit, game);
        bool buffs = cfg.targetBuffColumns || cfg.ownBuffColumns;
        InstallIf("buff_limit", buffs, InstallBuffLimit, game);
        InstallIf("stat_precision", cfg.statPrecision, InstallStatPrecision, game);
        InstallIf("quest_targets", cfg.questTargets, InstallQuestTargets, game);
        InstallIf("move_reports", cfg.moveReportWindow != 150 || cfg.moveReportDistance != 1 || cfg.moveReportTurnAngle != 30, InstallMoveReports,
            game);
        InstallIf("others_snap", cfg.othersSnapTime != 0.5f || cfg.othersHeightInAir, InstallOthersSnap, game);
        InstallIf("ui_scale", cfg.uiScaleFit, InstallUiScale, game);
        InstallIf("multi_client", cfg.multiClient, InstallMultiClient, game);
        InstallIf("skip_agreement", cfg.skipAgreement, InstallSkipAgreement, game);
        InstallIf("quick_destroy", cfg.skipTypedConfirm, InstallQuickDestroy, game);
        InstallIf("skip_logo", cfg.fastStart == FAST_START_FULL, InstallSkipLogo, game);
        InstallIf("fast_load", cfg.fastStart != FAST_START_OFF, InstallFastLoad, game);
        bool uiXml;
        {
            InstallScope scope("ui_xml");
            uiXml = InstallUiXml(game);
        }
        if (buffs && !uiXml) {
            // the larger buff windows are edits of their UI files
            ReportStatus("buff_limit", InstallStatus::SkippedDependency);
        }
        InstallIf("buff_slots", buffs, InstallBuffSlots, game, uiXml);
        LONG error = DetourTransactionCommit();
        ModsLog("game hooks committed: %ld", error);
    }
    s_gameModsInstalled = 1;
    LogInstallSummary("with Game.dll", true);
}

/// Must be called inside an open Detours transaction.
void InstallMods() {
    LoadConfig();
    ModsLog("mods: chatTimeFormat=%d antiAfk=%d noSessionTimeout=%d ping=%d macroLimit=%d stats=%d", g_modsConfig.chatTimeFormat, g_modsConfig.antiAfk,
        g_modsConfig.noSessionTimeout, g_modsConfig.pingOverlayMode, g_modsConfig.macroLimit, g_modsConfig.statPrecision);
    OnModuleLoad(L"Game.dll", InstallGameMods, true);
    OnModuleLoad(L"CryFont.dll", InstallGlyphCells, false);
    InstallIf("fast_start", g_modsConfig.fastStart != FAST_START_OFF, InstallFastStart);
    InstallIf("mimalloc", g_modsConfig.mimalloc, InstallMimalloc);
    InstallIf("overlay", g_modsConfig.pingOverlayMode != 0, InstallOverlay);
    InstallIf("plain_system_cfg", g_modsConfig.plainSystemCfg, InstallPlainSystemCfg);
}
