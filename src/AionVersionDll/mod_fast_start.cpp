#include "mods.h"
#include "module_load.h"
#include <string>
#include <thread>
#include <vector>
#include <intrin.h>
#include <d3d9.h>
#include <objbase.h>
#include <dxgi.h>
#include <bcrypt.h>
#include <tlhelp32.h>
#include <shlwapi.h>
#include <detours/detours.h>
#include "install_report.h"
#include <atomic>

#pragma comment(lib, "bcrypt.lib")
#include "mod_fast_start.h"


// Before it starts, the client waits until no closing client is unloading any more: it polls for a named mutex every 10 ms, but only
// once the whole seconds of GetTickCount since the wait began exceed 2, so every start waits 2 to 3 seconds for nothing:
// call GetTickCount (start); ... loop: call GetTickCount; ...; cmp esi, ebx (seconds, 2); jbe sleep; mov rcx, rdi; call poll
// aion.bin checks its own code and stops with "File corrupted!" when it finds it changed, so GetTickCount answers 3 seconds later
// to the call in the loop instead. A client that is really unloading is still waited for.

static decltype(GetTickCount)* real_GetTickCount = GetTickCount;
static std::atomic<BYTE*> s_waitTickReturn = nullptr;

static DWORD WINAPI zzGetTickCount() {
    DWORD now = real_GetTickCount();
    // a protected call returns one byte further, past the byte it skips
    size_t past = (BYTE*)_ReturnAddress() - s_waitTickReturn;
    return s_waitTickReturn && past <= 1 ? now + 3000 : now;
}

static BYTE* LocateUnloadWait(HMODULE exe) {
    static const char NAME[] = "AionUnloadMutex";
    BYTE* lea = FindStringLea(exe, NAME);
    if (!lea) {
        return nullptr;
    }
    // functions start after the int3 padding in front of them
    BYTE* poll = lea;
    while (poll > lea - 0x40 && !(poll[-1] == 0xCC && poll[0] != 0xCC)) {
        poll--;
    }
    BYTE* call = FindCallTo(exe, poll);
    if (!call || call[-5] != 0x76 || call[-3] != 0x48 || call[-2] != 0x8B) {
        return nullptr;
    }
    BYTE* wait = call;
    while (wait > call - 0x100 && !(wait[-1] == 0xCC && wait[0] != 0xCC)) {
        wait--;
    }
    // the second call of the function is the one in the loop
    int calls = 0;
    for (BYTE* p = wait; p < call - 5; p++) {
        if (p[0] == 0xE8 && ++calls == 2) {
            return p;
        }
    }
    return nullptr;
}

/// Lets the hook of GetTickCount tell the call in the loop by where it returns to.
static void ApplyUnloadWait(BYTE* call) {
    s_waitTickReturn = call + 5;
    ModsLog("fast start: unload wait at %p", call);
}

/// The call of GetTickCount in the loop that waits for another client to unload, if the code is there yet.
static bool FindUnloadWait(HMODULE exe) {
    BYTE* call;
    {
        LocatePhase phase;
        call = LocateUnloadWait(exe);
    }
    if (call) {
        ApplyUnloadWait(call);
    }
    return call != nullptr;
}

/// Some clients unpack aion.bin only after version.dll is loaded, so the code may not be there yet.
static DWORD WINAPI RetryUnloadWait(LPVOID) {
    HMODULE exe = ClientModule();
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
static std::atomic<bool> s_dxDiagSkipped = false;
static bool s_textComparesFound = false;
static bool FindTextCompares(HMODULE exe);

/// The client collects a DirectX diagnostic report at every start, which takes about 3 seconds, only to store it in the registry
/// (HKCU\Software\Aion\Client\SystemInfo). Without the provider it skips the report and keeps the values stored before.
static HRESULT WINAPI zzCoCreateInstance(REFCLSID clsid, LPUNKNOWN outer, DWORD context, REFIID iid, LPVOID* object) {
    if (IsEqualCLSID(clsid, CLSID_DXDIAG_PROVIDER)) {
        if (object) {
            *object = nullptr;
        }
        s_dxDiagSkipped = true;
        ModsLog("fast start: DxDiag skipped");
        // the comparison of the hardware comes next; some clients unpack aion.bin only after the mods are installed
        if (!s_textComparesFound) {
            InstallScope scope("fast_start");
            s_textComparesFound = FindTextCompares(ClientModule());
        }
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

// At every start aion.bin compares the hardware with what it stored in SystemInfo at an earlier start: the vendor of the graphics card,
// then two texts of the report. When one differs it sets change_hardware, and CrySystem resets the graphics settings to its preset.
// Without the report a text is empty, and the client never stores an empty text, so the stored one stays and differs at every start.
// The two calls that compare the texts now go through a check where an empty text counts as the stored one; a text that is there is
// compared as before, and so is the vendor. Found by crlfrancis (pull request 10). The place, after the lea of "Display0.video_vendor_id"
// in the function that compares (4.6 to 5.8 alike, CrySystem holds a copy the client does not call):
//   lea rdx, [rsp+30h] (the stored text); mov rcx, rbx or rdi (the one of this start); call _stricmp     twice, to the same function
// Once both have been made, they call the client's function directly again, as aion.bin checks its own code.

typedef int(__cdecl* CompareText_t)(const char* current, const char* stored);

namespace {

struct TextCompares {
    CodePatch calls[2];
    BYTE* compare = nullptr;
};

TextCompares s_textCompares;
CompareText_t s_compareText = nullptr;
std::atomic<int> s_textCompared = 0;

int __cdecl zzCompareText(const char* current, const char* stored) {
    if (!*current) {
        ModsLog("fast start: empty hardware text taken as the stored one");
    }
    int result = *current ? s_compareText(current, stored) : 0;
    if (++s_textCompared == 2) {
        for (const CodePatch& call : s_textCompares.calls) {
            PatchMemory(call.at, call.expected, call.size);
        }
    }
    return result;
}

TextCompares LocateTextCompares(HMODULE exe) {
    static const char VENDOR[] = "Display0.video_vendor_id";
    static const BYTE LOAD_TEXTS[] = { 0x48, 0x8D, 0x54, 0x24, 0x30, 0x48, 0x8B };
    TextCompares found;
    BYTE* vendor = FindBytes(exe, VENDOR, sizeof(VENDOR));
    if (!vendor) {
        return found;
    }
    // the vendor's name is loaded where the client reads, stores and compares the hardware; only the comparison has the two calls
    std::vector<BYTE*> calls;
    size_t functions = 0;
    for (BYTE* lea : FindAllLeasTo(exe, vendor)) {
        std::vector<BYTE*> here;
        for (BYTE* p = lea; p < lea + 0x180 && InModule(exe, p, 13); p++) {
            if (memcmp(p, LOAD_TEXTS, sizeof(LOAD_TEXTS)) == 0 && (p[7] == 0xCB || p[7] == 0xCF) && p[8] == 0xE8) {
                here.push_back(p + 8);
            }
        }
        if (!here.empty()) {
            functions++;
            calls = here;
        }
    }
    ReportSearch(functions);
    if (functions != 1 || calls.size() != 2) {
        ModsLog("fast start: hardware comparison not found (%zu functions, %zu calls)", functions, calls.size());
        return found;
    }
    BYTE* compare = ResolveRip(calls[0] + 1, calls[0] + 5);
    if (ResolveRip(calls[1] + 1, calls[1] + 5) != compare || !InModule(exe, compare, 1)) {
        ModsLog("fast start: hardware comparison calls two functions");
        return found;
    }
    found.calls[0] = ExpectBytes(calls[0] + 1, sizeof(INT32));
    found.calls[1] = ExpectBytes(calls[1] + 1, sizeof(INT32));
    found.compare = compare;
    return found;
}

void ApplyTextCompares(const TextCompares& found) {
    if (!found.compare) {
        return;
    }
    // jmp [rip+0]; dq zzCompareText, within reach of the calls
    static const BYTE JUMP[] = { 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00 };
    DWORD size = 0;
    BYTE* jump = (BYTE*)DetourAllocateRegionWithinJumpBounds(found.calls[0].at, &size);
    INT64 distances[2] = {};
    for (int i = 0; i < 2 && jump; i++) {
        distances[i] = jump - (found.calls[i].at + 4);
        if (distances[i] != (INT32)distances[i]) {
            jump = nullptr;
        }
    }
    if (!jump || size < sizeof(JUMP) + sizeof(void*)) {
        ModsLog("fast start: no memory near the hardware comparison");
        ReportStatus(InstallStatus::PatchFailed);
        return;
    }
    void* target = (void*)zzCompareText;
    memcpy(jump, JUMP, sizeof(JUMP));
    memcpy(jump + sizeof(JUMP), &target, sizeof(target));
    FlushInstructionCache(GetCurrentProcess(), jump, sizeof(JUMP) + sizeof(target));
    s_compareText = (CompareText_t)found.compare;
    s_textCompares = found;
    for (int i = 0; i < 2; i++) {
        INT32 distance = (INT32)distances[i];
        ApplyPatch(found.calls[i], &distance);
    }
    ModsLog("fast start: hardware comparison at %p and %p", found.calls[0].at - 1, found.calls[1].at - 1);
}

} // namespace

/// Searches aion.bin for the comparison, if the code is there yet.
static bool FindTextCompares(HMODULE exe) {
    TextCompares found;
    {
        LocatePhase phase;
        found = LocateTextCompares(exe);
    }
    ApplyTextCompares(found);
    return found.compare != nullptr;
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
    AttachHook(&(PVOID&)real_GetPrivateProfileIntA, zzGetPrivateProfileIntA);
    AttachHook(&(PVOID&)real_GetPrivateProfileIntW, zzGetPrivateProfileIntW);
    AttachHook(&(PVOID&)real_GetPrivateProfileStringA, zzGetPrivateProfileStringA);
    AttachHook(&(PVOID&)real_GetPrivateProfileStringW, zzGetPrivateProfileStringW);
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
    InstallScope scope("fast_start");
    if (FindRandPoll(libeay)) {
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        AttachHook(&(PVOID&)real_RAND_poll, zzRAND_poll);
        ModsLog("fast start: RAND_poll hooked: %ld", CommitHooks());
    }
}

// The renderer fills a 128x128x128 A8 noise volume with D3DXFillVolumeTexture when it starts, one texel after another: 2 million calls
// of its noise function, which only reads a table filled before. Volumes the renderer fills in A8 are filled by slices on several
// threads instead, with the texture coordinates and the conversion of D3DX; anything else goes to D3DX.

/// D3DXVECTOR4* out, const D3DXVECTOR3* texCoord, const D3DXVECTOR3* texelSize, LPVOID data
typedef void(WINAPI* Fill3D_t)(float* out, const float* texCoord, const float* texelSize, void* data);
typedef HRESULT(WINAPI* FillVolumeTexture_t)(IDirect3DVolumeTexture9* texture, Fill3D_t function, void* data);
static FillVolumeTexture_t real_FillVolumeTexture = nullptr;

/// D3DX dithers what it converts to fewer bits in a volume by this 4x4x4 matrix [z & 3][y & 3][x & 3]: the value is rounded down after
/// adding (15.5 - matrix) / 16.
static const BYTE DITHER[4][4][4] = {
    { { 0, 8, 2, 10 }, { 6, 14, 4, 12 }, { 3, 11, 1, 9 }, { 5, 13, 7, 15 } },
    { { 8, 2, 10, 0 }, { 12, 6, 14, 4 }, { 11, 1, 9, 3 }, { 15, 5, 13, 7 } },
    { { 2, 10, 0, 8 }, { 4, 12, 6, 14 }, { 1, 9, 3, 11 }, { 7, 15, 5, 13 } },
    { { 10, 0, 8, 2 }, { 14, 4, 12, 6 }, { 9, 3, 11, 1 }, { 13, 7, 15, 5 } },
};

static bool FromRenderer(const void* address) {
    HMODULE renderer = GetModuleHandleW(L"XRenderD3D9.dll");
    return renderer && (size_t)((const BYTE*)address - (const BYTE*)renderer) < ModuleImageSize(renderer);
}

static HRESULT WINAPI zzFillVolumeTexture(IDirect3DVolumeTexture9* texture, Fill3D_t function, void* data) {
    D3DVOLUME_DESC desc;
    if (!texture || !FromRenderer((const void*)function) || texture->GetLevelCount() != 1 || FAILED(texture->GetLevelDesc(0, &desc))
        || desc.Format != D3DFMT_A8) {
        return real_FillVolumeTexture(texture, function, data);
    }
    D3DLOCKED_BOX box;
    if (FAILED(texture->LockBox(0, &box, nullptr, 0))) {
        return real_FillVolumeTexture(texture, function, data);
    }
    const float texelSize[3] = { 1.0f / desc.Width, 1.0f / desc.Height, 1.0f / desc.Depth };
    auto fillSlices = [&](UINT from, UINT step) {
        for (UINT z = from; z < desc.Depth; z += step) {
            for (UINT y = 0; y < desc.Height; y++) {
                BYTE* row = (BYTE*)box.pBits + (size_t)z * box.SlicePitch + (size_t)y * box.RowPitch;
                for (UINT x = 0; x < desc.Width; x++) {
                    const float texCoord[3] = { (x + 0.5f) * texelSize[0], (y + 0.5f) * texelSize[1], (z + 0.5f) * texelSize[2] };
                    float out[4];
                    function(out, texCoord, texelSize, data);
                    row[x] = (BYTE)(min(max(out[3], 0.0f), 1.0f) * 255.0f + (15.5f - DITHER[z & 3][y & 3][x & 3]) / 16);
                }
            }
        }
    };
    UINT threads = min(max(std::thread::hardware_concurrency(), 1u), min(desc.Depth, 16u));
    std::vector<std::thread> workers;
    for (UINT i = 1; i < threads; i++) {
        workers.emplace_back(fillSlices, i, threads);
    }
    fillSlices(0, threads);
    for (std::thread& worker : workers) {
        worker.join();
    }
    texture->UnlockBox(0);
    return D3D_OK;
}

static bool FindFillVolumeTexture(HMODULE d3dx) {
    real_FillVolumeTexture = (FillVolumeTexture_t)GetProcAddress(d3dx, "D3DXFillVolumeTexture");
    if (!real_FillVolumeTexture) {
        ModsLog("fast start: D3DXFillVolumeTexture not found");
        return false;
    }
    return true;
}

/// When the loader loads d3dx9_38.dll later on, outside the transaction of the mods.
static void HookFillVolumeTexture(HMODULE d3dx) {
    InstallScope scope("fast_start");
    if (FindFillVolumeTexture(d3dx)) {
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        AttachHook(&(PVOID&)real_FillVolumeTexture, zzFillVolumeTexture);
        ModsLog("fast start: D3DXFillVolumeTexture hooked: %ld", CommitHooks());
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
    AttachHook(&(PVOID&)real_GetCommandLineA, zzGetCommandLineA);
    AttachHook(&(PVOID&)real_GetCommandLineW, zzGetCommandLineW);
    ModsLog("fast start: -ncg added to the command line");
}

// In pseudo fullscreen the renderer sets the size of its window as the display mode with ChangeDisplaySettingsA(CDS_FULLSCREEN), which
// takes the driver a good while even when the display already has that size. A change of the size alone to the size the display has is
// taken as done.
static decltype(ChangeDisplaySettingsA)* real_ChangeDisplaySettingsA = nullptr;

static LONG WINAPI zzChangeDisplaySettingsA(DEVMODEA* mode, DWORD flags) {
    DEVMODEA current = {};
    current.dmSize = sizeof(current);
    if (mode && flags == CDS_FULLSCREEN && (mode->dmFields & ~(DM_PELSWIDTH | DM_PELSHEIGHT)) == 0
        && EnumDisplaySettingsA(nullptr, ENUM_CURRENT_SETTINGS, &current) && mode->dmPelsWidth == current.dmPelsWidth
        && mode->dmPelsHeight == current.dmPelsHeight) {
        ModsLog("fast start: display already %lux%lu", current.dmPelsWidth, current.dmPelsHeight);
        return DISP_CHANGE_SUCCESSFUL;
    }
    return real_ChangeDisplaySettingsA(mode, flags);
}

// The log of CrySystem 5.x opens AionLog\log.txt anew for every line ("at"), some 4000 times while the .pak archives open. Without the
// folder AionLog every open fails after a lookup in the file system. An open for writing that failed because the path does not exist
// fails at once for the same name for a second after.
typedef errno_t(__cdecl* FopenS_t)(FILE** file, const char* name, const char* mode);
static FopenS_t real_fopen_s = nullptr;
static SRWLOCK s_missingLock = SRWLOCK_INIT;
static std::string s_missingName;
static DWORD s_missingSince;

static errno_t __cdecl zz_fopen_s(FILE** file, const char* name, const char* mode) {
    bool writing = name && mode && (mode[0] == 'w' || mode[0] == 'a');
    if (writing) {
        AcquireSRWLockShared(&s_missingLock);
        bool missing = s_missingName == name && real_GetTickCount() - s_missingSince < 1000;
        ReleaseSRWLockShared(&s_missingLock);
        if (missing && file) {
            *file = nullptr;
            return ENOENT;
        }
    }
    errno_t error = real_fopen_s(file, name, mode);
    if (writing && error == ENOENT) {
        AcquireSRWLockExclusive(&s_missingLock);
        s_missingName = name;
        s_missingSince = real_GetTickCount();
        ReleaseSRWLockExclusive(&s_missingLock);
    }
    return error;
}

void InstallFastStart() {
    SkipGuard();
    KeepCaches();
    HMODULE libeay = GetModuleHandleW(L"libeay32.dll");
    if (!libeay) {
        OnModuleLoad(L"libeay32.dll", HookRandPoll, false);
    } else if (FindRandPoll(libeay)) {
        AttachHook(&(PVOID&)real_RAND_poll, zzRAND_poll);
        ModsLog("fast start: RAND_poll hooked");
    }
    AttachHook(&(PVOID&)real_GetTickCount, zzGetTickCount);
    if (!FindUnloadWait(ClientModule())) {
        CloseHandle(CreateThread(nullptr, 0, RetryUnloadWait, nullptr, 0, nullptr));
    }
    HMODULE ole32 = LoadLibraryW(L"ole32.dll");
    real_CoCreateInstance = ole32 ? (decltype(CoCreateInstance)*)GetProcAddress(ole32, "CoCreateInstance") : nullptr;
    if (real_CoCreateInstance) {
        AttachHook(&(PVOID&)real_CoCreateInstance, zzCoCreateInstance);
    } else {
        ModsLog("fast start: CoCreateInstance not found");
    }
    HMODULE kernelBase = GetModuleHandleW(L"kernelbase.dll");
    real_RegSetValueExA = kernelBase ? (decltype(RegSetValueExA)*)GetProcAddress(kernelBase, "RegSetValueExA") : nullptr;
    if (real_RegSetValueExA) {
        AttachHook(&(PVOID&)real_RegSetValueExA, zzRegSetValueExA);
    }
    s_textComparesFound = FindTextCompares(ClientModule());
    OnModuleLoad(L"CrySystem.dll", FastStartOnCrySystem, true);
    HMODULE d3dx = GetModuleHandleW(L"d3dx9_38.dll");
    if (!d3dx) {
        OnModuleLoad(L"d3dx9_38.dll", HookFillVolumeTexture, true);
    } else if (FindFillVolumeTexture(d3dx)) {
        AttachHook(&(PVOID&)real_FillVolumeTexture, zzFillVolumeTexture);
        ModsLog("fast start: D3DXFillVolumeTexture hooked");
    }
    real_ChangeDisplaySettingsA = (decltype(ChangeDisplaySettingsA)*)GetProcAddress(GetModuleHandleW(L"user32.dll"), "ChangeDisplaySettingsA");
    if (real_ChangeDisplaySettingsA) {
        AttachHook(&(PVOID&)real_ChangeDisplaySettingsA, zzChangeDisplaySettingsA);
    }
    HMODULE runtime = GetModuleHandleW(L"MSVCR120.dll");
    real_fopen_s = runtime ? (FopenS_t)GetProcAddress(runtime, "fopen_s") : nullptr;
    if (real_fopen_s) {
        AttachHook(&(PVOID&)real_fopen_s, zz_fopen_s);
    }
}

namespace {

/// The timers of the logo before the login screen and of the picture shown on quitting, three each, all set to 0.
struct LogoTimers {
    BYTE* startLogo = nullptr;
    BYTE* ending = nullptr;
    std::vector<CodePatch> patches;
};

void PlanZero(LogoTimers& timers, BYTE* at) {
    static const DWORD ZERO = 0;
    timers.patches.push_back(PlanPatch(at, &ZERO, sizeof(ZERO)));
}

/// The logo before the login screen fades in until 750 ms, stays until 2250 ms and fades out until 3000 ms; only then the login
/// screen starts to load: cmp reg, 2EEh; ja ...; cmp reg, 8CAh; ja ...; mov eax, 0BB8h; cmp reg, eax; ja login
/// All three become 0, so the login screen follows on the next frame.
void FindStartLogo(HMODULE game, LogoTimers& timers) {
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
        timers.startLogo = fadeIn;
        PlanZero(timers, fadeIn + 2);
        PlanZero(timers, hold);
        PlanZero(timers, fadeOut);
        return;
    }
}

/// The picture shown on quitting fades in until 1000 ms, stays until 4000 ms and fades out until 5000 ms; only then the client closes:
/// cmp eax, 3E8h; jbe ...; cmp eax, 0FA0h; ja ...; (the full colour); mov ecx, 1388h; cmp eax, ecx; ja close
/// All three become 0 as well.
void FindEndingPicture(HMODULE game, LogoTimers& timers) {
    for (BYTE* ending : FindAllPatterns(game, "3D E8 03 00 00 76 ?? 3D A0 0F 00 00 77")) {
        for (BYTE* p = ending + 14; p < ending + 0x40; p++) {
            if (p[0] == 0xB9 && *(DWORD*)(p + 1) == 5000 && p[5] == 0x3B && p[6] == 0xC1 && p[7] == 0x77) {
                timers.ending = ending;
                PlanZero(timers, ending + 1);
                PlanZero(timers, ending + 8);
                PlanZero(timers, p + 1);
                return;
            }
        }
    }
}

LogoTimers LocateLogoTimers(HMODULE game) {
    LogoTimers timers;
    FindStartLogo(game, timers);
    FindEndingPicture(game, timers);
    return timers;
}

void ApplyLogoTimers(const LogoTimers& timers) {
    // the start logo's changes come first when it was found
    size_t i = 0;
    if (timers.startLogo) {
        for (; i < 3; i++) {
            ApplyPatch(timers.patches[i]);
        }
        ModsLog("fast start: logo timer at %p", timers.startLogo);
    } else {
        ModsLog("fast start: logo timer not found");
    }
    if (timers.ending) {
        for (; i < timers.patches.size(); i++) {
            ApplyPatch(timers.patches[i]);
        }
        ModsLog("fast start: ending timer at %p", timers.ending);
    } else {
        ModsLog("fast start: ending timer not found");
    }
}

} // namespace

void InstallSkipLogo(HMODULE game) {
    LocateThenApply(game, LocateLogoTimers, ApplyLogoTimers);
}
