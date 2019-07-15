#define _HAS_EXCEPTIONS 0

#include "exports.h"
#include <list>
#include <stdio.h>
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include "winsock2.h"
#include "detours.h"
using namespace std;

static const char s_officialIp[16] = "70.5.0.18";
static list<char> s_gameServerIps = {};
static bool s_gfxEnabled = false;

template <typename T>
bool contains(const list<T>& list, const T& element) {
    return !list.empty() && find(list.begin(), list.end(), element) != list.end();
}

static decltype(inet_ntoa)* real_inet_ntoa = inet_ntoa;
// This hook gathers all game server IPs from the server list (some other calls happen before the server list and also after login when connecting to the chat server, but that's fine)
char* WINAPI zzinet_ntoa(_In_ struct in_addr in) {
    char* addr = real_inet_ntoa(in);
    if (strcmp(addr, "211.233.91.10") != 0) { // ignore this IP (always queried on client start)
        s_gameServerIps.push_back(*addr);
    }
    return addr;
}

static decltype(lstrcpynA)* real_lstrcpynA = lstrcpynA;
// Instead of here we could also return the fake IP directly in inet_ntoa but then we have to restore it in the winsock2#connect() call.
LPSTR WINAPI zzlstrcpynA(_Out_writes_(iMaxLength) LPSTR lpString1, _In_ LPCSTR lpString2, _In_ int iMaxLength) {
    if (contains(s_gameServerIps, *lpString2)) {
        s_gameServerIps.clear();
        return real_lstrcpynA(lpString1, s_officialIp, iMaxLength);
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

bool IsWindows10FallCreatorsUpdateOrLater() {
    DWORD dwDummy;
    DWORD dwFVISize = zzGetFileVersionInfoSizeA("kernel32.dll", &dwDummy);
    LPBYTE lpVersionInfo = new BYTE[dwFVISize];
    if (zzGetFileVersionInfoA("kernel32.dll", 0, dwFVISize, lpVersionInfo)) {
        UINT puLen;
        LPSTR lplpBuffer;
        if (zzVerQueryValueA(lpVersionInfo, "\\", (LPVOID*)& lplpBuffer, &puLen) && puLen == sizeof(VS_FIXEDFILEINFO)) {
            VS_FIXEDFILEINFO* vinfo = (VS_FIXEDFILEINFO*)lplpBuffer;
            int majorVersion = (int)HIWORD(vinfo->dwProductVersionMS);
            int minorVersion = (int)LOWORD(vinfo->dwProductVersionMS);
            int buildNumber = (int)HIWORD(vinfo->dwProductVersionLS);
            return majorVersion == 10 && minorVersion == 0 && buildNumber >= 16299; // 16299 is update version 1709 (Fall Creators Update) which broke the WM_MOUSEMOVE event when dragging the mouse
        }
    }
    return false;
}

void InstallPatch() {
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());

    // enable client login to non-official game servers by spoofing game server IP as an official one
    DetourAttach(&(PVOID&)real_inet_ntoa, zzinet_ntoa);
    DetourAttach(&(PVOID&)real_lstrcpynA, zzlstrcpynA);

    // activate camera movement fix for windows 10
    if (IsWindows10FallCreatorsUpdateOrLater()) {
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