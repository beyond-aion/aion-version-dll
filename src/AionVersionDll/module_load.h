#ifndef AION_MODULE_LOAD
#define AION_MODULE_LOAD
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

typedef void (*Log_t)(const char* format, ...);

/// Where the module watch and the fixes using it report, nullptr for nowhere. The mods point it at mods.log.
extern Log_t g_moduleLog;

#define MODULE_LOG(...) (g_moduleLog ? g_moduleLog(__VA_ARGS__) : (void)0)

/// Calls install with a DLL of the given name when the loader loads it: once its entry point has returned, as some clients pack
/// their DLLs and unpack them there, or before the entry point runs. Up to 4 DLLs.
void OnModuleLoad(const wchar_t* name, void (*install)(HMODULE module), bool afterEntryPoint);

/// The size of the module's image in memory.
size_t ModuleImageSize(HMODULE module);

#endif
