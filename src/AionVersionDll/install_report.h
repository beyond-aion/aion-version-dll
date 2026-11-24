#ifndef AION_INSTALL_REPORT
#define AION_INSTALL_REPORT
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// What each fix and mod found, changed and hooked while it installed, and how long it took, summed up in version.log one line per fix
// or mod. The searches, changes and hooks made inside an InstallScope count for it on their own, so a fix or mod only reports a status
// when the counts cannot tell it.

enum class InstallStatus : BYTE {
    /// Nothing reported, the counts decide.
    None,
    /// Changed or hooked what it looked for.
    Applied,
    /// A pattern it needs matched nowhere.
    NotFound,
    /// A pattern it needs matched more than one place.
    Ambiguous,
    /// The client was checked and needs no change, as when it lacks the feature or already does it.
    NotApplicable,
    /// Turned off in mods.ini.
    Disabled,
    /// Found its place but could not change it: the memory could not be made writable, the bytes were not the expected ones, or Detours
    /// refused the hook.
    PatchFailed,
    /// Left out because something it builds on did not install.
    SkippedDependency,
    /// Found what it looked for and left it as it was, the setting asking for what the client does anyway.
    Unchanged,
    /// Waits for the loader to load the module it changes; set by OnModuleLoad.
    Waiting,
};

/// Counts what happens until it ends for the fix or mod of the given id. Scopes nest, and the innermost one gets the counts; one of an id
/// seen before adds to it, as for a mod that installs in parts while different modules load. A group only times what it holds and is left
/// out of the summary's status lines.
class InstallScope {
public:
    explicit InstallScope(const char* id, bool group = false);
    ~InstallScope();
    InstallScope(const InstallScope&) = delete;
    InstallScope& operator=(const InstallScope&) = delete;

private:
    int m_entry;
    int m_parent;
    LONGLONG m_started;
};

/// Sets the status of the current fix or mod where the counts would tell it wrong. Of several, the most telling one stays: a failure over a
/// skip, a skip over a success.
void ReportStatus(InstallStatus status);

/// The same for the fix or mod of the given id, as for one that mods.ini turned off or whose base did not install.
void ReportStatus(const char* id, InstallStatus status);

/// Marks the current fix or mod as waiting for a module, until it searches, changes or hooks something.
void ReportWaiting();

/// A search that needed a single match and found count of them; for the searches that take the first one or all, count is 0 or 1.
void ReportSearch(size_t count);

/// A change of code or data at the given address and whether it was made.
void ReportWrite(const void* at, bool done);

/// DetourAttach that counts the hook for the current fix or mod. Must be called inside an open Detours transaction.
LONG AttachHook(PVOID* real, PVOID hook);

/// Writes one line per fix and mod that installed or changed since the last summary. After the final one, a fix or mod that does something
/// more gets a line of its own as it ends.
void LogInstallSummary(const char* when, bool final);

/// Writes the client's build: the time stamp, image size and file version of the given module.
void LogClientBuild(HMODULE module, const char* name);

#endif
