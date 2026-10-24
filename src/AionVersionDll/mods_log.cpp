#include "mods_log.h"
#include "module_load.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <share.h>

wchar_t g_modsIniPath[MAX_PATH];

static FILE* s_log = nullptr;

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

static void Apply(const Setting& setting, float value) {
    switch (setting.type) {
        case Setting::Bool: *(bool*)setting.target = value != 0; break;
        case Setting::Int: *(int*)setting.target = (int)value; break;
        case Setting::Float: *(float*)setting.target = value; break;
    }
}

void ReadSetting(const Setting& setting) {
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

void OpenModsLog(HINSTANCE self) {
    wchar_t dir[MAX_PATH];
    GetModuleFileNameW(self, dir, MAX_PATH);
    wchar_t* slash = wcsrchr(dir, L'\\');
    if (slash) {
        *slash = 0;
    }
    swprintf_s(g_modsIniPath, L"%s\\mods.ini", dir);
    bool enabled;
    ReadSetting({ L"General", L"Log", Setting::Bool, &enabled, 0 });
    if (enabled) {
        wchar_t logPath[MAX_PATH];
        swprintf_s(logPath, L"%s\\mods.log", dir);
        s_log = _wfsopen(logPath, L"w", _SH_DENYNO);
        if (!s_log) {
            // another client from the same folder still holds the log
            swprintf_s(logPath, L"%s\\mods_%lu.log", dir, GetCurrentProcessId());
            s_log = _wfsopen(logPath, L"w", _SH_DENYNO);
        }
    }
    g_moduleLog = ModsLog;
    if (GetFileAttributesW(g_modsIniPath) == INVALID_FILE_ATTRIBUTES) {
        ModsLog("mods.ini: file not found, using the defaults");
    }
}
