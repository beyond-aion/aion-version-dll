#include "exports.h"
#include "module_load.h"
#include "mods_log.h"
#include "install_report.h"
#include <stdio.h>
#include <detours/detours.h>
#include "mods.h"

static bool s_gfxEnabled = false;

namespace {

/// The highest resolutions CrySystem lets the high graphics options run at, 1920x1200 pixels and, close to it, 2560x1600, raised to
/// 4096x4096. Searched in the first section of its code, which also holds them.
struct GraphicsLimits {
    CodePatch first;
    CodePatch second;
};

GraphicsLimits LocateGraphicsLimits(HMODULE crySystem) {
    GraphicsLimits limits;
    MEMORY_BASIC_INFORMATION mbi = {};
    if (!crySystem || !VirtualQuery((BYTE*)crySystem + 0x1000, &mbi, sizeof(mbi)) || !(mbi.AllocationProtect & 0xF0)) {
        return limits;
    }
    static const DWORD RAISED = 4096 * 4096;
    BYTE* begin = (BYTE*)mbi.BaseAddress;
    BYTE* end = begin + mbi.RegionSize - sizeof(DWORD);
    for (BYTE* c = begin; c < end; c++) {
        if (*(DWORD*)c != 1920 * 1200) {
            continue;
        }
        limits.first = PlanPatch(c, &RAISED, sizeof(RAISED));
        for (BYTE* e = c - 0x100; e < min(end, c + 0x100); e++) {
            if (*(DWORD*)e == 2560 * 1600) {
                limits.second = PlanPatch(e, &RAISED, sizeof(RAISED));
                break;
            }
        }
        break;
    }
    return limits;
}

void ApplyGraphicsLimits(const GraphicsLimits& limits) {
    ApplyPatch(limits.first);
    ApplyPatch(limits.second);
}

void EnableHighQualityGraphicsOptions(HMODULE crySystem) {
    LocateThenApply(crySystem, LocateGraphicsLimits, ApplyGraphicsLimits);
}

} // namespace

void InstallOrUpdateCameraFix(HWND hWnd);
void InstallIpFix();
void InstallXigncodeFix();
void InstallShaderFix();
void Install64BitStackFix();
void InstallFullscreenFix();
void InstallChatLogFix();
void InstallCrashLog();
void InstallReserveFix();
void InstallColorTagFix(HMODULE game);
void InstallServerCheckFix(HMODULE game);
void InstallPetDecorationFix(HMODULE game);
void InstallLightRectFixOnLoad();
void InstallScreenshotFix();

/// The fixes of Game.dll in the order they install, once its entry point has run.
static const InstallEntry GAME_FIXES[] = {
    { "color_tag_fix", nullptr, InstallColorTagFix },
    { "server_check_fix", nullptr, InstallServerCheckFix },
    { "pet_decoration_fix", nullptr, InstallPetDecorationFix },
};

static void InstallGameFixes(HMODULE game) {
    InstallScope scope("Game.dll fixes", true);
    LogClientBuild(game, "Game.dll");
    InstallAll(GAME_FIXES, game);
}

static decltype(SetWindowLongA)* real_SetWindowLongA = SetWindowLongA;
static LONG WINAPI zzSetWindowLongA(_In_ HWND hWnd, _In_ int nIndex, _In_ LONG dwNewLong) {
    if (!s_gfxEnabled) {
        InstallScope scope("graphics_options");
        EnableHighQualityGraphicsOptions(GetModuleHandle(L"crysystem"));
        s_gfxEnabled = true;
    }
    {
        InstallScope scope("camera_fix");
        InstallOrUpdateCameraFix(hWnd);
    }
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

/// The fixes that install at the process start, most of them hooks of Windows functions, some waiting for a DLL of the client.
static const InstallEntry PROCESS_FIXES[] = {
    { "ip_fix", nullptr, [](HMODULE) { InstallIpFix(); } },
    { "xigncode_fix", nullptr, [](HMODULE) { InstallXigncodeFix(); } },
    { "camera_fix", nullptr, [](HMODULE) { InstallGraphicsOptionsFixAndCameraFix(); } },
    { "shader_fix", nullptr, [](HMODULE) { InstallShaderFix(); } },
    { "fullscreen_fix", nullptr, [](HMODULE) { InstallFullscreenFix(); } },
    { "chat_log_fix", nullptr, [](HMODULE) { InstallChatLogFix(); } },
    { "stack_fix", nullptr, [](HMODULE) { Install64BitStackFix(); } },
    { "crash_log", nullptr, [](HMODULE) { InstallCrashLog(); } },
    { "reserve_fix", nullptr, [](HMODULE) { InstallReserveFix(); } },
    { "light_rect_fix", nullptr, [](HMODULE) { InstallLightRectFixOnLoad(); } },
    { "screenshot_fix", nullptr, [](HMODULE) { InstallScreenshotFix(); } },
};

/// version.log opens first, so that every fix and mod below can log. Of the handlers for the same DLL and phase (module_load.h), the
/// ones of the fixes run before those of the mods.
void InstallPatch(HINSTANCE self) {
    OpenModsLog(self);
    LogClientBuild(ClientModule(), "aion.bin");
    {
        InstallScope scope("process start", true);
        PreloadDXVK();

        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        InstallModuleDumps();
        void InstallProfiler();
        InstallProfiler();
        InstallAll(PROCESS_FIXES, nullptr);
        if (g_dryRun) {
            // the client changes the limits once it creates its window, which modcheck has not; the search is checked on the image
            OnModuleLoad(L"CrySystem.dll", [](HMODULE crySystem) {
                InstallScope scope("graphics_options");
                EnableHighQualityGraphicsOptions(crySystem);
            }, true);
        }
        OnModuleLoad(L"Game.dll", InstallGameFixes, true);
        InstallMods();
        LONG error = DetourTransactionCommit();
        ModsLog("fixes committed: %ld", error);

        InstallScope affinity("affinity_fix");
        FixClientStartupWithHighCoreCountCpus();
    }
    LogInstallSummary("at the start", false);
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
