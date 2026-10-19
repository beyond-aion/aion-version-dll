#include "mods.h"
#include "module_load.h"
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

// The searches below go through images of up to 30 MB some sixty times while Game.dll loads, so each one looks for a single byte
// with memchr first: the byte of the pattern least common in x64 code, or the opcode of the instruction.

/// How common a byte is in x64 code, roughly: 0 for rare ones, more for the opcodes, prefixes and immediates seen everywhere.
static int Commonness(BYTE b) {
    switch (b) {
        case 0x00: case 0xFF: case 0xCC:
            return 4;
        case 0x48: case 0x8B: case 0x89: case 0x24: case 0x4C: case 0x44: case 0x0F: case 0xE8: case 0x8D: case 0x83: case 0x01:
        case 0xC0: case 0x05: case 0x10: case 0x08: case 0x20: case 0x40: case 0x45: case 0x49: case 0x41: case 0x85: case 0x74:
        case 0x75: case 0xC3: case 0x33: case 0xC7: case 0x4D: case 0xE9: case 0x02: case 0x04: case 0x0C: case 0x18: case 0x28:
        case 0x30: case 0x38: case 0x50: case 0x54: case 0x5C: case 0x15: case 0x0D: case 0x1D: case 0xEB: case 0x3B: case 0xC8:
            return 2;
        default:
            return 0;
    }
}

/// Offset of the fixed byte to look for first, or length if none is fixed.
static size_t AnchorOf(const BYTE* bytes, const char* fixed, size_t length) {
    size_t best = length;
    for (size_t i = 0; i < length; i++) {
        if (fixed[i] && (best == length || Commonness(bytes[i]) < Commonness(bytes[best]))) {
            best = i;
        }
    }
    return best;
}

/// Calls found(c) for every c in [begin, end) with c[anchor] == value, until it returns true.
template <typename Found>
static void ForEachByte(BYTE* begin, BYTE* end, size_t anchor, BYTE value, Found found) {
    for (BYTE* c = begin; c < end;) {
        BYTE* hit = (BYTE*)memchr(c + anchor, value, end - c);
        if (!hit) {
            return;
        }
        c = hit - anchor;
        if (found(c)) {
            return;
        }
        c++;
    }
}

std::vector<BYTE*> FindAllPatterns(HMODULE module, const char* pattern) {
    std::vector<BYTE> bytes;
    std::vector<char> fixed;
    for (const char* p = pattern; *p;) {
        if (*p == ' ') {
            p++;
        } else if (*p == '?') {
            bytes.push_back(0);
            fixed.push_back(0);
            p += p[1] == '?' ? 2 : 1;
        } else {
            bytes.push_back((BYTE)strtoul(p, (char**)&p, 16));
            fixed.push_back(1);
        }
    }
    size_t length = bytes.size();
    BYTE* begin = (BYTE*)module;
    BYTE* end = begin + ModuleImageSize(module) - length;
    std::vector<BYTE*> found;
    size_t anchor = AnchorOf(bytes.data(), fixed.data(), length);
    if (anchor == length) {
        return found;
    }
    ForEachByte(begin, end, anchor, bytes[anchor], [&](BYTE* c) {
        size_t i = 0;
        while (i < length && (!fixed[i] || c[i] == bytes[i])) {
            i++;
        }
        if (i == length) {
            found.push_back(c);
        }
        return false;
    });
    return found;
}

BYTE* FindPattern(HMODULE module, const char* pattern) {
    std::vector<BYTE*> found = FindAllPatterns(module, pattern);
    return found.size() == 1 ? found[0] : nullptr;
}

bool InModule(HMODULE module, const void* address, size_t size) {
    return (const BYTE*)address >= (const BYTE*)module && (const BYTE*)address + size <= (const BYTE*)module + ModuleImageSize(module);
}

BYTE* FindBytes(HMODULE module, const void* bytes, size_t length) {
    BYTE* begin = (BYTE*)module;
    BYTE* end = begin + ModuleImageSize(module) - length;
    std::vector<char> fixed(length, 1);
    size_t anchor = AnchorOf((const BYTE*)bytes, fixed.data(), length);
    BYTE* result = nullptr;
    ForEachByte(begin, end, anchor, ((const BYTE*)bytes)[anchor], [&](BYTE* c) {
        if (memcmp(c, bytes, length) == 0) {
            result = c;
        }
        return result != nullptr;
    });
    return result;
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
    BYTE* end = begin + ModuleImageSize(module) - length;
    BYTE* result = nullptr;
    ForEachByte(begin, end, codeLength - 1, code[codeLength - 1], [&](BYTE* c) {
        if (memcmp(c, code, codeLength) == 0 && ResolveRip(c + length - 4, c + length) == target) {
            result = c;
        }
        return result != nullptr;
    });
    return result;
}

static constexpr int STATE_IN_WORLD = 14;

BYTE* FindLeaTo(HMODULE module, const void* target) {
    BYTE* begin = (BYTE*)module;
    BYTE* end = begin + ModuleImageSize(module) - 7;
    BYTE* result = nullptr;
    ForEachByte(begin, end, 1, 0x8D, [&](BYTE* c) {
        if ((c[0] == 0x48 || c[0] == 0x4C) && (c[2] & 0xC7) == 0x05 && ResolveRip(c + 3, c + 7) == target) {
            result = c;
        }
        return result != nullptr;
    });
    return result;
}

std::vector<BYTE*> FindAllLeasTo(HMODULE module, const void* target) {
    BYTE* begin = (BYTE*)module;
    BYTE* end = begin + ModuleImageSize(module) - 7;
    std::vector<BYTE*> found;
    ForEachByte(begin, end, 1, 0x8D, [&](BYTE* c) {
        if ((c[0] == 0x48 || c[0] == 0x4C) && (c[2] & 0xC7) == 0x05 && ResolveRip(c + 3, c + 7) == target) {
            found.push_back(c);
        }
        return false;
    });
    return found;
}

BYTE* FindCallTo(HMODULE module, const void* target) {
    BYTE* begin = (BYTE*)module;
    BYTE* end = begin + ModuleImageSize(module) - 5;
    BYTE* result = nullptr;
    ForEachByte(begin, end, 0, 0xE8, [&](BYTE* c) {
        if (ResolveRip(c + 1, c + 5) == target) {
            result = c;
        }
        return result != nullptr;
    });
    return result;
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

BYTE* FunctionBodyEnd(BYTE* start, size_t maxSize) {
    for (BYTE* p = start + 1; p < start + maxSize; p++) {
        BYTE* owner = FunctionStart(p);
        if (owner && owner != start) {
            return p;
        }
    }
    return start + maxSize;
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
    { L"ChatTime", L"Format", Setting::Int, &g_modsConfig.chatTimeFormat, 0, 0, 2 },
    { L"AntiAfk", L"Enabled", Setting::Bool, &g_modsConfig.antiAfk, 0 },
    { L"AntiAfk", L"NoSessionTimeout", Setting::Bool, &g_modsConfig.noSessionTimeout, 0 },
    { L"Ping", L"Enabled", Setting::Bool, &g_modsConfig.ping, 0 },
    { L"Ping", L"Interval", Setting::Int, &g_modsConfig.pingInterval, 3000, 500, 60000 },
    // the start of the base line, one line below the frame rate of the DXVK HUD
    { L"Ping", L"X", Setting::Int, &g_modsConfig.pingX, 8, 0, 16384 },
    { L"Ping", L"Y", Setting::Int, &g_modsConfig.pingY, 48, 0, 16384 },
    { L"Ping", L"Scale", Setting::Float, &g_modsConfig.pingScale, 1, 0.25f, 4 },
    // the limit is a signed byte immediate in the client
    { L"Macros", L"Limit", Setting::Int, &g_modsConfig.macroLimit, 0, 0, 127 },
    { L"Buffs", L"TargetColumns", Setting::Int, &g_modsConfig.targetBuffColumns, 0, 0, 64 },
    { L"Buffs", L"OwnColumns", Setting::Int, &g_modsConfig.ownBuffColumns, 0, 0, 64 },
    { L"Stats", L"Enabled", Setting::Bool, &g_modsConfig.statPrecision, 0 },
    { L"QuestTargets", L"Enabled", Setting::Bool, &g_modsConfig.questTargets, 0 },
    { L"MultiClient", L"Enabled", Setting::Bool, &g_modsConfig.multiClient, 0 },
    { L"Performance", L"LoadingMode", Setting::Int, &g_modsConfig.fastStart, FAST_START_OFF, FAST_START_OFF, FAST_START_FULL },
    { L"Performance", L"Mimalloc", Setting::Bool, &g_modsConfig.mimalloc, 0 },
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
    if (g_modsConfig.chatTimeFormat != 0) {
        InstallChatTime(game);
    }
    InstallTimeouts(game);
    if (g_modsConfig.ping) {
        InstallPing(game);
    }
    if (g_modsConfig.macroLimit) {
        InstallMacroLimit(game);
    }
    bool buffs = g_modsConfig.targetBuffColumns || g_modsConfig.ownBuffColumns;
    if (buffs) {
        InstallBuffLimit(game);
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
    if (g_modsConfig.multiClient) {
        InstallMultiClient(game);
    }
    if (g_modsConfig.fastStart == FAST_START_FULL) {
        InstallSkipLogo(game);
    }
    if (g_modsConfig.fastStart != FAST_START_OFF) {
        InstallFastLoad(game);
    }
    bool uiXml = InstallUiXml(game);
    if (buffs) {
        InstallBuffSlots(game, uiXml);
    }
    LONG error = DetourTransactionCommit();
    ModsLog("game hooks committed: %ld", error);
    s_gameModsInstalled = 1;
}

/// Must be called inside an open Detours transaction.
void InstallMods(HINSTANCE self) {
    GetModuleFileNameW(self, s_dir, MAX_PATH);
    wchar_t* slash = wcsrchr(s_dir, L'\\');
    if (slash) {
        *slash = 0;
    }
    LoadConfig();
    ModsLog("mods: chatTimeFormat=%d antiAfk=%d noSessionTimeout=%d ping=%d macroLimit=%d stats=%d", g_modsConfig.chatTimeFormat, g_modsConfig.antiAfk,
        g_modsConfig.noSessionTimeout, g_modsConfig.ping, g_modsConfig.macroLimit, g_modsConfig.statPrecision);
    g_moduleLog = ModsLog;
    InstallCrashLog();

    OnModuleLoad(L"Game.dll", InstallGameMods, true);
    OnModuleLoad(L"CryFont.dll", InstallGlyphCells, false);
    if (g_modsConfig.fastStart != FAST_START_OFF) {
        InstallFastStart();
    }
    if (g_modsConfig.mimalloc) {
        InstallMimalloc();
    }
    if (g_modsConfig.ping) {
        InstallOverlay();
    }
}
