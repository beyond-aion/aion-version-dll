#include "mods.h"
#include "detours.h"

// The client writes its own crash report next to aion.bin, which fails where the game folder is not writable (Program Files). So the crash
// goes to mods.log first: the faulting address and the frames as module+offset, the registers and the code addresses on the stack.
// The client's own filter runs after that as before.

typedef decltype(SetUnhandledExceptionFilter)* SetUnhandledExceptionFilter_t;
static SetUnhandledExceptionFilter_t real_SetUnhandledExceptionFilter = nullptr;
static LPTOP_LEVEL_EXCEPTION_FILTER s_clientFilter;
static volatile LONG s_crashed;

/// "CrySystem.dll+779F7" for an address in a loaded module, else the bare address.
static void Describe(DWORD64 address, char* text, size_t size) {
    HMODULE module = nullptr;
    wchar_t path[MAX_PATH];
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)address, &module)
        && GetModuleFileNameW(module, path, MAX_PATH)) {
        const wchar_t* name = wcsrchr(path, L'\\');
        sprintf_s(text, size, "%ls+%llX", name ? name + 1 : path, address - (DWORD64)module);
    } else {
        sprintf_s(text, size, "%016llX", address);
    }
}

static bool IsCode(DWORD64 address) {
    MEMORY_BASIC_INFORMATION region;
    return address > 0x10000 && VirtualQuery((void*)address, &region, sizeof(region)) && region.Type == MEM_IMAGE && region.State == MEM_COMMIT
        && (region.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY));
}

static void LogFrames(CONTEXT context) {
    char where[MAX_PATH + 32];
    for (int frame = 0; frame < 32 && context.Rip; frame++) {
        Describe(context.Rip, where, sizeof(where));
        ModsLog("crash: frame %d %s", frame, where);
        DWORD64 imageBase;
        PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &imageBase, nullptr);
        if (function) {
            void* handlerData;
            DWORD64 establisherFrame;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, context.Rip, function, &context, &handlerData, &establisherFrame, nullptr);
        } else {
            // a leaf function, or code without unwind data such as the packer's
            MEMORY_BASIC_INFORMATION region;
            if (!VirtualQuery((void*)context.Rsp, &region, sizeof(region)) || region.State != MEM_COMMIT) {
                break;
            }
            context.Rip = *(DWORD64*)context.Rsp;
            context.Rsp += 8;
        }
    }
}

/// Code addresses on the stack, where unwinding gets lost in code without unwind data.
static void LogStackCode(DWORD64 rsp) {
    char where[MAX_PATH + 32];
    int found = 0;
    for (int slot = 0; slot < 1024 && found < 32; slot++) {
        DWORD64* at = (DWORD64*)rsp + slot;
        MEMORY_BASIC_INFORMATION region;
        if (((DWORD64)at & 0xFFF) == 0 || slot == 0) {
            if (!VirtualQuery(at, &region, sizeof(region)) || region.State != MEM_COMMIT || (region.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
                break;
            }
        }
        if (IsCode(*at)) {
            Describe(*at, where, sizeof(where));
            ModsLog("crash: stack +%03X %s", slot * 8, where);
            found++;
        }
    }
}

static LONG WINAPI CrashFilter(EXCEPTION_POINTERS* pointers) {
    if (!InterlockedExchange(&s_crashed, 1)) {
        EXCEPTION_RECORD* record = pointers->ExceptionRecord;
        CONTEXT* c = pointers->ContextRecord;
        char where[MAX_PATH + 32];
        Describe((DWORD64)record->ExceptionAddress, where, sizeof(where));
        if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2) {
            static const char* const KINDS[] = { "read of", "write to", "", "", "", "", "", "", "execution of" };
            ULONG_PTR kind = record->ExceptionInformation[0];
            ModsLog("crash: %08lX (%s %016llX) at %s, thread %lu", record->ExceptionCode, kind < 9 ? KINDS[kind] : "access to",
                (DWORD64)record->ExceptionInformation[1], where, GetCurrentThreadId());
        } else {
            ModsLog("crash: %08lX at %s, thread %lu", record->ExceptionCode, where, GetCurrentThreadId());
        }
        ModsLog("crash: rax=%016llX rbx=%016llX rcx=%016llX rdx=%016llX rsi=%016llX rdi=%016llX", c->Rax, c->Rbx, c->Rcx, c->Rdx, c->Rsi, c->Rdi);
        ModsLog("crash: rbp=%016llX rsp=%016llX r8=%016llX r9=%016llX r10=%016llX r11=%016llX", c->Rbp, c->Rsp, c->R8, c->R9, c->R10, c->R11);
        ModsLog("crash: r12=%016llX r13=%016llX r14=%016llX r15=%016llX", c->R12, c->R13, c->R14, c->R15);
        LogFrames(*c);
        LogStackCode(c->Rsp);
    }
    return s_clientFilter ? s_clientFilter(pointers) : EXCEPTION_CONTINUE_SEARCH;
}

/// Keeps the crash filter on top: a filter the client sets runs after it.
static LPTOP_LEVEL_EXCEPTION_FILTER WINAPI zzSetUnhandledExceptionFilter(_In_opt_ LPTOP_LEVEL_EXCEPTION_FILTER filter) {
    LPTOP_LEVEL_EXCEPTION_FILTER previous = s_clientFilter;
    s_clientFilter = filter;
    return previous;
}

/// Called inside the Detours transaction of the patch.
void InstallCrashLog() {
    real_SetUnhandledExceptionFilter = (SetUnhandledExceptionFilter_t)GetProcAddress(GetModuleHandleW(L"kernelbase.dll"), "SetUnhandledExceptionFilter");
    if (!real_SetUnhandledExceptionFilter) {
        ModsLog("crash log: SetUnhandledExceptionFilter not found");
        return;
    }
    s_clientFilter = real_SetUnhandledExceptionFilter(CrashFilter);
    DetourAttach(&(PVOID&)real_SetUnhandledExceptionFilter, zzSetUnhandledExceptionFilter);
}
