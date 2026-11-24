#include "install_report.h"
#include "mods_log.h"
#include <detours/detours.h>
#include <stdio.h>
#include <string.h>

// A fixed table, as the fixes and mods install under the loader lock, where allocating is best avoided.
static constexpr int MAX_ENTRIES = 96;

struct Entry {
    const char* id;
    bool group;
    bool dirty; // changed since it was last written
    bool ended; // a scope of it ended
    bool waiting; // waits for a module to load
    InstallStatus reported;
    int writes;
    int failedWrites;
    int hooks;
    int failedHooks;
    int searches;
    int misses;
    int ambiguous;
    size_t mostMatches;
    const void* firstChange;
    LONGLONG ticks;
};

static Entry s_entries[MAX_ENTRIES];
static int s_count = 0;
static SRWLOCK s_lock = SRWLOCK_INIT;
static thread_local int t_current = -1;
static bool s_finalSummaryDone = false;
static thread_local bool t_locating = false;
// the entries whose hooks AttachHook counted in the open Detours transaction of this thread, one per hook, taken back if the commit fails
static thread_local int t_pendingHooks[64];
static thread_local int t_pendingCount = 0;

LocatePhase::LocatePhase() : m_outer(t_locating) {
    t_locating = true;
}

LocatePhase::~LocatePhase() {
    t_locating = m_outer;
}

/// A change during a search breaks the split of search and change, so it is logged.
static bool ChangedWhileLocating(const void* at) {
    if (!t_locating) {
        return false;
    }
    ModsLog("change at %p during a search, which must only read", at);
    return true;
}

static LONGLONG Now() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

static double Milliseconds(LONGLONG ticks) {
    static LONGLONG frequency = 0;
    if (!frequency) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        frequency = f.QuadPart;
    }
    return ticks * 1000.0 / frequency;
}

/// The entry of the id, added if new; -1 when the table is full. Called with the lock held.
static int EntryOf(const char* id, bool group) {
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_entries[i].id, id) == 0) {
            return i;
        }
    }
    if (s_count == MAX_ENTRIES) {
        return -1;
    }
    Entry& e = s_entries[s_count];
    e = {};
    e.id = id;
    e.group = group;
    return s_count++;
}

/// Runs change on the current entry under the lock; outside any scope the counts go nowhere.
template <typename Change>
static void UpdateCurrent(Change change) {
    if (t_current < 0) {
        return;
    }
    AcquireSRWLockExclusive(&s_lock);
    Entry& e = s_entries[t_current];
    change(e);
    e.dirty = true;
    ReleaseSRWLockExclusive(&s_lock);
}

/// How much a reported status tells, the most telling one winning when several are reported.
static int Weight(InstallStatus status) {
    switch (status) {
        case InstallStatus::PatchFailed: return 8;
        case InstallStatus::SkippedDependency: return 7;
        case InstallStatus::Disabled: return 6;
        case InstallStatus::Ambiguous: return 5;
        case InstallStatus::NotFound: return 4;
        case InstallStatus::Unsupported: return 4;
        case InstallStatus::Applied: return 3;
        case InstallStatus::NotApplicable: return 2;
        case InstallStatus::Unchanged: return 1;
        default: return 0;
    }
}

static void Report(Entry& e, InstallStatus status) {
    if (Weight(status) > Weight(e.reported)) {
        e.reported = status;
    }
}

static InstallStatus StatusOf(const Entry& e) {
    if (e.failedWrites || e.failedHooks || e.reported == InstallStatus::PatchFailed) {
        return InstallStatus::PatchFailed;
    }
    if (e.reported == InstallStatus::Disabled || e.reported == InstallStatus::SkippedDependency) {
        return e.reported;
    }
    if (e.writes || e.hooks) {
        return InstallStatus::Applied;
    }
    if (e.reported != InstallStatus::None) {
        return e.reported;
    }
    if (e.ambiguous) {
        return InstallStatus::Ambiguous;
    }
    if (e.misses) {
        return InstallStatus::NotFound;
    }
    return e.waiting && !e.searches ? InstallStatus::Waiting : InstallStatus::Unchanged;
}

static const char* NameOf(InstallStatus status) {
    switch (status) {
        case InstallStatus::Applied: return "applied";
        case InstallStatus::NotFound: return "not_found";
        case InstallStatus::Ambiguous: return "ambiguous";
        case InstallStatus::NotApplicable: return "not_applicable";
        case InstallStatus::Disabled: return "disabled";
        case InstallStatus::PatchFailed: return "patch_failed";
        case InstallStatus::SkippedDependency: return "skipped_dependency";
        case InstallStatus::Unchanged: return "unchanged";
        case InstallStatus::Waiting: return "waiting";
        case InstallStatus::Unsupported: return "unsupported";
        default: return "none";
    }
}

// the images modcheck put in place of modules the loader did not load
struct NamedImage {
    const wchar_t* name;
    const BYTE* base;
    size_t size;
};
static NamedImage s_images[8];
static int s_imageCount = 0;

static HMODULE s_clientModule = nullptr;

HMODULE ClientModule() {
    return s_clientModule ? s_clientModule : GetModuleHandleW(nullptr);
}

void SetClientModule(HMODULE module) {
    s_clientModule = module;
}

void NameImage(const wchar_t* name, HMODULE base, size_t size) {
    if (s_imageCount < (int)(sizeof(s_images) / sizeof(s_images[0]))) {
        s_images[s_imageCount++] = { name, (const BYTE*)base, size };
    }
}

/// "Game.dll+8A80E2" for an address in a module, so that it reads the same in every run; "-" for none.
static void FormatPlace(const void* at, char* text, size_t size) {
    for (int i = 0; i < s_imageCount; i++) {
        if (at >= s_images[i].base && at < s_images[i].base + s_images[i].size) {
            sprintf_s(text, size, "%ls+%llX", s_images[i].name, (unsigned long long)((const BYTE*)at - s_images[i].base));
            return;
        }
    }
    HMODULE module;
    if (!at || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)at, &module)) {
        strcpy_s(text, size, at ? "?" : "-");
        return;
    }
    wchar_t path[MAX_PATH];
    DWORD length = GetModuleFileNameW(module, path, MAX_PATH);
    const wchar_t* name = path;
    for (DWORD i = 0; i < length; i++) {
        if (path[i] == L'\\' || path[i] == L'/') {
            name = path + i + 1;
        }
    }
    sprintf_s(text, size, "%ls+%llX", name, (unsigned long long)((const BYTE*)at - (const BYTE*)module));
}

/// Writes the entry and marks it written. Called with the lock held.
static void LogEntry(Entry& e, const char* prefix) {
    e.dirty = false;
    if (e.group) {
        ModsLog("%s%-20s %8.2f ms", prefix, e.id, Milliseconds(e.ticks));
        return;
    }
    InstallStatus status = StatusOf(e);
    char place[MAX_PATH + 24];
    FormatPlace(e.firstChange, place, sizeof(place));
    char ambiguous[32] = "";
    if (e.ambiguous) {
        sprintf_s(ambiguous, " (up to %zu matches)", e.mostMatches);
    }
    ModsLog("%s%-20s %-18s %-28s writes %d/%d, hooks %d/%d, searches %d, misses %d, ambiguous %d%s, %.2f ms", prefix, e.id, NameOf(status),
        place, e.writes, e.writes + e.failedWrites, e.hooks, e.hooks + e.failedHooks, e.searches, e.misses, e.ambiguous, ambiguous,
        Milliseconds(e.ticks));
}

InstallScope::InstallScope(const char* id, bool group) : m_parent(t_current), m_started(Now()) {
    AcquireSRWLockExclusive(&s_lock);
    m_entry = EntryOf(id, group);
    ReleaseSRWLockExclusive(&s_lock);
    if (m_entry >= 0) {
        t_current = m_entry;
    }
}

InstallScope::~InstallScope() {
    if (m_entry < 0) {
        return;
    }
    LONGLONG elapsed = Now() - m_started;
    t_current = m_parent;
    AcquireSRWLockExclusive(&s_lock);
    Entry& e = s_entries[m_entry];
    e.ticks += elapsed;
    // the first time, it goes into the summary even when it found and changed nothing; a group always does, for its time
    if (!e.ended || e.group) {
        e.ended = true;
        e.dirty = true;
    }
    // later scopes run again and again, as the camera fix with every window change, only matter when they did something
    if (e.dirty && s_finalSummaryDone && m_parent < 0 && !e.group) {
        LogEntry(e, "late    ");
    }
    ReleaseSRWLockExclusive(&s_lock);
}

void ReportStatus(InstallStatus status) {
    UpdateCurrent([&](Entry& e) { Report(e, status); });
}

void ReportStatus(const char* id, InstallStatus status) {
    AcquireSRWLockExclusive(&s_lock);
    int i = EntryOf(id, false);
    if (i >= 0) {
        Report(s_entries[i], status);
        s_entries[i].dirty = true;
    }
    ReleaseSRWLockExclusive(&s_lock);
}

void ReportWaiting() {
    UpdateCurrent([&](Entry& e) { e.waiting = true; });
}

void ReportSearch(size_t count) {
    UpdateCurrent([&](Entry& e) {
        e.searches++;
        if (count == 0) {
            e.misses++;
        } else if (count > 1) {
            e.ambiguous++;
            e.mostMatches = max(e.mostMatches, count);
        }
    });
}

void ReportWrite(const void* at, bool done) {
    done = done && !ChangedWhileLocating(at);
    UpdateCurrent([&](Entry& e) {
        if (done) {
            e.writes++;
            if (!e.firstChange) {
                e.firstChange = at;
            }
        } else {
            e.failedWrites++;
        }
    });
}

bool g_dryRun = false;

LONG AttachHook(PVOID* real, PVOID hook) {
    PVOID target = *real;
    LONG error = g_dryRun ? NO_ERROR : DetourAttach(real, hook);
    bool misplaced = ChangedWhileLocating(target);
    UpdateCurrent([&](Entry& e) {
        if (error == NO_ERROR && !misplaced) {
            e.hooks++;
            if (t_pendingCount < (int)(sizeof(t_pendingHooks) / sizeof(t_pendingHooks[0]))) {
                t_pendingHooks[t_pendingCount++] = t_current;
            }
            if (!e.firstChange) {
                e.firstChange = target;
            }
        } else {
            e.failedHooks++;
        }
    });
    return error;
}

LONG CommitHooks() {
    LONG error = DetourTransactionCommit();
    if (error != NO_ERROR) {
        AcquireSRWLockExclusive(&s_lock);
        for (int i = 0; i < t_pendingCount; i++) {
            Entry& e = s_entries[t_pendingHooks[i]];
            e.hooks--;
            e.failedHooks++;
            e.dirty = true;
        }
        ReleaseSRWLockExclusive(&s_lock);
    }
    t_pendingCount = 0;
    return error;
}

static double UptimeSeconds() {
    FILETIME created, exited, kernel, user, now;
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
        return 0;
    }
    GetSystemTimeAsFileTime(&now);  // the precise one is Windows 8+, and an import the loader of Windows 7 refuses
    ULARGE_INTEGER a = { { created.dwLowDateTime, created.dwHighDateTime } };
    ULARGE_INTEGER b = { { now.dwLowDateTime, now.dwHighDateTime } };
    return (b.QuadPart - a.QuadPart) / 1e7;
}

void LogInstallSummary(const char* when, bool final) {
    AcquireSRWLockExclusive(&s_lock);
    ModsLog("summary %s, %.3f s after the process started:", when, UptimeSeconds());
    for (int i = 0; i < s_count; i++) {
        if (s_entries[i].dirty && !s_entries[i].group) {
            LogEntry(s_entries[i], "        ");
        }
    }
    for (int i = 0; i < s_count; i++) {
        if (s_entries[i].dirty && s_entries[i].group) {
            LogEntry(s_entries[i], "time    ");
        }
    }
    if (final) {
        s_finalSummaryDone = true;
    }
    ReleaseSRWLockExclusive(&s_lock);
}

void LogClientBuild(HMODULE module, const char* name) {
    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)module;
    const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)((const BYTE*)module + dos->e_lfanew);
    char version[48] = "no version";
    HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(VS_VERSION_INFO), MAKEINTRESOURCEW(16)); // RT_VERSION
    const BYTE* data = resource ? (const BYTE*)LockResource(LoadResource(module, resource)) : nullptr;
    DWORD size = resource ? SizeofResource(module, resource) : 0;
    // VS_FIXEDFILEINFO follows the key L"VS_VERSION_INFO" and its padding
    for (DWORD i = 0; data && i + sizeof(VS_FIXEDFILEINFO) <= size && i < 0x100; i += 4) {
        const VS_FIXEDFILEINFO* info = (const VS_FIXEDFILEINFO*)(data + i);
        if (info->dwSignature == 0xFEEF04BD) {
            sprintf_s(version, "%u.%u.%u.%u", HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS), HIWORD(info->dwFileVersionLS),
                LOWORD(info->dwFileVersionLS));
            break;
        }
    }
    ModsLog("client: %s time stamp %08lX, image size %lX, version %s", name, nt->FileHeader.TimeDateStamp, nt->OptionalHeader.SizeOfImage,
        version);
}

void InstallAll(const InstallEntry* entries, size_t count, HMODULE module) {
    for (size_t i = 0; i < count; i++) {
        const InstallEntry& entry = entries[i];
        if (entry.enabled && !entry.enabled()) {
            ReportStatus(entry.id, InstallStatus::Disabled);
            continue;
        }
        InstallScope scope(entry.id);
        entry.install(module);
    }
}
