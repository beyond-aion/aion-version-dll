# Aion-Version-Dll
Fixes and optional mods for the Aion game client, loaded as `version.dll` next to the client.

Fixes:
- **Connection:** connects to non-official game server IPs (no "No game server is available to the authorization server (6)"), and 64-bit clients no longer crash now and then when another player comes into sight.
- **Start:** launches the 64-bit client on Windows 11 24H2 and later, on systems with 32 or more logical processors, 5.x clients without XignCode, and 64-bit clients no longer crash after loading on systems whose video driver reserves a large part of the address space (AMD with DXVK).
- **Camera:** turns by raw mouse input, so it no longer jumps on Windows 10 (since version 1709) and 11 and keeps working, together with the keys, while Bandicam captures the window.
- **PrintScreen:** no longer opens the Snipping Tool over the game's own screenshot.
- **Graphics:** no black flickering with the High Quality engine (Nvidia, and AMD with DXVK), all graphics sliders available at high resolutions, no stretched picture or misplaced cursor on screens larger than 2560x1920, and no blurred picture on screens scaled above 100 % (a DPI setting in the compatibility tab of `aion.bin` still comes first).
- **Text:** color tags in 64-bit 4.6 clients show their whole text, and `Chat.log` is written in UTF-8 with milliseconds, without question marks for letters outside the keyboard layout or lines cut at 255 bytes.
- [DXVK](https://github.com/doitsujin/dxvk) support, which can improve performance and frame times.

## Client mods (64-bit only)
Optional additions to the game client, all of them off until turned on in `mods.ini`:
- **Chat time:** a grey `(HH:MM)` or `(HH:MM:SS)` in front of every chat message. 5.x clients have this built in (chat tab settings), so the mod stays off there.
- **Ping:** the round trip time to the game server, in the style of the DXVK HUD and right below its frame rate. Drag it with the left mouse button to move it. It is measured with the packet of the `/ping` command while in the world, every 3 seconds by default (0.5 to 60), and answers to it never reach the chat. Its color runs from green up to 60 ms over yellow at 100 ms to red from 200 ms.
- **Anti AFK:** no disconnect after 60 minutes without input, and none 26 hours after logging in.
- **Macros:** more macros than the client allows (12 in 4.x, 24 in 5.x), as many as `Limit` says. The game server has to accept the higher macro slots too.
- **Buffs:** the own buff and debuff windows and the buff window of the target show up to 64 buffs and 64 debuffs each, in all client versions, laid out in as many columns as `OwnColumns` and `TargetColumns` say. Without the mod they show 24 buffs and 24 debuffs in 4.x, 40 buffs and 24 debuffs in 5.x. The game server decides how many effects a character has; retail servers allow 16 buffs and 20 debuffs. In 5.x, where the target of the target sits to the right of the target's buffs, wider rows move below it while it shows, as in 4.x.
- **Stats:** attack, casting and movement speed in the character window with as many decimals as they have (up to three).
- **Quest targets (4.x):** monsters a quest in progress needs (to kill or to loot) get the icon of the quest kind in front of their name, the same one the quest window shows, as 5.x clients do by themselves, and on 4.6 gatherable objects it needs get the green glow later clients show. The client's own quest monster data decides which ones.
- **Multi client:** more than two clients at once. Without it, a third client closes the other two when it starts.
- **Faster loading:** `LoadingMode=1` gets to the login screen several times sooner and closes the client window at once. The client no longer waits needlessly for a closing client, skips the DirectX diagnostic report, measures the processor clock once, starts as with `-ncg` and reads its tables, strings, .pak archives and compressed data (libdeflate in place of zlib) much faster; 5.x clients also rebuild their table dump several times faster. A second client of the same folder keeps the shader cache and the table dump. `LoadingMode=2` also skips the logos before the login screen and the picture after quitting. aion.bin itself stays unchanged.
- **mimalloc:** `Mimalloc=1` replaces the memory allocators of CrySystem with mimalloc for the whole session, which loads faster still, for 600 MB to 1 GB more memory.
- **UI scale (5.x):** the UI scale option goes past 130 %, up to the screen size relative to 1280x960 (225 % on 3840x2160), and large fonts no longer lose the tails of letters like g and y.

The mods find their places in the client by content rather than by fixed addresses, so they work with different client versions (4.6, 4.8 and 5.8 have been verified; 4.x and 5.x above stand for them). A mod that finds nothing to change simply stays off, which `mods.log` next to `version.dll` shows, together with any problems in `mods.ini`. When the client crashes, `mods.log` also gets the crash: where it happened, the registers and the calls that led there.

The ping text uses the font of the [DXVK](https://github.com/doitsujin/dxvk) HUD (zlib/libpng license, see `dxvk_hud_font.h`), so it looks the same as the DXVK frame rate next to it.

## Building
This project depends on [MS Detours](https://github.com/Microsoft/Detours), [libdeflate](https://github.com/ebiggers/libdeflate) and [mimalloc](https://github.com/microsoft/mimalloc), which [vcpkg](https://vcpkg.io) fetches and builds as static libraries on the first build, as listed in `vcpkg.json`. Visual Studio 2026 includes vcpkg, so the project builds straight away; another vcpkg is used when `VCPKG_ROOT` points to it. The 64-bit build copies `mods.ini` next to `version.dll`.

## Installation
1. Copy each `version.dll` to the respective `bin32` or `bin64` folder within the game root directory.
2. Copy `mods.ini` to the `bin64` folder. Mods are only supported for the 64 bit client.
3. **Optional:** If you want to use DXVK, copy `d3d9.dll` from the [latest DXVK release](https://github.com/doitsujin/dxvk/releases) to the same folders and make sure you're using the latest graphics driver. If your graphics card is incompatible because it is too old, you can try the [latest DXVK-Sarek release](https://github.com/pythonlover02/DXVK-Sarek/releases/latest) instead.  
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

# Only against single frames of 30-40 ms near certain animated models: the client's animation code reads vertex buffers back on
# the CPU, which on some systems is very slow from the uncached memory DXVK puts them in. Leave it off otherwise, it does not
# make frames shorter in general and made them slightly longer in a crowd of 300 monsters
# d3d9.cachedWriteOnlyBuffers = True
```
You can check the DXVK documentation for more info about all possible config values.
