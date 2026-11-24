#include "mods.h"
#include <stddef.h>

// The UI scale option is a percentage from 70 on, the slider covers the range above that.
static constexpr int MIN_PERCENT = 70;

namespace {

/// Glyphs are rendered into atlas cells of 32 pixels, or 64 for fonts taller than 32 pixels: cmp dword ptr [r14+tmHeight], 32; jle;
/// mov dword ptr [r14+largeCells], 1. Only 5.x has the check; the 32 is kept.
CodePatch LocateGlyphCells(HMODULE cryFont) {
    BYTE* p = FindPattern(cryFont, "41 83 7E ?? 20 7E ?? 41 C7 86 ?? ?? 00 00 01 00 00 00");
    return p ? ExpectBytes(p + 4, 1) : CodePatch();
}

/// With the UI scaled up, fonts just below that limit get their descenders cut off by the cell, so the larger cells start at a lower
/// height.
void ApplyGlyphCells(const CodePatch& limit) {
    if (!limit) {
        ModsLog("glyph cells: not needed in this client, only 5.x has the check");
        ReportStatus(InstallStatus::NotApplicable);
        return;
    }
    BYTE value = (BYTE)(g_modsConfig.largeGlyphFrom - 1);
    ApplyPatch(limit, &value);
    ModsLog("glyph cells: large from %d px", g_modsConfig.largeGlyphFrom);
}

// the option table limits every stored value: { name, min, max, ... }
struct OptionRange {
    const wchar_t* name;
    INT32 min;
    INT32 max;
};

struct ScalePlaces {
    size_t clampCount = 0;
    size_t fitCount = 0;
    BYTE* conversionAt = nullptr;
    BYTE* optionEntry = nullptr;
    // each clamp rewritten to scale by the screen size, the slider ranges, the clamp of the older option and the option's maximum
    std::vector<CodePatch> clamps;
    std::vector<CodePatch> sliders;
    CodePatch conversion;
    CodePatch optionMax;
};

constexpr INT32 MAX_PERCENT = 100;

/// Scales the UI as 4.x clients do: the option is a share of the screen size relative to 1280x960 (2.25 on a 3840x2160 screen),
/// so 100 % fits the screen at any resolution and the slider goes from 70 to 100 %. 5.x takes the option as the scale itself
/// (70 to 130 %) and only caps it at the screen size.
ScalePlaces LocateUiScale(HMODULE game) {
    ScalePlaces places;
    // mov dword ptr [rax+sliderMax], 60.0f
    std::vector<BYTE*> sliders = FindAllPatterns(game, "C7 80 ?? ?? 00 00 00 00 70 42");
    // cmp eax, 70; jge; mov eax, 70; jmp; mov eax, 130; cmp ecx, eax; cmovl eax, ecx; then the percentage becomes the scale:
    // movd xmmN, eax; cvtdq2pd xmmN, xmmN; divsd xmmN, [100.0]; and the screen size in xmm6 caps it: comisd xmmN, xmm6 or xmm6, xmmN
    std::vector<BYTE*> clamps = FindAllPatterns(game, "83 F8 46 7D 07 B8 46 00 00 00 EB 0A B8 82 00 00 00 3B C8 0F 4C C1 "
                                                      "66 0F 6E ?? F3 0F E6 ?? F2 0F 5E ?? ?? ?? ?? ?? 66 0F 2F");
    // the same clamp when the older ui_scale_factor option is converted
    BYTE* conversion = FindPattern(game, "83 F9 46 7D 09 C7 45 ?? 46 00 00 00 EB 0D B8 82 00 00 00 3B C8");
    places.clampCount = clamps.size();
    places.conversionAt = conversion;
    for (BYTE* slider : sliders) {
        places.sliders.push_back(ExpectBytes(slider + 6, sizeof(float)));
    }
    if (sliders.empty() || clamps.empty()) {
        return places;
    }
    static constexpr int CLAMP_SIZE = 22;
    static constexpr int CODE_SIZE = CLAMP_SIZE + 16;
    for (BYTE* clamp : clamps) {
        BYTE* movd = clamp + CLAMP_SIZE;
        int reg = (movd[3] >> 3) & 7;
        BYTE* divsd = movd + 8;
        BYTE* comisd = divsd + 8;
        bool sameRegister = movd[3] == (0xC0 | reg << 3) && movd[7] == (0xC0 | reg << 3 | reg) && divsd[3] == (0x05 | reg << 3);
        bool screenInXmm6 = comisd[3] == (0xC0 | reg << 3 | 6) || comisd[3] == (0xC0 | 6 << 3 | reg);
        if (!sameRegister || !screenInXmm6) {
            continue;
        }
        places.fitCount++;
        BYTE* hundred = ResolveRip(clamp + CLAMP_SIZE + 12, clamp + CLAMP_SIZE + 16);
        // cmp eax, 100; jle; mov eax, 100; movd xmmN, eax; cvtdq2pd xmmN, xmmN; mulsd xmmN, xmm6; divsd xmmN, [100.0]
        BYTE code[CODE_SIZE] = { 0x83, 0xF8, 0x64, 0x7E, 0x05, 0xB8, 0x64, 0x00, 0x00, 0x00,
            0x66, 0x0F, 0x6E, (BYTE)(0xC0 | reg << 3),
            0xF3, 0x0F, 0xE6, (BYTE)(0xC0 | reg << 3 | reg),
            0xF2, 0x0F, 0x59, (BYTE)(0xC0 | reg << 3 | 6),
            0xF2, 0x0F, 0x5E, (BYTE)(0x05 | reg << 3) };
        INT32 displacement = (INT32)(hundred - (clamp + 30));
        memcpy(code + 26, &displacement, sizeof(displacement));
        // the bytes left over: one long nop
        static const BYTE NOP8[] = { 0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00 };
        memcpy(code + 30, NOP8, sizeof(NOP8));
        places.clamps.push_back(PlanPatch(clamp, code, sizeof(code)));
    }
    if (conversion) {
        places.conversion = PlanPatch(conversion + 15, &MAX_PERCENT, sizeof(MAX_PERCENT));
    }
    static const wchar_t OPTION_NAME[] = L"ui_scale_fix";
    const wchar_t* name = (const wchar_t*)FindBytes(game, OPTION_NAME, sizeof(OPTION_NAME));
    OptionRange range = { name, MIN_PERCENT, 130 };
    places.optionEntry = name ? FindBytes(game, &range, sizeof(range)) : nullptr;
    if (places.optionEntry) {
        places.optionMax = PlanPatch(places.optionEntry + offsetof(OptionRange, max), &MAX_PERCENT, sizeof(MAX_PERCENT));
    }
    return places;
}

void ApplyUiScale(const ScalePlaces& places) {
    ModsLog("ui scale: %zu sliders, %zu clamps, conversion=%p", places.sliders.size(), places.clampCount, places.conversionAt);
    if (places.sliders.empty() || places.clampCount == 0) {
        return;
    }
    if (places.fitCount != places.clampCount) {
        ModsLog("ui scale: only %zu of the clamps have the screen size at hand, nothing changed", places.fitCount);
        ReportStatus(InstallStatus::PatchFailed);
        return;
    }
    for (const CodePatch& clamp : places.clamps) {
        ApplyPatch(clamp);
    }
    float sliderRange = (float)(MAX_PERCENT - MIN_PERCENT);
    for (const CodePatch& slider : places.sliders) {
        ApplyPatch(slider, &sliderRange);
    }
    ApplyPatch(places.conversion);
    ModsLog("ui scale: fits the screen at %zu places, option range %p", places.fitCount, places.optionEntry);
    ApplyPatch(places.optionMax);
}

} // namespace

void InstallGlyphCells(HMODULE cryFont) {
    InstallScope scope("glyph_cells");
    LocateThenApply(cryFont, LocateGlyphCells, ApplyGlyphCells);
}

void InstallUiScale(HMODULE game) {
    LocateThenApply(game, LocateUiScale, ApplyUiScale);
}
