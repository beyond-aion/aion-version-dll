#include "mods.h"
#include <stddef.h>

// The UI scale option is a percentage from 70 on, the slider covers the range above that.
static constexpr int MIN_PERCENT = 70;
// fonts taller than 32 pixels get the larger glyph cells
static constexpr int CLIENT_LARGE_GLYPH_FROM = 33;

static void Patch(BYTE* at, const void* value, size_t size) {
    DWORD oldProtect;
    if (VirtualProtect(at, size, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        memcpy(at, value, size);
        VirtualProtect(at, size, oldProtect, &oldProtect);
        FlushInstructionCache(GetCurrentProcess(), at, size);
    }
}

/// Glyphs are rendered into atlas cells of 32 pixels, or 64 for fonts taller than 32 pixels. With the UI scaled up, fonts
/// just below that limit get their descenders cut off by the cell, so the larger cells start at a lower height.
void InstallGlyphCells(HMODULE cryFont) {
    if (g_modsConfig.largeGlyphFrom == CLIENT_LARGE_GLYPH_FROM) {
        return;
    }
    // cmp dword ptr [r14+tmHeight], 32; jle; mov dword ptr [r14+largeCells], 1
    BYTE* p = FindPattern(cryFont, "41 83 7E ?? 20 7E ?? 41 C7 86 ?? ?? 00 00 01 00 00 00");
    if (!p) {
        ModsLog("glyph cells: check not found");
        return;
    }
    BYTE limit = (BYTE)(g_modsConfig.largeGlyphFrom - 1);
    PatchMemory(p + 4, &limit, 1);
    ModsLog("glyph cells: large from %d px", g_modsConfig.largeGlyphFrom);
}

/// Lets the UI scale option go past 130 %. The client still caps the result at the screen size relative to 1280x960,
/// which is 225 % on a 3840x2160 screen.
void InstallUiScale(HMODULE game) {
    INT32 maxPercent = g_modsConfig.uiScaleMax;
    float sliderRange = (float)(maxPercent - MIN_PERCENT);

    // mov dword ptr [rax+sliderMax], 60.0f
    std::vector<BYTE*> sliders = FindAllPatterns(game, "C7 80 ?? ?? 00 00 00 00 70 42");
    // cmp eax, 70; jge; mov eax, 70; jmp; mov eax, 130; cmp ecx, eax; cmovl eax, ecx
    std::vector<BYTE*> clamps = FindAllPatterns(game, "83 F8 46 7D 07 B8 46 00 00 00 EB 0A B8 82 00 00 00 3B C8 0F 4C C1");
    // the same clamp when the older ui_scale_factor option is converted
    BYTE* conversion = FindPattern(game, "83 F9 46 7D 09 C7 45 ?? 46 00 00 00 EB 0D B8 82 00 00 00 3B C8");
    ModsLog("ui scale: %zu sliders, %zu clamps, conversion=%p, max=%d%%", sliders.size(), clamps.size(), conversion, maxPercent);
    if (sliders.empty() || clamps.empty()) {
        return;
    }
    for (BYTE* slider : sliders) {
        Patch(slider + 6, &sliderRange, sizeof(sliderRange));
    }
    for (BYTE* clamp : clamps) {
        Patch(clamp + 13, &maxPercent, sizeof(maxPercent));
    }
    if (conversion) {
        Patch(conversion + 15, &maxPercent, sizeof(maxPercent));
    }

    // the option table limits every stored value: { name, min, max, ... }
    static const wchar_t OPTION_NAME[] = L"ui_scale_fix";
    const wchar_t* name = (const wchar_t*)FindBytes(game, OPTION_NAME, sizeof(OPTION_NAME));
    struct OptionRange {
        const wchar_t* name;
        INT32 min;
        INT32 max;
    } range = { name, MIN_PERCENT, 130 };
    BYTE* entry = name ? FindBytes(game, &range, sizeof(range)) : nullptr;
    ModsLog("ui scale: option range %p", entry);
    if (entry) {
        Patch(entry + offsetof(OptionRange, max), &maxPercent, sizeof(maxPercent));
    }
}
