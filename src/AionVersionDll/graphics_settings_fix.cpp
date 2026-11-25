// Keeps the graphics settings while faster loading is on.
//
// At every start CrySystem compares the hardware with what it saved in HKCU\Software\Aion\Client\SystemInfo at an earlier start: the
// vendor of the graphics card, the text with the size of the memory and the one with the processor. If one differs, it resets the
// graphics settings of both engines to its own preset. The two texts come from DxDiag, which faster loading skips, so they are empty;
// and CrySystem never saves an empty text, so the saved ones stay and differ at every start.
//
// With faster loading on, an empty text now counts as the saved one. The vendor is compared as before, and so is a text DxDiag did
// fill in. Only the two calls that compare the texts change: they go through a slot of ours, and from there to the client's own.
#include "mods.h"
#include <string.h>
#include <vector>
#include <detours/detours.h>

typedef int(__cdecl* CompareText_t)(const char* current, const char* saved);

/// CrySystem's own slot of the comparison, read at each call.
static CompareText_t* s_compareText = nullptr;

static int __cdecl zzCompareText(const char* current, const char* saved) {
    return *current ? (*s_compareText)(current, saved) : 0;
}

/// Calls found(c) for every c of the image with length readable bytes, until it returns true. Packers can leave pages without access.
template <typename Found>
static void ForEachReadable(BYTE* begin, BYTE* end, size_t length, Found found) {
    MEMORY_BASIC_INFORMATION region;
    for (BYTE* at = begin; at < end && VirtualQuery(at, &region, sizeof(region)); at = (BYTE*)region.BaseAddress + region.RegionSize) {
        if (region.State != MEM_COMMIT || (region.Protect & (PAGE_NOACCESS | PAGE_GUARD)) || !(region.Protect & 0xEE)) {
            continue;
        }
        BYTE* regionEnd = min(end, (BYTE*)region.BaseAddress + region.RegionSize);
        for (BYTE* c = at; c + length <= regionEnd; c++) {
            if (found(c)) {
                return;
            }
        }
    }
}

/// The displacements of the two calls after the lea of the vendor's name:
///   call [r11+10h]; lea rdx, [rsp+40h]; mov rcx, rdi; call [slot]
static std::vector<BYTE*> FindTextCalls(BYTE* vendorLea, BYTE* imageEnd) {
    static const BYTE CODE[] = { 0x41, 0xFF, 0x53, 0x10, 0x48, 0x8D, 0x54, 0x24, 0x40, 0x48, 0x8B, 0xCF, 0xFF, 0x15 };
    std::vector<BYTE*> calls;
    ForEachReadable(vendorLea, min(imageEnd, vendorLea + 0x180), sizeof(CODE) + 4, [&](BYTE* c) {
        if (memcmp(c, CODE, sizeof(CODE)) == 0) {
            calls.push_back(c + sizeof(CODE));
        }
        return false;
    });
    return calls;
}

/// Called by the fullscreen fix (fullscreen_fix.cpp) once CrySystem's entry point has run; the client loads CrySystem.dll once more
/// when it closes, where the calls already have the slot of ours.
void KeepGraphicsSettings(HMODULE crySystem) {
    if (g_modsConfig.fastStart == FAST_START_OFF) {
        return;
    }
    static const char VENDOR[] = "Display0.video_vendor_id";
    BYTE* image = (BYTE*)crySystem;
    BYTE* imageEnd = image + ((IMAGE_NT_HEADERS*)(image + ((IMAGE_DOS_HEADER*)image)->e_lfanew))->OptionalHeader.SizeOfImage;
    BYTE* vendor = nullptr;
    ForEachReadable(image, imageEnd, sizeof(VENDOR), [&](BYTE* c) {
        if (*c == VENDOR[0] && memcmp(c, VENDOR, sizeof(VENDOR)) == 0) {
            vendor = c;
        }
        return vendor != nullptr;
    });
    std::vector<BYTE*> calls;
    int functions = 0;
    if (vendor) {
        ForEachReadable(image, imageEnd, 7, [&](BYTE* c) {
            // lea r8, [rip+vendor]
            if (c[0] == 0x4C && c[1] == 0x8D && c[2] == 0x05 && c + 7 + *(INT32*)(c + 3) == vendor) {
                std::vector<BYTE*> found = FindTextCalls(c, imageEnd);
                if (!found.empty()) {
                    functions++;
                    calls = found;
                }
            }
            return false;
        });
    }
    if (functions != 1 || calls.size() != 2) {
        ModsLog("graphics settings: comparison not found (%d functions, %zu calls), left as it is", functions, calls.size());
        return;
    }
    BYTE* slot = ResolveRip(calls[0], calls[0] + 4);
    if (ResolveRip(calls[1], calls[1] + 4) != slot) {
        ModsLog("graphics settings: the two comparisons differ, left as they are");
        return;
    }
    if (slot < image || slot + sizeof(void*) > imageEnd) {
        return;
    }
    DWORD size = 0;
    void** ours = (void**)DetourAllocateRegionWithinJumpBounds(calls[0], &size);
    if (!ours || size < sizeof(void*)) {
        ModsLog("graphics settings: no memory near CrySystem, left as it is");
        return;
    }
    INT64 displacements[2];
    for (int i = 0; i < 2; i++) {
        displacements[i] = (BYTE*)ours - (calls[i] + 4);
        if (displacements[i] != (INT32)displacements[i]) {
            ModsLog("graphics settings: the memory is too far from CrySystem, left as it is");
            return;
        }
    }
    *ours = (void*)zzCompareText;
    s_compareText = (CompareText_t*)slot;
    bool patched = true;
    for (int i = 0; i < 2; i++) {
        INT32 displacement = (INT32)displacements[i];
        patched = PatchMemory(calls[i], &displacement, sizeof(displacement)) && patched;
    }
    ModsLog("graphics settings: %s at CrySystem+%llX and +%llX (Faster loading skips DxDiag)",
        patched ? "empty hardware texts not compared" : "NOT patched", (unsigned long long)(calls[0] - image), (unsigned long long)(calls[1] - image));
}
