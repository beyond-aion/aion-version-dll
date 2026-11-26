#ifndef AION_MOD_FAST_START
#define AION_MOD_FAST_START
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// Fast start is split by what it speeds up: mod_fast_start.cpp the start of the process and the installation, *_crysystem.cpp what
// CrySystem does while it opens the .pak archives, *_runtime.cpp the string functions of the runtime, *_tables.cpp the loading of the
// game tables by Game.dll. These are what one part calls in another.

/// Once CrySystem is loaded (mod_fast_start_crysystem.cpp).
void FastStartOnCrySystem(HMODULE crySystem);

/// The runtime's string functions, once CrySystem has loaded the runtime (mod_fast_start_runtime.cpp).
void FastStartHookRuntime();

#endif
