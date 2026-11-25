#include "code_patch.h"
#include "module_load.h"
#include "mods_log.h"
#include <detours/detours.h>
#include "install_report.h"

// Before drawing an object whose material or fade makes it see-through, the renderer sets alpha blending and, for shaders that do not
// keep depth writes, turns depth writes off and locks them so that the passes of the object cannot turn them back on (a bit in its
// per-flush flags that its SetState honours). It locks them only when depth writes are already off: after an object that wrote depth
// they are turned off without the lock, and a pass that asks for depth writes turns them on again. Which object comes before changes
// with the camera, so some see-through objects wrote depth and some did not. The light cards laid flat under a pond in Cygnea (also
// Kaldor, Enshar, Idian Depths) are drawn after the water in several passes; a card that wrote depth hid the later passes of the cards
// a few centimetres under it, and its edge showed on the water as a straight pale slab that came and went as the camera turned and in
// flight (the owner's captures: the five passes after an alpha-tested object wrote depth, those after another card did not). The lock
// is now set in both cases, after the state with depth writes off has been set.
//
// The place: btr edi, 8; and eax, 0FFFFFF00h; cmp eax, edi; jnz +7; or dword [rbx+flags], 40h in the function that sets the alpha of
// an object (4.6 to 5.8 alike), then after its SetState call or dword [rbx+flags], 210h. A stub at the first notes that this object's
// depth writes go off, one at the second sets the lock.

extern "C" void* g_fadeDepthOffResume = nullptr;
extern "C" void* g_fadeDepthLockResume = nullptr;
extern "C" INT64 g_fadeFlagsOffset = 0;
extern "C" BYTE g_fadeDepthLock = 0;
extern "C" void FadeDepthOffStub();
extern "C" void FadeDepthLockStub();

namespace {

struct Place {
    BYTE* off = nullptr;
    BYTE* lock = nullptr;
    int flags = 0;
};

Place Locate(HMODULE renderer) {
    Place place;
    std::vector<BYTE*> found = FindAllPatterns(renderer, "0F BA F7 08 25 00 FF FF FF 3B C7 75 07 83 8B ?? ?? ?? ?? 40");
    if (found.size() != 1) {
        ModsLog("fade depth write fix: %zu places", found.size());
        return place;
    }
    BYTE* off = found[0];
    int flags = *(int*)(off + 15);
    if (flags <= 0 || flags > 0x10000) {
        return place;
    }
    // or dword [rbx+flags], 210h after the SetState call, within 0x80 bytes (0x5B in 4.6 to 4.9, 0x65 in 5.6 and 5.8)
    BYTE* lock = nullptr;
    for (BYTE* p = off + 20; p < off + 0x80; p++) {
        if (p[0] == 0x81 && p[1] == 0x8B && *(int*)(p + 2) == flags && *(int*)(p + 6) == 0x210) {
            lock = p;
            break;
        }
    }
    if (!lock) {
        ModsLog("fade depth write fix: no or 210h after %p", off);
        return place;
    }
    place.off = off;
    place.lock = lock;
    place.flags = flags;
    return place;
}

void Apply(const Place& place) {
    if (!place.off) {
        ModsLog("fade depth write fix: not found");
        return;
    }
    g_fadeFlagsOffset = place.flags;
    g_fadeDepthOffResume = place.off;
    g_fadeDepthLockResume = place.lock;
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    AttachHook(&g_fadeDepthOffResume, FadeDepthOffStub);
    AttachHook(&g_fadeDepthLockResume, FadeDepthLockStub);
    if (CommitHooks() == NO_ERROR) {
        ModsLog("fade depth write fix: hooked at %p and %p (flags %X)", place.off, place.lock, place.flags);
    }
}

void InstallFadeDepthWriteFix(HMODULE renderer) {
    InstallScope scope("fade_depth_write_fix");
    LocateThenApply(renderer, Locate, Apply);
}

} // namespace

void InstallFadeDepthWriteFixOnLoad() {
    OnModuleLoad(L"XRenderD3D9.dll", InstallFadeDepthWriteFix, true);
}
