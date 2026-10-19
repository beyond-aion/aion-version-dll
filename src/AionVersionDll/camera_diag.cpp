#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <share.h>

// Diagnostic build, not for merging: keeps the last camera events in memory and writes them to camera_diag_<pid>.log next to version.dll
// when something looks like a jump (a mouse move outside the window or far from where the client put the cursor, a large raw mouse delta,
// the client not putting the cursor back for a while) and when F12 is pressed. Each line: ms since the start, the event.

static const int RING = 512;
static char s_ring[RING][192];
static double s_ringTime[RING];
static int s_ringNext, s_ringCount;
static FILE* s_log;
static LARGE_INTEGER s_frequency, s_start;
static CRITICAL_SECTION s_lock;
static int s_dumps;
static const int MAX_DUMPS = 400;

double CameraDiagNow() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (double)(now.QuadPart - s_start.QuadPart) * 1000.0 / (double)s_frequency.QuadPart;
}

void CameraDiagTrace(const char* format, ...) {
    if (!s_log) {
        return;
    }
    char text[192];
    va_list args;
    va_start(args, format);
    vsprintf_s(text, format, args);
    va_end(args);
    EnterCriticalSection(&s_lock);
    s_ringTime[s_ringNext] = CameraDiagNow();
    strcpy_s(s_ring[s_ringNext], text);
    s_ringNext = (s_ringNext + 1) % RING;
    if (s_ringCount < RING) {
        s_ringCount++;
    }
    LeaveCriticalSection(&s_lock);
}

/// Writes the reason and the events kept since the last dump.
void CameraDiagDump(const char* format, ...) {
    if (!s_log) {
        return;
    }
    char reason[256];
    va_list args;
    va_start(args, format);
    vsprintf_s(reason, format, args);
    va_end(args);
    EnterCriticalSection(&s_lock);
    if (s_dumps < MAX_DUMPS) {
        s_dumps++;
        fprintf(s_log, "\n%11.1f ===== %s (%d events before)\n", CameraDiagNow(), reason, s_ringCount);
        for (int i = 0; i < s_ringCount; i++) {
            int at = (s_ringNext - s_ringCount + i + RING) % RING;
            fprintf(s_log, "%11.1f %s\n", s_ringTime[at], s_ring[at]);
        }
        if (s_dumps == MAX_DUMPS) {
            fputs("dump limit reached\n", s_log);
        }
        fflush(s_log);
    }
    s_ringCount = 0;
    LeaveCriticalSection(&s_lock);
}

/// Writes one line at once, outside the events.
void CameraDiagLine(const char* format, ...) {
    if (!s_log) {
        return;
    }
    char text[256];
    va_list args;
    va_start(args, format);
    vsprintf_s(text, format, args);
    va_end(args);
    EnterCriticalSection(&s_lock);
    fprintf(s_log, "%11.1f %s\n", CameraDiagNow(), text);
    fflush(s_log);
    LeaveCriticalSection(&s_lock);
}

void InstallCameraDiag() {
    if (s_log) {
        return;
    }
    InitializeCriticalSection(&s_lock);
    QueryPerformanceFrequency(&s_frequency);
    QueryPerformanceCounter(&s_start);
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)&InstallCameraDiag, &self);
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(self, path, MAX_PATH);
    wchar_t* slash = wcsrchr(path, L'\\');
    swprintf_s(slash ? slash + 1 : path, MAX_PATH - (slash ? slash + 1 - path : 0), L"camera_diag_%lu.log", GetCurrentProcessId());
    s_log = _wfsopen(path, L"w", _SH_DENYWR);
    SYSTEMTIME t;
    GetLocalTime(&t);
    CameraDiagLine("camera diag started %04d-%02d-%02d %02d:%02d:%02d; press F12 right after the camera misbehaves", t.wYear, t.wMonth,
        t.wDay, t.wHour, t.wMinute, t.wSecond);
}
