#include "mods.h"
#include <math.h>
#include <detours/detours.h>
#include "code_patch.h"
#include "install_report.h"
#include "mods_log.h"

// [Performance] AnimationLod=N: characters farther than N meters from the camera update their pose every second frame, farther than 2N
// every fourth, so a crowd costs less. CryAnimation already has such a level of detail, behind ca_SkipBoneAnim, which is off by default:
// CryCharInstance::Update asks a function for a mask by the character's place and updates the bones only in the frames where the frame
// number and the mask give the character's own phase (a counter given out as characters are made, so they take turns), and in any case
// once a quarter of a second has passed. Its mask is 7 (every 8th frame) beyond the bounding radius plus 32 meters, 3 (every 4th) for
// characters off the screen by more than four radii and 0 (every frame) elsewhere. The skinning follows the pose: CryModelState::Deform
// redoes the vertices only after the bones changed, and each character keeps its own vertex buffer, so a frame without a pose update costs
// neither the animation nor the skinning, and the character is drawn as in the frame before. The mod turns that check on and puts the
// distance steps in front of the engine's mask: off the screen or beyond its 32 meters every 8th frame as the engine has it, nearer
// every frame up to N meters, every second frame up to 2N and every fourth beyond. The animation runs at the same speed, as each update
// gets the time since the one before.

namespace {

typedef int (*Mask_t)(void* instance, const float* position, float radius);
Mask_t real_Mask = nullptr;
void* s_engine = nullptr; // the global the engine's mask takes the camera from
DWORD s_cameraMethod;
float s_near2, s_far2;

const float* Camera() {
    void* engine = *(void**)s_engine;
    return engine ? (*(const float* (**)(void*))(*(BYTE**)engine + s_cameraMethod))(engine) : nullptr;
}

int AnimationMask(void* instance, const float* position, float radius) {
    int mask = real_Mask(instance, position, radius);
    if (mask != 0) {
        return 7;
    }
    if (!(radius > 0)) {
        return 0;
    }
    const float* camera = Camera();
    if (!camera) {
        return 0;
    }
    float dx = position[0] - camera[12], dy = position[1] - camera[13], dz = position[2] - camera[14];
    float distance2 = dx * dx + dy * dy + dz * dz;
    return distance2 < s_near2 ? 0 : distance2 < s_far2 ? 1 : 3;
}

struct Place {
    BYTE* mask = nullptr; // the engine's mask function
    void* engine = nullptr;
    DWORD cameraMethod = 0; // offset of the engine's method that gives the camera
    CodePatch check; // je past the mask when ca_SkipBoneAnim is 0
};

/// The engine's mask: push rbx; sub rsp, 40h; mov rcx, [engine]; (keeps xmm6); mov rbx, rdx; mov rax, [rcx]; ...; call [rax+camera];
/// then the position minus the camera's at +30h, +34h, +38h.
bool IsMask(HMODULE animation, BYTE* mask, DWORD* cameraMethod) {
    if (!InModule(animation, mask, 96) || memcmp(mask, "\x40\x53\x48\x83\xEC\x40\x48\x8B\x0D", 9) != 0) {
        return false;
    }
    for (BYTE* p = mask + 13; p < mask + 48; p++) {
        if (p[0] == 0xFF && p[1] == 0x90 && memcmp(p + 6, "\xF3\x0F\x10\x0B", 4) == 0) {
            *cameraMethod = *(DWORD*)(p + 2);
            return memcmp(p + 20, "\xF3\x0F\x5C\x48\x30\xF3\x0F\x5C\x50\x34\xF3\x0F\x5C\x40\x38", 15) == 0;
        }
    }
    return false;
}

Place Locate(HMODULE animation) {
    Place place;
    // CryCharInstance::Update: mov rax, [cvars]; cmp dword [rax+50h], 0 (ca_SkipBoneAnim); je; mov rcx, [renderer]; ... call [rax+5xxh]
    // (the frame number); ... mov rcx, rdi; ... call mask
    for (BYTE* check : FindAllPatterns(animation, "48 8B 05 ?? ?? ?? ?? 83 78 50 00 0F 84 ?? ?? ?? ?? 48 8B 0D")) {
        for (BYTE* p = check + 24; p < check + 24 + 80; p++) {
            DWORD cameraMethod;
            if (p[0] == 0xE8 && IsMask(animation, ResolveRip(p + 1, p + 5), &cameraMethod)) {
                if (place.mask) {
                    ModsLog("animation lod: a second check at %p, nothing changed", check);
                    return {};
                }
                static const BYTE NOP6[] = { 0x66, 0x0F, 0x1F, 0x44, 0x00, 0x00 };
                place.check = PlanPatch(check + 11, NOP6, sizeof(NOP6));
                place.mask = ResolveRip(p + 1, p + 5);
                place.engine = ResolveRip(place.mask + 9, place.mask + 13);
                place.cameraMethod = cameraMethod;
                break;
            }
        }
    }
    if (!place.mask) {
        ModsLog("animation lod: the check of ca_SkipBoneAnim before the mask was not found");
    }
    return place;
}

void Apply(const Place& place) {
    if (!place.mask) {
        return;
    }
    float nearest = (float)g_modsConfig.animationLod;
    s_near2 = nearest * nearest;
    s_far2 = 4 * nearest * nearest;
    s_engine = place.engine;
    s_cameraMethod = place.cameraMethod;
    real_Mask = (Mask_t)place.mask;
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    LONG error = AttachHook(&(PVOID&)real_Mask, AnimationMask);
    DetourTransactionCommit();
    if (error != NO_ERROR) {
        ModsLog("animation lod: could not hook the mask at %p: %ld", place.mask, error);
        return;
    }
    ApplyPatch(place.check);
    ModsLog("animation lod: every frame up to %.0f m, every second frame up to %.0f m, mask at %p", nearest, 2 * nearest, place.mask);
}

} // namespace

void InstallAnimationLod(HMODULE animation) {
    InstallScope scope("animation_lod");
    LocateThenApply(animation, Locate, Apply);
}
