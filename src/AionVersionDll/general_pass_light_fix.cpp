#include "code_patch.h"
#include "module_load.h"
#include "mods_log.h"
#include <cstdio>
#include <detours/detours.h>
#include "install_report.h"

// The renderer draws some passes of a render item in its general pass (the one profiled as "Renderer:DrawShader_GeneralPasses"),
// which does not choose a light: the shaders there that light the surface by the sun read the position of the renderer's current
// light, and that is whatever the item drawn before left, the sun after an item lit by it and none after an item drawn with its ambient
// light only. With none the position reads as zero and the surface turns dark. Which item comes before changes with what is in view, so
// walls of the Gelkmaros fortress went dark and back with a few degrees of camera turn. Before the general pass the first light of the
// item, the sun for these, is made current when no light is.
//
// The renderer keeps the current light, then 16 bytes further the number of the item's lights and the list of them; where the current
// light sits is read from the getter of the shader parameter $LightPos (0x630 in 4.6 to 5.8 alike).

extern "C" void* g_generalPassResume = nullptr;
extern "C" INT64 g_currentLightOffset = 0;
extern "C" INT64 g_itemLightCountOffset = 0;
extern "C" INT64 g_itemLightsOffset = 0;
extern "C" void GeneralPassLightStub();

namespace {

struct Place {
    BYTE* general = nullptr;
    int current = 0;
};

Place Locate(HMODULE renderer) {
    Place place;
    static const char label[] = "Renderer:DrawShader_GeneralPasses";
    BYTE* text = FindBytes(renderer, label, sizeof(label));
    std::vector<BYTE*> leas = text ? FindAllLeasTo(renderer, text) : std::vector<BYTE*>();
    if (leas.size() != 1) {
        return place;
    }
    BYTE* general = FunctionStart(leas[0]);
    // the getter of $LightPos: movsxd rdx, [rcx+10h]; mov rax, [renderer]; mov rcx, [rax+current]; movss xmm0, [rcx+rdx*4+4]; ret
    BYTE* getter = FindPattern(renderer, "48 63 51 10 48 8B 05 ?? ?? ?? ?? 48 8B 88 ?? ?? ?? ?? F3 0F 10 44 91 04 C3");
    if (!general || !getter) {
        return place;
    }
    int current = *(int*)(getter + 14);
    // the light list builder stops at 16 lights: cmp dword [reg+count], 10h, with count 16 bytes after the current light
    int count = current + 0x10;
    char pattern[32];
    sprintf_s(pattern, "83 ?? %02X %02X %02X %02X 10", count & 0xFF, (count >> 8) & 0xFF, (count >> 16) & 0xFF, (count >> 24) & 0xFF);
    if (FindAllPatterns(renderer, pattern).empty()) {
        return place;
    }
    place.general = general;
    place.current = current;
    return place;
}

void Apply(const Place& place) {
    if (!place.general) {
        ModsLog("general pass light fix: not found");
        return;
    }
    g_currentLightOffset = place.current;
    g_itemLightCountOffset = place.current + 0x10;
    g_itemLightsOffset = place.current + 0x18;
    g_generalPassResume = place.general;
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    if (AttachHook(&g_generalPassResume, GeneralPassLightStub) == NO_ERROR) {
        ModsLog("general pass light fix: hooked at %p (current light %X)", place.general, place.current);
    }
    CommitHooks();
}

void InstallGeneralPassLightFix(HMODULE renderer) {
    InstallScope scope("general_pass_light_fix");
    LocateThenApply(renderer, Locate, Apply);
}

} // namespace

void InstallGeneralPassLightFixOnLoad() {
    OnModuleLoad(L"XRenderD3D9.dll", InstallGeneralPassLightFix, true);
}
