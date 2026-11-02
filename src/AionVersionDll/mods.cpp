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

static void InstallGameMods(HMODULE game) {
    if (s_gameModsInstalled) {
        return;
    }
    FindGameState(game);
    ModsLog("Game.dll at %p, game state at %p", game, g_gameState);
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    if (g_modsConfig.chatTimeFormat != 0) {
        InstallChatTime(game);
    }
    InstallTimeouts(game);
    if (g_modsConfig.pingOverlayMode) {
        InstallPing(game);
    }
    if (g_modsConfig.macroLimit) {
        InstallMacroLimit(game);
    }
    bool buffs = g_modsConfig.targetBuffColumns || g_modsConfig.ownBuffColumns;
    if (buffs) {
        InstallBuffLimit(game);
    }
    if (g_modsConfig.statPrecision) {
        InstallStatPrecision(game);
    }
    if (g_modsConfig.questTargets) {
        InstallQuestTargets(game);
    }
    if (g_modsConfig.uiScaleFit) {
        InstallUiScale(game);
    }
    if (g_modsConfig.multiClient) {
        InstallMultiClient(game);
    }
    if (g_modsConfig.skipAgreement) {
        InstallSkipAgreement(game);
    }
    if (g_modsConfig.skipTypedConfirm) {
        InstallQuickDestroy(game);
    }
    if (g_modsConfig.fastStart == FAST_START_FULL) {
        InstallSkipLogo(game);
    }
    if (g_modsConfig.fastStart != FAST_START_OFF) {
        InstallFastLoad(game);
    }
    bool uiXml = InstallUiXml(game);
    if (buffs) {
        InstallBuffSlots(game, uiXml);
    }
    LONG error = DetourTransactionCommit();
    ModsLog("game hooks committed: %ld", error);
    s_gameModsInstalled = 1;
}

/// Must be called inside an open Detours transaction.
void InstallMods() {
    LoadConfig();
    ModsLog("mods: chatTimeFormat=%d antiAfk=%d noSessionTimeout=%d ping=%d macroLimit=%d stats=%d", g_modsConfig.chatTimeFormat, g_modsConfig.antiAfk,
        g_modsConfig.noSessionTimeout, g_modsConfig.pingOverlayMode, g_modsConfig.macroLimit, g_modsConfig.statPrecision);
    OnModuleLoad(L"Game.dll", InstallGameMods, true);
    OnModuleLoad(L"CryFont.dll", InstallGlyphCells, false);
    if (g_modsConfig.fastStart != FAST_START_OFF) {
        InstallFastStart();
    }
    if (g_modsConfig.mimalloc) {
        InstallMimalloc();
    }
    if (g_modsConfig.pingOverlayMode) {
        InstallOverlay();
    }
    if (g_modsConfig.plainSystemCfg) {
        InstallPlainSystemCfg();
    }
}
