# Aion-Version-Dll
Fixes and optional mods for the **64-bit** Aion game client (`bin64`), loaded as `version.dll` next to the client. 32-bit clients (`bin32`) are no longer supported, see [32-bit clients](#32-bit-clients).

## Fixes
The DLL automatically applies fixes for the following problems:
<details>
<summary><b>Game crashes</b></summary>

- the game client no longer crashes on systems with 32 or more logical processors
- 5.x clients no longer crash with a XignCode error (disables XignCode)
- the client no longer crashes on Windows 11 24H2 and later
- the client no longer crashes now and then when another player comes into sight
- the client no longer crashes on systems whose video driver reserves a large part of the address space (AMD with DXVK)
</details>

<details>
<summary><b>Connection problems</b></summary>

- connections to unofficial game server IPs are no longer blocked with the message `No game server is available to the authorization server (6)`
</details>

<details>
<summary><b>Camera movement</b></summary>

- the camera no longer stutters or hangs when moving it via the mouse on Windows 10 (since version 1709) and 11
- camera movement is no longer blocked when using screen capture software such as Bandicam
- the camera follows the Windows "Enhance pointer precision" setting, as in the original client
</details>

<details>
<summary><b>Graphics issues</b></summary>

- no black flickering with the High Quality engine (Nvidia, and AMD with DXVK)
- graphics option sliders are no longer disabled at high resolutions
- no stretched picture or misplaced cursor at resolutions exceeding 2560 pixels wide or 1920 pixels high
- no blurred picture on screens scaled above 100 %
- no straight-edged cut-off light from lamps and fires that moves with the camera when using the High Quality engine
- no walls that go dark and back with a slight turn of the camera, as at the Gelkmaros fortress, when using the High Quality engine
- no straight seams in the sky that water and other shiny surfaces reflect
- no pale straight-edged slabs that come and go on the water of ponds lit from below, as in Cygnea, Kaldor and Enshar
- the frame rate is no longer capped at 60 (120 in 5.x clients) by default; a `g_maxfps` set in `system.cfg` still applies unless the loaded DXVK has a frame rate limit set in `dxvk.conf`. DXVK then controls the frame rate while the value of `g_maxfps` is preserved.
</details>

<details>
<summary><b>Pet decoration</b></summary>

- 4.8 clients now show the decoration that was chosen when adopting the pet
</details>

<details>
<summary><b>Text files</b></summary>

- XML and HTML files of the game data in UTF-8 no longer show garbled letters
</details>

<details>
<summary><b>PrintScreen</b></summary>

- the `Print Screen` key no longer opens the Snipping Tool over the game's own screenshot, and puts that screenshot on the clipboard
- taking a screenshot no longer freezes the game: the picture is encoded and written in the background
</details>

<details>
<summary><b>Chat</b></summary>

- `[color:…]` tags in 4.6 clients are no longer truncated
- `Chat.log` is written in UTF-8 with milliseconds, without question marks for letters outside the keyboard layout or lines cut at 255 bytes
</details>

## Performance
Loads [DXVK](https://github.com/doitsujin/dxvk) if present, which can improve performance and frame times.

## Client mods
Optional additions to the game client, which can be enabled via `mods.ini`:
- **Chat time:** displays message times in the chat window, as 5.x clients already support.
- **Ping overlay:** displays the round trip time to the game server in the top left corner of the screen, below the DXVK frame rate; drag it with the left mouse button to move it.
- **Anti AFK:** no disconnect after 60 minutes without input, and none 26 hours after logging in.
- **Macros:** raise the macro limit beyond the client's default (the game server must also support it).
- **Buffs:** displays up to 64 buffs and debuffs each, in a configurable layout.
- **Stats:** displays attack, casting and movement speed stats in the character window with as many decimals as they have (up to three).
- **Quest targets:** displays quest icons in front of the names of monsters that a quest step needs, as 5.x clients already do. On 4.6, gatherable objects also get the green glow later clients show.
- **DXVK switch:** `[Graphics] DXVK=0` makes the client use the Direct3D 9 of Windows even when a DXVK `d3d9.dll` lies next to it, so the two can be compared without moving files.
- **Move reports (`[MoveReports]`):** the game server checks skill range, area hits and line of sight along the path the client reports, and the client's reports cut corners by up to a meter or two. The mod reports the character's movement sooner when it strays from the last reported direction (a wall, a pillar, a turn), so the reported path stays within 0.5 m of the real one; running straight is not affected. Off by default, `Enabled=1` turns it on.
- **Others' position (`[OthersPosition]`):** puts other characters at the position their move packets report once they lag a quarter of a second behind it (the client waits half a second, 5.x a whole one), in the air too, so you aim at where the server has them. It changes only what you see. Off by default, `Enabled=1` turns it on.
- **Multi client:** allows running more than two clients simultaneously.
- **Skip agreement:** skips the user agreement on login.
- **Quick destroy:** asks the usual yes-or-no question for the items that otherwise want a word typed before them.
- **Faster loading (`[Performance] LoadingMode`):** speeds up client start and exit. 4.x clients also load the large tables (items, skills, NPCs) from dumps kept in `Data/Dump` (about 200 MB), as 5.x clients do by themselves; they are written anew when a `.pak` file of the game data changes or the client did not quit normally. `LoadingMode=2` also skips the logo before the login screen and the picture after quitting.
- **Mimalloc (`[Performance] Mimalloc`):** uses [mimalloc](https://github.com/microsoft/mimalloc) in place of the game's memory allocators, which speeds up loading and can improve performance overall, at the cost of 600 MB to 1 GB more memory.
- **Animation level of detail (`[Performance] AnimationLod`):** animates characters farther than this many meters from your character every second frame, twice as far every fourth, and those off the screen or far from the camera every eighth, with their skinning; your own character and those within that distance of it always every frame, however far the camera is. A crowd costs noticeably less.
- **UI scale (5.x):** different settings to resolve UI scaling issues at high resolutions.
- **Plain system.cfg:** reads and saves `system.cfg` in plain text, so it can be edited. A client without the mod ignores the settings of such a file and starts with its defaults.

The mods find their places in the client by content rather than by fixed addresses, so they work with different client versions (3.9, 4.6, 4.8, 5.6 and 5.8 have been verified). A fix or mod that finds nothing to change simply stays off, which `version.log` next to `version.dll` shows with `[Logging] Enabled=1`, together with any problems in `mods.ini`. When the client crashes, `version.log` also gets the crash: where it happened, the registers and the calls that led there.

## Checking a client without starting it
`modcheck.exe`, built from the same sources, tells what the fixes and mods find in a client from images of its modules:
1. Start the client once with only `[Logging] Enabled=1` and `[Debug] DumpModules=1` in `mods.ini` (every mod off). `version.dll` writes the images of `aion.bin`, `Game.dll`, `CrySystem.dll`, `CryFont.dll`, `XRenderD3D9.dll`, `Cry3DEngine.dll` and `CryAnimation.dll` to `dumps\` next to it, as they are once each module unpacked itself and before anything changes them.
2. Run `modcheck <dumps folder>` with the `mods.ini` to check next to `modcheck.exe`. It installs everything as a dry run against the images, counting the changes and hooks without making them, and writes `version.log` with the same summary as a start of the client, apart from what waits for the client's window or device. The exit code is nonzero if any of the seven required images is missing or could not be mapped.

`tests/modcheck46.txt`, `modcheck48.txt`, `modcheck49.txt`, `modcheck56.txt` and `modcheck58.txt` hold the summaries for 4.6, 4.8, 4.9, 5.6 and 5.8 with every mod on; a change to a search that finds another place shows as a change of its line there.

The other keys of `[Debug]` measure the client and write to `version.log` as well: `Profile` samples the start, `ProfileWorld` the frame in the world, with call stacks, and `FrameStat` writes the frame times; `mods.ini` describes them.

## Building
This project depends on [MS Detours](https://github.com/Microsoft/Detours), [libdeflate](https://github.com/ebiggers/libdeflate) and [mimalloc](https://github.com/microsoft/mimalloc), which [vcpkg](https://vcpkg.io) fetches and builds as static libraries on the first build, as listed in `vcpkg.json`. Visual Studio 2026 includes vcpkg, so the project builds straight away; another vcpkg is used when `VCPKG_ROOT` points to it. The build copies `mods.ini` next to `version.dll`. Only the 64-bit client is supported: the 32-bit build was dropped, as it carried a part of the fixes and none of the mods (see [32-bit clients](#32-bit-clients)). The overlay triplet in `triplets` builds mimalloc with `MI_USE_RTLGENRANDOM`: its default random seed, `BCryptGenRandom`, hangs the 64-bit client on Windows 7 while Windows loads `version.dll`.

## Installation
1. Copy `version.dll` to the `bin64` folder within the game root directory.
2. **Optional:** If you want to enable any mods, copy `mods.ini` to the `bin64` folder and edit the file.
3. **Optional:** If you want to use DXVK, copy `d3d9.dll` (x64) from the [latest DXVK release](https://github.com/doitsujin/dxvk/releases) to the same folder and make sure you're using the latest graphics driver. If your graphics card is incompatible because it is too old, you can try the [latest DXVK-Sarek release](https://github.com/pythonlover02/DXVK-Sarek/releases/latest) instead.  
DXVK can be tweaked via an optional config file. See below for recommended settings.

## 32-bit clients
This version supports only the 64-bit client (`bin64`). If you need `version.dll` for a 32-bit client (`bin32`), take it from [v2.0.1](https://github.com/beyond-aion/aion-version-dll/releases/tag/v2.0.1), the last release with a 32-bit build: copy `bin32/version.dll` from its `version-dll.zip` to the `bin32` folder of the game. It has the fixes of that release that apply to 32-bit clients and no mods, and gets no further updates.

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
