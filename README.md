# Aion-Version-Dll
Aion No-IP and Windows 10 camera fix.

Features:
- Allows the game client to connect to non-official game server IPs (prevents the error message "No game server is available to the authorization server (6)").
- Fixes the camera movement issue on Windows 10 (introduced by the Fall Creators Update 2017) without needing to run a separate program.
- Enables all graphics options sliders (shadows, water quality, etc) which are otherwise disabled at high resolutions.

## Building
To build this project you need to add [MS Detours](https://github.com/Microsoft/Detours) to the include path.

### Setting up Detours
In case you already built Detours you can skip this step.  
Visual Studio comes bundled with `nmake` which we need to create the required x86 and x64 detours.lib files.
First open a command prompt in your Detours folder. Then you need to start a Visual Studio Developer Command Prompt by typing `<Visual Studio path>\VC\Auxilliary\Build\vcvars64.bat`. Now you can enter `nmake` and Detours will be built for 64 bit. To build for 32 bit you can run `vcvars32.bat`.  
If you're having trouble please consult the [MSVC toolset documentation](https://docs.microsoft.com/en-us/cpp/build/building-on-the-command-line?view=vs-2019#developer_command_file_locations).

### Including Detours
The project already references a compile time variable `DETOURS_PATH` which just needs to be set. For that you open the properties manager in Visual Studio and create a property (or "Macro" in MS terms) called `DETOURS_PATH` with the corresponding path. You need to set this for all Release configurations.  
Now you should be able to build the project.

## Installation
To install, copy each version.dll to the respective bin32 or bin64 folder under the Aion client root.

Note: Existing patch dlls should be removed from the bin32/bin64 directories as they may have conflicting functionality.
- Remove: dbghelp.dll
- Remove: d3d8thk.dll
- Remove: d3dx9_38.dll

Tested with the Aion 4.8.15.107 game client on Windows 7 and Windows 10.