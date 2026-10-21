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
    bool ping;
    int pingInterval;
    int pingX;
    int pingY;
    float pingScale;
    int macroLimit;
    int targetBuffColumns;
    int ownBuffColumns;
    bool statPrecision;
    bool questTargets;
    bool multiClient;
    bool skipAgreement;
    int uiScaleMax; // percent
    int largeGlyphFrom; // pixel height
    int fastStart; // FAST_START_*
    bool mimalloc;
};

enum { FAST_START_OFF, FAST_START_SHOW_LOGO, FAST_START_FULL };

extern ModsConfig g_modsConfig;

/// The idle check, which compares the time since the last input against the idle limit (901652 = STR_MSG_AUTO_DISCONNECTED).
BYTE* FindIdleCheck(HMODULE module);

/// Game state shared by several mods, nullptr until Game.dll is loaded and the pattern was found.
extern volatile int* g_gameState;
bool IsInWorld();

/// Must be called inside an open Detours transaction.
void InstallMods();
void InstallChatTime(HMODULE game);
void InstallTimeouts(HMODULE game);
void InstallPing(HMODULE game);
void InstallMacroLimit(HMODULE game);
void InstallBuffLimit(HMODULE game);
void InstallBuffSlots(HMODULE game, bool uiXml);
/// Returns whether the edits can be applied.
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
void InstallUiScale(HMODULE game);
void InstallMultiClient(HMODULE game);
void InstallSkipAgreement(HMODULE game);
/// The start-up shortcuts outside Game.dll; must be called inside an open Detours transaction.
void InstallFastStart();
void InstallSkipLogo(HMODULE game);
void InstallFastLoad(HMODULE game);
/// mimalloc in place of the allocators of CrySystem.
void InstallMimalloc();
void InstallGlyphCells(HMODULE cryFont);
void InstallOverlay();
/// Hooks the device methods again when something else wrote over them; called once a frame.
void KeepOverlayHooks();

#endif
