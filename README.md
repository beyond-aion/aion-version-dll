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
<summary><b>Pet decoration</b></summary>

- 4.8 64-bit clients now show the decoration that was chosen when adopting the pet
</details>

<details>
<summary><b>Text files</b></summary>

- XML and HTML files of the game data in UTF-8 no longer show garbled letters on 64-bit clients
</details>

<details>
<summary><b>PrintScreen</b></summary>

- the `Print Screen` key no longer opens the Snipping Tool over the game's own screenshot, and puts that screenshot on the clipboard
- taking a screenshot no longer freezes the game: the client used to open every screenshot taken before to find a free name, a matter of seconds on a hard disk the first time, and to encode the picture in the frame; it is now encoded and written in the background
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
- **Move reports (4.x):** reports the character's movement sooner when it strays from the direction it last reported, as when it runs into a wall or along a pillar, or turns, so the game server sees the path around corners and obstacles. By default (`Window=50`, `Distance=0.5`, `TurnAngle=15`) that is one report per 0.06 s instead of 0.23 s there, and the reported path stays within 0.5 m of the real one instead of 1 m; running in a straight line is not affected. The client's own values are 150, 1 and 30. The same section puts other characters at the position their move packets report once they drift a quarter of a second of their speed from it (`OthersSnapTime=0.25`, the client waits for half a second, 3-4 m running), and corrects their height in the air too (`OthersHeightInAir=1`), so one the client let fall off a ledge comes back up.
- **Multi client:** allows running more than two clients simultaneously.
- **Skip agreement:** skips the user agreement on login.
- **Quick destroy:** asks the usual yes-or-no question for the items that otherwise want a word typed before them.
- **Faster loading:** different settings to speed up client start and exit times.
- **UI scale (5.x):** different settings to resolve UI scaling issues at high resolutions.
- **Plain system.cfg:** reads and saves `system.cfg` in plain text, so it can be edited. A client without the mod ignores the settings of such a file and starts with its defaults.

The mods find their places in the client by content rather than by fixed addresses, so they work with different client versions (3.9, 4.6, 4.8 and 5.8 have been verified). A fix or mod that finds nothing to change simply stays off, which `version.log` next to `version.dll` shows, together with any problems in `mods.ini`. When the client crashes, `version.log` also gets the crash: where it happened, the registers and the calls that led there.

## Checking a client without starting it
`modcheck.exe`, built from the same sources, tells what the fixes and mods find in a client from images of its modules:
1. Start the client once with only `[Logging] Enabled=1` and `[Debug] DumpModules=1` in `mods.ini` (every mod off). `version.dll` writes the images of `Game.dll`, `CrySystem.dll`, `CryFont.dll` and `XRenderD3D9.dll` to `dumps\` next to it, as they are once each module unpacked itself and before anything changes them.
2. Run `modcheck <dumps folder>` with the `mods.ini` to check next to `modcheck.exe`. It installs everything as a dry run against the images, counting the changes and hooks without making them, and writes `version.log` with the same summary as a start of the client, apart from what waits for the client's window or device.

`tests/modcheck48.txt` and `tests/modcheck58.txt` hold the summaries for 4.8 and 5.8 with every mod on; a change to a search that finds another place shows as a change of its line there.

## Building
This project depends on [MS Detours](https://github.com/Microsoft/Detours), [libdeflate](https://github.com/ebiggers/libdeflate) and [mimalloc](https://github.com/microsoft/mimalloc), which [vcpkg](https://vcpkg.io) fetches and builds as static libraries on the first build, as listed in `vcpkg.json`. Visual Studio 2026 includes vcpkg, so the project builds straight away; another vcpkg is used when `VCPKG_ROOT` points to it. The build copies `mods.ini` next to `version.dll`. Only the 64-bit client is supported: the 32-bit build was dropped, as it carried a part of the fixes and none of the mods. The overlay triplet in `triplets` builds mimalloc with `MI_USE_RTLGENRANDOM`: its default random seed, `BCryptGenRandom`, hangs the 64-bit client on Windows 7 while Windows loads `version.dll`.

## Installation
1. Copy `version.dll` to the `bin64` folder within the game root directory.
2. **Optional:** If you want to enable any mods, copy `mods.ini` to the `bin64` folder and edit the file.
3. **Optional:** If you want to use DXVK, copy `d3d9.dll` from the [latest DXVK release](https://github.com/doitsujin/dxvk/releases) to the same folders and make sure you're using the latest graphics driver. If your graphics card is incompatible because it is too old, you can try the [latest DXVK-Sarek release](https://github.com/pythonlover02/DXVK-Sarek/releases/latest) instead.  
DXVK can be tweaked via an optional config file. See below for recommended settings.

## ⚙️ Recommended DXVK Configuration (optional)
Create a file named `dxvk.conf` in the **game root directory** (NOT `bin64`). The options below are checked against DXVK 3.1.1.
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

# Turn on only if single frames take 30-40 ms near certain animated models. The client's animation code reads their vertex
# buffers back on the CPU, which on some systems is very slow from the uncached memory DXVK puts them in. It does not make
# frames shorter otherwise, and made them slightly longer in a crowd of 300 monsters
# d3d9.cachedWriteOnlyBuffers = True
```
You can check the DXVK documentation for more info about all possible config values.
