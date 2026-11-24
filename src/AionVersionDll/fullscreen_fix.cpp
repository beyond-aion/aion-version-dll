#include "code_patch.h"
#include "module_load.h"

static constexpr DWORD MAX_SIZE = 16384;

/// Length of mov dword ptr [mem], imm32 (C7 /0) without its immediate.
static size_t StoreLength(const BYTE* code) {
    BYTE modRm = code[1];
    int mod = modRm >> 6, rm = modRm & 7;
    size_t length = 2;
    if (mod == 3) {
        return length;
    }
    if (rm == 4) {
        length++;
        if (mod == 0 && (code[2] & 7) == 5) {
            length += 4;
        }
    }
    if (mod == 1) {
        length += 1;
    } else if (mod == 2 || (mod == 0 && rm == 5)) {
        length += 4;
    }
    return length;
}

/// Finds the upper bound of one key in the table of SystemOptionGraphics.cfg keys, where two consecutive dword stores put its lower
/// and its upper bound on the stack. Returns nullptr unless there is exactly one such pair.
static BYTE* FindBound(HMODULE crySystem, DWORD lower, DWORD upper) {
    BYTE* image = (BYTE*)crySystem;
    BYTE* imageEnd = image + ModuleImageSize(crySystem);
    BYTE* found = nullptr;
    MEMORY_BASIC_INFORMATION region;
    for (BYTE* begin = image; begin < imageEnd && VirtualQuery(begin, &region, sizeof(region)); begin = (BYTE*)region.BaseAddress + region.RegionSize) {
        // packers can leave pages of the image without access
        bool readable = region.State == MEM_COMMIT && !(region.Protect & (PAGE_NOACCESS | PAGE_GUARD)) && (region.Protect & 0xEE);
        if (!readable) {
            continue;
        }
        BYTE* end = min(imageEnd, (BYTE*)region.BaseAddress + region.RegionSize) - 32;
        for (BYTE* p = begin; p < end; p++) {
            if (p[0] != 0xC7 || (p[1] & 0x38) != 0) {
                continue;
            }
            BYTE* next = p + StoreLength(p);
            if (*(DWORD*)next != lower) {
                continue;
            }
            next += 4;
            if (next[0] != 0xC7 || (next[1] & 0x38) != 0) {
                continue;
            }
            BYTE* immediate = next + StoreLength(next);
            if (*(DWORD*)immediate == upper) {
                if (found) {
                    return nullptr;
                }
                found = immediate;
            }
        }
    }
    return found;
}

namespace {

/// The upper bounds of the width and the height, each with its stored value kept.
struct Bounds {
    CodePatch width;
    CodePatch height;
};

Bounds LocateBounds(HMODULE crySystem) {
    Bounds bounds;
    BYTE* width = FindBound(crySystem, 800, 2560);
    ReportSearch(width ? 1 : 0);
    BYTE* height = FindBound(crySystem, 600, 1920);
    ReportSearch(height ? 1 : 0);
    bounds.width = PlanPatch(width, &MAX_SIZE, sizeof(MAX_SIZE));
    bounds.height = PlanPatch(height, &MAX_SIZE, sizeof(MAX_SIZE));
    return bounds;
}

void RaiseBound(const CodePatch& bound, const char* name) {
    if (!bound) {
        MODULE_LOG("fullscreen fix: %s bound not found", name);
        return;
    }
    if (ApplyPatch(bound)) {
        MODULE_LOG("fullscreen fix: %s bound at %p, %lu -> %lu", name, bound.at, *(const DWORD*)bound.expected, MAX_SIZE);
    }
}

void RaiseBounds(const Bounds& bounds) {
    RaiseBound(bounds.width, "width");
    RaiseBound(bounds.height, "height");
}

void InstallOnCrySystem(HMODULE crySystem) {
    InstallScope scope("fullscreen_fix");
    LocateThenApply(crySystem, LocateBounds, RaiseBounds);
}

} // namespace

/// Without a DPI awareness of its own the client sees a screen scaled above 100 % as smaller than it is, renders that size and Windows
/// stretches the frame over the screen, blurred. A DPI setting from the compatibility tab of aion.bin comes first: the client then has its
/// awareness already, and setting it again fails.
static void MakeDpiAware() {
    typedef BOOL(WINAPI* SetProcessDpiAwarenessContext_t)(DPI_AWARENESS_CONTEXT value);
    auto setContext = (SetProcessDpiAwarenessContext_t)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext");
    BOOL done = setContext ? setContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) : SetProcessDPIAware();
    MODULE_LOG("fullscreen fix: DPI aware %d (error %lu)", done, done ? 0 : GetLastError());
}

/// The client clamps FULLSCREEN_WIDTH and FULLSCREEN_HEIGHT from SystemOptionGraphics.cfg to 2560x1920, while it saves the screen
/// size there unclamped. From the second start on, a larger screen then gets a 2560x1920 frame stretched over it, with the mouse
/// off by the stretch. CrySystem.dll is packed in some clients, so the bounds are raised once its entry point has unpacked it.
void InstallFullscreenFix() {
    MakeDpiAware();
    OnModuleLoad(L"CrySystem.dll", InstallOnCrySystem, true);
}
