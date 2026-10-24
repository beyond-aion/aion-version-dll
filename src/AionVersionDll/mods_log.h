#ifndef AION_MODS_LOG
#define AION_MODS_LOG
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/// mods.ini next to version.dll.
extern wchar_t g_modsIniPath[MAX_PATH];

/// Opens mods.log next to version.dll when [General] Log is on, for the fixes and the mods of both builds, and points the module
/// watch's log at it. Comes first in the patch, so that whatever installs after it can log, in whatever order.
void OpenModsLog(HINSTANCE self);

void ModsLog(const char* format, ...);

/// One value of mods.ini.
struct Setting {
    const wchar_t* section;
    const wchar_t* key;
    enum { Bool, Int, Float } type;
    void* target;
    float defaultValue;
    float min;
    float max;
};

/// Reads one value, reporting values that are not a number, not 0/1 for switches, or out of range.
void ReadSetting(const Setting& setting);

#endif
