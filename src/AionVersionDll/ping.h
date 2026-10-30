#ifndef AION_MODS_PING
#define AION_MODS_PING

/// Round trip time in milliseconds for display, or -1 while there is nothing to show.
int GetDisplayedPing();

/// Whether the ping overlay is currently visible.
bool IsPingOverlayVisible();

#endif
