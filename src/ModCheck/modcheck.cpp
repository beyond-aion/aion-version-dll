// modcheck: what the fixes and mods of version.dll find in a client, without starting it. usage: modcheck <dumps folder>
//
// The folder holds the images that [Debug] DumpModules wrote from a start of the client (<name>@<base>.dump). modcheck puts each one
// back at its base, then runs the installation of version.dll as a dry run: the fixes and mods search as in the client, their changes
// and hooks are counted but left out, and version.log next to modcheck.exe gets the same summary as a start of the client. The fixes and
// mods of the process start find Windows functions in this process instead of the client's, and those waiting for something the client
// does later (its window, its device) do not run.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string>
#include <vector>
#include "../AionVersionDll/install_report.h"
#include "../AionVersionDll/module_load.h"
#include "../AionVersionDll/mods_log.h"

void InstallPatch(HINSTANCE self);

// the modules in the order the client loads them; Game.dll last, whose mods end with the final summary
static const wchar_t* const MODULES[] = { L"CrySystem.dll", L"CryFont.dll", L"XRenderD3D9.dll", L"Game.dll" };

/// Puts the image back at its base and registers its unwind table, which the searches use to find where functions start.
static HMODULE MapDump(const std::wstring& path, ULONG_PTR base) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        fwprintf(stderr, L"%s: cannot open\n", path.c_str());
        return nullptr;
    }
    DWORD size = GetFileSize(file, nullptr);
    BYTE* image = (BYTE*)VirtualAlloc((void*)base, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    DWORD read = 0;
    bool ok = image == (BYTE*)base && ReadFile(file, image, size, &read, nullptr) && read == size;
    CloseHandle(file);
    if (!ok) {
        fwprintf(stderr, L"%s: cannot put it at %p (got %p)\n", path.c_str(), (void*)base, image);
        return nullptr;
    }
    auto nt = (PIMAGE_NT_HEADERS)(image + ((PIMAGE_DOS_HEADER)image)->e_lfanew);
    const IMAGE_DATA_DIRECTORY& unwind = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    if (!unwind.VirtualAddress || !RtlAddFunctionTable((PRUNTIME_FUNCTION)(image + unwind.VirtualAddress),
            unwind.Size / sizeof(RUNTIME_FUNCTION), (DWORD64)image)) {
        fwprintf(stderr, L"%s: no unwind table\n", path.c_str());
    }
    return (HMODULE)image;
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) {
        fwprintf(stderr, L"usage: modcheck <folder with the dumps of DumpModules>\n");
        return 2;
    }
    std::wstring folder = argv[1];
    g_dryRun = true;
    InstallPatch(GetModuleHandleW(nullptr));
    int mapped = 0;
    for (const wchar_t* name : MODULES) {
        WIN32_FIND_DATAW found;
        HANDLE search = FindFirstFileW((folder + L"\\" + name + L"@*.dump").c_str(), &found);
        if (search == INVALID_HANDLE_VALUE) {
            continue;
        }
        FindClose(search);
        const wchar_t* at = wcschr(found.cFileName, L'@');
        ULONG_PTR base = (ULONG_PTR)wcstoull(at + 1, nullptr, 16);
        HMODULE module = MapDump(folder + L"\\" + found.cFileName, base);
        if (module) {
            NameImage(name, module, ModuleImageSize(module));
            ModsLog("modcheck: %ls at %p", found.cFileName, module);
            RunModuleWatches(name, module);
            mapped++;
        }
    }
    fwprintf(stderr, L"modcheck: %d images checked, summary in version.log\n", mapped);
    return mapped ? 0 : 1;
}
