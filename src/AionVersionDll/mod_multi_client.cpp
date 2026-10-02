#include "mods.h"

/// The client counts the windows of its class when it starts and closes the other clients if two of them are open:
/// lea rcx, "Running %d instances."; call log; cmp dword ptr [reg+count], 2; jl start
/// The jl becomes a jmp, so any number of clients can run.
void InstallMultiClient(HMODULE game) {
    static const char RUNNING[] = "Running %d instances.";
    const void* text = FindBytes(game, RUNNING, sizeof(RUNNING));
    BYTE* lea = text ? FindLeaTo(game, text) : nullptr;
    for (BYTE* p = lea; p && p < lea + 0x30; p++) {
        if (p[0] != 0x83 || (p[1] & 0x38) != 0x38) {
            continue;
        }
        int dispSize = (p[1] & 0xC0) == 0x80 ? 4 : (p[1] & 0xC0) == 0x40 ? 1 : -1;
        if (dispSize < 0 || (p[1] & 7) == 4) {
            continue;
        }
        BYTE* jump = p + 3 + dispSize;
        if (jump[-1] != 2) {
            continue;
        }
        if (jump[0] == 0x0F && jump[1] == 0x8C) {
            static const BYTE NEAR_JMP[] = { 0x90, 0xE9 };
            PatchMemory(jump, NEAR_JMP, sizeof(NEAR_JMP));
        } else if (jump[0] == 0x7C) {
            static const BYTE SHORT_JMP = 0xEB;
            PatchMemory(jump, &SHORT_JMP, 1);
        } else {
            continue;
        }
        ModsLog("multi client: instance check at %p", p);
        return;
    }
    ModsLog("multi client: instance check not found");
}
