#include "module_load.h"
#include <intrin.h>
#include <stdlib.h>

Log_t g_moduleLog = nullptr;

typedef BOOL(WINAPI* DllEntry_t)(HINSTANCE instance, DWORD reason, LPVOID reserved);

struct Watch {
    const wchar_t* name;
    void (*install)(HMODULE module);
    bool afterEntryPoint;
    DllEntry_t realEntry;
};

static Watch s_watches[4];
static int s_watchCount = 0;
static PVOID s_ldrCookie = nullptr;

template <int index>
static BOOL WINAPI zzEntry(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    Watch& watch = s_watches[index];
    BOOL result = watch.realEntry(instance, reason, reserved);
    if (reason == DLL_PROCESS_ATTACH && result) {
        watch.install((HMODULE)instance);
    }
    return result;
}

static const DllEntry_t ENTRY_WRAPPERS[] = { zzEntry<0>, zzEntry<1>, zzEntry<2>, zzEntry<3> };
static_assert(_countof(ENTRY_WRAPPERS) == _countof(s_watches), "one entry wrapper per watch");

/// Points the loader's entry for the module at the wrapper of the watch.
static bool WrapEntry(HMODULE module, int index) {
    // PEB->Ldr->InLoadOrderModuleList; each entry starts with its links, followed by DllBase and EntryPoint after one more pair of links
#if defined(_M_AMD64)
    BYTE* peb = (BYTE*)__readgsqword(0x60);
    LIST_ENTRY* head = (LIST_ENTRY*)(*(BYTE**)(peb + 0x18) + 0x10);
    const int dllBase = 0x30, entryPointOffset = 0x38;
#else
    BYTE* peb = (BYTE*)__readfsdword(0x30);
    LIST_ENTRY* head = (LIST_ENTRY*)(*(BYTE**)(peb + 0x0C) + 0x0C);
    const int dllBase = 0x18, entryPointOffset = 0x1C;
#endif
    for (LIST_ENTRY* link = head->Flink; link != head; link = link->Flink) {
        BYTE* entry = (BYTE*)link;
        if (*(HMODULE*)(entry + dllBase) != module) {
            continue;
        }
        PVOID* entryPoint = (PVOID*)(entry + entryPointOffset);
        if (!*entryPoint) {
            return false;
        }
        s_watches[index].realEntry = (DllEntry_t)*entryPoint;
        *entryPoint = (PVOID)ENTRY_WRAPPERS[index];
        return true;
    }
    return false;
}

typedef struct _UNICODE_STRING {
    USHORT Length;
    USHORT MaximumLength;
    PWSTR Buffer;
} UNICODE_STRING, *PUNICODE_STRING;

typedef struct _LDR_DLL_LOADED_NOTIFICATION_DATA {
    ULONG Flags;
    PUNICODE_STRING FullDllName;
    PUNICODE_STRING BaseDllName;
    PVOID DllBase;
    ULONG SizeOfImage;
} LDR_DLL_LOADED_NOTIFICATION_DATA, *PLDR_DLL_LOADED_NOTIFICATION_DATA;

typedef struct _LDR_DLL_NOTIFICATION_DATA {
    union {
        LDR_DLL_LOADED_NOTIFICATION_DATA Loaded;
        LDR_DLL_LOADED_NOTIFICATION_DATA Unloaded;
    } U;
} LDR_DLL_NOTIFICATION_DATA, *PLDR_DLL_NOTIFICATION_DATA;

typedef VOID(NTAPI* PLDR_DLL_NOTIFICATION_FUNCTION)(ULONG NotificationReason, PLDR_DLL_NOTIFICATION_DATA NotificationData, PVOID Context);
typedef LONG (NTAPI* LdrRegisterDllNotification_t)(ULONG Flags, PLDR_DLL_NOTIFICATION_FUNCTION NotificationFunction, PVOID Context, PVOID* Cookie);

static VOID NTAPI LdrDllNotification(ULONG NotificationReason, PLDR_DLL_NOTIFICATION_DATA NotificationData, PVOID Context) {
    // Reason 1 == Loaded, 2 == Unloaded (per SDK examples)
    if (NotificationReason != 1 || !NotificationData) {
        return;
    }
    PLDR_DLL_LOADED_NOTIFICATION_DATA d = &NotificationData->U.Loaded;
    if (!d->BaseDllName || !d->BaseDllName->Buffer) {
        return;
    }
    HMODULE module = (HMODULE)d->DllBase;
    for (int i = 0; i < s_watchCount; i++) {
        Watch& watch = s_watches[i];
        if (_wcsicmp(d->BaseDllName->Buffer, watch.name) != 0) {
            continue;
        }
        if (!watch.afterEntryPoint) {
            watch.install(module);
        } else if (!WrapEntry(module, i)) {
            MODULE_LOG("%ls entry point not found, patching it before it runs", watch.name);
            watch.install(module);
        }
    }
}

void OnModuleLoad(const wchar_t* name, void (*install)(HMODULE module), bool afterEntryPoint) {
    if (s_watchCount == _countof(s_watches)) {
        MODULE_LOG("module watch: no room for %ls", name);
        return;
    }
    s_watches[s_watchCount++] = { name, install, afterEntryPoint, nullptr };
    if (s_ldrCookie) {
        return;
    }
    LdrRegisterDllNotification_t reg = (LdrRegisterDllNotification_t)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "LdrRegisterDllNotification");
    if (!reg) {
        MODULE_LOG("module watch: LdrRegisterDllNotification not found");
        return;
    }
    LONG st = reg(0, LdrDllNotification, nullptr, &s_ldrCookie);
    if (st < 0) {
        MODULE_LOG("module watch: LdrRegisterDllNotification failed: 0x%08X", (unsigned)st);
    }
}

size_t ModuleImageSize(HMODULE module) {
    auto dos = (PIMAGE_DOS_HEADER)module;
    return ((PIMAGE_NT_HEADERS)((BYTE*)module + dos->e_lfanew))->OptionalHeader.SizeOfImage;
}
