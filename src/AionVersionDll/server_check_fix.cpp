#include "code_patch.h"
#include "mods_log.h"

/// About once in 500 experience updates the client compares the address of the game server against a list of official servers and private
/// networks. On any other server, 127.0.0.1 included, it marks itself and crashes on purpose with the next other player who comes into
/// sight, until it is restarted. The draw is followed by cmp r8d, 2; jge skip, which is made unconditional, so the check never runs.
static CodePatch LocateServerCheck(HMODULE game) {
    // imul r32, r32, 1000; sub r8d, r32; cmp r8d, 2; jge skip; mov eax, [server id]; cmp eax, -1
    BYTE* p = FindPattern(game, "69 ?? E8 03 00 00 44 2B ?? 41 83 F8 02 7D ?? 8B 05 ?? ?? ?? ?? 83 F8 FF");
    static const BYTE JMP = 0xEB;
    return p ? PlanPatch(p + 13, &JMP, 1) : CodePatch();
}

static void ApplyServerCheck(const CodePatch& jump) {
    if (!jump) {
        ModsLog("server check: not found");
        return;
    }
    ApplyPatch(jump);
    ModsLog("server check: skipped at %p", jump.at);
}

void InstallServerCheckFix(HMODULE game) {
    LocateThenApply(game, LocateServerCheck, ApplyServerCheck);
}
