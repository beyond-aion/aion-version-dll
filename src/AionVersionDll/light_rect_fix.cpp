#include "code_patch.h"
#include "module_load.h"
#include "mods_log.h"
#include <cmath>
#include <set>
#include <vector>
#include <detours/detours.h>

// The High Quality engine shades each point light in a screen rectangle around it. The renderer takes that rectangle from the two corners
// of a square of the light's radius facing the camera through the light's center, which is smaller than the light's sphere on screen
// near the camera and towards the edges of the screen: the light then ends in a straight line that moves with the camera. With a corner
// behind the camera it gives up, and the light keeps the rectangle of the light before it. The rectangle is taken from the eight corners of
// the box around the sphere instead, and is the whole screen once the camera is close enough for one of them to lie behind it.
//
// The renderer's methods and the light's fields sit at other offsets in each client version (3.9 has them 8 and 4 bytes lower than
// 4.x and 5.x), so they are read from the code of the function being replaced.

struct Renderer;
typedef int(__fastcall* GetSize_t)(Renderer*);
typedef void(__fastcall* ProjectToScreen_t)(Renderer*, float x, float y, float z, float* sx, float* sy, float* sz);

/// Offsets of the renderer's vtable slots and of the light's fields the function uses.
struct Layout {
    int width, height, project;
    int position, radius;

    bool operator==(const Layout& other) const {
        return width == other.width && height == other.height && project == other.project && position == other.position
            && radius == other.radius;
    }
};

static Layout s_layout;

template <typename T>
static T Slot(Renderer* renderer, int offset) {
    return *(T*)(*(BYTE**)renderer + offset);
}

static bool __fastcall LightRect(void* self, Renderer* renderer, const BYTE* light, int* x, int* y, int* width, int* height) {
    int screenWidth = Slot<GetSize_t>(renderer, s_layout.width)(renderer);
    int screenHeight = Slot<GetSize_t>(renderer, s_layout.height)(renderer);
    ProjectToScreen_t project = Slot<ProjectToScreen_t>(renderer, s_layout.project);
    const float* center = (const float*)(light + s_layout.position);
    float radius = *(const float*)(light + s_layout.radius);
    float left = INFINITY, top = INFINITY, right = -INFINITY, bottom = -INFINITY;
    bool inFront = true;
    for (int corner = 0; corner < 8 && inFront; corner++) {
        float sx, sy, sz;
        project(renderer, center[0] + (corner & 1 ? radius : -radius), center[1] + (corner & 2 ? radius : -radius),
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

/// Reads the layout from the code of the function from start to end, whose depth test of the first projected corner is at check. The
/// renderer's methods are called through its vtable: GetWidth and GetHeight in neighbouring slots, ProjectToScreen last before the
/// depth test apart from those two. The light's center is added to the corners as three floats in a row, its radius loaded after them.
static bool ReadLayout(const BYTE* start, const BYTE* end, const BYTE* check, Layout* layout) {
    struct Call {
        const BYTE* at;
        int slot;
    };
    std::vector<Call> calls;
    std::set<int> slots;
    // call qword ptr [reg+disp32]
    for (const BYTE* p = start; p + 6 <= end; p++) {
        if (p[0] == 0xFF && (p[1] & 0xC0) == 0x80 && ((p[1] >> 3) & 7) == 2 && (p[1] & 7) != 4) {
            int slot = *(const INT32*)(p + 2);
            calls.push_back({ p, slot });
            slots.insert(slot);
        }
    }
    int pairs = 0;
    for (int slot : slots) {
        if (slots.count(slot + 8)) {
            layout->width = slot;
            pairs++;
        }
    }
    if (pairs != 1) {
        return false;
    }
    layout->height = layout->width + 8;
    layout->project = -1;
    for (const Call& call : calls) {
        if (call.at < check && call.slot != layout->width && call.slot != layout->height) {
            layout->project = call.slot;
        }
    }
    if (layout->project < 0) {
        return false;
    }
    // movss or addss xmm, dword ptr [base+disp8 or disp32], by base register
    std::set<int> adds[16], loads[16];
    for (const BYTE* p = start; p + 8 <= end; p++) {
        if (p[0] != 0xF3) {
            continue;
        }
        const BYTE* q = p + 1;
        int rex = *q >= 0x40 && *q <= 0x4F ? *q++ : 0;
        if (q[0] != 0x0F || (q[1] != 0x10 && q[1] != 0x58)) {
            continue;
        }
        int mod = q[2] >> 6, rm = q[2] & 7;
        if ((mod != 1 && mod != 2) || rm == 4) {
            continue;
        }
        int disp = mod == 1 ? (signed char)q[3] : *(const INT32*)(q + 3);
        int base = rm | (rex & 1) << 3;
        (q[1] == 0x58 ? adds : loads)[base].insert(disp);
    }
    for (int base = 0; base < 16; base++) {
        // locals on the frame are no light
        if (base == 4 || base == 5) {
            continue;
        }
        for (int position : adds[base]) {
            if (!adds[base].count(position + 4) || !adds[base].count(position + 8)) {
                continue;
            }
            auto radius = loads[base].upper_bound(position + 8);
            if (radius != loads[base].end()) {
                layout->position = position;
                layout->radius = *radius;
                return true;
            }
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
        Layout layout;
        if (!function || hooked == _countof(targets) || !ReadLayout(function, FunctionBodyEnd(function, 0x1000), check, &layout)) {
            continue;
        }
        // both functions are replaced by the same one, which knows a single layout
        if (hooked && !(layout == s_layout)) {
            ModsLog("light rect fix: %p has another layout, left alone", function);
            continue;
        }
        s_layout = layout;
        targets[hooked] = function;
        if (DetourAttach(&targets[hooked], LightRect) == NO_ERROR) {
            ModsLog("light rect fix: hooked at %p (renderer %X %X %X, light %X %X)", function, layout.width, layout.height, layout.project,
                layout.position, layout.radius);
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
