#include "mods.h"
#include <detours/detours.h>

// The pool of CrySystem looks its blocks up in a page table that only covers the first 16 GB of the address space, without a bounds check.
// Some video drivers (the Vulkan driver of AMD) reserve hundreds of gigabytes early, and the system puts such a reservation at the lowest free
// address. The pool then lands above it, and the first free of such a block crashes. Large reservations that let the system choose the place
// are moved to the top of the address space instead, which keeps the first 16 GB free.

typedef LONG(NTAPI* NtAllocateVirtualMemory_t)(HANDLE, PVOID*, ULONG_PTR, PSIZE_T, ULONG, ULONG);
typedef LONG(NTAPI* NtAllocateVirtualMemoryEx_t)(HANDLE, PVOID*, PSIZE_T, ULONG, ULONG, PVOID, ULONG);
static NtAllocateVirtualMemory_t real_NtAllocateVirtualMemory;
static NtAllocateVirtualMemoryEx_t real_NtAllocateVirtualMemoryEx;

/// A reservation of 16 GB or more in this process at a place of the system's choice. Zero bits and extended parameters can ask for a range
/// of their own, so those are left alone.
static bool IsMovable(HANDLE process, PVOID* base, PSIZE_T size, ULONG type, bool rangeRequired) {
    return (type & MEM_RESERVE) && !rangeRequired && base && !*base && size && *size >= (16ULL << 30)
        && (process == GetCurrentProcess() || GetProcessId(process) == GetCurrentProcessId());
}

static LONG NTAPI zzNtAllocateVirtualMemory(HANDLE process, PVOID* base, ULONG_PTR zeroBits, PSIZE_T size, ULONG type, ULONG protect) {
    if (!IsMovable(process, base, size, type, zeroBits != 0)) {
        return real_NtAllocateVirtualMemory(process, base, zeroBits, size, type, protect);
    }
    LONG status = real_NtAllocateVirtualMemory(process, base, zeroBits, size, type | MEM_TOP_DOWN, protect);
    ModsLog("reserve fix: %llu MB placed at %p, status %08lX", (DWORD64)(*size >> 20), *base, status);
    return status;
}

static LONG NTAPI zzNtAllocateVirtualMemoryEx(HANDLE process, PVOID* base, PSIZE_T size, ULONG type, ULONG protect, PVOID parameters, ULONG count) {
    if (!IsMovable(process, base, size, type, count != 0)) {
        return real_NtAllocateVirtualMemoryEx(process, base, size, type, protect, parameters, count);
    }
    LONG status = real_NtAllocateVirtualMemoryEx(process, base, size, type | MEM_TOP_DOWN, protect, parameters, count);
    ModsLog("reserve fix: %llu MB placed at %p, status %08lX", (DWORD64)(*size >> 20), *base, status);
    return status;
}

/// Called inside the Detours transaction of the patch.
void InstallReserveFix() {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    real_NtAllocateVirtualMemory = (NtAllocateVirtualMemory_t)GetProcAddress(ntdll, "NtAllocateVirtualMemory");
    real_NtAllocateVirtualMemoryEx = (NtAllocateVirtualMemoryEx_t)GetProcAddress(ntdll, "NtAllocateVirtualMemoryEx");
    if (real_NtAllocateVirtualMemory) {
        DetourAttach(&(PVOID&)real_NtAllocateVirtualMemory, zzNtAllocateVirtualMemory);
    }
    if (real_NtAllocateVirtualMemoryEx) {
        DetourAttach(&(PVOID&)real_NtAllocateVirtualMemoryEx, zzNtAllocateVirtualMemoryEx);
    }
}
