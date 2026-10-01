#ifndef AION_MODS
#define AION_MODS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <vector>

/// Settings from mods.ini next to version.dll. Missing keys fall back to the defaults in mods.cpp.
struct ModsConfig {
    bool log;
    bool chatTime;
    bool antiAfk;
    bool noSessionTimeout;
    bool ping;
    int pingInterval;
    int pingX;
    int pingY;
    float pingScale;
    int macroLimit;
    bool statPrecision;
    bool questTargets;
    int uiScaleMax; // percent
    int largeGlyphFrom; // pixel height
};

extern wchar_t g_modsIniPath[MAX_PATH];

extern ModsConfig g_modsConfig;

void ModsLog(const char* format, ...);

/// Finds an IDA-style byte pattern ("48 8B ?? 05") in the given module's image, returns nullptr if missing or not unique.
BYTE* FindPattern(HMODULE module, const char* pattern);
std::vector<BYTE*> FindAllPatterns(HMODULE module, const char* pattern);

/// Finds the first occurrence of exact bytes in the module's image.
BYTE* FindBytes(HMODULE module, const void* bytes, size_t length);

/// Finds the instruction starting with the given opcode bytes whose RIP-relative operand, ending the instruction, points at target.
BYTE* FindRipReference(HMODULE module, const char* opcode, int length, const void* target);

bool InModule(HMODULE module, const void* address, size_t size);

/// Finds a RIP-relative lea (any register) that loads target.
BYTE* FindLeaTo(HMODULE module, const void* target);

/// Finds a direct call (E8 rel32) to target.
BYTE* FindCallTo(HMODULE module, const void* target);

/// Start of the function containing address, following chained unwind entries back to the primary one.
BYTE* FunctionStart(const void* address);

/// End of the primary unwind range of the function starting at start.
BYTE* FunctionEnd(const void* start);

/// Overwrites code or read-only data.
bool PatchMemory(void* at, const void* value, size_t size);

/// The idle check, which compares the time since the last input against the idle limit (901652 = STR_MSG_AUTO_DISCONNECTED).
BYTE* FindIdleCheck(HMODULE module);

/// Resolves the target of a RIP-relative operand whose 32-bit displacement starts at dispAt and whose instruction ends at nextInsn.
inline BYTE* ResolveRip(BYTE* dispAt, BYTE* nextInsn) {
    return nextInsn + *(INT32*)dispAt;
}

/// Game state shared by several mods, nullptr until Game.dll is loaded and the pattern was found.
extern volatile int* g_gameState;
bool IsInWorld();

void InstallMods(HINSTANCE self);
void InstallChatTime(HMODULE game);
void InstallTimeouts(HMODULE game);
void InstallPing(HMODULE game);
void InstallMacroLimit(HMODULE game);
void InstallStatPrecision(HMODULE game);
void InstallQuestTargets(HMODULE game);
void InstallUiScale(HMODULE game);
void InstallGlyphCells(HMODULE cryFont);
void InstallOverlay();

#endif
