#include "exports.h"
#include "module_load.h"
#include "mods_log.h"
#include "install_report.h"
#include <stdio.h>
#include <detours/detours.h>
#if defined(_M_AMD64)
#include "mods.h"
#endif

static bool s_gfxEnabled = false;

static void EnableHighQualityGraphicsOptions() {
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
            ReportWrite(d, true);
            char* e = c - 0x100;
            char* e_end = c + 0x100;
            e_end = min(end, e_end);
            for (; e < e_end; e++) {
                DWORD* d2 = (DWORD*)e;
                if (*d2 == 2560 * 1600) {
                    *d2 = 4096 * 4096;
                    ReportWrite(d2, true);
                    break;
                }
            }
            break;
        }
    }

    VirtualProtect(mbi.BaseAddress, mbi.RegionSize, oldProtect, &oldProtect);
}

void InstallOrUpdateCameraFix(HWND hWnd);
void InstallIpFix();
void InstallXigncodeFix();
void InstallShaderFix();
void Install64BitStackFix();
void InstallFullscreenFix();
void InstallChatLogFix();
/// Installs one fix in a scope of its own, for the summary in version.log.
template <typename... Args>
static void Install(const char* id, void (*install)(Args...), Args... args) {
    InstallScope scope(id);
    install(args...);
}

#if defined(_M_AMD64)
void InstallCrashLog();
void InstallReserveFix();
void InstallColorTagFix(HMODULE game);
void InstallServerCheckFix(HMODULE game);
void InstallPetDecorationFix(HMODULE game);
void InstallLightRectFixOnLoad();

/// The fixes of Game.dll, once its entry point has run.
static void InstallGameFixes(HMODULE game) {
    InstallScope scope("Game.dll fixes", true);
    LogClientBuild(game, "Game.dll");
    Install("color_tag_fix", InstallColorTagFix, game);
    Install("server_check_fix", InstallServerCheckFix, game);
    Install("pet_decoration_fix", InstallPetDecorationFix, game);
}
#endif

static decltype(SetWindowLongA)* real_SetWindowLongA = SetWindowLongA;
static LONG WINAPI zzSetWindowLongA(_In_ HWND hWnd, _In_ int nIndex, _In_ LONG dwNewLong) {
    if (!s_gfxEnabled) {
        Install("graphics_options", EnableHighQualityGraphicsOptions);
        s_gfxEnabled = true;
    }
    Install("camera_fix", InstallOrUpdateCameraFix, hWnd);
    return real_SetWindowLongA(hWnd, nIndex, dwNewLong);
}


static void PreloadDXVK() {
    LoadLibrary(L"d3d9.dll");
}

static void InstallGraphicsOptionsFixAndCameraFix() {
    // enable disabled graphics settings on high resolutions and install camera fix
    AttachHook(&(PVOID&)real_SetWindowLongA, zzSetWindowLongA);
}

static void FixClientStartupWithHighCoreCountCpus() {
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    if (info.dwNumberOfProcessors >= 32) {
        int maxCpu = min(64, info.dwNumberOfProcessors) - 1;
        DWORD_PTR mask = (1ULL << maxCpu) - 1;
        SetProcessAffinityMask(GetCurrentProcess(), mask);
        ReportStatus(InstallStatus::Applied);
    } else {
        ReportStatus(InstallStatus::NotApplicable);
    }
}

/// version.log opens first, so that every fix and mod below can log. Of the handlers for the same DLL and phase (module_load.h), the
/// ones of the fixes run before those of the mods.
static void InstallPatch(HINSTANCE self) {
    OpenModsLog(self);
    LogClientBuild(GetModuleHandleW(nullptr), "aion.bin");
    {
        InstallScope scope("process start", true);
        PreloadDXVK();

        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        Install("ip_fix", InstallIpFix);
        Install("xigncode_fix", InstallXigncodeFix);
        Install("camera_fix", InstallGraphicsOptionsFixAndCameraFix);
        Install("shader_fix", InstallShaderFix);
        Install("fullscreen_fix", InstallFullscreenFix);
        Install("chat_log_fix", InstallChatLogFix);
#if defined(_M_AMD64)
        Install("stack_fix", Install64BitStackFix);
        Install("crash_log", InstallCrashLog);
        Install("reserve_fix", InstallReserveFix);
        Install("light_rect_fix", InstallLightRectFixOnLoad);
        OnModuleLoad(L"Game.dll", InstallGameFixes, true);
        InstallMods();
#endif
        LONG error = DetourTransactionCommit();
        ModsLog("fixes committed: %ld", error);

        Install("affinity_fix", FixClientStartupWithHighCoreCountCpus);
    }
#if defined(_M_AMD64)
    LogInstallSummary("at the start", false);
#else
    // the 32-bit build has no mods, and so no summary with Game.dll
    LogInstallSummary("at the start", true);
#endif
}

BOOL WINAPI DllMain(_In_ HINSTANCE hinstDLL, _In_ DWORD fdwReason, _In_ LPVOID lpvReserved) {
    if (fdwReason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinstDLL);
        wchar_t exe[MAX_PATH];
        GetModuleFileName(nullptr, exe, MAX_PATH);
        const wchar_t* exeName = wcsrchr(exe, L'\\');
        // other programs in the game folder, like the web browser process, load version.dll as well
        if (_wcsicmp(exeName ? exeName + 1 : exe, L"aion.bin") == 0) {
            InstallPatch(hinstDLL);
        }
    }
    return TRUE;
}
