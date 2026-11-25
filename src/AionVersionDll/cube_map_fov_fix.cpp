#include "code_patch.h"
#include "module_load.h"
#include "mods_log.h"
#include <cstring>
#include <detours/detours.h>
#include "install_report.h"

// The renderer renders the environment cube maps the water and other shiny surfaces reflect as six pictures, one per face, each with
// a square camera of 90 degrees. Its camera setter builds the projection with fovY = min(ratio, 0.8) * fov unless a byte of the camera
// says otherwise, a limit for tall screens that also catches these square cameras: the faces are drawn with 72 degrees, each covers
// +-36 degrees of the +-45 it stands for, and the reflection shows straight seams where the faces meet (the sky above the seam a
// different color from the sky below it). For the cube face cameras (fov exactly pi/2, ratio exactly 1) the byte is set while the
// setter runs. The check, its byte and the fov and ratio fields are the same in 4.6 to 5.8.

namespace {

typedef void(__fastcall* SetCamera_t)(void* renderer, BYTE* camera);
SetCamera_t g_realSetCamera = nullptr;
int g_noLimitByte = 0, g_fovField = 0, g_ratioField = 0;

const DWORD kCubeFaceFov = 0x3FC90FDB; // pi/2 as the cube face camera stores it
const DWORD kSquare = 0x3F800000;      // 1.0

void __fastcall SetCameraHook(void* renderer, BYTE* camera) {
    DWORD fov, ratio;
    memcpy(&fov, camera + g_fovField, 4);
    memcpy(&ratio, camera + g_ratioField, 4);
    if (fov == kCubeFaceFov && ratio == kSquare && !camera[g_noLimitByte]) {
        camera[g_noLimitByte] = 1;
        g_realSetCamera(renderer, camera);
        camera[g_noLimitByte] = 0;
        return;
    }
    g_realSetCamera(renderer, camera);
}

struct Place {
    BYTE* setter = nullptr;
    int noLimit = 0, fov = 0, ratio = 0;
};

Place Locate(HMODULE renderer) {
    Place place;
    // cmp byte [rsi+noLimit], 0; movaps xmm1, xmm0; jnz skip; movss xmm0, [0.8]; comiss xmm1, xmm0; jbe skip; movaps xmm1, xmm0;
    // movss xmm0, [rsi+near]; divss xmm6, [rsi+ratio]; mulss xmm1, [rsi+fov]
    size_t count = 0;
    BYTE* check = FindPattern(renderer,
        "80 7E ?? 00 0F 28 C8 75 ?? F3 0F 10 05 ?? ?? ?? ?? 0F 2F C8 76 ?? 0F 28 C8 F3 0F 10 86 ?? ?? ?? ?? F3 0F 5E 76 ?? F3 0F 59 4E ??",
        &count);
    if (!check || count != 1) {
        return place;
    }
    float limit;
    memcpy(&limit, check + 17 + *(int*)(check + 13), 4);
    BYTE* setter = FunctionStart(check);
    if (limit != 0.8f || !setter) {
        return place;
    }
    place.setter = setter;
    place.noLimit = check[2];
    place.ratio = check[37];
    place.fov = check[42];
    return place;
}

void Apply(const Place& place) {
    if (!place.setter) {
        ModsLog("cube map fov fix: not found");
        return;
    }
    g_noLimitByte = place.noLimit;
    g_fovField = place.fov;
    g_ratioField = place.ratio;
    g_realSetCamera = (SetCamera_t)place.setter;
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    if (AttachHook((PVOID*)&g_realSetCamera, SetCameraHook) == NO_ERROR) {
        ModsLog("cube map fov fix: hooked at %p (byte %X, fov %X, ratio %X)", place.setter, place.noLimit, place.fov, place.ratio);
    }
    CommitHooks();
}

void InstallCubeMapFovFix(HMODULE renderer) {
    InstallScope scope("cube_map_fov_fix");
    LocateThenApply(renderer, Locate, Apply);
}

} // namespace

void InstallCubeMapFovFixOnLoad() {
    OnModuleLoad(L"XRenderD3D9.dll", InstallCubeMapFovFix, true);
}
