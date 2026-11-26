// Keeps the graphics settings while faster loading is on.
//
// At every start the client (aion.bin, before it loads CrySystem.dll) compares the hardware with what it saved in
// HKCU\Software\Aion\Client\SystemInfo at an earlier start: the vendor of the graphics card, the text with the size of the memory and
// the one with the processor. If one differs, it sets change_hardware, and CrySystem then resets the graphics settings of both engines
// to its own preset. The two texts come from DxDiag, which faster loading skips, so they are empty; and the client never saves an
// empty text, so the saved ones stay and differ at every start.
//
// With faster loading on, an empty text now counts as the saved one. The vendor is compared as before, and so is a text DxDiag did
// fill in. Only the two calls that compare the texts change: they go through a jump of ours, and from there to the client's own.
// CrySystem.dll holds a copy of the same check that the 64-bit 4.8 client never calls; it is left alone.
#include "mods.h"
#include <string.h>
#include <vector>
#include <detours/detours.h>

typedef int(__cdecl* CompareText_t)(const char* current, const char* saved);

/// The client's own comparison.
static CompareText_t s_compareText = nullptr;

static int __cdecl zzCompareText(const char* current, const char* saved) {
    return *current ? s_compareText(current, saved) : 0;
}

/// Calls found(c) for every c of the image with length readable bytes, until it returns true.
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

/// The calls after the lea of the vendor's name that compare a text read into [rsp+30h]:
///   lea rdx, [rsp+30h]; mov rcx, rbx or rdi; call compare
static std::vector<BYTE*> FindTextCalls(BYTE* vendorLea, BYTE* imageEnd) {
    static const BYTE CODE[] = { 0x48, 0x8D, 0x54, 0x24, 0x30, 0x48, 0x8B };
    std::vector<BYTE*> calls;
    ForEachReadable(vendorLea, min(imageEnd, vendorLea + 0x180), sizeof(CODE) + 6, [&](BYTE* c) {
        if (memcmp(c, CODE, sizeof(CODE)) == 0 && (c[7] == 0xCB || c[7] == 0xCF) && c[8] == 0xE8) {
            calls.push_back(c + 8);
        }
        return false;
    });
    return calls;
}

/// The client's image is given, so that a test can give another one.
void KeepGraphicsSettingsIn(HMODULE client) {
    if (g_modsConfig.fastStart == FAST_START_OFF) {
        return;
    }
    static const char VENDOR[] = "Display0.video_vendor_id";
    BYTE* image = (BYTE*)client;
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
    BYTE* compare = calls[0] + 5 + *(INT32*)(calls[0] + 1);
    if (calls[1] + 5 + *(INT32*)(calls[1] + 1) != compare) {
        ModsLog("graphics settings: the two comparisons differ, left as they are");
        return;
    }
    if (compare < image || compare >= imageEnd) {
        return;
    }
    // jmp [rip+0]; dq zzCompareText
    static const BYTE JUMP[] = { 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00 };
    DWORD size = 0;
    BYTE* ours = (BYTE*)DetourAllocateRegionWithinJumpBounds(calls[0], &size);
    if (!ours || size < sizeof(JUMP) + sizeof(void*)) {
        ModsLog("graphics settings: no memory near the client, left as it is");
        return;
    }
    INT64 displacements[2];
    for (int i = 0; i < 2; i++) {
        displacements[i] = ours - (calls[i] + 5);
        if (displacements[i] != (INT32)displacements[i]) {
            ModsLog("graphics settings: the memory is too far from the client, left as it is");
            return;
        }
    }
    void* target = (void*)zzCompareText;
    memcpy(ours, JUMP, sizeof(JUMP));
    memcpy(ours + sizeof(JUMP), &target, sizeof(target));
    FlushInstructionCache(GetCurrentProcess(), ours, sizeof(JUMP) + sizeof(target));
    s_compareText = (CompareText_t)compare;
    bool patched = true;
    for (int i = 0; i < 2; i++) {
        INT32 displacement = (INT32)displacements[i];
        patched = PatchMemory(calls[i] + 1, &displacement, sizeof(displacement)) && patched;
    }
    ModsLog("graphics settings: %s at aion.bin+%llX and +%llX (Faster loading skips DxDiag)",
        patched ? "empty hardware texts not compared" : "NOT patched", (unsigned long long)(calls[0] - image), (unsigned long long)(calls[1] - image));
}

/// Called right after Faster loading is installed, before the client runs its hardware check.
void KeepGraphicsSettings() {
    KeepGraphicsSettingsIn(GetModuleHandleW(nullptr));
}
