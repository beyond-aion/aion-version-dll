# Aion-Version-Dll
Fixes and optional mods for the Aion game client, loaded as `version.dll` next to the client.

## Fixes
The DLL automatically applies fixes for the following problems:
<details>
<summary><b>Game crashes</b></summary>

- the game client no longer crashes on systems with 32 or more logical processors
- 5.x clients no longer crash with a XignCode error (disables XignCode)
- 64-bit clients no longer crash on Windows 11 24H2 and later
- 64-bit clients no longer crash now and then when another player comes into sight
- 64-bit clients no longer crash on systems whose video driver reserves a large part of the address space (AMD with DXVK)
</details>

<details>
<summary><b>Connection problems</b></summary>

- connections to unofficial game server IPs are no longer blocked with the message `No game server is available to the authorization server (6)`
</details>

<details>
<summary><b>Camera movement</b></summary>

- the camera no longer stutters or hangs when moving it via the mouse on Windows 10 (since version 1709) and 11
- camera movement is no longer blocked when using screen capture software such as Bandicam
</details>

<details>
<summary><b>Graphics issues</b></summary>

- no black flickering with the High Quality engine (Nvidia, and AMD with DXVK)
- graphics option sliders are no longer disabled at high resolutions
- no stretched picture or misplaced cursor at resolutions exceeding 2560 pixels wide or 1920 pixels high
- no blurred picture on screens scaled above 100 %
- no straight-edged cut-off light from lamps and fires that moves with the camera on 64-bit clients using the High Quality engine 
</details>

<details>
<summary><b>PrintScreen</b></summary>

- the `Print Screen` key no longer opens the Snipping Tool over the game's own screenshot
</details>

<details>
<summary><b>Chat</b></summary>

- `[color:…]` tags in 64-bit 4.6 clients are no longer truncated
- `Chat.log` is written in UTF-8 with milliseconds, without question marks for letters outside the keyboard layout or lines cut at 255 bytes
</details>

## Performance
Loads [DXVK](https://github.com/doitsujin/dxvk) if present, which can improve performance and frame times.

## Client mods (64-bit only)
Optional additions to the game client, which can be enabled via `mods.ini`:
- **Chat time:** displays message times in the chat window, as 5.x clients already support.
- **Ping overlay:** displays the round trip time to the game server in the top left corner of the screen.
- **Anti AFK:** no disconnect after 60 minutes without input, and none 26 hours after logging in.
- **Macros:** raise the macro limit beyond the client's default (the game server must also support it).
- **Buffs:** displays up to 64 buffs and debuffs each, in a configurable layout.
- **Stats:** displays attack, casting and movement speed stats in the character window with as many decimals as they have (up to three).
- **Quest targets:** displays quest icons in front of the names of monsters that a quest step needs, as 5.x clients already do. On 4.6, gatherable objects also get the green glow later clients show.
- **Multi client:** allows running more than two clients simultaneously.
- **Skip agreement:** skips the user agreement on login.
- **Faster loading:** different settings to speed up client start and exit times.
- **UI scale (5.x):** different settings to resolve UI scaling issues at high resolutions.

The mods find their places in the client by content rather than by fixed addresses, so they work with different client versions (4.6, 4.8 and 5.8 have been verified). A mod that finds nothing to change simply stays off, which `mods.log` next to `version.dll` shows, together with any problems in `mods.ini`. When the client crashes, `mods.log` also gets the crash: where it happened, the registers and the calls that led there.

## Building
This project depends on [MS Detours](https://github.com/Microsoft/Detours), [libdeflate](https://github.com/ebiggers/libdeflate) and [mimalloc](https://github.com/microsoft/mimalloc), which [vcpkg](https://vcpkg.io) fetches and builds as static libraries on the first build, as listed in `vcpkg.json`. Visual Studio 2026 includes vcpkg, so the project builds straight away; another vcpkg is used when `VCPKG_ROOT` points to it. The 64-bit build copies `mods.ini` next to `version.dll`.

## Installation
1. Copy each `version.dll` to the respective `bin32` or `bin64` folder within the game root directory.
2. **Optional:** If you want to enable any mods, copy `mods.ini` to the `bin64` folder and edit the file. Mods are only supported for the 64 bit client.
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
