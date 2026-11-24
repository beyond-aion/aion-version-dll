#include "mods.h"
#include "module_load.h"
#include <stdio.h>

// [Debug] DumpModules=1 in mods.ini writes the images of the client's modules that the fixes and mods search to dumps\<name>@<base>.dump
// next to version.dll, for modcheck. Each image is taken once the loader ran the module's entry point, which unpacks a packed one,
// and before any fix or mod changes it, as the watch is added before theirs: a run for dumps has the mods turned off, so that none
// changes a module before its entry point either. The file holds the image as it lies in memory, every byte at its RVA; modcheck puts
// it back at the same base, as the method tables and other data hold absolute addresses.

static const wchar_t* const MODULES[] = { L"Game.dll", L"CrySystem.dll", L"CryFont.dll", L"XRenderD3D9.dll" };

static void Dump(HMODULE module) {
    wchar_t path[MAX_PATH], name[MAX_PATH];
    GetModuleFileNameW(module, name, MAX_PATH);
    const wchar_t* fileName = wcsrchr(name, L'\\') ? wcsrchr(name, L'\\') + 1 : name;
    wcscpy_s(path, g_modsIniPath);
    wchar_t* folder = wcsrchr(path, L'\\');
    if (!folder) {
        return;
    }
    wcscpy_s(folder + 1, MAX_PATH - (folder + 1 - path), L"dumps");
    CreateDirectoryW(path, nullptr);
    size_t length = wcslen(path);
    swprintf_s(path + length, MAX_PATH - length, L"\\%s@%llX.dump", fileName, (unsigned long long)module);
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        ModsLog("dump: %ls could not be created (%lu)", path, GetLastError());
        return;
    }
    // page by page, as a packer can leave pages without access; those are written as zeros
    size_t size = ModuleImageSize(module);
    static BYTE page[0x1000];
    size_t unreadable = 0;
    for (size_t offset = 0; offset < size; offset += sizeof(page)) {
        SIZE_T read = 0;
        if (!ReadProcessMemory(GetCurrentProcess(), (BYTE*)module + offset, page, sizeof(page), &read)) {
            memset(page, 0, sizeof(page));
            unreadable++;
        }
        DWORD written = 0;
        WriteFile(file, page, (DWORD)min(sizeof(page), size - offset), &written, nullptr);
    }
    CloseHandle(file);
    ModsLog("dump: %ls at %p, %zu bytes, %zu pages unreadable", fileName, module, size, unreadable);
}

void InstallModuleDumps() {
    bool enabled = false;
    ReadSetting({ L"Debug", L"DumpModules", Setting::Bool, &enabled, 0 });
    if (!enabled) {
        return;
    }
    for (const wchar_t* name : MODULES) {
        OnModuleLoad(name, Dump, true);
    }
}
