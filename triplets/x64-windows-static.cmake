set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE static)
set(VCPKG_LIBRARY_LINKAGE static)

# mimalloc initialises itself from a TLS callback while Windows loads version.dll, and on Windows 7 its default random seed
# (LoadLibrary of bcrypt.dll + BCryptGenRandom) hangs the 64-bit client there, before DllMain runs. RtlGenRandom has no such problem.
if(PORT STREQUAL "mimalloc")
    set(VCPKG_C_FLAGS "/DMI_USE_RTLGENRANDOM")
    set(VCPKG_CXX_FLAGS "/DMI_USE_RTLGENRANDOM")
endif()

# libdeflate and mimalloc sit on the loading hot path; built with /GL, their IL reaches the final /LTCG link, which
# makes zlib compression 3-8% faster (1 MB blocks: ~7-8%) and mimalloc's small allocations 3-8% faster, while
# decompression stays the same and the DLL gets about 10 KB smaller. Measured on 4.8 game data, 27.11.
if(PORT STREQUAL "mimalloc" OR PORT STREQUAL "libdeflate")
    set(VCPKG_C_FLAGS_RELEASE "${VCPKG_C_FLAGS_RELEASE} /GL")
    set(VCPKG_CXX_FLAGS_RELEASE "${VCPKG_CXX_FLAGS_RELEASE} /GL")
endif()
