# Aion-Version-Dll
Aion No-IP and Windows 10 camera fix.

Features:
- Allows the game client to connect to non-official game server IPs (prevents the error message "No game server is available to the authorization server (6)").
- Fixes the camera movement issue on Windows 10 (introduced by the Fall Creators Update 2017) without needing to run a separate program.
- Enables all graphics options sliders (shadows, water quality, etc) which are otherwise disabled at high resolutions.

## Building
This project depends on [MS Detours](https://github.com/Microsoft/Detours). Since it's served via NuGet, you should be able to build the project straight away.

## Installation
To install, copy each version.dll to the respective bin32 or bin64 folder under the Aion client root.

Note: Existing patch dlls should be removed from the bin32/bin64 directories as they may have conflicting functionality.
- Remove: dbghelp.dll
- Remove: d3d8thk.dll
- Remove: d3dx9_38.dll

Tested with the Aion 4.8.15.107 game client on Windows 7 and Windows 10.