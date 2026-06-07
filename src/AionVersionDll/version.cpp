#define _HAS_EXCEPTIONS 0

#include "exports.h"
#include <atomic>
#include <list>
#include <stdio.h>
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <winsock2.h>
#include <winternl.h>
#include "detours.h"
using namespace std;

static const char s_officialIp[16] = "70.5.0.18";
static list<char> s_gameServerIps = {};
static bool s_gfxEnabled = false;
HWND clientHwnd = nullptr;
std::atomic<bool> windowedMode(false); //Atomic because mouse hook runs in a different thread.

//=== FIX 3: mouseHookInstalled is now atomic. 
//Accessed from main thread (zzSetCursorPos) and the camera-fix thread (CameraFixThread on exit). 
//Previously a plain bool, which was a formal data race on the C++ standard (UB).
std::atomic<bool> mouseHookInstalled(false);

//=== FIX 6: keep one persistent camera-fix thread instead of recreating it on every windowed/fullscreen transition.
//Also store the HANDLE so it can be closed properly on DLL unload (previously leaked).
static HANDLE g_cameraThreadHandle = nullptr;
static DWORD g_cameraThreadId = 0;

template <typename T>
bool contains(const list<T>& list, const T& element) {
    return !list.empty() && find(list.begin(), list.end(), element) != list.end();
}

static decltype(inet_ntoa)* real_inet_ntoa = inet_ntoa;
//This hook gathers all game server IPs from the server list (some other calls happen before the server list and also after login when connecting to the chat server, but that's fine).
char* WINAPI zzinet_ntoa(_In_ struct in_addr in) {
    char* addr = real_inet_ntoa(in);
    if (strcmp(addr, "211.233.91.10") != 0) { //Ignore this IP (always queried on client start).
        s_gameServerIps.push_back(*addr);
    }
    return addr;
}

static decltype(lstrcpynA)* real_lstrcpynA = lstrcpynA;
//Instead of here we could also return the fake IP directly in inet_ntoa but then we have to restore it in the winsock2#connect() call.
LPSTR WINAPI zzlstrcpynA(_Out_writes_(iMaxLength) LPSTR lpString1, _In_ LPCSTR lpString2, _In_ int iMaxLength) {
    if (contains(s_gameServerIps, *lpString2)) {
        s_gameServerIps.clear();
        return real_lstrcpynA(lpString1, s_officialIp, iMaxLength);
    }
    return real_lstrcpynA(lpString1, lpString2, iMaxLength);
}

//=== FIX 4: check GetCursorInfo return value. 
//On failure, the previous code returned true (because cursorInfo.flags is zero-initialized), causing false positives.
//Returning false on error is safer: we'd rather miss one synthesized WM_MOUSEMOVE than send spurious ones (which cause drift).
bool IsCursorHidden() {
    CURSORINFO cursorInfo = { sizeof(cursorInfo) };
    if (!GetCursorInfo(&cursorInfo)) {
        return false;
    }
    return cursorInfo.flags == 0;
}

//=== FIX 5: use GetAsyncKeyState instead of GetKeyState.
//GetKeyState returns the thread-local virtual key state, which depends on the calling thread's message queue.
//When called from CameraFixThread (which has its own queue and almost never pumps key events), it returns stale or zero state - especially for modifiers (Shift, Ctrl).
//GetAsyncKeyState reads the real-time global state, which is what we want.
WPARAM MakeMouseMoveWParam() {
    return ((GetAsyncKeyState(VK_LBUTTON)  & 0x8000) ? 0x0001 : 0) |
           ((GetAsyncKeyState(VK_RBUTTON)  & 0x8000) ? 0x0002 : 0) |
           ((GetAsyncKeyState(VK_SHIFT)    & 0x8000) ? 0x0004 : 0) |
           ((GetAsyncKeyState(VK_CONTROL)  & 0x8000) ? 0x0008 : 0) |
           ((GetAsyncKeyState(VK_MBUTTON)  & 0x8000) ? 0x0010 : 0) |
           ((GetAsyncKeyState(VK_XBUTTON1) & 0x8000) ? 0x0020 : 0) |
           ((GetAsyncKeyState(VK_XBUTTON2) & 0x8000) ? 0x0040 : 0);
}

//=== FIX 2 + FIX 6:
//=== FIX 2: replaced SendMessage with SendMessageTimeout (synchronous, but bounded).
//The original SendMessage was synchronous - which is what the client needs - but had no timeout protection and could be silently uninstalled by Windows if the game window stalled (300 ms LowLevelHooksTimeout).
//SendMessageTimeout keeps the synchronous behavior the client relies on (so consecutive SetCursorPos and WM_MOUSEMOVE remain strictly ordered for correct camera delta calculation) while protecting against
//hook removal: 50 ms is well below the LL hook limit, and SMTO_ABORTIFHUNG returns immediately if Windows considers the window hung.
//Note: PostMessage (asynchronous) was tried first - it broke ordering, multiple WM_MOUSEMOVE events accumulated in the queue and produced wrong camera deltas in windowed mode.
//=== FIX 6: removed the PostThreadMessage(WM_QUIT) hack that killed the hook thread on every transition to fullscreen.
//The thread now lives for the entire DLL lifetime and the hook simply no-ops while windowedMode is false.
LRESULT CALLBACK MouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode >= 0 && windowedMode) {
        if (wParam == WM_MOUSEMOVE && IsCursorHidden()) {
            POINT pt = ((MSLLHOOKSTRUCT*)lParam)->pt;
            ScreenToClient(clientHwnd, &pt);
            DWORD_PTR sendResult;
            SendMessageTimeout(clientHwnd, WM_MOUSEMOVE,
                MakeMouseMoveWParam(), MAKELPARAM(pt.x, pt.y),
                SMTO_NORMAL | SMTO_ABORTIFHUNG, 50, &sendResult);
        }
    }
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

//=== FIX 6: thread now runs for the DLL lifetime.
//The hook is installed once at thread startup and uninstalled only when the message loop exits (on WM_QUIT during DLL unload). 
//We no longer flip mouseHookInstalled back to false at the end, because there's no longer a "reinstall later" path - the thread is permanent.
DWORD WINAPI CameraFixThread(LPVOID lParam) {
    HHOOK hook = SetWindowsHookEx(WH_MOUSE_LL, MouseProc, GetModuleHandle(0), 0);
    if (!hook) {
        mouseHookInstalled = false;
        return 1;
    }

    MSG message;
    while (GetMessage(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessage(&message);
    }

    UnhookWindowsHookEx(hook);
    return 0;
}

static decltype(SetCursorPos)* real_SetCursorPos = SetCursorPos;

//=== FIX 1 + FIX 6:
//=== FIX 1: use the X, Y arguments directly instead of calling GetCursorPos again.
//The previous code did SetCursorPos(X, Y) -> GetCursorPos(&pt), which races with the mouse driver:
//any real mouse movement between the two calls would make GetCursorPos return a different position than the one we just set.
//The client then interprets the discrepancy as a huge camera delta - this is the root cause of the "snap to center" bug.
//=== FIX 6: thread is started exactly once via atomic compare_exchange. 
//After the first call to zzSetCursorPos in windowed mode, the thread runs forever and mouseHookInstalled never flips back to false.
BOOL WINAPI zzSetCursorPos(_In_ int X, _In_ int Y) {
    BOOL result = real_SetCursorPos(X, Y);
    if (clientHwnd && IsCursorHidden()) {
        if (windowedMode) {
            bool expected = false;
            if (mouseHookInstalled.compare_exchange_strong(expected, true)) {
                //Windowed mode needs decoupled processing, otherwise camera movements will stutter while any key on the keyboard is pressed.
                g_cameraThreadHandle = CreateThread(nullptr, 0, CameraFixThread, nullptr, 0, &g_cameraThreadId);
                if (!g_cameraThreadHandle) {
                    mouseHookInstalled = false; // roll back so we can retry next time
                }
            }
        }
        else {
            //Use X/Y from the arguments directly - they are exactly the coordinates we just set via real_SetCursorPos, with no race window.
            //Calling GetCursorPos here was the source of the snap-to-center bug.
            POINT pt = { X, Y };
            ScreenToClient(clientHwnd, &pt);
            PostMessage(clientHwnd, WM_MOUSEMOVE, MakeMouseMoveWParam(), MAKELPARAM(pt.x, pt.y));
        }
    }
    return result;
}

void EnableHighQualityGraphicsOptions() {
    MEMORY_BASIC_INFORMATION mbi = {};
    if (!VirtualQuery(GetModuleHandle(L"crysystem") + 0x1000, &mbi, sizeof(mbi))) {
        return;
    }

    if (!(mbi.AllocationProtect & 0xF0)) {
        return;
    }

    DWORD oldProtect;
    if (!VirtualProtect(mbi.BaseAddress, mbi.RegionSize, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        return;
    }

    char* c = (char*)mbi.BaseAddress;
    char* end = c + mbi.RegionSize - sizeof(DWORD);

    for (; c < end; c++) {
        DWORD* d = (DWORD*)c;
        if (*d == 1920 * 1200) {
            *d = 4096 * 4096;
            char* e = c - 0x100;
            char* e_end = c + 0x100;
            e_end = min(end, e_end);
            for (; e < e_end; e++) {
                DWORD* d2 = (DWORD*)e;
                if (*d2 == 2560 * 1600) {
                    *d2 = 4096 * 4096;
                    break;
                }
            }
            break;
        }
    }

    VirtualProtect(mbi.BaseAddress, mbi.RegionSize, oldProtect, &oldProtect);
}

static decltype(SetWindowLongA)* real_SetWindowLongA = SetWindowLongA;
LONG WINAPI zzSetWindowLongA(_In_ HWND hWnd, _In_ int nIndex, _In_ LONG dwNewLong) {
    if (!s_gfxEnabled) {
        EnableHighQualityGraphicsOptions();
        s_gfxEnabled = true;
    }
    clientHwnd = hWnd;
    windowedMode = dwNewLong & WS_CAPTION;
    return real_SetWindowLongA(hWnd, nIndex, dwNewLong);
}

bool IsWindowsVersionOrLater(int major, int minor, int build) {
    DWORD dwDummy;
    DWORD dwFVISize = zzGetFileVersionInfoSizeA("kernel32.dll", &dwDummy);
    LPBYTE lpVersionInfo = new BYTE[dwFVISize];
    if (zzGetFileVersionInfoA("kernel32.dll", 0, dwFVISize, lpVersionInfo)) {
        UINT puLen;
        LPSTR lplpBuffer;
        if (zzVerQueryValueA(lpVersionInfo, "\\", (LPVOID*)&lplpBuffer, &puLen) && puLen == sizeof(VS_FIXEDFILEINFO)) {
            VS_FIXEDFILEINFO* vinfo = (VS_FIXEDFILEINFO*)lplpBuffer;
            int majorVersion = (int)HIWORD(vinfo->dwProductVersionMS);
            int minorVersion = (int)LOWORD(vinfo->dwProductVersionMS);
            int buildNumber = (int)HIWORD(vinfo->dwProductVersionLS);
            return majorVersion > major || majorVersion == major && (minorVersion > minor || minorVersion == minor && buildNumber >= build);
        }
    }
    return false;
}

bool IsWindows10FallCreatorsUpdateOrLater() {
    return IsWindowsVersionOrLater(10, 0, 16299); //Update version 1709 (Fall Creators Update) broke the WM_MOUSEMOVE event when dragging the mouse.
}

bool IsWindows1124H2UpdateOrLater() {
    return IsWindowsVersionOrLater(10, 0, 26100);
}

void PreloadDXVK() {
    if (GetModuleHandleW(L"d3d9.dll"))
        return;
    LoadLibraryW(L"d3d9.dll");
}




#if defined(_M_AMD64)
/* Code from https://github.com/AbdouRoumi/Custom-GetProcAddress */
FARPROC CustomGetProcProcess(IN HMODULE hModule, IN LPCSTR FuncName) {
    PBYTE pBase = (PBYTE)hModule;

    //Getting the DOS header and checking the signature.
    PIMAGE_DOS_HEADER pImgDosHdr = (PIMAGE_DOS_HEADER)pBase;
    if (pImgDosHdr->e_magic != IMAGE_DOS_SIGNATURE) {
        return NULL;
    }

    //Getting the NT headers and checking the signature.
    PIMAGE_NT_HEADERS pImgNtHdrs = (PIMAGE_NT_HEADERS)(pImgDosHdr->e_lfanew + pBase);
    if (pImgNtHdrs->Signature != IMAGE_NT_SIGNATURE) {
        return NULL;
    }

    //Getting the optional header.
    IMAGE_OPTIONAL_HEADER ImgOptHdr = pImgNtHdrs->OptionalHeader;

    //Getting the export table.
    PIMAGE_EXPORT_DIRECTORY pImgExportDir = (PIMAGE_EXPORT_DIRECTORY)(pBase + ImgOptHdr.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress);

    //Getting the function name array.
    PDWORD FunctionNameArray = (PDWORD)(pBase + pImgExportDir->AddressOfNames);

    // Getting the func address array.
    PDWORD FunctionAddressArray = (PDWORD)(pBase + pImgExportDir->AddressOfFunctions);

    //Getting the func ordinal array.
    PWORD FunctionOrdinalArray = (PWORD)(pBase + pImgExportDir->AddressOfNameOrdinals); //Use PWORD, not PDWORD.

    for (DWORD i = 0; i < pImgExportDir->NumberOfNames; i++) { //Iterate over NumberOfNames.
        //Getting the name of the function.
        CHAR* pFunctionName = (CHAR*)(pBase + FunctionNameArray[i]);

        //Getting the address of the function through its ordinal.
        FARPROC pFunctionAddress = (FARPROC)(pBase + FunctionAddressArray[FunctionOrdinalArray[i]]);

        //Searching for the function.
        if (strcmp(FuncName, pFunctionName) == 0) {
            return pFunctionAddress;
        }
    }

    return NULL;
}


extern "C" decltype(CloseHandle)* real_CloseHandle = CloseHandle;
extern "C"
BOOL
WINAPI
zzCloseHandle(_In_ _Post_ptr_invalid_ HANDLE hObject);


int except_aionbin = 1;
int except_gamedll = 1;
int except_crysystem = 1;
int except_aegisty = 2;


bool moduleinfo_aionbin = false;
bool moduleinfo_gamedll = false;
bool moduleinfo_crysystem = false;
bool moduleinfo_aegisty = false;

HMODULE haionbin = NULL;
HMODULE hgamedll = NULL;
HMODULE hcrysystem = NULL;
HMODULE haegisty = NULL;


extern "C" int CheckInjectExcept(void* addr) {
    HMODULE caller = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCSTR)addr, &caller);

    if (except_aionbin) {
        if (!haionbin) {
            haionbin = GetModuleHandleA(NULL);
        }
        if (haionbin) {
            if (caller == haionbin) {
                except_aionbin--;
                return 1;
            }
        }
    }

    if (except_gamedll) {
        if (!hgamedll) {
            hgamedll = GetModuleHandleA("game.dll");
        }
        if (hgamedll) {
            if (caller == hgamedll) {
                except_gamedll--;
                return 1;
            }
        }
    }

    if (except_crysystem) {
        if (!hcrysystem) {
            hcrysystem = GetModuleHandleA("crysystem.dll");
        }
        if (hcrysystem) {
            if (caller == hcrysystem) {
                except_crysystem--;
                return 1;
            }
        }
    }

    if (except_aegisty) {
        if (!haegisty) {
            haegisty = GetModuleHandleA("aegisty64.bin");
        }
        if (haegisty) {
            if (caller == haegisty) {
                except_aegisty--;
                //Only for second instance?
                if (except_aegisty == 0) {
                    return 1;
                }
            }
        }
    }

    return 0;
}



typedef PVOID(WINAPI* TRtlAddVectoredExceptionHandler)(ULONG, PVECTORED_EXCEPTION_HANDLER);

extern "C" TRtlAddVectoredExceptionHandler real_RtlAddVectoredExceptionHandler = nullptr;

extern "C" PVOID
WINAPI
zzRtlAddVectoredExceptionHandler(ULONG first, PVECTORED_EXCEPTION_HANDLER func);




extern "C" decltype(CreateFileA)* real_CreateFileA = nullptr;

extern "C"
HANDLE
WINAPI
zzCreateFileA(
    _In_ LPCSTR lpFileName,
    _In_ DWORD dwDesiredAccess,
    _In_ DWORD dwShareMode,
    _In_opt_ LPSECURITY_ATTRIBUTES lpSecurityAttributes,
    _In_ DWORD dwCreationDisposition,
    _In_ DWORD dwFlagsAndAttributes,
    _In_opt_ HANDLE hTemplateFile);


extern "C" decltype(LoadLibraryA)* real_LoadLibraryA = LoadLibraryA;

extern "C"
HMODULE
WINAPI
zzLoadLibraryA(
    _In_ LPCSTR lpLibFileName
);
#endif


void InstallPatch() {
    PreloadDXVK();
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());

    //Enable client login to non-official game servers by spoofing game server IP as an official one.
    DetourAttach(&(PVOID&)real_inet_ntoa, zzinet_ntoa);
    DetourAttach(&(PVOID&)real_lstrcpynA, zzlstrcpynA);

    //Activate camera movement fix for windows 10+.
    if (IsWindows10FallCreatorsUpdateOrLater()) {
        DetourAttach(&(PVOID&)real_SetCursorPos, zzSetCursorPos);
    }

    //Update window info and enable disabled graphics settings on high resolutions.
    DetourAttach(&(PVOID&)real_SetWindowLongA, zzSetWindowLongA);

#if defined(_M_AMD64)
    if (IsWindows1124H2UpdateOrLater()) {
        //Fix stack on call CloseHandle.
        DetourAttach(&(PVOID&)real_CloseHandle, zzCloseHandle);

        //Fix for rbp restore in themida.
        HMODULE hntdll = GetModuleHandleA("ntdll.dll");
        real_RtlAddVectoredExceptionHandler = (TRtlAddVectoredExceptionHandler)GetProcAddress(hntdll, "RtlAddVectoredExceptionHandler");

        DetourAttach(&(PVOID&)real_RtlAddVectoredExceptionHandler, zzRtlAddVectoredExceptionHandler);

        //Fix stack on call. Use custom getprocaddress because standard return it from aclayer.
        HMODULE hkernel32 = GetModuleHandleA("kernel32.dll");
        real_CreateFileA = (decltype(CreateFileA)*)CustomGetProcProcess(hkernel32, "CreateFileA");

        DetourAttach(&(PVOID&)real_CreateFileA, zzCreateFileA);

        //Same fix.
        DetourAttach(&(PVOID&)real_LoadLibraryA, zzLoadLibraryA);
    }
#endif

    LONG error = DetourTransactionCommit();
}

//=== FIX 6: clean shutdown of the camera-fix thread on DLL unload.
//Only do this when the DLL is being unloaded via FreeLibrary (lpvReserved == NULL).
//When the process is terminating (lpvReserved != NULL), all other threads are already gone and the OS will clean up handles, so we don't touch anything to avoid loader-lock issues.
BOOL WINAPI DllMain(_In_ HINSTANCE hinstDLL, _In_ DWORD fdwReason, _In_ LPVOID lpvReserved) {
    if (fdwReason == DLL_PROCESS_ATTACH) {
        InstallPatch();
    }
    else if (fdwReason == DLL_PROCESS_DETACH) {
        if (lpvReserved == nullptr) {
            if (g_cameraThreadHandle && g_cameraThreadId) {
                PostThreadMessage(g_cameraThreadId, WM_QUIT, 0, 0);
                CloseHandle(g_cameraThreadHandle);
                g_cameraThreadHandle = nullptr;
                g_cameraThreadId = 0;
            }
        }
    }
    return TRUE;
}