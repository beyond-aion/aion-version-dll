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
#include "../AionVersionDll/install_report.h"
#include "../AionVersionDll/module_load.h"
#include "../AionVersionDll/mods_log.h"

void InstallPatch(HINSTANCE self);

// the modules in the order the client loads them; Game.dll last, whose mods end with the final summary
static const wchar_t* const MODULES[] = { L"CrySystem.dll", L"CryFont.dll", L"XRenderD3D9.dll", L"CryAnimation.dll", L"Cry3DEngine.dll",
    L"Game.dll" };

/// Puts the image back at its base and registers its unwind table, which the searches use to find where functions start.
static HMODULE MapDump(const std::wstring& path, ULONG_PTR base) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        fwprintf(stderr, L"%s: cannot open\n", path.c_str());
        return nullptr;
    }
    LARGE_INTEGER length;
    BYTE* image = nullptr;
    if (GetFileSizeEx(file, &length) && length.QuadPart >= (LONGLONG)sizeof(IMAGE_DOS_HEADER) && length.QuadPart <= MAXDWORD) {
        image = (BYTE*)VirtualAlloc((void*)base, (SIZE_T)length.QuadPart, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    }
    DWORD read = 0;
    bool ok = image && image == (BYTE*)base && ReadFile(file, image, (DWORD)length.QuadPart, &read, nullptr) && read == length.QuadPart;
    CloseHandle(file);
    if (!ok) {
        if (image) {
            VirtualFree(image, 0, MEM_RELEASE);
        }
        fwprintf(stderr, L"%ls: cannot map the complete image at %p\n", path.c_str(), (void*)base);
        return nullptr;
    }

    auto dos = (PIMAGE_DOS_HEADER)image;
    auto nt = dos->e_magic == IMAGE_DOS_SIGNATURE && dos->e_lfanew >= (LONG)sizeof(IMAGE_DOS_HEADER)
        && (ULONGLONG)dos->e_lfanew + sizeof(IMAGE_NT_HEADERS) <= read ? (PIMAGE_NT_HEADERS)(image + dos->e_lfanew) : nullptr;
    if (!nt || nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64
        || nt->FileHeader.SizeOfOptionalHeader != sizeof(IMAGE_OPTIONAL_HEADER) || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC
        || nt->OptionalHeader.SizeOfImage != read
        || (ULONGLONG)dos->e_lfanew + sizeof(*nt) + nt->FileHeader.NumberOfSections * sizeof(IMAGE_SECTION_HEADER) > read) {
        fwprintf(stderr, L"%ls: invalid or truncated x64 module image\n", path.c_str());
        VirtualFree(image, 0, MEM_RELEASE);
        return nullptr;
    }

    const IMAGE_DATA_DIRECTORY& unwind = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    if (nt->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_EXCEPTION || !unwind.VirtualAddress || !unwind.Size
        || unwind.Size % sizeof(RUNTIME_FUNCTION) != 0 || (ULONGLONG)unwind.VirtualAddress + unwind.Size > read
        || !RtlAddFunctionTable((PRUNTIME_FUNCTION)(image + unwind.VirtualAddress), unwind.Size / sizeof(RUNTIME_FUNCTION), (DWORD64)image)) {
        fwprintf(stderr, L"%ls: no usable unwind table\n", path.c_str());
        VirtualFree(image, 0, MEM_RELEASE);
        return nullptr;
    }
    return (HMODULE)image;
}

static HMODULE LoadDump(const std::wstring& folder, const wchar_t* name) {
    WIN32_FIND_DATAW found;
    HANDLE search = FindFirstFileW((folder + L"\\" + name + L"@*.dump").c_str(), &found);
    if (search == INVALID_HANDLE_VALUE) {
        fwprintf(stderr, L"%ls: cannot find %ls dump (error %lu)\n", folder.c_str(), name, GetLastError());
        return nullptr;
    }
    FindClose(search);

    std::wstring path = folder + L"\\" + found.cFileName;
    const wchar_t* address = found.cFileName + wcslen(name) + 1;
    wchar_t* end = nullptr;
    ULONG_PTR base = (ULONG_PTR)wcstoull(address, &end, 16);
    if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !base || _wcsicmp(end, L".dump") != 0) {
        fwprintf(stderr, L"%ls: expected a dump with a hexadecimal base address\n", path.c_str());
        return nullptr;
    }

    HMODULE module = MapDump(path, base);
    if (module) {
        NameImage(name, module, ModuleImageSize(module));
        ModsLog("modcheck: %ls at %p", found.cFileName, module);
    }
    return module;
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) {
        fwprintf(stderr, L"usage: modcheck <folder with the dumps of DumpModules>\n");
        return 2;
    }
    std::wstring folder = argv[1];
    g_dryRun = true;
    // aion.bin first, which the fixes of the process start search
    HMODULE client = LoadDump(folder, L"aion.bin");
    if (client) {
        SetClientModule(client);
    }
    InstallPatch(GetModuleHandleW(nullptr));
    int mapped = client ? 1 : 0;
    for (const wchar_t* name : MODULES) {
        HMODULE module = LoadDump(folder, name);
        if (module) {
            RunModuleWatches(name, module);
            mapped++;
        }
    }
    constexpr int expected = 1 + _countof(MODULES);
    fwprintf(stderr, L"modcheck: %d/%d images checked, summary in version.log\n", mapped, expected);
    return mapped == expected ? 0 : 1;
}
