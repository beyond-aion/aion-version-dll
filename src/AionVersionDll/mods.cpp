#include "mods.h"
#include <stdio.h>
#include <stdarg.h>
#include <share.h>
#include <shlwapi.h>
#include <string>
#include <vector>
#include "detours.h"

ModsConfig g_modsConfig = {};
wchar_t g_modsIniPath[MAX_PATH];
volatile int* g_gameState = nullptr;

static wchar_t s_dir[MAX_PATH];
static FILE* s_log = nullptr;
static volatile LONG s_gameModsInstalled = 0;
static volatile LONG s_installing = 0;

void ModsLog(const char* format, ...) {
    if (!s_log) {
        return;
    }
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(s_log, "%02d:%02d:%02d ", t.wHour, t.wMinute, t.wSecond);
    va_list args;
    va_start(args, format);
    vfprintf(s_log, format, args);
    va_end(args);
    fputc('\n', s_log);
    fflush(s_log);
}

std::vector<BYTE*> FindAllPatterns(HMODULE module, const char* pattern) {
    std::vector<int> bytes;
    for (const char* p = pattern; *p;) {
        if (*p == ' ') {
            p++;
        } else if (*p == '?') {
            bytes.push_back(-1);
            p += p[1] == '?' ? 2 : 1;
        } else {
            bytes.push_back((int)strtoul(p, (char**)&p, 16));
        }
    }
    auto dos = (PIMAGE_DOS_HEADER)module;
    auto nt = (PIMAGE_NT_HEADERS)((BYTE*)module + dos->e_lfanew);
    BYTE* begin = (BYTE*)module;
    BYTE* end = begin + nt->OptionalHeader.SizeOfImage - bytes.size();
    std::vector<BYTE*> found;
    for (BYTE* c = begin; c < end; c++) {
        size_t i = 0;
        while (i < bytes.size() && (bytes[i] < 0 || c[i] == bytes[i])) {
            i++;
        }
        if (i == bytes.size()) {
            found.push_back(c);
        }
    }
    return found;
}

BYTE* FindPattern(HMODULE module, const char* pattern) {
    std::vector<BYTE*> found = FindAllPatterns(module, pattern);
    return found.size() == 1 ? found[0] : nullptr;
}

static size_t ImageSize(HMODULE module) {
    auto dos = (PIMAGE_DOS_HEADER)module;
    return ((PIMAGE_NT_HEADERS)((BYTE*)module + dos->e_lfanew))->OptionalHeader.SizeOfImage;
}

bool InModule(HMODULE module, const void* address, size_t size) {
    return (const BYTE*)address >= (const BYTE*)module && (const BYTE*)address + size <= (const BYTE*)module + ImageSize(module);
}

BYTE* FindBytes(HMODULE module, const void* bytes, size_t length) {
    BYTE* begin = (BYTE*)module;
    BYTE* end = begin + ImageSize(module) - length;
    for (BYTE* c = begin; c < end; c++) {
        if (*c == *(const BYTE*)bytes && memcmp(c, bytes, length) == 0) {
            return c;
        }
    }
    return nullptr;
}

BYTE* FindRipReference(HMODULE module, const char* opcode, int length, const void* target) {
    BYTE code[4];
    int codeLength = 0;
    for (const char* p = opcode; *p && codeLength < 4;) {
        if (*p == ' ') {
            p++;
        } else {
            code[codeLength++] = (BYTE)strtoul(p, (char**)&p, 16);
        }
    }
    BYTE* begin = (BYTE*)module;
    BYTE* end = begin + ImageSize(module) - length;
    for (BYTE* c = begin; c < end; c++) {
        if (memcmp(c, code, codeLength) == 0 && ResolveRip(c + length - 4, c + length) == target) {
            return c;
        }
    }
    return nullptr;
}

static constexpr int STATE_IN_WORLD = 14;

BYTE* FindLeaTo(HMODULE module, const void* target) {
    BYTE* begin = (BYTE*)module;
    BYTE* end = begin + ImageSize(module) - 7;
    for (BYTE* c = begin; c < end; c++) {
        if ((c[0] == 0x48 || c[0] == 0x4C) && c[1] == 0x8D && (c[2] & 0xC7) == 0x05 && ResolveRip(c + 3, c + 7) == target) {
            return c;
        }
    }
    return nullptr;
}

BYTE* FindCallTo(HMODULE module, const void* target) {
    BYTE* begin = (BYTE*)module;
    BYTE* end = begin + ImageSize(module) - 5;
    for (BYTE* c = begin; c < end; c++) {
        if (c[0] == 0xE8 && ResolveRip(c + 1, c + 5) == target) {
            return c;
        }
    }
    return nullptr;
}

BYTE* FunctionStart(const void* address) {
    DWORD64 imageBase;
    PRUNTIME_FUNCTION function = RtlLookupFunctionEntry((DWORD64)address, &imageBase, nullptr);
    for (int depth = 0; function && depth < 32; depth++) {
        BYTE* unwind = (BYTE*)imageBase + function->UnwindData;
        if (!((unwind[0] >> 3) & UNW_FLAG_CHAININFO)) {
            return (BYTE*)imageBase + function->BeginAddress;
        }
        BYTE codes = unwind[2];
        function = (PRUNTIME_FUNCTION)(unwind + 4 + ((codes + 1) & ~1) * 2);
    }
    return nullptr;
}

BYTE* FunctionEnd(const void* start) {
    DWORD64 imageBase;
    PRUNTIME_FUNCTION function = RtlLookupFunctionEntry((DWORD64)start, &imageBase, nullptr);
    return function ? (BYTE*)imageBase + function->EndAddress : nullptr;
}

bool PatchMemory(void* at, const void* value, size_t size) {
    DWORD oldProtect;
    // code and data share one section in some clients
    if (!VirtualProtect(at, size, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        return false;
    }
    memcpy(at, value, size);
    VirtualProtect(at, size, oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), at, size);
    return true;
}

BYTE* FindIdleCheck(HMODULE module) {
    // sub eax, [rbx+lastInput]; cmp eax, [limit]; jbe; mov r9d, ebp; xor r8d, r8d; mov edx, 901652
    return FindPattern(module, "2B 83 ?? ?? 00 00 3B 05 ?? ?? ?? ?? 76 ?? 44 8B CD 45 33 C0 BA 14 C2 0D 00");
}

bool IsInWorld() {
    if (!g_gameState) {
        return false;
    }
    static int lastLogged = -1;
    int state = *g_gameState;
    if (state != lastLogged) {
        lastLogged = state;
        ModsLog("game state %d", state);
    }
    return state == STATE_IN_WORLD;
}

struct Setting {
    const wchar_t* section;
    const wchar_t* key;
    enum { Bool, Int, Float } type;
    void* target;
    float defaultValue;
    float min;
    float max;
};

static const Setting SETTINGS[] = {
    { L"General", L"Log", Setting::Bool, &g_modsConfig.log, 1 },
    { L"ChatTime", L"Enabled", Setting::Bool, &g_modsConfig.chatTime, 0 },
    { L"AntiAfk", L"Enabled", Setting::Bool, &g_modsConfig.antiAfk, 0 },
    { L"AntiAfk", L"NoSessionTimeout", Setting::Bool, &g_modsConfig.noSessionTimeout, 0 },
    { L"Ping", L"Enabled", Setting::Bool, &g_modsConfig.ping, 0 },
    { L"Ping", L"Interval", Setting::Int, &g_modsConfig.pingInterval, 3000, 1000, 60000 },
    // the start of the base line, one line below the frame rate of the DXVK HUD
    { L"Ping", L"X", Setting::Int, &g_modsConfig.pingX, 8, 0, 16384 },
    { L"Ping", L"Y", Setting::Int, &g_modsConfig.pingY, 48, 0, 16384 },
    { L"Ping", L"Scale", Setting::Float, &g_modsConfig.pingScale, 1, 0.25f, 4 },
    // the limit is a signed byte immediate in the client
    { L"Macros", L"Limit", Setting::Int, &g_modsConfig.macroLimit, 0, 0, 127 },
    { L"Stats", L"Enabled", Setting::Bool, &g_modsConfig.statPrecision, 0 },
    { L"QuestTargets", L"Enabled", Setting::Bool, &g_modsConfig.questTargets, 0 },
    { L"MultiClient", L"Enabled", Setting::Bool, &g_modsConfig.multiClient, 0 },
    { L"UiScale", L"Max", Setting::Int, &g_modsConfig.uiScaleMax, 130, 130, 400 },
    { L"UiScale", L"LargeGlyphFrom", Setting::Int, &g_modsConfig.largeGlyphFrom, 33, 8, 64 },
};

static void Apply(const Setting& setting, float value) {
    switch (setting.type) {
        case Setting::Bool: *(bool*)setting.target = value != 0; break;
        case Setting::Int: *(int*)setting.target = (int)value; break;
        case Setting::Float: *(float*)setting.target = value; break;
    }
}

static std::vector<std::string> s_configProblems;

static void ConfigProblem(const wchar_t* format, ...) {
    wchar_t text[512];
    va_list args;
    va_start(args, format);
    vswprintf_s(text, format, args);
    va_end(args);
    char utf8[1024];
    WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8, sizeof(utf8), nullptr, nullptr);
    s_configProblems.push_back(utf8);
}

/// Reads one value, reporting values that are not a number, not 0/1 for switches, or out of range.
static void Read(const Setting& setting) {
    Apply(setting, setting.defaultValue);
    wchar_t text[64];
    GetPrivateProfileStringW(setting.section, setting.key, L"", text, _countof(text), g_modsIniPath);
    if (!*text) {
        return;
    }
    wchar_t* end;
    float value = setting.type == Setting::Float ? wcstof(text, &end) : (float)wcstol(text, &end, 10);
    while (*end == L' ' || *end == L'\t') {
        end++;
    }
    if (*end || end == text) {
        ConfigProblem(L"[%s] %s = %s is not a number, using %g", setting.section, setting.key, text, setting.defaultValue);
        return;
    }
    if (setting.type == Setting::Bool && value != 0 && value != 1) {
        ConfigProblem(L"[%s] %s = %s should be 0 or 1, using %g", setting.section, setting.key, text, setting.defaultValue);
        return;
    }
    if (setting.type != Setting::Bool && (value < setting.min || value > setting.max)) {
        float clamped = min(setting.max, max(setting.min, value));
        ConfigProblem(L"[%s] %s = %s is outside %g..%g, using %g", setting.section, setting.key, text, setting.min, setting.max, clamped);
        value = clamped;
    }
    Apply(setting, value);
}

/// Reports sections and keys the mods do not know, which are usually typos.
static void CheckUnknownKeys() {
    static wchar_t names[4096];
    static wchar_t entries[8192];
    GetPrivateProfileSectionNamesW(names, _countof(names), g_modsIniPath);
    for (wchar_t* section = names; *section; section += wcslen(section) + 1) {
        bool knownSection = false;
        for (const Setting& setting : SETTINGS) {
            knownSection |= _wcsicmp(setting.section, section) == 0;
        }
        if (!knownSection) {
            ConfigProblem(L"unknown section [%s]", section);
            continue;
        }
        GetPrivateProfileSectionW(section, entries, _countof(entries), g_modsIniPath);
        for (wchar_t* entry = entries; *entry; entry += wcslen(entry) + 1) {
            wchar_t key[64] = {};
            const wchar_t* equals = wcschr(entry, L'=');
            wcsncpy_s(key, entry, equals ? min((size_t)(equals - entry), _countof(key) - 1) : _TRUNCATE);
            for (wchar_t* k = key + wcslen(key); k > key && (k[-1] == L' ' || k[-1] == L'\t'); k--) {
                k[-1] = 0;
            }
            bool knownKey = false;
            for (const Setting& setting : SETTINGS) {
                knownKey |= _wcsicmp(setting.section, section) == 0 && _wcsicmp(setting.key, key) == 0;
            }
            if (!knownKey) {
                ConfigProblem(L"unknown key [%s] %s", section, key);
            }
        }
    }
}

static void LoadConfig() {
    swprintf_s(g_modsIniPath, L"%s\\mods.ini", s_dir);
    for (const Setting& setting : SETTINGS) {
        Read(setting);
    }
    if (GetFileAttributesW(g_modsIniPath) == INVALID_FILE_ATTRIBUTES) {
        ConfigProblem(L"mods.ini not found, using the defaults");
    } else {
        CheckUnknownKeys();
    }
}

/// The idle check skips itself early on for some values of the global game state: mov eax, [state]; cmp eax, 10
static bool FindGameState(HMODULE game) {
    BYTE* check = FindIdleCheck(game);
    BYTE* function = check ? FunctionStart(check) : nullptr;
    for (BYTE* p = function; p && p < function + 0x40; p++) {
        if (p[0] == 0x8B && p[1] == 0x05 && p[6] == 0x83 && p[7] == 0xF8 && p[8] == 0x0A) {
            g_gameState = (volatile int*)ResolveRip(p + 2, p + 6);
            return true;
        }
    }
    return false;
}

static void InstallGameMods(HMODULE game) {
    if (InterlockedExchange(&s_installing, 1)) {
        return;
    }
    if (!s_gameModsInstalled) {
        FindGameState(game);
        ModsLog("Game.dll at %p, game state at %p", game, g_gameState);
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        InstallColorTagFix(game);
        if (g_modsConfig.chatTime) {
            InstallChatTime(game);
        }
        InstallTimeouts(game);
        if (g_modsConfig.ping) {
            InstallPing(game);
        }
        if (g_modsConfig.macroLimit) {
            InstallMacroLimit(game);
        }
        if (g_modsConfig.statPrecision) {
            InstallStatPrecision(game);
        }
        if (g_modsConfig.questTargets) {
            InstallQuestTargets(game);
        }
        InstallUiScale(game);
        if (g_modsConfig.multiClient) {
            InstallMultiClient(game);
        }
        LONG error = DetourTransactionCommit();
        ModsLog("game hooks committed: %ld", error);
        s_gameModsInstalled = 1;
    }
    s_installing = 0;
}

typedef LONG(NTAPI* LdrLoadDll_t)(PWSTR, PULONG, PVOID, PVOID*);
static LdrLoadDll_t real_LdrLoadDll = nullptr;

/// Game.dll unpacks itself while it loads, so its code is ready once the load call that returns it is done.
static volatile LONG s_cryFontPatched = 0;

static void CheckCryFont(HMODULE loaded) {
    if (loaded && !s_cryFontPatched && loaded == GetModuleHandleW(L"CryFont.dll") && !InterlockedExchange(&s_cryFontPatched, 1)) {
        InstallGlyphCells(loaded);
    }
}

static LONG NTAPI zzLdrLoadDll(PWSTR searchPath, PULONG characteristics, PVOID name, PVOID* handle) {
    LONG status = real_LdrLoadDll(searchPath, characteristics, name, handle);
    if (status >= 0 && handle) {
        CheckCryFont((HMODULE)*handle);
    }
    if (status >= 0 && handle && *handle && !s_gameModsInstalled && *handle == GetModuleHandleW(L"Game.dll")) {
        InstallGameMods((HMODULE)*handle);
    }
    return status;
}

/// Backup for the case that the game module arrives without going through the loader hook: waits until the game state
/// can be found, or a while after the module appeared for clients where it cannot.
static DWORD WINAPI WatchGameModule(LPVOID) {
    int seen = -1;
    for (int i = 0; i < 1200 && !s_gameModsInstalled; i++) {
        Sleep(100);
        HMODULE game = GetModuleHandleW(L"Game.dll");
        if (!game || s_gameModsInstalled) {
            continue;
        }
        if (seen < 0) {
            seen = i;
        }
        if (FindGameState(game) || i - seen >= 100) {
            InstallGameMods(game);
        }
    }
    if (!s_gameModsInstalled) {
        ModsLog("Game.dll hooks not installed: module not loaded");
    }
    return 0;
}

/// Must be called inside an open Detours transaction.
void InstallMods(HINSTANCE self) {
    // other programs in the game folder, like the web browser process, load version.dll as well
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const wchar_t* exeName = wcsrchr(exe, L'\\');
    if (_wcsicmp(exeName ? exeName + 1 : exe, L"aion.bin") != 0) {
        return;
    }
    GetModuleFileNameW(self, s_dir, MAX_PATH);
    wchar_t* slash = wcsrchr(s_dir, L'\\');
    if (slash) {
        *slash = 0;
    }
    LoadConfig();
    if (g_modsConfig.log) {
        wchar_t logPath[MAX_PATH];
        swprintf_s(logPath, L"%s\\mods.log", s_dir);
        s_log = _wfsopen(logPath, L"w", _SH_DENYNO);
        if (!s_log) {
            // another client from the same folder still holds the log
            swprintf_s(logPath, L"%s\\mods_%lu.log", s_dir, GetCurrentProcessId());
            s_log = _wfsopen(logPath, L"w", _SH_DENYNO);
        }
    }
    for (const std::string& problem : s_configProblems) {
        ModsLog("mods.ini: %s", problem.c_str());
    }
    ModsLog("mods: chatTime=%d antiAfk=%d noSessionTimeout=%d ping=%d macroLimit=%d stats=%d", g_modsConfig.chatTime, g_modsConfig.antiAfk,
        g_modsConfig.noSessionTimeout, g_modsConfig.ping, g_modsConfig.macroLimit, g_modsConfig.statPrecision);

    real_LdrLoadDll = (LdrLoadDll_t)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "LdrLoadDll");
    if (real_LdrLoadDll) {
        DetourAttach(&(PVOID&)real_LdrLoadDll, zzLdrLoadDll);
    }
    CheckCryFont(GetModuleHandleW(L"CryFont.dll"));
    CloseHandle(CreateThread(nullptr, 0, WatchGameModule, nullptr, 0, nullptr));
    if (g_modsConfig.ping) {
        InstallOverlay();
    }
}
