#include "mods.h"

/// After the login the client shows the user agreement before the server list, except for one country code (4), for which it shows
/// the list at once. The jump taken for that code becomes unconditional.
void InstallSkipAgreement(HMODULE game) {
    // 4.x: cmp eax, 4; jz list; lea rcx, [game]; call agreement; jmp done; lea rcx, [game]; call list
    BYTE* jump = FindPattern(game, "83 F8 04 74 ?? 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? EB ?? 48 8D 0D ?? ?? ?? ?? E8");
    if (!jump) {
        // 5.x: cmp eax, 4; jz list; call agreement; jmp done; call list; cmp [state], 23
        jump = FindPattern(game, "83 F8 04 74 ?? E8 ?? ?? ?? ?? EB ?? E8 ?? ?? ?? ?? 83 3D ?? ?? ?? ?? 17");
    }
    if (!jump) {
        ModsLog("skip agreement: not found");
        return;
    }
    static const BYTE SHORT_JMP = 0xEB;
    PatchMemory(jump + 3, &SHORT_JMP, 1);
    ModsLog("skip agreement: at %p", jump + 3);
}
