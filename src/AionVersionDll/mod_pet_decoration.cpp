#include "mods.h"

// The world applies the decorations sent with a pet (the three attachment ids of the appearance block) only when the pet type allows
// them. 4.8 asks for customize_color there, which is 0 for every pet type, while the adoption preview and the pet window ask for
// customize_attach, so a summoned pet never wears what the player picked. The mod drops that check; the ids still apply only when
// nonzero, so a pet sent without them keeps its defaults. 4.6, 5.8 and 3.9 already ask for customize_attach, and there the mod only
// logs which flag the check reads.
namespace {

// The three attachment ids, each copied into its slot only when nonzero. In 4.8 the leading jz is the customize_color check.
const char* const PATTERN_48 =
    "74 ?? 41 8B 44 24 05 85 C0 74 ?? 89 44 24 ?? C6 44 24 ?? 01 "
    "41 8B 44 24 09 85 C0 74 ?? 89 44 24 ?? C6 44 24 ?? 01 "
    "41 8B 44 24 0D 85 C0";
const char* const PATTERN_46 =
    "74 ?? 8B 47 05 85 C0 74 ?? 89 44 24 ?? C6 44 24 ?? 01 "
    "8B 47 09 85 C0 74 ?? 89 44 24 ?? C6 44 24 ?? 01 "
    "8B 47 0D 85 C0";
const char* const PATTERN_39 =
    "74 ?? 8B 47 05 85 C0 74 ?? 41 83 CB 01 89 44 24 ?? 44 89 5C 24 ?? "
    "8B 47 09 85 C0 74 ?? 83 CA 01 89 44 24 ?? 89 54 24 ?? "
    "8B 47 0D 85 C0";
const char* const PATTERN_58 =
    "74 ?? 8B 47 05 85 C0 74 ?? 89 45 ?? C6 45 ?? 01 "
    "8B 47 09 85 C0 74 ?? 89 45 ?? C6 45 ?? 01 "
    "8B 47 0D 85 C0";

/// 4.6 and 3.9: test byte ptr [reg+5Ch], mask; mask 1 is customize_color, 2 is customize_attach. The nearest such test before the
/// block is its check (0x45 bytes up in 4.6); the one further up gates the color. null when there is none.
const char* FlagOfTest(HMODULE game, const BYTE* block) {
    for (int back = 4; back <= 0x100; back++) {
        const BYTE* p = block - back;
        if (!InModule(game, p, 4)) {
            break;
        }
        // F6 /0 with a one-byte displacement and no SIB byte
        if (p[0] == 0xF6 && (p[1] & 0xF8) == 0x40 && (p[1] & 7) != 4 && p[2] == 0x5C && (p[3] == 1 || p[3] == 2)) {
            return p[3] == 2 ? "customize_attach" : "customize_color";
        }
    }
    return nullptr;
}

/// 5.8: shr eax, bit; and eax, 1 right before the block; bit 1 is customize_color, 2 is customize_attach. null when it is not there.
const char* FlagOfShift(HMODULE game, const BYTE* block) {
    const BYTE* p = block - 6;
    if (!InModule(game, p, 6) || p[0] != 0xC1 || p[1] != 0xE8 || p[3] != 0x83 || p[4] != 0xE0 || p[5] != 0x01) {
        return nullptr;
    }
    return p[2] == 2 ? "customize_attach" : p[2] == 1 ? "customize_color" : nullptr;
}

/// The clients whose check is left as it is.
const struct {
    const char* version;
    const char* pattern;
    const char* (*flag)(HMODULE game, const BYTE* block);
} CHECKED[] = {
    { "4.6", PATTERN_46, FlagOfTest },
    { "5.8", PATTERN_58, FlagOfShift },
    { "3.9", PATTERN_39, FlagOfTest },
};

} // namespace

void InstallPetDecoration(HMODULE game) {
    if (BYTE* block = FindPattern(game, PATTERN_48)) {
        static const BYTE FALL_THROUGH[2] = { 0x90, 0x90 };
        bool done = PatchMemory(block, FALL_THROUGH, sizeof(FALL_THROUGH));
        ModsLog("pet decoration 4.8: at %p, customize_color check %s", block, done ? "skipped" : "not patched");
        return;
    }
    for (const auto& client : CHECKED) {
        if (BYTE* block = FindPattern(game, client.pattern)) {
            const char* flag = client.flag(game, block);
            ModsLog("pet decoration %s: at %p, the check reads %s, left as it is", client.version, block, flag ? flag : "an unknown flag");
            return;
        }
    }
    ModsLog("pet decoration: not found");
}
