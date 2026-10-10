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

static void RaiseBound(HMODULE crySystem, const char* name, DWORD lower, DWORD upper) {
    BYTE* bound = FindBound(crySystem, lower, upper);
    if (!bound) {
        MODULE_LOG("fullscreen fix: %s bound not found", name);
        return;
    }
    DWORD oldProtect;
    if (!VirtualProtect(bound, sizeof(MAX_SIZE), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        return;
    }
    *(DWORD*)bound = MAX_SIZE;
    VirtualProtect(bound, sizeof(MAX_SIZE), oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), bound, sizeof(MAX_SIZE));
    MODULE_LOG("fullscreen fix: %s bound at %p, %lu -> %lu", name, bound, upper, MAX_SIZE);
}

static void RaiseBounds(HMODULE crySystem) {
    RaiseBound(crySystem, "width", 800, 2560);
    RaiseBound(crySystem, "height", 600, 1920);
}

/// The client clamps FULLSCREEN_WIDTH and FULLSCREEN_HEIGHT from SystemOptionGraphics.cfg to 2560x1920, while it saves the screen
/// size there unclamped. From the second start on, a larger screen then gets a 2560x1920 frame stretched over it, with the mouse
/// off by the stretch. CrySystem.dll is packed in some clients, so the bounds are raised once its entry point has unpacked it.
void InstallFullscreenFix() {
    OnModuleLoad(L"CrySystem.dll", RaiseBounds, true);
}
