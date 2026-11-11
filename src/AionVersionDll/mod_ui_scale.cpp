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
        ModsLog("glyph cells: not needed in this client, only 5.x has the check");
        return;
    }
    BYTE limit = (BYTE)(g_modsConfig.largeGlyphFrom - 1);
    PatchMemory(p + 4, &limit, 1);
    ModsLog("glyph cells: large from %d px", g_modsConfig.largeGlyphFrom);
}

/// Scales the UI as 4.x clients do: the option is a share of the screen size relative to 1280x960 (2.25 on a 3840x2160 screen),
/// so 100 % fits the screen at any resolution and the slider goes from 70 to 100 %. 5.x takes the option as the scale itself
/// (70 to 130 %) and only caps it at the screen size.
void InstallUiScale(HMODULE game) {
    // mov dword ptr [rax+sliderMax], 60.0f
    std::vector<BYTE*> sliders = FindAllPatterns(game, "C7 80 ?? ?? 00 00 00 00 70 42");
    // cmp eax, 70; jge; mov eax, 70; jmp; mov eax, 130; cmp ecx, eax; cmovl eax, ecx; then the percentage becomes the scale:
    // movd xmmN, eax; cvtdq2pd xmmN, xmmN; divsd xmmN, [100.0]; and the screen size in xmm6 caps it: comisd xmmN, xmm6 or xmm6, xmmN
    std::vector<BYTE*> clamps = FindAllPatterns(game, "83 F8 46 7D 07 B8 46 00 00 00 EB 0A B8 82 00 00 00 3B C8 0F 4C C1 "
                                                      "66 0F 6E ?? F3 0F E6 ?? F2 0F 5E ?? ?? ?? ?? ?? 66 0F 2F");
    // the same clamp when the older ui_scale_factor option is converted
    BYTE* conversion = FindPattern(game, "83 F9 46 7D 09 C7 45 ?? 46 00 00 00 EB 0D B8 82 00 00 00 3B C8");
    ModsLog("ui scale: %zu sliders, %zu clamps, conversion=%p", sliders.size(), clamps.size(), conversion);
    if (sliders.empty() || clamps.empty()) {
        return;
    }

    static constexpr int CLAMP_SIZE = 22;
    static constexpr int CODE_SIZE = CLAMP_SIZE + 16;
    std::vector<BYTE*> fits;
    for (BYTE* clamp : clamps) {
        BYTE* movd = clamp + CLAMP_SIZE;
        int reg = (movd[3] >> 3) & 7;
        BYTE* divsd = movd + 8;
        BYTE* comisd = divsd + 8;
        bool sameRegister = movd[3] == (0xC0 | reg << 3) && movd[7] == (0xC0 | reg << 3 | reg) && divsd[3] == (0x05 | reg << 3);
        bool screenInXmm6 = comisd[3] == (0xC0 | reg << 3 | 6) || comisd[3] == (0xC0 | 6 << 3 | reg);
        if (sameRegister && screenInXmm6) {
            fits.push_back(clamp);
        }
    }
    if (fits.size() != clamps.size()) {
        ModsLog("ui scale: only %zu of the clamps have the screen size at hand, nothing changed", fits.size());
        return;
    }
    for (BYTE* clamp : fits) {
        int reg = (clamp[CLAMP_SIZE + 3] >> 3) & 7;
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
        Patch(clamp, code, sizeof(code));
    }

    INT32 maxPercent = 100;
    float sliderRange = (float)(maxPercent - MIN_PERCENT);
    for (BYTE* slider : sliders) {
        Patch(slider + 6, &sliderRange, sizeof(sliderRange));
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
    ModsLog("ui scale: fits the screen at %zu places, option range %p", fits.size(), entry);
    if (entry) {
        Patch(entry + offsetof(OptionRange, max), &maxPercent, sizeof(maxPercent));
    }
}
