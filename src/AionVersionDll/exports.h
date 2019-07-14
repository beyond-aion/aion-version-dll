#ifndef VERSION_EXPORTS
#define VERSION_EXPORTS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

void* FindRealAddress(PCSTR name);

DWORD APIENTRY zzVerFindFileA(
    _In_                         DWORD uFlags,
    _In_                         LPCSTR szFileName,
    _In_opt_                     LPCSTR szWinDir,
    _In_                         LPCSTR szAppDir,
    _Out_writes_(*puCurDirLen)   LPSTR szCurDir,
    _Inout_                      PUINT puCurDirLen,
    _Out_writes_(*puDestDirLen)  LPSTR szDestDir,
    _Inout_                      PUINT puDestDirLen
);

DWORD APIENTRY zzVerFindFileW(
    _In_                         DWORD uFlags,
    _In_                         LPCWSTR szFileName,
    _In_opt_                     LPCWSTR szWinDir,
    _In_                         LPCWSTR szAppDir,
    _Out_writes_(*puCurDirLen)   LPWSTR szCurDir,
    _Inout_                      PUINT puCurDirLen,
    _Out_writes_(*puDestDirLen)  LPWSTR szDestDir,
    _Inout_                      PUINT puDestDirLen
);

DWORD APIENTRY zzVerInstallFileA(
    _In_                         DWORD uFlags,
    _In_                         LPCSTR szSrcFileName,
    _In_                         LPCSTR szDestFileName,
    _In_                         LPCSTR szSrcDir,
    _In_                         LPCSTR szDestDir,
    _In_                         LPCSTR szCurDir,
    _Out_writes_(*puTmpFileLen)  LPSTR szTmpFile,
    _Inout_                      PUINT puTmpFileLen
);

DWORD APIENTRY zzVerInstallFileW(
    _In_                         DWORD uFlags,
    _In_                         LPCWSTR szSrcFileName,
    _In_                         LPCWSTR szDestFileName,
    _In_                         LPCWSTR szSrcDir,
    _In_                         LPCWSTR szDestDir,
    _In_                         LPCWSTR szCurDir,
    _Out_writes_(*puTmpFileLen)  LPWSTR szTmpFile,
    _Inout_                      PUINT puTmpFileLen
);

DWORD APIENTRY zzGetFileVersionInfoSizeA(
    _In_        LPCSTR lptstrFilename, /* Filename of version stamped file */
    _Out_opt_ LPDWORD lpdwHandle       /* Information for use by GetFileVersionInfo */
);

DWORD APIENTRY zzGetFileVersionInfoSizeW(
    _In_        LPCWSTR lptstrFilename, /* Filename of version stamped file */
    _Out_opt_ LPDWORD lpdwHandle       /* Information for use by GetFileVersionInfo */
);

BOOL APIENTRY zzGetFileVersionInfoA(
    _In_                LPCSTR lptstrFilename, /* Filename of version stamped file */
    _Reserved_          DWORD dwHandle,          /* Information from GetFileVersionSize */
    _In_                DWORD dwLen,             /* Length of buffer for info */
    _Out_writes_bytes_(dwLen) LPVOID lpData            /* Buffer to place the data structure */
);

BOOL APIENTRY zzGetFileVersionInfoW(
    _In_                LPCWSTR lptstrFilename, /* Filename of version stamped file */
    _Reserved_          DWORD dwHandle,          /* Information from GetFileVersionSize */
    _In_                DWORD dwLen,             /* Length of buffer for info */
    _Out_writes_bytes_(dwLen) LPVOID lpData            /* Buffer to place the data structure */
);

DWORD APIENTRY zzGetFileVersionInfoSizeExA(_In_ DWORD dwFlags, _In_ LPCSTR lpwstrFilename, _Out_ LPDWORD lpdwHandle);

DWORD APIENTRY zzGetFileVersionInfoSizeExW(_In_ DWORD dwFlags, _In_ LPCWSTR lpwstrFilename, _Out_ LPDWORD lpdwHandle);

BOOL APIENTRY zzGetFileVersionInfoExA(_In_ DWORD dwFlags,
    _In_ LPCSTR lpwstrFilename,
    _Reserved_ DWORD dwHandle,
    _In_ DWORD dwLen,
    _Out_writes_bytes_(dwLen) LPVOID lpData);

BOOL APIENTRY zzGetFileVersionInfoExW(_In_ DWORD dwFlags,
    _In_ LPCWSTR lpwstrFilename,
    _Reserved_ DWORD dwHandle,
    _In_ DWORD dwLen,
    _Out_writes_bytes_(dwLen) LPVOID lpData);

DWORD APIENTRY zzVerLanguageNameA(
    _In_                  DWORD wLang,
    _Out_writes_(cchLang) LPSTR szLang,
    _In_                  DWORD cchLang
);

DWORD APIENTRY zzVerLanguageNameW(
    _In_                  DWORD wLang,
    _Out_writes_(cchLang) LPWSTR szLang,
    _In_                  DWORD cchLang
);

BOOL APIENTRY zzVerQueryValueA(
    _In_ LPCVOID pBlock,
    _In_ LPCSTR lpSubBlock,
    _Outptr_result_buffer_(_Inexpressible_("buffer can be PWSTR or DWORD*")) LPVOID* lplpBuffer,
    _Out_ PUINT puLen
);

BOOL APIENTRY zzVerQueryValueW(
    _In_ LPCVOID pBlock,
    _In_ LPCWSTR lpSubBlock,
    _Outptr_result_buffer_(_Inexpressible_("buffer can be PWSTR or DWORD*")) LPVOID* lplpBuffer,
    _Out_ PUINT puLen
);
#endif
