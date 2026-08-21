#include <algorithm>
#include <cstring>
#include <string>
#include <shlobj.h>
#include <shlwapi.h>
#pragma comment(lib, "Shlwapi.lib")
#include "detours.h"

typedef HRESULT(WINAPI* PFN_D3DXCompileShader)(LPCSTR, UINT, const void*, void*, LPCSTR, LPCSTR, DWORD, void**, void**, void**);
static PFN_D3DXCompileShader real_D3DXCompileShader = nullptr;

static HRESULT WINAPI zzD3DXCompileShader(LPCSTR pSrcData, UINT SrcDataLen, const void* pDefines, void* pInclude, LPCSTR pFunctionName, LPCSTR pProfile, DWORD Flags, void** ppShader, void** ppErrorMsgs, void** ppConstantTable) {
    static constexpr char target[] = "  fogInt *= ( 1.0f - exp( -t ) ) / t;";
    static constexpr char insertFix[] = " if (t != 0)";
    const char* begin = pSrcData;
    const char* end = pSrcData + SrcDataLen;
    auto it = std::search(begin, end, std::begin(target), std::end(target) - 1);
    if (it != end) {
        size_t pos = it - begin;
        std::string modified(pSrcData, SrcDataLen);
        modified.insert(pos + 1, insertFix);
        UINT modifiedLen = static_cast<UINT>(modified.size());
        return real_D3DXCompileShader(modified.c_str(), modifiedLen, pDefines, pInclude, pFunctionName, pProfile, Flags, ppShader, ppErrorMsgs, ppConstantTable);
    }
    return real_D3DXCompileShader(pSrcData, SrcDataLen, pDefines, pInclude, pFunctionName, pProfile, Flags, ppShader, ppErrorMsgs, ppConstantTable);
}

static std::string getGameRootDir() {
    char gameDir[MAX_PATH];
    GetModuleFileNameA(NULL, gameDir, MAX_PATH);
    PathRemoveFileSpecA(gameDir);
    PathRemoveFileSpecA(gameDir);
    return std::string(gameDir);
}

static void CreateFoldersRecursive(const std::string& path) {
    SHCreateDirectoryExA(NULL, path.c_str(), NULL);
}

static void DeleteFiles(const std::string& folder, const std::string& pattern) {
    WIN32_FIND_DATAA findData;
    HANDLE hFind = FindFirstFileA((folder + "\\" + pattern).c_str(), &findData);
    if (hFind == INVALID_HANDLE_VALUE) {
        return;
    }
    do {
        if (!(findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            std::string filePath = folder + "\\" + findData.cFileName;
            DeleteFileA(filePath.c_str());
        }
    } while (FindNextFileA(hFind, &findData) != 0);
    FindClose(hFind);
}

static void SetupShaderCache() {
    std::string cacheDir = getGameRootDir() + "\\Shaders\\Cache";
    // the game client deletes all cached shader files on startup if it crashed or closed improperly.
    // we name the marker file so that it also gets deleted on such recompilation events.
    // this way there remains no stale marker file if the game client recompiled shaders without the fix installed
    std::string markerFile = cacheDir + "\\CGPShaders\\#shader_fix_marker.cgps";
    CreateFoldersRecursive(cacheDir + "\\CGPShaders");
    CreateFoldersRecursive(cacheDir + "\\CGVShaders");
    bool markerFileMissing = GetFileAttributesA(markerFile.c_str()) == INVALID_FILE_ATTRIBUTES;
    if (markerFileMissing) {
        // delete all cached shader files to force recompilation with the fix
        DeleteFiles(cacheDir + "\\CGPShaders", "*.cgps");
        DeleteFiles(cacheDir + "\\CGVShaders", "*.cgvp");
        HANDLE hFile = CreateFileA(markerFile.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile != INVALID_HANDLE_VALUE) {
            CloseHandle(hFile);
        }
    }
}

/// Fixes black flickering when using the High Quality graphics engine with Nvidia drivers from 2016 or later, as well as rendering issues with AMD's Vulkan driver for Windows
void InstallShaderFix() {
    HMODULE hD3dx9 = LoadLibrary(L"d3dx9_38.dll");
    if (hD3dx9) {
        SetupShaderCache();
        real_D3DXCompileShader = (PFN_D3DXCompileShader)GetProcAddress(hD3dx9, "D3DXCompileShader");
        DetourAttach(&(PVOID&)real_D3DXCompileShader, zzD3DXCompileShader);
    }
}