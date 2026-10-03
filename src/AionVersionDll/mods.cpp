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

static const Setting LOG_SETTING = { L"General", L"Log", Setting::Bool, &g_modsConfig.log, 0 };

static const Setting SETTINGS[] = {
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
        ModsLog("mods.ini: [%ls] %ls = %ls is not a number, using %g", setting.section, setting.key, text, setting.defaultValue);
        return;
    }
    if (setting.type == Setting::Bool && value != 0 && value != 1) {
        ModsLog("mods.ini: [%ls] %ls = %ls should be 0 or 1, using %g", setting.section, setting.key, text, setting.defaultValue);
        return;
    }
    if (setting.type != Setting::Bool && (value < setting.min || value > setting.max)) {
        float clamped = min(setting.max, max(setting.min, value));
        ModsLog("mods.ini: [%ls] %ls = %ls is outside %g..%g, using %g", setting.section, setting.key, text, setting.min, setting.max, clamped);
        value = clamped;
    }
    Apply(setting, value);
}

static void LoadConfig() {
    swprintf_s(g_modsIniPath, L"%s\\mods.ini", s_dir);
    Read(LOG_SETTING);
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
    for (const Setting& setting : SETTINGS) {
        Read(setting);
    }
    if (GetFileAttributesW(g_modsIniPath) == INVALID_FILE_ATTRIBUTES) {
        ModsLog("mods.ini: file not found, using the defaults");
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
    if (s_gameModsInstalled) {
        return;
    }
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
    if (g_modsConfig.uiScaleMax > 130) {
        InstallUiScale(game);
    }
    LONG error = DetourTransactionCommit();
    ModsLog("game hooks committed: %ld", error);
    s_gameModsInstalled = 1;
}

static PVOID s_ldrCookie = nullptr;

/// Game.dll unpacks itself while it loads, so its code is ready once the load notification arrives.
static volatile LONG s_cryFontPatched = 0;

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
    PWSTR name = d->BaseDllName->Buffer;
    HMODULE hDll = (HMODULE)d->DllBase;
    if (_wcsicmp(name, L"Game.dll") == 0) {
        InstallGameMods(hDll);
    } else if (_wcsicmp(name, L"CryFont.dll") == 0) {
        InstallGlyphCells(hDll);
    }
}

/// Must be called inside an open Detours transaction.
void InstallMods(HINSTANCE self) {
    GetModuleFileNameW(self, s_dir, MAX_PATH);
    wchar_t* slash = wcsrchr(s_dir, L'\\');
    if (slash) {
        *slash = 0;
    }
    LoadConfig();
    ModsLog("mods: chatTime=%d antiAfk=%d noSessionTimeout=%d ping=%d macroLimit=%d stats=%d", g_modsConfig.chatTime, g_modsConfig.antiAfk,
        g_modsConfig.noSessionTimeout, g_modsConfig.ping, g_modsConfig.macroLimit, g_modsConfig.statPrecision);

    LdrRegisterDllNotification_t reg = (LdrRegisterDllNotification_t)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "LdrRegisterDllNotification");
    if (reg) {
        LONG st = reg(0, LdrDllNotification, nullptr, &s_ldrCookie);
        if (st < 0) {
            ModsLog("mods: LdrRegisterDllNotification failed: 0x%08X", (unsigned)st);
        }
    } else {
        ModsLog("mods: LdrRegisterDllNotification not found");
    }
    if (g_modsConfig.ping) {
        InstallOverlay();
    }
}
