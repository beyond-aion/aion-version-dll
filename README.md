# Aion-Version-Dll
Aion No-IP and Windows 10/11 camera fix.

Features:
- Allows the game client to connect to non-official game server IPs (prevents the error message "No game server is available to the authorization server (6)").
- Fixes the camera movement issue on Windows 10/11 (introduced by the Fall Creators Update 2017).
- Enables all graphics options sliders (shadows, water quality, etc) which are otherwise disabled at high resolutions.
- [DXVK support](https://github.com/doitsujin/dxvk)

## Building
This project depends on [MS Detours](https://github.com/Microsoft/Detours). Since it's served via NuGet, you should be able to build the project straight away.

## Installation
1. Copy each `version.dll` to the respective `bin32` or `bin64` folder within the game root directory.
2. **Optional:** If you want to use DXVK, copy `d3d9.dll` from the [latest DXVK release](https://github.com/doitsujin/dxvk/releases) to the same folders.  
DXVK can be tweaked via an optional config file. See below for recommended settings.

## ⚙️ Recommended DXVK Configuration (optional)
Create a file named `dxvk.conf` in the **game root directory** (NOT `bin32/bin64`):
```ini
# Enable FPS counter (different from the one the game client provides)
# dxvk.hud = fps

# Allow exclusive full-screen mode
dxvk.allowFse = True

# Performance
d3d9.cachedDynamicBuffers = true
d3d9.deferSurfaceCreation = true

# Quality
d3d9.samplerAnisotropy = 16

# Compatibility / shader fixes
d3d9.forceSamplerTypeSpecConstants = true
```
