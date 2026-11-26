#include "code_patch.h"
#include "mods_log.h"
#include <string.h>

// The client registers g_maxfps with the default "60" (4.x, "120" in 5.x), so that without the line in system.cfg it sleeps away every
// frame above that. The default becomes "0", no limit; a value in system.cfg still wins, and the client saves whatever the cvar holds.
// The string is the default of other cvars as well, so the registration of g_maxfps is pointed at a "0" of its own module instead of
// changing the string.
namespace {

/// A frame rate of two or three digits, as the default string of the cvar.
bool IsLimit(const char* value) {
    size_t length = strspn(value, "0123456789");
    return value[length] == 0 && (length == 2 || length == 3) && value[0] != '0';
}

/// lea rdx, "g_maxfps" names the cvar; the lea r8 of its default string comes a few instructions before it.
CodePatch Locate(HMODULE game) {
    static const char NAME[] = "g_maxfps";
    static const char ZERO[] = "\0" "0";
    BYTE* lea = FindStringLea(game, NAME);
    const BYTE* zero = FindBytes(game, ZERO, sizeof(ZERO));
    if (!lea || !zero) {
        ModsLog("max fps fix: the registration of g_maxfps %p, a string \"0\" %p", lea, zero);
        return {};
    }
    zero++;
    for (BYTE* p = lea - 7; p >= lea - 0x30; p--) {
        if (p[0] != 0x4C || p[1] != 0x8D || p[2] != 0x05) {
            continue;
        }
        const char* value = (const char*)ResolveRip(p + 3, p + 7);
        if (!InModule(game, value, 4) || !IsLimit(value)) {
            continue;
        }
        LONGLONG disp = zero - (p + 7);
        if (disp != (LONG)disp) {
            return {};
        }
        LONG newDisp = (LONG)disp;
        return PlanPatch(p + 3, &newDisp, sizeof(newDisp));
    }
    ModsLog("max fps fix: no default frame rate before the registration of g_maxfps at %p", lea);
    return {};
}

void Apply(const CodePatch& patch) {
    ApplyPatch(patch);
}

} // namespace

void InstallMaxFpsFix(HMODULE game) {
    LocateThenApply(game, Locate, Apply);
}
