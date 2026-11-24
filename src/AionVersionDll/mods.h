#ifndef AION_MODS
#define AION_MODS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <vector>
#include <string>
#include "code_patch.h"
#include "mods_log.h"

/// Settings from mods.ini next to version.dll. Missing keys fall back to the defaults in mods.cpp.
struct ModsConfig {
    int chatTimeFormat;
    bool antiAfk;
    bool noSessionTimeout;
    int pingOverlayMode;
    int pingInterval;
    int pingX;
    int pingY;
    float pingScale;
    int macroLimit;
    int targetBuffColumns;
    int ownBuffColumns;
    bool statPrecision;
    bool questTargets;
    bool moveReports;
    bool othersSnap;
    bool multiClient;
    bool skipAgreement;
    bool skipTypedConfirm;
    bool uiScaleFit;
    int largeGlyphFrom; // pixel height
    int fastStart; // FAST_START_*
    bool mimalloc;
    int animationLod; // meters, 0 = off
    bool plainSystemCfg;
};

enum { FAST_START_OFF, FAST_START_SHOW_LOGO, FAST_START_FULL };

/// The clients give fonts taller than 32 pixels the larger glyph cells.
constexpr int CLIENT_LARGE_GLYPH_FROM = 33;

extern ModsConfig g_modsConfig;

/// [Debug] FrameStat: seconds between the frame time lines in version.log, 0 = off (profiler.cpp, called from the overlay's Present).
extern int g_frameStatSeconds;
void FrameStatBeforePresent(LARGE_INTEGER& before);
void FrameStatAfterPresent(const LARGE_INTEGER& before);

/// The idle check, which compares the time since the last input against the idle limit (901652 = STR_MSG_AUTO_DISCONNECTED).
BYTE* FindIdleCheck(HMODULE module);

/// Game state shared by several mods, nullptr until Game.dll is loaded and the pattern was found.
extern volatile int* g_gameState;
bool IsInWorld();

/// The table loop is over: mark the dumps on disk complete, so that an early exit does not make the next start rebuild them.
void MarkDumpsComplete();

/// Must be called inside an open Detours transaction.
void InstallMods();
/// [Debug] DumpModules: images of the client's modules for modcheck; added before the fixes and mods, to come before their changes.
void InstallModuleDumps();
void InstallChatTime(HMODULE game);
/// Raises the idle limit of the client out of reach.
void InstallAntiAfk(HMODULE game);
/// Turns off the 26-hour session limit.
void InstallSessionTimeout(HMODULE game);
void InstallPing(HMODULE game);
void InstallMacroLimit(HMODULE game);
void InstallBuffLimit(HMODULE game);
/// Up to 64 buffs and debuffs in the own windows and the target window, once the UI XML hooks are in place.
void InstallBuffCount(HMODULE game, bool uiXml);
/// The configured columns of the own buff windows.
void InstallOwnGrid(HMODULE game, bool uiXml);
/// The configured columns of the target window, and its rows below the target of the target in 5.x.
void InstallTargetGrid(HMODULE game);
/// Also reads the text files that are valid UTF-8 as UTF-8. Returns whether the edits can be applied.
bool InstallUiXml(HMODULE game);

/// A node of a UI XML file as the client's binary format stores it.
struct XmlNode {
    std::wstring name;
    std::vector<std::pair<std::wstring, std::wstring>> attributes;
    bool hasText = false;
    std::wstring text;
    std::vector<XmlNode> children;

    const std::wstring* Attribute(const wchar_t* key) const;
    void SetAttribute(const wchar_t* key, const std::wstring& value);
    /// The child whose name attribute is the given one.
    XmlNode* ChildNamed(const std::wstring& nameAttribute);
};

/// Changes a UI dialog every time the client loads it, from its own file (<name>.xml, any folder) or from the screen file that
/// holds the dialogs of older clients. Register before InstallUiXml.
void AddUiXmlEdit(const wchar_t* dialogName, void (*apply)(XmlNode& dialog));
void InstallStatPrecision(HMODULE game);
void InstallQuestTargets(HMODULE game);
void InstallMoveReports(HMODULE game);
void InstallOthersSnap(HMODULE game);
/// Loads the largest tables of 4.x clients from the dumps the client keeps in Data/Dump.
void InstallTableDump(HMODULE game);
void InstallUiScale(HMODULE game);
void InstallMultiClient(HMODULE game);
void InstallSkipAgreement(HMODULE game);
/// Shows the usual yes-or-no question instead of the ones that ask to type a word.
void InstallQuickDestroy(HMODULE game);
/// The start-up shortcuts outside Game.dll; must be called inside an open Detours transaction.
void InstallFastStart();
void InstallSkipLogo(HMODULE game);
void InstallFastLoad(HMODULE game);
/// mimalloc in place of the allocators of CrySystem.
void InstallMimalloc();
void InstallGlyphCells(HMODULE cryFont);
void InstallAnimationLod(HMODULE animation);
void InstallOverlay();
/// Reads and saves system.cfg in plain text.
void InstallPlainSystemCfg();
/// Hooks the device methods again when something else wrote over them; called once a frame.
void KeepOverlayHooks();

#endif
