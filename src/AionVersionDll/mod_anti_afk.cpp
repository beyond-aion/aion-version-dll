#include "mods.h"

namespace {

/// The check for the 26-hour session limit: cmp [rcx+loginTime], 0; ...; jnz check; mov eax, 1 (return "not expired"). The jnz is
/// kept, to become two nops.
CodePatch LocateSessionTimeout(HMODULE game) {
    // the check adds the limit to the login time: add r32, 93600
    for (BYTE* add : FindAllPatterns(game, "81 ?? A0 6D 01 00")) {
        if ((add[1] & 0xF8) != 0xC0) {
            continue;
        }
        BYTE* function = FunctionStart(add);
        for (BYTE* p = function; p && p < function + 0x30; p++) {
            if (p[0] == 0x75 && p[2] == 0xB8 && p[3] == 1 && p[4] == 0 && p[5] == 0 && p[6] == 0) {
                static const BYTE NOPS[] = { 0x90, 0x90 };
                return PlanPatch(p, NOPS, sizeof(NOPS));
            }
        }
    }
    return CodePatch();
}

/// Makes the check take its "timer not started" exit, so there are neither warnings nor a kick.
void ApplySessionTimeout(const CodePatch& jump) {
    if (!jump) {
        ModsLog("session timeout: check not found");
        return;
    }
    ApplyPatch(jump);
    ModsLog("session timeout: disabled at %p", jump.at);
}

/// The idle limit the idle check compares against, in milliseconds.
CodePatch LocateIdleLimit(HMODULE game) {
    BYTE* check = FindIdleCheck(game);
    return check ? ExpectBytes(ResolveRip(check + 8, check + 12), sizeof(DWORD)) : CodePatch();
}

/// Raises the idle time after which the client drops the connection (60 minutes by default) out of reach.
void ApplyIdleLimit(const CodePatch& limit) {
    if (!limit) {
        ModsLog("anti afk: idle check not found");
        return;
    }
    DWORD never = MAXDWORD;
    ApplyPatch(limit, &never);
    ModsLog("anti afk: threshold at %p was %lu ms", limit.at, *(const DWORD*)limit.expected);
}

} // namespace

void InstallAntiAfk(HMODULE game) {
    LocateThenApply(game, LocateIdleLimit, ApplyIdleLimit);
}

void InstallSessionTimeout(HMODULE game) {
    LocateThenApply(game, LocateSessionTimeout, ApplySessionTimeout);
}
