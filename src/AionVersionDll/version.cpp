#define WIN32_LEAN_AND_MEAN
#define _HAS_EXCEPTIONS 0

#include <windows.h>
#include <stdio.h>
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include "winsock2.h"
#include "detours.h"

static const char s_officialIp[16] = "70.5.0.18";
static char s_loginServerIp[16] = "";
static bool s_needPatch = false;
static bool s_gfxEnabled = false;

static unsigned long (PASCAL FAR* real_inet_addr)(_In_z_ const char FAR* cp) = inet_addr;
unsigned long PASCAL FAR zzinet_addr(_In_z_ const char FAR* cp) {
    s_needPatch = true;
    if (!strcmp(cp, s_officialIp)) {
        return real_inet_addr(s_loginServerIp);
    }
    return real_inet_addr(cp);
}

static decltype(lstrcpynA)* real_lstrcpynA = lstrcpynA;
LPSTR WINAPI zzlstrcpynA(_Out_writes_(iMaxLength) LPSTR lpString1, _In_ LPCSTR lpString2, _In_ int iMaxLength) {
    if (s_needPatch) {
        if (!strcmp(lpString2, s_loginServerIp)) {
            s_needPatch = false;
            return real_lstrcpynA(lpString1, s_officialIp, iMaxLength);
        }
    }
    return real_lstrcpynA(lpString1, lpString2, iMaxLength);
}

static decltype(SetCursorPos)* real_SetCursorPos = SetCursorPos;
BOOL WINAPI zzSetCursorPos(_In_ int X, _In_ int Y) {
    BOOL result = real_SetCursorPos(X, Y);
    HWND hwnd = GetActiveWindow();
    if (hwnd) {

        POINT pt;
        GetCursorPos(&pt);
        ScreenToClient(hwnd, &pt);

        WPARAM wParam =
            (GetKeyState(VK_LBUTTON) & 0x8000) ? 0x0001 : 0 |
            (GetKeyState(VK_RBUTTON) & 0x8000) ? 0x0002 : 0 |
            (GetKeyState(VK_SHIFT) & 0x8000) ? 0x0004 : 0 |
            (GetKeyState(VK_CONTROL) & 0x8000) ? 0x0008 : 0 |
            (GetKeyState(VK_MBUTTON) & 0x8000) ? 0x0010 : 0 |
            (GetKeyState(VK_XBUTTON1) & 0x8000) ? 0x0020 : 0 |
            (GetKeyState(VK_XBUTTON2) & 0x8000) ? 0x0040 : 0;

        PostMessage(hwnd, WM_MOUSEMOVE, wParam, MAKELPARAM(pt.x, pt.y));
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

static decltype(ChangeDisplaySettingsA)* real_ChangeDisplaySettingsA = ChangeDisplaySettingsA;
LONG WINAPI zzChangeDisplaySettingsA(_In_opt_ DEVMODEA* lpDevMode, _In_ DWORD dwFlags) {
    if (!s_gfxEnabled) {
        EnableHighQualityGraphicsOptions();
        s_gfxEnabled = true;
    }
    return real_ChangeDisplaySettingsA(lpDevMode, dwFlags);
}

void InstallPatch() {
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());

    // enable client login to non-official game servers
    if (GetServerIpFromCommandLine(s_loginServerIp, sizeof(s_loginServerIp))) {
        DetourAttach(&(PVOID&)real_inet_addr, zzinet_addr);
        DetourAttach(&(PVOID&)real_lstrcpynA, zzlstrcpynA);
    }

    if (strstr(GetCommandLineA(), "-win10-mouse-fix") > 0) {
        DetourAttach(&(PVOID&)real_SetCursorPos, zzSetCursorPos);
    }

    // enable disabled graphics settings on high resolutions
    DetourAttach(&(PVOID&)real_ChangeDisplaySettingsA, zzChangeDisplaySettingsA);

    LONG error = DetourTransactionCommit();
}

BOOL WINAPI DllMain(_In_ HINSTANCE hinstDLL, _In_ DWORD fdwReason, _In_ LPVOID lpvReserved) {
    if (fdwReason == DLL_PROCESS_ATTACH) {
        InstallPatch();
    }
    return TRUE;
}