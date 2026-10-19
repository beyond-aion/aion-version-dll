#include "mods.h"
#include "module_load.h"
#include <stdarg.h>
#include <stdlib.h>
#include <string>
#include <unordered_map>
#include <vector>
#include <intrin.h>
#include <objbase.h>
#include <dxgi.h>
#include <bcrypt.h>
#include <tlhelp32.h>
#include <shlwapi.h>
#include "detours.h"
#include "../libdeflate/libdeflate.h"

#pragma comment(lib, "bcrypt.lib")

// Before it starts, the client waits until no closing client is unloading any more: it polls for a named mutex every 10 ms, but only
// once the whole seconds of GetTickCount since the wait began exceed 2, so every start waits 2 to 3 seconds for nothing:
// call GetTickCount (start); ... loop: call GetTickCount; ...; cmp esi, ebx (seconds, 2); jbe sleep; mov rcx, rdi; call poll
// aion.bin checks its own code and stops with "File corrupted!" when it finds it changed, so GetTickCount answers 3 seconds later
// to the call in the loop instead. A client that is really unloading is still waited for.

static decltype(GetTickCount)* real_GetTickCount = GetTickCount;
static BYTE* volatile s_waitTickReturn = nullptr;

static DWORD WINAPI zzGetTickCount() {
    DWORD now = real_GetTickCount();
    // a protected call returns one byte further, past the byte it skips
    size_t past = (BYTE*)_ReturnAddress() - s_waitTickReturn;
    return s_waitTickReturn && past <= 1 ? now + 3000 : now;
}

static bool FindUnloadWait(HMODULE exe) {
    static const char NAME[] = "AionUnloadMutex";
    const void* text = FindBytes(exe, NAME, sizeof(NAME));
    BYTE* lea = text ? FindLeaTo(exe, text) : nullptr;
    if (!lea) {
        return false;
    }
    // functions start after the int3 padding in front of them
    BYTE* poll = lea;
    while (poll > lea - 0x40 && !(poll[-1] == 0xCC && poll[0] != 0xCC)) {
        poll--;
    }
    BYTE* call = FindCallTo(exe, poll);
    if (!call || call[-5] != 0x76 || call[-3] != 0x48 || call[-2] != 0x8B) {
        return false;
    }
    BYTE* wait = call;
    while (wait > call - 0x100 && !(wait[-1] == 0xCC && wait[0] != 0xCC)) {
        wait--;
    }
    // the second call of the function is the one in the loop
    int calls = 0;
    for (BYTE* p = wait; p < call - 5; p++) {
        if (p[0] == 0xE8 && ++calls == 2) {
            s_waitTickReturn = p + 5;
            ModsLog("fast start: unload wait at %p", p);
            return true;
        }
    }
    return false;
}

/// Some clients unpack aion.bin only after version.dll is loaded, so the code may not be there yet.
static DWORD WINAPI RetryUnloadWait(LPVOID) {
    HMODULE exe = GetModuleHandleW(nullptr);
    for (int i = 0; i < 1000; i++) {
        if (FindUnloadWait(exe)) {
            return 0;
        }
        Sleep(5);
    }
    ModsLog("fast start: unload wait not found");
    return 0;
}

static const CLSID CLSID_DXDIAG_PROVIDER = { 0xA65B8071, 0x3BFE, 0x4213, { 0x9A, 0x5B, 0x49, 0x1D, 0xA4, 0x46, 0x1C, 0xA7 } };

static decltype(CoCreateInstance)* real_CoCreateInstance = nullptr;
static decltype(RegSetValueExA)* real_RegSetValueExA = nullptr;
static volatile bool s_dxDiagSkipped = false;

/// The client collects a DirectX diagnostic report at every start, which takes about 3 seconds, only to store it in the registry
/// (HKCU\Software\Aion\Client\SystemInfo). Without the provider it skips the report and keeps the values stored before.
static HRESULT WINAPI zzCoCreateInstance(REFCLSID clsid, LPUNKNOWN outer, DWORD context, REFIID iid, LPVOID* object) {
    if (IsEqualCLSID(clsid, CLSID_DXDIAG_PROVIDER)) {
        if (object) {
            *object = nullptr;
        }
        s_dxDiagSkipped = true;
        ModsLog("fast start: DxDiag skipped");
        return REGDB_E_CLASSNOTREG;
    }
    return real_CoCreateInstance(clsid, outer, context, iid, object);
}

/// Dedicated and shared memory of the first adapter in MB, as the report gives it, or 0.
static DWORD AdapterMemory() {
    HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
    auto create = dxgi ? (HRESULT(WINAPI*)(REFIID, void**))GetProcAddress(dxgi, "CreateDXGIFactory1") : nullptr;
    IDXGIFactory1* factory = nullptr;
    if (!create || FAILED(create(__uuidof(IDXGIFactory1), (void**)&factory))) {
        return 0;
    }
    DWORD megabytes = 0;
    IDXGIAdapter1* adapter = nullptr;
    DXGI_ADAPTER_DESC1 desc;
    if (SUCCEEDED(factory->EnumAdapters1(0, &adapter))) {
        if (SUCCEEDED(adapter->GetDesc1(&desc))) {
            megabytes = (DWORD)((desc.DedicatedVideoMemory + desc.SharedSystemMemory) >> 20);
        }
        adapter->Release();
    }
    factory->Release();
    return megabytes;
}

/// The video memory of the stored report comes from the report alone; without it the client stores 0, and 5.x clients then judge the
/// computer as a weak one and lower the graphics, the screen size first. The value stored before stays, or the adapter's when there is none.
static LSTATUS WINAPI zzRegSetValueExA(HKEY key, LPCSTR name, DWORD reserved, DWORD type, const BYTE* data, DWORD size) {
    if (s_dxDiagSkipped && name && type == REG_DWORD && size == 4 && *(const DWORD*)data == 0 && _stricmp(name, "Display0.dedi_video_memory") == 0) {
        DWORD stored = 0, storedSize = sizeof(stored);
        if (RegQueryValueExA(key, name, nullptr, nullptr, (BYTE*)&stored, &storedSize) == ERROR_SUCCESS && stored) {
            return ERROR_SUCCESS;
        }
        DWORD memory = AdapterMemory();
        ModsLog("fast start: video memory %lu MB stored in place of 0", memory);
        return real_RegSetValueExA(key, name, reserved, type, (const BYTE*)&memory, sizeof(memory));
    }
    return real_RegSetValueExA(key, name, reserved, type, data, size);
}

typedef double(__fastcall* ClockMeasure_t)(double* secondsPerTick);
static ClockMeasure_t real_ClockMeasure = nullptr;
static double s_clockMhz = 0;
static double s_secondsPerTick = 0;

static double __fastcall zzClockMeasure(double* secondsPerTick) {
    if (!s_clockMhz) {
        s_clockMhz = real_ClockMeasure(secondsPerTick);
        s_secondsPerTick = *secondsPerTick;
        return s_clockMhz;
    }
    *secondsPerTick = s_secondsPerTick;
    return s_clockMhz;
}

/// CrySystem measures the clock of every logical processor one after another, on a thread bound to it, each over Sleep(100): 1.6 seconds
/// with 16 of them. rdtsc; shl rdx, 20h; mov ecx, 100 becomes 10 ms, which still gives the clock to about a percent, but the timer of
/// Windows makes that 15 ms. The time stamp counter runs at one rate on every processor, so only the first one is measured and the
/// others get its clock in MHz and seconds per tick.
static void ShortenClockMeasure(HMODULE crySystem) {
    // 5.x clients have an xorps between the shift and the mov
    for (BYTE* rdtsc : FindAllPatterns(crySystem, "0F 31 48 C1 E2 20")) {
        for (BYTE* p = rdtsc + 6; p < rdtsc + 12; p++) {
            DWORD ms = 10;
            if (p[0] == 0xB9 && *(DWORD*)(p + 1) == ms) {
                // the client loads CrySystem.dll once more when it closes
                return;
            }
            if (p[0] == 0xB9 && *(DWORD*)(p + 1) == 100) {
                PatchMemory(p + 1, &ms, sizeof(ms));
                DWORD64 base = 0;
                PRUNTIME_FUNCTION function = RtlLookupFunctionEntry((DWORD64)p, &base, nullptr);
                LONG error = -1;
                if (function && !real_ClockMeasure) {
                    real_ClockMeasure = (ClockMeasure_t)(base + function->BeginAddress);
                    DetourTransactionBegin();
                    DetourUpdateThread(GetCurrentThread());
                    DetourAttach(&(PVOID&)real_ClockMeasure, zzClockMeasure);
                    error = DetourTransactionCommit();
                }
                ModsLog("fast start: clock measure at %p, once: %ld", p, error);
                return;
            }
        }
    }
    ModsLog("fast start: clock measure not found");
}

// 4.x clients keep much of their game data in memory as zlib blocks of 16 KB, which CrySystem compresses with the zlib 1.1.4 it
// carries while the data loads (a sixth of the loading time) and decompresses when it is used; 5.x clients, with zlib 1.2.8, do the
// same when they build their table dump. libdeflate does both several times
// faster; its blocks are plain zlib streams the client's own code reads as well. Whatever libdeflate cannot do (a block that does not
// fit, data it rejects) goes to the client's zlib, which then gives its own result.

typedef int(__cdecl* Compress2_t)(BYTE* dest, unsigned long* destLen, const BYTE* source, unsigned long sourceLen, int level);
typedef int(__cdecl* Uncompress_t)(BYTE* dest, unsigned long* destLen, const BYTE* source, unsigned long sourceLen);

static Compress2_t real_compress2 = nullptr;
static Uncompress_t real_uncompress = nullptr;
static bool s_blocksInMemory = false;

static int __cdecl zz_compress2(BYTE* dest, unsigned long* destLen, const BYTE* source, unsigned long sourceLen, int level) {
    // zlib's levels 0 to 9 mean the same in libdeflate. 4.x clients ask for level 6, the default, for blocks that stay in memory;
    // level 1 takes a quarter second less in 4.8 and 1 MB more for its 220 MB of data. 5.x clients store theirs in the table dump.
    int deflateLevel = level == -1 ? 6 : level;
    if (s_blocksInMemory && deflateLevel == 6) {
        deflateLevel = 1;
    }
    static thread_local libdeflate_compressor* compressors[10] = {};
    if (deflateLevel < 0 || deflateLevel > 9) {
        return real_compress2(dest, destLen, source, sourceLen, level);
    }
    libdeflate_compressor*& compressor = compressors[deflateLevel];
    if (!compressor) {
        compressor = libdeflate_alloc_compressor(deflateLevel);
    }
    size_t size = compressor ? libdeflate_zlib_compress(compressor, source, sourceLen, dest, *destLen) : 0;
    if (!size) {
        return real_compress2(dest, destLen, source, sourceLen, level);
    }
    *destLen = (unsigned long)size;
    return 0;
}

static int __cdecl zz_uncompress(BYTE* dest, unsigned long* destLen, const BYTE* source, unsigned long sourceLen) {
    static thread_local libdeflate_decompressor* decompressor = nullptr;
    if (!decompressor) {
        decompressor = libdeflate_alloc_decompressor();
    }
    size_t read, written;
    if (!decompressor || libdeflate_zlib_decompress_ex(decompressor, source, sourceLen, dest, *destLen, &read, &written) != LIBDEFLATE_SUCCESS) {
        return real_uncompress(dest, destLen, source, sourceLen);
    }
    *destLen = (unsigned long)written;
    return 0;
}

/// compress2 and uncompress of zlib 1.1.4 as the 4.x clients build them, both starting by filling a z_stream on the stack and then
/// passing the version "1.1.4" to the init function: ...; lea r8/rdx, "1.1.4"
static BYTE* FindZlibFunction(HMODULE crySystem, const char* pattern, const void* version) {
    BYTE* function = FindPattern(crySystem, pattern);
    return function && ResolveRip(function + strlen(pattern) / 3 + 1, function + strlen(pattern) / 3 + 5) == version ? function : nullptr;
}

static void HookZlib(HMODULE crySystem) {
    static const char VERSION4[] = "1.1.4", VERSION5[] = "1.2.8";
    if (real_compress2) {
        return;
    }
    BYTE* compress = nullptr;
    BYTE* uncompress = nullptr;
    if (const void* version = FindBytes(crySystem, VERSION4, sizeof(VERSION4))) {
        compress = FindZlibFunction(crySystem, "4C 8B DC 48 81 EC 98 00 00 00 8B 02 4D 89 43 88 45 89 4B 90 49 89 4B 98 89 44 24 38 33 C0 49 89 "
            "7B F0 48 8B FA 8B 94 24 C0 00 00 00 44 8D 48 58 4C 8D 05", version);
        uncompress = FindZlibFunction(crySystem, "4C 8B DC 48 81 EC 88 00 00 00 8B 02 4D 89 43 98 49 89 4B A8 45 89 4B A0 89 44 24 38 33 C0 49 89 "
            "7B 20 48 8B FA 49 8D 4B 98 44 8D 40 58 48 8D 15", version);
        s_blocksInMemory = true;
    } else if (const void* version = FindBytes(crySystem, VERSION5, sizeof(VERSION5))) {
        compress = FindZlibFunction(crySystem, "4C 8B DC 57 48 81 EC 80 00 00 00 8B 02 4D 89 43 98 45 89 4B A0 49 89 4B A8 89 44 24 38 33 C0 48 8B "
            "FA 8B 94 24 B0 00 00 00 4C 8D 05", version);
        uncompress = FindZlibFunction(crySystem, "40 57 48 81 EC 80 00 00 00 8B 02 48 8B FA 4C 89 44 24 20 89 44 24 38 33 C0 48 89 4C 24 30 48 8D 15",
            version);
    }
    if (!compress || !uncompress) {
        ModsLog("fast start: zlib not found (compress2 %p, uncompress %p)", compress, uncompress);
        return;
    }
    real_compress2 = (Compress2_t)compress;
    real_uncompress = (Uncompress_t)uncompress;
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&)real_compress2, zz_compress2);
    DetourAttach(&(PVOID&)real_uncompress, zz_uncompress);
    ModsLog("fast start: zlib at %p and %p: %ld", compress, uncompress, DetourTransactionCommit());
}

// The files packed in the .pak archives are raw deflate streams, which CrySystem inflates with its zlib as it reads them, on the main
// thread: about a third of the work of 5.8 before the login screen. libdeflate inflates a whole stream that fits the output at once;
// anything else, such as a stream that does not fit, goes to the client's zlib.

/// 4.x: inflates src into dest, setting *destLen to the size written; 0 when done.
typedef int(__cdecl* RawInflate4_t)(void* allocator, BYTE* dest, unsigned long* destLen, const BYTE* src, unsigned long srcLen);

/// 5.x: inflates into a chain of output blocks, reducing the size of each to what it got and setting *total; 0 when done.
struct OutputBlock {
    OutputBlock* next;
    unsigned long size;
    BYTE data[1];
};
typedef int(__cdecl* RawInflate5_t)(void* allocator, OutputBlock* out, unsigned long* total, const BYTE* src, unsigned long srcLen);

static RawInflate4_t real_RawInflate4 = nullptr;
static RawInflate5_t real_RawInflate5 = nullptr;

static bool RawInflate(const BYTE* src, unsigned long srcLen, BYTE* dest, unsigned long destSize, unsigned long& written) {
    static thread_local libdeflate_decompressor* decompressor = nullptr;
    if (!decompressor) {
        decompressor = libdeflate_alloc_decompressor();
    }
    size_t read, size;
    if (!decompressor || libdeflate_deflate_decompress_ex(decompressor, src, srcLen, dest, destSize, &read, &size) != LIBDEFLATE_SUCCESS) {
        return false;
    }
    written = (unsigned long)size;
    return true;
}

static int __cdecl zzRawInflate4(void* allocator, BYTE* dest, unsigned long* destLen, const BYTE* src, unsigned long srcLen) {
    unsigned long written;
    if (!RawInflate(src, srcLen, dest, *destLen, written)) {
        return real_RawInflate4(allocator, dest, destLen, src, srcLen);
    }
    *destLen = written;
    return 0;
}

static int __cdecl zzRawInflate5(void* allocator, OutputBlock* out, unsigned long* total, const BYTE* src, unsigned long srcLen) {
    unsigned long written;
    if (out->next || !RawInflate(src, srcLen, out->data, out->size, written)) {
        return real_RawInflate5(allocator, out, total, src, srcLen);
    }
    out->size = written;
    *total = written;
    return 0;
}

static void HookRawInflate(HMODULE crySystem) {
    static const char VERSION4[] = "1.1.4", VERSION5[] = "1.2.8";
    if (real_RawInflate4 || real_RawInflate5) {
        return;
    }
    const void* version4 = FindBytes(crySystem, VERSION4, sizeof(VERSION4));
    const void* version5 = FindBytes(crySystem, VERSION5, sizeof(VERSION5));
    BYTE* inflate4 = version4 ? FindZlibFunction(crySystem, "4C 8B DC 48 81 EC 98 00 00 00 8B 84 24 C0 00 00 00 4D 89 4B 88 41 B9 58 00 00 00 89 44 24 "
        "28 41 8B 00 49 89 53 98 89 44 24 38 48 8D 05 ?? ?? ?? ?? 49 89 7B F0 49 89 43 B8 49 8B F8 49 89 4B C8 48 8D 05 ?? ?? ?? ?? 4C 8D 05", version4) : nullptr;
    BYTE* inflate5 = version5 ? FindZlibFunction(crySystem, "4C 8B DC 49 89 5B 10 56 48 81 EC 80 00 00 00 8B 84 24 B0 00 00 00 4D 89 4B 98 41 B9 58 00 00 "
        "00 89 44 24 28 48 8D 42 0C 49 8B F0 49 89 43 A8 8B 42 08 48 8B DA 89 44 24 38 48 8D 05 ?? ?? ?? ?? 49 89 4B D8 49 89 43 C8 48 8D 05 ?? ?? ?? ?? 4C "
        "8D 05", version5) : nullptr;
    if (!inflate4 && !inflate5) {
        ModsLog("fast start: pak inflate not found");
        return;
    }
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    if (inflate4) {
        real_RawInflate4 = (RawInflate4_t)inflate4;
        DetourAttach(&(PVOID&)real_RawInflate4, zzRawInflate4);
    } else {
        real_RawInflate5 = (RawInflate5_t)inflate5;
        DetourAttach(&(PVOID&)real_RawInflate5, zzRawInflate5);
    }
    ModsLog("fast start: pak inflate at %p: %ld", inflate4 ? inflate4 : inflate5, DetourTransactionCommit());
}

/// 5.x clients show their logos in CrySystem while the game data loads, in phases timed by the milliseconds since each began: shown
/// for 2000, faded in over 750, shown for 2000, then faded out over 750 once the loading thread is done (cmp ebx, 7D0h / 2EEh), 5.5
/// seconds even when the data is long loaded. All six limits become 0, so the logos end as soon as the loading does.
static void SkipIntroLogos(HMODULE crySystem) {
    std::vector<BYTE*> limits = FindAllPatterns(crySystem, "81 FB D0 07 00 00");
    std::vector<BYTE*> fades = FindAllPatterns(crySystem, "81 FB EE 02 00 00");
    limits.insert(limits.end(), fades.begin(), fades.end());
    BYTE* function = limits.size() == 6 ? FunctionStart(limits[0]) : nullptr;
    for (BYTE* limit : limits) {
        function = function && FunctionStart(limit) == function ? function : nullptr;
    }
    if (!function) {
        // the client loads CrySystem.dll once more when it closes, without them
        return;
    }
    DWORD zero = 0;
    for (BYTE* limit : limits) {
        PatchMemory(limit + 2, &zero, sizeof(zero));
    }
    ModsLog("fast start: intro logos at %p", function);
}

static void HookRuntime();

typedef void*(__cdecl* CreateSystemInterface_t)(void* params);
static CreateSystemInterface_t real_CreateSystemInterface = nullptr;
static HMODULE s_crySystem = nullptr;

static void* __cdecl zzCreateSystemInterface(void* params) {
    RestoreImportCalls(s_crySystem);
    return real_CreateSystemInterface(params);
}

/// The client calls CreateSystemInterface right after loading CrySystem, out of the loader lock and before anything else of it runs.
static void RestoreImportCallsBeforeUse(HMODULE crySystem) {
    if (real_CreateSystemInterface || !HasImportStubs(crySystem)) {
        return;
    }
    real_CreateSystemInterface = (CreateSystemInterface_t)GetProcAddress(crySystem, "CreateSystemInterface");
    if (real_CreateSystemInterface) {
        s_crySystem = crySystem;
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourAttach(&(PVOID&)real_CreateSystemInterface, zzCreateSystemInterface);
        DetourTransactionCommit();
    }
}

static void OnCrySystem(HMODULE crySystem) {
    RestoreImportCallsBeforeUse(crySystem);
    HookRuntime();
    ShortenClockMeasure(crySystem);
    HookZlib(crySystem);
    HookRawInflate(crySystem);
    if (g_modsConfig.fastStart == FAST_START_FULL && GetModuleHandleW(L"MSVCR120.dll")) {
        SkipIntroLogos(crySystem);
    }
}

// Loading the game data spends about a third of its time in two functions of the runtime (VC 8 in 4.x, VC 12 in 5.x): the case-insensitive compare, with
// which the tables look up their fields by name, one field after another, and the wide printf, which builds field names such as
// effect%d_remain1 for every skill. Both look up the thread's locale for every character. The replacements below handle the plain
// cases themselves, with the same result, and leave everything else to the runtime.

typedef int(__cdecl* Wcsicmp_t)(const wchar_t*, const wchar_t*);
typedef int(__cdecl* Wcsnicmp_t)(const wchar_t*, const wchar_t*, size_t);
typedef int(__cdecl* Stricmp_t)(const char*, const char*);
typedef int(__cdecl* Vsnwprintf_t)(wchar_t*, size_t, size_t, const wchar_t*, va_list);
typedef int(__cdecl* Vswprintf_t)(wchar_t*, const wchar_t*, va_list);

static Wcsicmp_t real_wcsicmp = nullptr;
static Wcsnicmp_t real_wcsnicmp = nullptr;
static Stricmp_t real_stricmp = nullptr;
static Vsnwprintf_t real_vsnwprintf_s = nullptr;
static Vswprintf_t real_vswprintf = nullptr;

/// Whether 8 bytes from p can be read without leaving its page, past the end of the string as that may be.
static bool InPage(const void* p) {
    return ((ULONG_PTR)p & 0xFFF) <= 0xFF8;
}

/// Skips equal characters four at a time, as equal characters stay equal in every locale. Folds A-Z like the runtime does in every
/// locale but the Turkish one; at the first differing character outside ASCII the runtime compares the rest, which gives its own
/// result as the parts before are equal.
static int __cdecl zz_wcsicmp(const wchar_t* a, const wchar_t* b) {
    if (!a || !b) {
        return real_wcsicmp(a, b);
    }
    for (;;) {
        while (InPage(a) && InPage(b)) {
            unsigned __int64 x = *(const unsigned __int64*)a;
            if (x != *(const unsigned __int64*)b) {
                break;
            }
            // a zero character among the four ends both strings
            if ((x - 0x0001000100010001ull) & ~x & 0x8000800080008000ull) {
                return 0;
            }
            a += 4;
            b += 4;
        }
        unsigned c = *a, d = *b;
        if (c != d) {
            if (c >= 0x80 || d >= 0x80) {
                return real_wcsicmp(a, b);
            }
            if (c - 'A' < 26) {
                c += 32;
            }
            if (d - 'A' < 26) {
                d += 32;
            }
            if (c != d) {
                return (int)c - (int)d;
            }
        } else if (!c) {
            return 0;
        }
        a++;
        b++;
    }
}

/// The same for byte strings, which CrySystem compares while it opens the .pak archives.
static int __cdecl zz_stricmp(const char* a, const char* b) {
    if (!a || !b) {
        return real_stricmp(a, b);
    }
    for (;; a++, b++) {
        while (InPage(a) && InPage(b)) {
            unsigned __int64 x = *(const unsigned __int64*)a;
            if (x != *(const unsigned __int64*)b) {
                break;
            }
            // a zero byte among the eight ends both strings
            if ((x - 0x0101010101010101ull) & ~x & 0x8080808080808080ull) {
                return 0;
            }
            a += 8;
            b += 8;
        }
        unsigned c = (unsigned char)*a, d = (unsigned char)*b;
        if (c != d) {
            if (c >= 0x80 || d >= 0x80) {
                return real_stricmp(a, b);
            }
            if (c - 'A' < 26) {
                c += 32;
            }
            if (d - 'A' < 26) {
                d += 32;
            }
            if (c != d) {
                return (int)c - (int)d;
            }
        } else if (!c) {
            return 0;
        }
    }
}

/// The same for at most count characters, with which 4.6 finds the closing tag of every XML element it reads.
static int __cdecl zz_wcsnicmp(const wchar_t* a, const wchar_t* b, size_t count) {
    if (!a || !b) {
        return real_wcsnicmp(a, b, count);
    }
    for (; count; a++, b++, count--) {
        while (count >= 4 && InPage(a) && InPage(b)) {
            unsigned __int64 x = *(const unsigned __int64*)a;
            if (x != *(const unsigned __int64*)b) {
                break;
            }
            if ((x - 0x0001000100010001ull) & ~x & 0x8000800080008000ull) {
                return 0;
            }
            a += 4;
            b += 4;
            count -= 4;
        }
        if (!count) {
            return 0;
        }
        unsigned c = *a, d = *b;
        if (c != d) {
            if (c >= 0x80 || d >= 0x80) {
                return real_wcsnicmp(a, b, count);
            }
            if (c - 'A' < 26) {
                c += 32;
            }
            if (d - 'A' < 26) {
                d += 32;
            }
            if (c != d) {
                return (int)c - (int)d;
            }
        } else if (!c) {
            return 0;
        }
    }
    return 0;
}

/// Writes format into at most size - 1 characters and a terminating zero, handling only %d, %i, %u, %s and %% without flags, width or
/// precision; returns the length, -1 if it did not all fit, or -2 for anything else, having written part of it.
static int FormatPlain(wchar_t* buffer, size_t size, const wchar_t* format, va_list args) {
    va_list ap = args;
    size_t length = 0;
    bool truncated = false;
    auto put = [&](wchar_t c) {
        if (length + 1 < size) {
            buffer[length++] = c;
        } else {
            truncated = true;
        }
    };
    for (const wchar_t* f = format; *f; f++) {
        if (*f != L'%') {
            put(*f);
            continue;
        }
        wchar_t spec = *++f;
        if (spec == L'%') {
            put(L'%');
        } else if (spec == L'd' || spec == L'i' || spec == L'u') {
            int value = va_arg(ap, int);
            unsigned long long magnitude = spec == L'u' ? (unsigned)value : value < 0 ? 0ULL - (long long)value : (unsigned long long)value;
            if (spec != L'u' && value < 0) {
                put(L'-');
            }
            wchar_t digits[24];
            int n = 0;
            do {
                digits[n++] = (wchar_t)(L'0' + magnitude % 10);
                magnitude /= 10;
            } while (magnitude);
            while (n) {
                put(digits[--n]);
            }
        } else if (spec == L's') {
            const wchar_t* text = va_arg(ap, const wchar_t*);
            for (const wchar_t* t = text ? text : L"(null)"; *t; t++) {
                put(*t);
            }
        } else {
            return -2;
        }
    }
    buffer[length] = 0;
    return truncated ? -1 : (int)length;
}

/// Only with _TRUNCATE as the count, which writes what fits and returns -1 if it did not all fit. Anything else goes to the runtime,
/// which then reads the arguments from the start.
static int __cdecl zz_vsnwprintf_s(wchar_t* buffer, size_t size, size_t count, const wchar_t* format, va_list args) {
    if (count != _TRUNCATE || !buffer || !size || !format) {
        return real_vsnwprintf_s(buffer, size, count, format, args);
    }
    int length = FormatPlain(buffer, size, format, args);
    if (length == -2) {
        return real_vsnwprintf_s(buffer, size, count, format, args);
    }
    // the runtime clears the last element of the buffer as well
    buffer[size - 1] = 0;
    return length;
}

/// The legacy swprintf without a size, with which 4.6 builds the closing tag of every XML element it reads.
static int __cdecl zz_vswprintf(wchar_t* buffer, const wchar_t* format, va_list args) {
    int length = buffer && format ? FormatPlain(buffer, SIZE_MAX, format, args) : -2;
    return length == -2 ? real_vswprintf(buffer, format, args) : length;
}

// A table row sets each of its XML attributes through one function, which looks the attribute up in the field list of the table by
// comparing its name with every field name in turn. The fields are entries of 32 bytes ending with a type of 0: type at +0, name at
// +8; the function reads everything else from the entry it found, so starting the list at that entry gives the same result. The
// index of every name is kept per list, for lists in the image of Game.dll with plain ASCII names; anything else takes the long way.

typedef __int64(__fastcall* SetField_t)(const DWORD* fields, __int64 row, const wchar_t* name, const wchar_t* value);

static SetField_t real_SetField = nullptr;
static HMODULE s_game = nullptr;
static BYTE* s_gameEnd = nullptr;
static SRWLOCK s_fieldLock = SRWLOCK_INIT;

/// Open addressing over the field numbers of one list, by the hash of the folded name; the names stay in the list itself.
struct FieldIndex {
    bool handled = false;
    std::vector<int> slots; // field number or -1, a power of two in size
    std::vector<DWORD> hashes;
};

static std::unordered_map<const DWORD*, FieldIndex*>* s_fieldIndex;

static const wchar_t* FieldName(const DWORD* fields, int i) {
    return *(const wchar_t* const*)(fields + 8 * i + 2);
}

/// FNV-1a over the name with A-Z folded; false at the first character outside ASCII.
static bool FoldedHash(const wchar_t* text, DWORD& hash) {
    hash = 2166136261u;
    for (; *text; text++) {
        unsigned c = *text;
        if (c >= 0x80) {
            return false;
        }
        hash = (hash ^ (c - 'A' < 26 ? c + 32 : c)) * 16777619u;
    }
    return true;
}

static bool EqualFolded(const wchar_t* a, const wchar_t* b) {
    for (;; a++, b++) {
        unsigned c = *a, d = *b;
        c = c - 'A' < 26 ? c + 32 : c;
        d = d - 'A' < 26 ? d + 32 : d;
        if (c != d) {
            return false;
        }
        if (!c) {
            return true;
        }
    }
}

/// Slot of the name, holding its field number, or the empty slot where it would go.
static size_t FindSlot(const DWORD* fields, const FieldIndex& index, const wchar_t* name, DWORD hash) {
    size_t mask = index.slots.size() - 1;
    for (size_t slot = hash & mask;; slot = (slot + 1) & mask) {
        int i = index.slots[slot];
        if (i < 0 || index.hashes[slot] == hash && EqualFolded(FieldName(fields, i), name)) {
            return slot;
        }
    }
}

static FieldIndex* IndexFields(const DWORD* fields) {
    FieldIndex* index = new FieldIndex();
    int count = 0;
    while (fields[8 * count]) {
        count++;
    }
    size_t size = 16;
    while (size < 2 * (size_t)count) {
        size *= 2;
    }
    index->slots.assign(size, -1);
    index->hashes.assign(size, 0);
    for (int i = 0; i < count; i++) {
        const wchar_t* name = FieldName(fields, i);
        DWORD hash;
        if (!name || !FoldedHash(name, hash)) {
            return index;
        }
        size_t slot = FindSlot(fields, *index, name, hash);
        // the first field of a name is the one the client finds
        if (index->slots[slot] < 0) {
            index->slots[slot] = i;
            index->hashes[slot] = hash;
        }
    }
    index->handled = true;
    return index;
}

static __int64 __fastcall zzSetField(const DWORD* fields, __int64 row, const wchar_t* name, const wchar_t* value) {
    DWORD hash;
    if (!value || !name || (BYTE*)fields < (BYTE*)s_game || (BYTE*)fields + 32 > s_gameEnd || !FoldedHash(name, hash)) {
        return real_SetField(fields, row, name, value);
    }
    // the tables load one after another, so the last list is nearly always the one asked again
    static thread_local const DWORD* lastFields = nullptr;
    static thread_local FieldIndex* lastIndex = nullptr;
    FieldIndex* index = lastFields == fields ? lastIndex : nullptr;
    if (!index) {
        AcquireSRWLockShared(&s_fieldLock);
        auto list = s_fieldIndex->find(fields);
        index = list != s_fieldIndex->end() ? list->second : nullptr;
        ReleaseSRWLockShared(&s_fieldLock);
        if (!index) {
            AcquireSRWLockExclusive(&s_fieldLock);
            FieldIndex*& entry = (*s_fieldIndex)[fields];
            if (!entry) {
                entry = IndexFields(fields);
            }
            index = entry;
            ReleaseSRWLockExclusive(&s_fieldLock);
        }
        lastFields = fields;
        lastIndex = index;
    }
    if (!index->handled) {
        return real_SetField(fields, row, name, value);
    }
    int found = index->slots[FindSlot(fields, *index, name, hash)];
    // a name the list does not have: the function would compare it with all fields and return 0
    return found < 0 ? 0 : real_SetField(fields + 8 * found, row, name, value);
}

/// The lookup loop: mov rdx, [rax+8]; mov rcx, (name); call [_wcsicmp]; test eax, eax; jz found; ...; shl rax, 5; ...; cmp dword ptr [rax], 0; jnz loop
static void HookFieldLookup(HMODULE game) {
    for (BYTE* loop : FindAllPatterns(game, "48 8B 50 08 ?? 8B ?? FF 15 ?? ?? ?? ?? 85 C0 74")) {
        BYTE* shift = nullptr;
        for (BYTE* p = loop + 17; p < loop + 0x30; p++) {
            if (p[0] == 0x48 && p[1] == 0xC1 && p[2] == 0xE0 && p[3] == 0x05) {
                shift = p;
                break;
            }
        }
        BYTE* end = nullptr;
        for (BYTE* p = shift; p && p < shift + 12; p++) {
            if (p[0] == 0x83 && p[1] == 0x38 && p[2] == 0x00 && p[3] == 0x75) {
                end = p;
                break;
            }
        }
        BYTE* function = end ? FunctionStart(loop) : nullptr;
        if (!function) {
            continue;
        }
        s_game = game;
        s_gameEnd = (BYTE*)game + ModuleImageSize(game);
        s_fieldIndex = new std::unordered_map<const DWORD*, FieldIndex*>();
        real_SetField = (SetField_t)function;
        DetourAttach(&(PVOID&)real_SetField, zzSetField);
        ModsLog("fast start: field lookup at %p", function);
        return;
    }
    ModsLog("fast start: field lookup not in this client, only 4.x has it");
}

// 5.x clients write a report of the table heap after loading each table: they walk the whole heap with HeapWalk and print every
// region and large block. Without a file to write to (memstat.txt is written only on request), the lines go to the client's log
// system, which drops them, and the walks take about 40 % of loading all tables. A report without a file is skipped.

typedef void(__fastcall* HeapReport_t)(HANDLE heap, HANDLE file, char flag, const char* name);

static HeapReport_t real_HeapReport = nullptr;

static void __fastcall zzHeapReport(HANDLE heap, HANDLE file, char flag, const char* name) {
    static HANDLE logged = nullptr;
    if (file != logged) {
        logged = file;
        ModsLog("fast start: heap report to %p, file type %lu", file, GetFileType(file));
    }
    if (GetFileType(file) != FILE_TYPE_UNKNOWN) {
        real_HeapReport(heap, file, flag, name);
    }
}

/// lea rdx, "\t+ LBlock %03d : %p ~ %p (%d k)" in the report function
static void HookHeapReport(HMODULE game) {
    static const char LINE[] = "\t+ LBlock %03d : %p ~ %p (%d k)";
    const void* text = FindBytes(game, LINE, sizeof(LINE));
    BYTE* lea = text ? FindLeaTo(game, text) : nullptr;
    BYTE* function = lea ? FunctionStart(lea) : nullptr;
    if (!function) {
        ModsLog("fast start: heap report not found");
        return;
    }
    real_HeapReport = (HeapReport_t)function;
    DetourAttach(&(PVOID&)real_HeapReport, zzHeapReport);
    ModsLog("fast start: heap report at %p", function);
}

// 5.x clients look keys up in a std::map<int, ...> while loading the tables with std::lower_bound over its iterators, which finds
// each middle by stepping through the tree node by node, so a lookup walks half the map. Over the whole map the result is the
// node a descent from the root finds; any other range takes the long way.

struct MapNode {
    MapNode* left;
    MapNode* parent;
    MapNode* right;
    char color;
    char isNil;
    int key;
};

typedef MapNode** (__fastcall* LowerBound_t)(MapNode** result, MapNode* first, MapNode* last, const int* key);

static LowerBound_t real_LowerBound = nullptr;

static MapNode** __fastcall zzLowerBound(MapNode** result, MapNode* first, MapNode* last, const int* key) {
    // the head of the map is the end; its left is the first node and its parent the root
    if (!last->isNil || first != last->left) {
        return real_LowerBound(result, first, last, key);
    }
    MapNode* found = last;
    for (MapNode* node = last->parent; !node->isNil;) {
        if (node->key < *key) {
            node = node->right;
        } else {
            found = node;
            node = node->left;
        }
    }
    *result = found;
    return result;
}

/// The middle of the range: call (advance); mov rcx, [rsp+40h]; cmp [rcx+1Ch], ebp; jge
static void HookMapLowerBound(HMODULE game) {
    BYTE* middle = FindPattern(game, "E8 ?? ?? ?? ?? 48 8B 4C 24 40 39 69 1C 7D");
    BYTE* function = middle ? FunctionStart(middle) : nullptr;
    if (!function) {
        ModsLog("fast start: map lookup not in this client, only 5.x has it");
        return;
    }
    real_LowerBound = (LowerBound_t)function;
    DetourAttach(&(PVOID&)real_LowerBound, zzLowerBound);
    ModsLog("fast start: map lookup at %p", function);
}

/// Once CrySystem is loaded, which needs the runtime (VC 8 in 4.x, VC 12 in 5.x), before it opens the .pak archives.
static void HookRuntime() {
    HMODULE crt = GetModuleHandleW(L"MSVCR80.dll");
    crt = crt ? crt : GetModuleHandleW(L"MSVCR120.dll");
    if (!crt || real_wcsicmp) {
        return;
    }
    real_wcsicmp = (Wcsicmp_t)GetProcAddress(crt, "_wcsicmp");
    real_vsnwprintf_s = (Vsnwprintf_t)GetProcAddress(crt, "_vsnwprintf_s");
    if (!real_wcsicmp || !real_vsnwprintf_s) {
        ModsLog("fast start: runtime functions not found");
        return;
    }
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&)real_wcsicmp, zz_wcsicmp);
    DetourAttach(&(PVOID&)real_vsnwprintf_s, zz_vsnwprintf_s);
    real_vswprintf = (Vswprintf_t)GetProcAddress(crt, "_vswprintf");
    if (real_vswprintf) {
        DetourAttach(&(PVOID&)real_vswprintf, zz_vswprintf);
    }
    real_wcsnicmp = (Wcsnicmp_t)GetProcAddress(crt, "_wcsnicmp");
    if (real_wcsnicmp) {
        DetourAttach(&(PVOID&)real_wcsnicmp, zz_wcsnicmp);
    }
    real_stricmp = (Stricmp_t)GetProcAddress(crt, "_stricmp");
    if (real_stricmp) {
        DetourAttach(&(PVOID&)real_stricmp, zz_stricmp);
    }
    ModsLog("fast start: runtime string functions at %p: %ld", crt, DetourTransactionCommit());
}

/// Once Game.dll is loaded, which needs the runtime, and before it loads the game data.
// 4.x clients look the name of every attribute of every quest up in a table of some 3700 tokens, comparing it with one token after
// another: a fifth of the compares while the game data loads. The loop now starts at the token of that name, found by a hash.
extern "C" {
UINT64 g_questTokenTable = 0;
UINT64 g_questTokenEnd = 0;
UINT64 g_questTokenLoop = 0;
void QuestTokenStub();
}
static std::unordered_map<DWORD, int> s_questTokens;

/// Where the loop starts: at the token of that name, at the last token when there is none (it differs, so the loop ends as without
/// a match), or at the first when the hash cannot tell.
extern "C" int QuestTokenIndex(const wchar_t* name) {
    const wchar_t** tokens = (const wchar_t**)g_questTokenTable;
    int count = (int)((g_questTokenEnd - g_questTokenTable) / sizeof(void*));
    DWORD hash;
    if (!name || !FoldedHash(name, hash)) {
        return 0;
    }
    auto found = s_questTokens.find(hash);
    if (found == s_questTokens.end()) {
        return count - 1;
    }
    return EqualFolded(name, tokens[found->second]) ? found->second : 0;
}

/// mov ebx, ebp; lea r12, <tokens>; lea r14, <end>; nops; loop: mov rdx, [r12]; mov rcx, r13; call [_wcsicmp]; test eax, eax; jz found;
/// add r12, 8; add ebx, 1; cmp r12, r14; jl loop
static void HookQuestTokens(HMODULE game) {
    BYTE* p = FindPattern(game, "8B DD 4C 8D 25 ?? ?? ?? ?? 4C 8D 35 ?? ?? ?? ?? 66 66 90 66 66 66 90 49 8B 14 24 49 8B CD FF 15 ?? ?? ?? ?? 85 C0 74 ?? 49 83 C4 08 83 C3 01 4D 3B E6 7C E3");
    if (!p) {
        ModsLog("fast start: quest tokens not in this client, only 4.x has them");
        return;
    }
    const wchar_t** tokens = (const wchar_t**)ResolveRip(p + 5, p + 9);
    const wchar_t** end = (const wchar_t**)ResolveRip(p + 12, p + 16);
    for (const wchar_t** token = tokens; token < end; token++) {
        DWORD hash;
        if (!*token || !FoldedHash(*token, hash)) {
            ModsLog("fast start: quest token %d not in ASCII", (int)(token - tokens));
            s_questTokens.clear();
            return;
        }
        // the first of equal names, as the loop finds it
        s_questTokens.emplace(hash, (int)(token - tokens));
    }
    g_questTokenTable = (UINT64)tokens;
    g_questTokenEnd = (UINT64)end;
    g_questTokenLoop = (UINT64)(p + 23);
    static PVOID patched = p + 2;
    DetourAttach(&patched, QuestTokenStub);
    ModsLog("fast start: quest tokens at %p, %d of them", p, (int)(end - tokens));
}

void InstallFastLoad(HMODULE game) {
    HookFieldLookup(game);
    HookQuestTokens(game);
    HookHeapReport(game);
    HookMapLowerBound(game);
}

// A running client deletes sc_renew_local.dat and sets NormalStop=0 in data\dump\config.ini, and puts both back when it closes normally.
// A client starting meanwhile takes that for a crash: it deletes the shader cache, and 5.x clients load all game tables again (a minute
// and more, behind a progress bar) instead of reading their dump. While another client of the same folder runs, neither happens.

/// Whether another client started from the same aion.bin is running.
static bool OtherClientRunning() {
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return false;
    }
    bool found = false;
    PROCESSENTRY32W entry = { sizeof(entry) };
    for (BOOL ok = Process32FirstW(snapshot, &entry); ok && !found; ok = Process32NextW(snapshot, &entry)) {
        if (entry.th32ProcessID == GetCurrentProcessId() || _wcsicmp(entry.szExeFile, PathFindFileNameW(self)) != 0) {
            continue;
        }
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
        if (!process) {
            continue;
        }
        wchar_t path[MAX_PATH];
        DWORD length = MAX_PATH;
        found = QueryFullProcessImageNameW(process, 0, path, &length) && _wcsicmp(path, self) == 0;
        CloseHandle(process);
    }
    CloseHandle(snapshot);
    return found;
}

/// The client deletes the shader cache when it starts unless sc_renew_local.dat holds the same number as sc_renew.dat; the running
/// client has deleted it, so a copy of sc_renew.dat goes in its place. aion.bin checks its own code, so the check itself stays as it is.
static void KeepShaderCache() {
    wchar_t root[MAX_PATH], renew[MAX_PATH], local[MAX_PATH];
    GetModuleFileNameW(nullptr, root, MAX_PATH);
    PathRemoveFileSpecW(root);
    PathRemoveFileSpecW(root);
    PathCombineW(renew, root, L"sc_renew.dat");
    PathCombineW(local, root, L"sc_renew_local.dat");
    if (GetFileAttributesW(local) == INVALID_FILE_ATTRIBUTES && CopyFileW(renew, local, TRUE)) {
        ModsLog("fast start: sc_renew_local.dat put back, the shader cache stays");
    }
}

static bool IsNormalStop(const char* key) {
    return key && _stricmp(key, "NormalStop") == 0;
}

static bool IsNormalStop(const wchar_t* key) {
    return key && _wcsicmp(key, L"NormalStop") == 0;
}

static decltype(GetPrivateProfileIntA)* real_GetPrivateProfileIntA = GetPrivateProfileIntA;
static decltype(GetPrivateProfileIntW)* real_GetPrivateProfileIntW = GetPrivateProfileIntW;
static decltype(GetPrivateProfileStringA)* real_GetPrivateProfileStringA = GetPrivateProfileStringA;
static decltype(GetPrivateProfileStringW)* real_GetPrivateProfileStringW = GetPrivateProfileStringW;

static UINT WINAPI zzGetPrivateProfileIntA(LPCSTR app, LPCSTR key, INT fallback, LPCSTR file) {
    if (IsNormalStop(key)) {
        ModsLog("fast start: NormalStop read as 1");
        return 1;
    }
    return real_GetPrivateProfileIntA(app, key, fallback, file);
}

static UINT WINAPI zzGetPrivateProfileIntW(LPCWSTR app, LPCWSTR key, INT fallback, LPCWSTR file) {
    if (IsNormalStop(key)) {
        ModsLog("fast start: NormalStop read as 1");
        return 1;
    }
    return real_GetPrivateProfileIntW(app, key, fallback, file);
}

static DWORD WINAPI zzGetPrivateProfileStringA(LPCSTR app, LPCSTR key, LPCSTR fallback, LPSTR out, DWORD size, LPCSTR file) {
    if (IsNormalStop(key) && out && size >= 2) {
        ModsLog("fast start: NormalStop read as 1");
        strcpy_s(out, size, "1");
        return 1;
    }
    return real_GetPrivateProfileStringA(app, key, fallback, out, size, file);
}

static DWORD WINAPI zzGetPrivateProfileStringW(LPCWSTR app, LPCWSTR key, LPCWSTR fallback, LPWSTR out, DWORD size, LPCWSTR file) {
    if (IsNormalStop(key) && out && size >= 2) {
        ModsLog("fast start: NormalStop read as 1");
        wcscpy_s(out, size, L"1");
        return 1;
    }
    return real_GetPrivateProfileStringW(app, key, fallback, out, size, file);
}

/// Keeps a client from taking another running one for a crash.
static void KeepCaches() {
    if (!OtherClientRunning()) {
        return;
    }
    ModsLog("fast start: another client of this folder runs, keeping its caches");
    KeepShaderCache();
    DetourAttach(&(PVOID&)real_GetPrivateProfileIntA, zzGetPrivateProfileIntA);
    DetourAttach(&(PVOID&)real_GetPrivateProfileIntW, zzGetPrivateProfileIntW);
    DetourAttach(&(PVOID&)real_GetPrivateProfileStringA, zzGetPrivateProfileStringA);
    DetourAttach(&(PVOID&)real_GetPrivateProfileStringW, zzGetPrivateProfileStringW);
}

// OpenSSL 1.0 seeds its generator on first use with RAND_poll, which on Windows walks every heap block and thread of the process
// through Toolhelp: half a second or more in a client that has loaded its data. The system generator gives the seed at once.

typedef int(__cdecl* RandPoll_t)();
typedef void(__cdecl* RandAdd_t)(const void* buffer, int length, double entropy);

static RandPoll_t real_RAND_poll = nullptr;
static RandAdd_t s_RAND_add = nullptr;

static int __cdecl zzRAND_poll() {
    BYTE seed[64];
    if (BCryptGenRandom(nullptr, seed, sizeof(seed), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        return real_RAND_poll();
    }
    s_RAND_add(seed, sizeof(seed), sizeof(seed));
    SecureZeroMemory(seed, sizeof(seed));
    return 1;
}

static bool FindRandPoll(HMODULE libeay) {
    real_RAND_poll = (RandPoll_t)GetProcAddress(libeay, "RAND_poll");
    s_RAND_add = (RandAdd_t)GetProcAddress(libeay, "RAND_add");
    if (!real_RAND_poll || !s_RAND_add) {
        ModsLog("fast start: RAND_poll not found");
        return false;
    }
    return true;
}

/// When the loader loads libeay32.dll later on, outside the transaction of the mods.
static void HookRandPoll(HMODULE libeay) {
    if (FindRandPoll(libeay)) {
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourAttach(&(PVOID&)real_RAND_poll, zzRAND_poll);
        ModsLog("fast start: RAND_poll hooked: %ld", DetourTransactionCommit());
    }
}

// Without -ncg on its command line aion.bin starts the guard of NCsoft (aegisty64.bin): it hashes the whole file to check its signature
// and then sets it up, half a second before the client window shows. The command line the client reads gets -ncg when it has not.
static decltype(GetCommandLineA)* real_GetCommandLineA = nullptr;
static decltype(GetCommandLineW)* real_GetCommandLineW = nullptr;
static std::string s_commandLineA;
static std::wstring s_commandLineW;

static LPSTR WINAPI zzGetCommandLineA() {
    return s_commandLineA.data();
}

static LPWSTR WINAPI zzGetCommandLineW() {
    return s_commandLineW.data();
}

static void SkipGuard() {
    HMODULE kernelBase = GetModuleHandleW(L"kernelbase.dll");
    real_GetCommandLineA = kernelBase ? (decltype(GetCommandLineA)*)GetProcAddress(kernelBase, "GetCommandLineA") : nullptr;
    real_GetCommandLineW = kernelBase ? (decltype(GetCommandLineW)*)GetProcAddress(kernelBase, "GetCommandLineW") : nullptr;
    if (!real_GetCommandLineA || !real_GetCommandLineW) {
        ModsLog("fast start: command line not found");
        return;
    }
    s_commandLineA = real_GetCommandLineA();
    s_commandLineW = real_GetCommandLineW();
    if (s_commandLineA.find(" -ncg") != std::string::npos) {
        return;
    }
    s_commandLineA += " -ncg";
    s_commandLineW += L" -ncg";
    DetourAttach(&(PVOID&)real_GetCommandLineA, zzGetCommandLineA);
    DetourAttach(&(PVOID&)real_GetCommandLineW, zzGetCommandLineW);
    ModsLog("fast start: -ncg added to the command line");
}

void InstallFastStart() {
    SkipGuard();
    KeepCaches();
    HMODULE libeay = GetModuleHandleW(L"libeay32.dll");
    if (!libeay) {
        OnModuleLoad(L"libeay32.dll", HookRandPoll, false);
    } else if (FindRandPoll(libeay)) {
        DetourAttach(&(PVOID&)real_RAND_poll, zzRAND_poll);
        ModsLog("fast start: RAND_poll hooked");
    }
    DetourAttach(&(PVOID&)real_GetTickCount, zzGetTickCount);
    if (!FindUnloadWait(GetModuleHandleW(nullptr))) {
        CloseHandle(CreateThread(nullptr, 0, RetryUnloadWait, nullptr, 0, nullptr));
    }
    HMODULE ole32 = LoadLibraryW(L"ole32.dll");
    real_CoCreateInstance = ole32 ? (decltype(CoCreateInstance)*)GetProcAddress(ole32, "CoCreateInstance") : nullptr;
    if (real_CoCreateInstance) {
        DetourAttach(&(PVOID&)real_CoCreateInstance, zzCoCreateInstance);
    } else {
        ModsLog("fast start: CoCreateInstance not found");
    }
    HMODULE kernelBase = GetModuleHandleW(L"kernelbase.dll");
    real_RegSetValueExA = kernelBase ? (decltype(RegSetValueExA)*)GetProcAddress(kernelBase, "RegSetValueExA") : nullptr;
    if (real_RegSetValueExA) {
        DetourAttach(&(PVOID&)real_RegSetValueExA, zzRegSetValueExA);
    }
    OnModuleLoad(L"CrySystem.dll", OnCrySystem, true);
}

/// The logo before the login screen fades in until 750 ms, stays until 2250 ms and fades out until 3000 ms; only then the login
/// screen starts to load: cmp reg, 2EEh; ja ...; cmp reg, 8CAh; ja ...; mov eax, 0BB8h; cmp reg, eax; ja login
/// All three become 0, so the login screen follows on the next frame.
static void SkipStartLogo(HMODULE game) {
    for (BYTE* fadeIn : FindAllPatterns(game, "81 ?? EE 02 00 00 77")) {
        BYTE* hold = nullptr;
        BYTE* fadeOut = nullptr;
        for (BYTE* p = fadeIn + 8; p < fadeIn + 0x100; p++) {
            if (!hold && p[0] == 0x81 && *(DWORD*)(p + 2) == 2250) {
                hold = p + 2;
            } else if (hold && p[0] == 0xB8 && *(DWORD*)(p + 1) == 3000) {
                fadeOut = p + 1;
                break;
            }
        }
        if (!fadeOut) {
            continue;
        }
        DWORD zero = 0;
        PatchMemory(fadeIn + 2, &zero, sizeof(zero));
        PatchMemory(hold, &zero, sizeof(zero));
        PatchMemory(fadeOut, &zero, sizeof(zero));
        ModsLog("fast start: logo timer at %p", fadeIn);
        return;
    }
    ModsLog("fast start: logo timer not found");
}

/// The picture shown on quitting fades in until 1000 ms, stays until 4000 ms and fades out until 5000 ms; only then the client closes:
/// cmp eax, 3E8h; jbe ...; cmp eax, 0FA0h; ja ...; (the full colour); mov ecx, 1388h; cmp eax, ecx; ja close
/// All three become 0 as well.
static void SkipEndingPicture(HMODULE game) {
    for (BYTE* ending : FindAllPatterns(game, "3D E8 03 00 00 76 ?? 3D A0 0F 00 00 77")) {
        for (BYTE* p = ending + 14; p < ending + 0x40; p++) {
            if (p[0] == 0xB9 && *(DWORD*)(p + 1) == 5000 && p[5] == 0x3B && p[6] == 0xC1 && p[7] == 0x77) {
                DWORD zero = 0;
                PatchMemory(ending + 1, &zero, sizeof(zero));
                PatchMemory(ending + 8, &zero, sizeof(zero));
                PatchMemory(p + 1, &zero, sizeof(zero));
                ModsLog("fast start: ending timer at %p", ending);
                return;
            }
        }
    }
    ModsLog("fast start: ending timer not found");
}

void InstallSkipLogo(HMODULE game) {
    SkipStartLogo(game);
    SkipEndingPicture(game);
}
