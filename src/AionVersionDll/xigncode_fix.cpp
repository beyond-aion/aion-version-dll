#include <intrin.h>
#include <string.h>
#include <windows.h>
#include "detours.h"

static decltype(LoadLibraryW)* real_LoadLibraryW = LoadLibraryW;
/// Skips loading the xigncode module (.xem) by returning straight into the caller's success branch, which sets eax to 1.
static HMODULE WINAPI zzLoadLibraryW(_In_ LPCWSTR lpLibFileName) {
    if (lpLibFileName && wcsstr(lpLibFileName, L".xem")) {
#if defined(_M_AMD64)
        // mov eax, 1
        static const BYTE success[] = { 0xB8, 1, 0, 0, 0 };
#else
        // xor eax, eax; add esp, 0Ch; inc eax
        static const BYTE success[] = { 0x33, 0xC0, 0x83, 0xC4, 0x0C, 0x40 };
#endif
        BYTE* caller = (BYTE*)_ReturnAddress();
        for (BYTE* p = caller; p < caller + 1000; p++) {
            if (memcmp(p, success, sizeof(success)) == 0) {
                *(void**)_AddressOfReturnAddress() = p;
                return nullptr;
            }
        }
    }
    return real_LoadLibraryW(lpLibFileName);
}

static decltype(MessageBoxW)* real_MessageBoxW = MessageBoxW;
/// Suppresses the warning that follows: "Your PC has a high likelihood of getting hacked into or infected by viruses..."
static int WINAPI zzMessageBoxW(_In_opt_ HWND hWnd, _In_opt_ LPCWSTR lpText, _In_opt_ LPCWSTR lpCaption, _In_ UINT uType) {
    if (lpText && wcsncmp(lpText, L"Your PC", 7) == 0) {
        return IDOK;
    }
    return real_MessageBoxW(hWnd, lpText, lpCaption, uType);
}

/// Lets 5.x clients start without xigncode when launched with -disable-xigncode
void InstallXigncodeFix() {
    if (strstr(GetCommandLineA(), "-disable-xigncode")) {
        DetourAttach(&(PVOID&)real_LoadLibraryW, zzLoadLibraryW);
        DetourAttach(&(PVOID&)real_MessageBoxW, zzMessageBoxW);
    }
}
