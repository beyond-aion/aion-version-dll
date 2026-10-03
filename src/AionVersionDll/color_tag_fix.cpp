#include "mods.h"

/// Older clients (4.6) cut the text of a color tag to the length of the tag name "color:" minus one, so 5 characters; later
/// ones take 127. The limit is set up before the copy loop as lea r9d, [r12-1] with r12 the name length (6), and turned into
/// lea r9d, [r12+79h] = 127.
void InstallColorTagFix(HMODULE game) {
    BYTE* p = FindPattern(game, "45 8D 4C 24 FF 48 89 B4 24 ?? ?? ?? ?? 48 8D 74 7B 02 33 FF 45 85 C9 8B D7 8B DF 7E ?? 4C 8B C5 48 8B CE "
                                "4C 2B C6 0F B7 01 66 85 C0 74 ?? 66 3D 3B 00 74 ?? 66 3D 5D 00");
    if (!p) {
        return;
    }
    BYTE displacement = 0x79;
    PatchMemory(p + 4, &displacement, 1);
    ModsLog("color tag text length fixed at %p", p);
}
