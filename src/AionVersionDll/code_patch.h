#ifndef AION_CODE_PATCH
#define AION_CODE_PATCH
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <vector>

// Searching and changing the x64 code of the client's modules, for the fixes and the mods alike.

/// Finds an IDA-style byte pattern ("48 8B ?? 05") in the given module's image, returns nullptr if missing or not unique.
BYTE* FindPattern(HMODULE module, const char* pattern);
std::vector<BYTE*> FindAllPatterns(HMODULE module, const char* pattern);

/// Finds the first occurrence of exact bytes in the module's image.
BYTE* FindBytes(HMODULE module, const void* bytes, size_t length);

/// Finds the instruction starting with the given opcode bytes whose RIP-relative operand, ending the instruction, points at target.
BYTE* FindRipReference(HMODULE module, const char* opcode, int length, const void* target);

bool InModule(HMODULE module, const void* address, size_t size);

/// Finds a RIP-relative lea (any register) that loads target.
BYTE* FindLeaTo(HMODULE module, const void* target);

/// All RIP-relative leas (any register) that load target, in address order.
std::vector<BYTE*> FindAllLeasTo(HMODULE module, const void* target);

/// Finds a direct call (E8 rel32) to target.
BYTE* FindCallTo(HMODULE module, const void* target);

/// Start of the function containing address, following chained unwind entries back to the primary one.
BYTE* FunctionStart(const void* address);

/// End of the primary unwind range of the function starting at start.
BYTE* FunctionEnd(const void* start);

/// End of the whole body of the function starting at start, which can be split into chained unwind ranges: where the code of another
/// function begins, at most maxSize bytes on.
BYTE* FunctionBodyEnd(BYTE* start, size_t maxSize);

/// Whether a packer sent the calls of the module to imported functions through stubs of its own.
bool HasImportStubs(HMODULE module);

/// Points the calls a packer sent through stubs of its own back at the functions they call. Starts threads, so not under the loader lock.
void RestoreImportCalls(HMODULE module);

/// Overwrites code or read-only data.
bool PatchMemory(void* at, const void* value, size_t size);

/// Resolves the target of a RIP-relative operand whose 32-bit displacement starts at dispAt and whose instruction ends at nextInsn.
inline BYTE* ResolveRip(BYTE* dispAt, BYTE* nextInsn) {
    return nextInsn + *(INT32*)dispAt;
}

#endif
