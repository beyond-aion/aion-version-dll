#include "code_patch.h"
#include "module_load.h"
#include "mods_log.h"
#include <cmath>
#include <vector>
#include <detours/detours.h>

// The High Quality engine shades each point light in a screen rectangle around it. The renderer takes that rectangle from the two corners
// of a square of the light's radius facing the camera through the light's center, which is smaller than the light's sphere on screen
// near the camera and towards the edges of the screen: the light then ends in a straight line that moves with the camera. With a corner
// behind the camera it gives up, and the light keeps the rectangle of the light before it. The rectangle is taken from the eight corners of
// the box around the sphere instead, and is the whole screen once the camera is close enough for one of them to lie behind it.

struct Renderer;

struct RendererVtbl {
    BYTE before[0x218];
    int(__fastcall* GetWidth)(Renderer*);
    int(__fastcall* GetHeight)(Renderer*);
    BYTE between[0x278 - 0x228];
    void(__fastcall* ProjectToScreen)(Renderer*, float x, float y, float z, float* sx, float* sy, float* sz);
};

struct Renderer {
    RendererVtbl* vtbl;
};

typedef bool(__fastcall* LightRect_t)(void* self, Renderer* renderer, const BYTE* light, int* x, int* y, int* width, int* height);

static constexpr int LIGHT_POSITION = 0x70;
static constexpr int LIGHT_RADIUS = 0x80;

static bool __fastcall LightRect(void* self, Renderer* renderer, const BYTE* light, int* x, int* y, int* width, int* height) {
    int screenWidth = renderer->vtbl->GetWidth(renderer);
    int screenHeight = renderer->vtbl->GetHeight(renderer);
    const float* center = (const float*)(light + LIGHT_POSITION);
    float radius = *(const float*)(light + LIGHT_RADIUS);
    float left = INFINITY, top = INFINITY, right = -INFINITY, bottom = -INFINITY;
    bool inFront = true;
    for (int corner = 0; corner < 8 && inFront; corner++) {
        float sx, sy, sz;
        renderer->vtbl->ProjectToScreen(renderer, center[0] + (corner & 1 ? radius : -radius), center[1] + (corner & 2 ? radius : -radius),
            center[2] + (corner & 4 ? radius : -radius), &sx, &sy, &sz);
        // the screen position comes in percent of the screen
        sx = sx * screenWidth / 100;
        sy = sy * screenHeight / 100;
        inFront = sz >= 0 && sz <= 1 && std::isfinite(sx) && std::isfinite(sy);
        left = min(left, sx);
        top = min(top, sy);
        right = max(right, sx);
        bottom = max(bottom, sy);
    }
    if (!inFront) {
        left = top = 0;
        right = (float)screenWidth;
        bottom = (float)screenHeight;
    }
    *x = (int)max(left, 0.0f);
    *y = (int)max(top, 0.0f);
    *width = min((int)(right - left) + 1, screenWidth);
    *height = min((int)(bottom - top) + 1, screenHeight);
    return true;
}

/// Whether the code from start to end calls ProjectToScreen through the renderer's vtable: call qword ptr [rax+278h]
static bool ProjectsBefore(const BYTE* start, const BYTE* end) {
    static const BYTE CALL[] = { 0xFF, 0x90, 0x78, 0x02, 0x00, 0x00 };
    for (const BYTE* p = start; p + sizeof(CALL) <= end; p++) {
        if (memcmp(p, CALL, sizeof(CALL)) == 0) {
            return true;
        }
    }
    return false;
}

static void InstallLightRectFix(HMODULE renderer) {
    // the depth test of the first projected corner: comiss xmm7, xmm0; ja fail; movss xmm6, [1.0]; comiss xmm0, xmm6; ja fail
    std::vector<BYTE*> checks = FindAllPatterns(renderer, "0F 2F F8 0F 87 ?? ?? ?? ?? F3 0F 10 35 ?? ?? ?? ?? 0F 2F C6 0F 87");
    // one for the point lights and one for the air light
    static PVOID targets[2];
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    int hooked = 0;
    for (BYTE* check : checks) {
        BYTE* function = FunctionStart(check);
        if (!function || hooked == _countof(targets) || !ProjectsBefore(function, check)) {
            continue;
        }
        targets[hooked] = function;
        if (DetourAttach(&targets[hooked], LightRect) == NO_ERROR) {
            ModsLog("light rect fix: hooked at %p", function);
            hooked++;
        }
    }
    DetourTransactionCommit();
    if (!hooked) {
        ModsLog("light rect fix: not found");
    }
}

void InstallLightRectFixOnLoad() {
    OnModuleLoad(L"XRenderD3D9.dll", InstallLightRectFix, true);
}
