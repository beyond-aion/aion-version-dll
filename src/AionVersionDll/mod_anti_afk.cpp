#include "mods.h"

/// Makes the check for the 26-hour session limit take its "timer not started" exit, so there are neither warnings nor a kick.
static void DisableSessionTimeout(HMODULE game) {
    // the check adds the limit to the login time: add r32, 93600
    std::vector<BYTE*> adds = FindAllPatterns(game, "81 ?? A0 6D 01 00");
    for (BYTE* add : adds) {
        if ((add[1] & 0xF8) != 0xC0) {
            continue;
        }
        BYTE* function = FunctionStart(add);
        // cmp [rcx+loginTime], 0; ...; jnz check; mov eax, 1 (return "not expired")
        for (BYTE* p = function; p && p < function + 0x30; p++) {
            if (p[0] == 0x75 && p[2] == 0xB8 && p[3] == 1 && p[4] == 0 && p[5] == 0 && p[6] == 0) {
                static const BYTE nops[] = { 0x90, 0x90 };
                PatchMemory(p, nops, sizeof(nops));
                ModsLog("session timeout: disabled at %p", p);
                return;
            }
        }
    }
    ModsLog("session timeout: check not found");
}

/// Raises the idle time after which the client drops the connection (60 minutes by default) out of reach.
static void InstallAntiAfk(HMODULE game) {
    BYTE* check = FindIdleCheck(game);
    if (!check) {
        ModsLog("anti afk: idle check not found");
        return;
    }
    DWORD* threshold = (DWORD*)ResolveRip(check + 8, check + 12);
    DWORD old = *threshold;
    DWORD never = MAXDWORD;
    PatchMemory(threshold, &never, sizeof(never));
    ModsLog("anti afk: threshold at %p was %lu ms", threshold, old);
}

void InstallTimeouts(HMODULE game) {
    if (g_modsConfig.antiAfk) {
        InstallAntiAfk(game);
    }
    if (g_modsConfig.noSessionTimeout) {
        DisableSessionTimeout(game);
    }
}
