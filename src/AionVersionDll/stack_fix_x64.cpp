#include "exports.h"
#include <winternl.h>
#include "detours.h"

/* Code from https://github.com/AbdouRoumi/Custom-GetProcAddress */
FARPROC CustomGetProcProcess(IN HMODULE hModule, IN LPCSTR FuncName) {
    PBYTE pBase = (PBYTE)hModule;

    // Getting the DOS header and checking the signature
    PIMAGE_DOS_HEADER pImgDosHdr = (PIMAGE_DOS_HEADER)pBase;
    if (pImgDosHdr->e_magic != IMAGE_DOS_SIGNATURE) {
        return NULL;
    }

    // Getting the NT headers and checking the signature
    PIMAGE_NT_HEADERS pImgNtHdrs = (PIMAGE_NT_HEADERS)(pImgDosHdr->e_lfanew + pBase);
    if (pImgNtHdrs->Signature != IMAGE_NT_SIGNATURE) {
        return NULL;
    }

    // Getting the optional header
    IMAGE_OPTIONAL_HEADER ImgOptHdr = pImgNtHdrs->OptionalHeader;

    // Getting the export table
    PIMAGE_EXPORT_DIRECTORY pImgExportDir = (PIMAGE_EXPORT_DIRECTORY)(pBase + ImgOptHdr.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress);

    // Getting the function name array
    PDWORD FunctionNameArray = (PDWORD)(pBase + pImgExportDir->AddressOfNames);

    // Getting the func address array
    PDWORD FunctionAddressArray = (PDWORD)(pBase + pImgExportDir->AddressOfFunctions);

    // Getting the func ordinal array
    PWORD FunctionOrdinalArray = (PWORD)(pBase + pImgExportDir->AddressOfNameOrdinals); // Use PWORD, not PDWORD

    for (DWORD i = 0; i < pImgExportDir->NumberOfNames; i++) { // Iterate over NumberOfNames
        // Getting the name of the function
        CHAR* pFunctionName = (CHAR*)(pBase + FunctionNameArray[i]);

        // Getting the address of the function through its ordinal
        FARPROC pFunctionAddress = (FARPROC)(pBase + FunctionAddressArray[FunctionOrdinalArray[i]]);

        // Searching for the function
        if (strcmp(FuncName, pFunctionName) == 0) {
            return pFunctionAddress;
        }
    }

    return NULL;
}


extern "C" decltype(CloseHandle)* real_CloseHandle = CloseHandle;
extern "C"
BOOL
WINAPI
zzCloseHandle(_In_ _Post_ptr_invalid_ HANDLE hObject);


int except_aionbin = 1;
int except_gamedll = 1;
int except_crysystem = 1;
int except_aegisty = 2;


bool moduleinfo_aionbin = false;
bool moduleinfo_gamedll = false;
bool moduleinfo_crysystem = false;
bool moduleinfo_aegisty = false;

HMODULE haionbin = NULL;
HMODULE hgamedll = NULL;
HMODULE hcrysystem = NULL;
HMODULE haegisty = NULL;


extern "C" int CheckInjectExcept(void* addr) {
    HMODULE caller = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCSTR)addr, &caller);

    if (except_aionbin)
    {
        if (!haionbin)
            haionbin = GetModuleHandleA(NULL);

        if (haionbin)
        {
            if (caller == haionbin)
            {
                except_aionbin--;
                return 1;
            }
        }
    }

    if (except_gamedll)
    {
        if (!hgamedll)
            hgamedll = GetModuleHandleA("game.dll");
        if (hgamedll)
        {
            if (caller == hgamedll)
            {
                except_gamedll--;
                return 1;
            }
        }
    }

    if (except_crysystem)
    {
        if (!hcrysystem)
            hcrysystem = GetModuleHandleA("crysystem.dll");
        if (hcrysystem)
        {
            if (caller == hcrysystem)
            {
                except_crysystem--;
                return 1;
            }
        }
    }

    if (except_aegisty)
    {
        if (!haegisty)
            haegisty = GetModuleHandleA("aegisty64.bin");
        if (haegisty)
        {
            if (caller == haegisty)
            {
                except_aegisty--;
                if (except_aegisty == 0) // only for second instance?
                    return 1;
            }
        }
    }

    return 0;
}



typedef PVOID(WINAPI* TRtlAddVectoredExceptionHandler)(ULONG, PVECTORED_EXCEPTION_HANDLER);

extern "C" TRtlAddVectoredExceptionHandler real_RtlAddVectoredExceptionHandler = nullptr;

extern "C" PVOID
WINAPI
zzRtlAddVectoredExceptionHandler(ULONG first, PVECTORED_EXCEPTION_HANDLER func);




extern "C" decltype(CreateFileA)* real_CreateFileA = nullptr;

extern "C"
HANDLE
WINAPI
zzCreateFileA(
    _In_ LPCSTR lpFileName,
    _In_ DWORD dwDesiredAccess,
    _In_ DWORD dwShareMode,
    _In_opt_ LPSECURITY_ATTRIBUTES lpSecurityAttributes,
    _In_ DWORD dwCreationDisposition,
    _In_ DWORD dwFlagsAndAttributes,
    _In_opt_ HANDLE hTemplateFile);


extern "C" decltype(LoadLibraryA)* real_LoadLibraryA = LoadLibraryA;

extern "C"
HMODULE
WINAPI
zzLoadLibraryA(
    _In_ LPCSTR lpLibFileName
);

/// Fixes the 64-bit client crashing on startup on Windows 11 version 24H2 or later
void Install64BitStackFix() {
    /* fix stack on call CloseHandle */
    DetourAttach(&(PVOID&)real_CloseHandle, zzCloseHandle);

    /* fix for rbp restore in themida */
    HMODULE hntdll = GetModuleHandleA("ntdll.dll");
    real_RtlAddVectoredExceptionHandler = (TRtlAddVectoredExceptionHandler)GetProcAddress(hntdll, "RtlAddVectoredExceptionHandler");

    DetourAttach(&(PVOID&)real_RtlAddVectoredExceptionHandler, zzRtlAddVectoredExceptionHandler);

    /* fix stack on call. Use custom getprocaddress because standard return it from aclayer */
    HMODULE hkernel32 = GetModuleHandleA("kernel32.dll");
    real_CreateFileA = (decltype(CreateFileA)*)CustomGetProcProcess(hkernel32, "CreateFileA");

    DetourAttach(&(PVOID&)real_CreateFileA, zzCreateFileA);

    /* same fix*/
    DetourAttach(&(PVOID&)real_LoadLibraryA, zzLoadLibraryA);
}