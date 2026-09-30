# Aion-Version-Dll
Aion No-IP and Windows 10/11 fix.

Features:
- Allows the game client to connect to non-official game server IPs (prevents the error message "No game server is available to the authorization server (6)"). 5.x clients are supported as well.
- Fixes the camera movement issue on Windows 10/11 (introduced by the Fall Creators Update 2017).
- Fixes an issue with launching the 64-bit client on Windows 11 24H2 and later versions.
- Fixes an issue with launching the game client on systems with 32 or more logical processors.
- Fixes black flickering that occurs when using the game's High Quality graphics engine on Nvidia graphics cards and similar issues on AMD cards when using DXVK on Windows.
- Enables all graphics options sliders (shadows, water quality, etc) which are otherwise disabled at high resolutions.
- Starts 5.x clients without XIGNCODE when launched with `-disable-xigncode`.
- [DXVK support](https://github.com/doitsujin/dxvk) - DXVK can improve performance and frame times.

## Client mods (64-bit)
Optional additions to the game client, each of them can be turned off in `mods.ini`:
- **Chat time:** a grey `(HH:MM:SS)` in front of every chat message. 5.x clients have this built in (chat tab settings), so the mod stays off there.
- **Ping:** the round trip time to the game server, in the style of the DXVK HUD and right below its frame rate. Drag it with the left mouse button to move it. It is measured with the packet of the `/ping` command every 3 seconds while in the world, and answers to it never reach the chat.
- **Anti AFK:** no disconnect after 60 minutes without input, and none 26 hours after logging in.
- **Macros:** more macros than the client allows (12 in 4.x, 24 in 5.x). The game server has to accept the higher macro slots too.
- **Stats:** attack, casting and movement speed in the character window with as many decimals as they have (up to three).
- **Quest targets (4.6, 4.8):** monsters a quest in progress needs (to kill or to loot) get the icon of the quest kind in front of their name, the same one the quest window shows, as 5.x clients do by themselves, and on 4.6 gatherable objects it needs get the green glow later clients show. The client's own quest monster data decides which ones.
- **UI scale (5.x):** the UI scale option goes past 130 %, up to the screen size relative to 1280x960 (225 % on 3840x2160), and large fonts no longer lose the tails of letters like g and y.

The mods find their places in the client by content rather than by fixed addresses, so they work with the 4.6, 4.8 and 5.8 clients. A mod that finds nothing to change simply stays off, which `mods.log` next to `version.dll` shows, together with any problems in `mods.ini`.

The ping text uses the font of the [DXVK](https://github.com/doitsujin/dxvk) HUD (zlib/libpng license, see `dxvk_hud_font.h`), so it looks the same as the DXVK frame rate next to it.

## Building
This project depends on [MS Detours](https://github.com/Microsoft/Detours). Since it's served via NuGet, you should be able to build the project straight away. The 64-bit build copies `mods.ini` next to `version.dll`.

## Installation
1. Copy each `version.dll` to the respective `bin32` or `bin64` folder within the game root directory, and `mods.ini` next to the 64-bit one.
2. **Optional:** If you want to use DXVK, copy `d3d9.dll` from the [latest DXVK release](https://github.com/doitsujin/dxvk/releases) to the same folders and make sure you're using the latest graphics driver. If your graphics card is incompatible because it is too old, you can try the [latest DXVK-Sarek release](https://github.com/pythonlover02/DXVK-Sarek/releases/latest) instead.  
DXVK can be tweaked via an optional config file. See below for recommended settings.

## ⚙️ Recommended DXVK Configuration (optional)
Create a file named `dxvk.conf` in the **game root directory** (NOT `bin32/bin64`). The options below are checked against DXVK 3.1.1.
```ini
# Enable FPS counter (different from the one the game client provides)
# dxvk.hud = fps

# Allow exclusive full-screen mode (set to False if you encounter problems such as a missing cursor)
dxvk.allowFse = True

# Quality
d3d9.samplerAnisotropy = 16

# Compatibility / shader fixes
d3d9.forceSamplerTypeSpecConstants = True
# Create the Vulkan surface on the first frame (helps if the window stays black)
d3d9.deferSurfaceCreation = True
```
`d3d9.cachedDynamicBuffers` from older versions of this guide no longer exists in DXVK. Its successor `d3d9.cachedWriteOnlyBuffers` only helps applications that read such buffers back on the CPU and can cost GPU performance, so it is best left off. The DXVK defaults are the fastest choice for everything else (for example, shader compilation already uses all CPU cores). You can check the DXVK documentation for more info about all possible config values.
