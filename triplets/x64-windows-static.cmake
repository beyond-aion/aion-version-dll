set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE static)
set(VCPKG_LIBRARY_LINKAGE static)

# mimalloc initialises itself from a TLS callback while Windows loads version.dll, and on Windows 7 its default random seed
# (LoadLibrary of bcrypt.dll + BCryptGenRandom) hangs the 64-bit client there, before DllMain runs. RtlGenRandom has no such problem.
if(PORT STREQUAL "mimalloc")
    set(VCPKG_C_FLAGS "/DMI_USE_RTLGENRANDOM")
    set(VCPKG_CXX_FLAGS "/DMI_USE_RTLGENRANDOM")
endif()
