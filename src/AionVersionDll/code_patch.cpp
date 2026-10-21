#include "code_patch.h"
#include "module_load.h"
#include <stdlib.h>
#include <string.h>

// The searches below go through images of up to 30 MB some sixty times while Game.dll loads, so each one looks for a single byte
// with memchr first: the byte of the pattern least common in x64 code, or the opcode of the instruction.

/// How common a byte is in x64 code, roughly: 0 for rare ones, more for the opcodes, prefixes and immediates seen everywhere.
static int Commonness(BYTE b) {
    switch (b) {
        case 0x00: case 0xFF: case 0xCC:
            return 4;
        case 0x48: case 0x8B: case 0x89: case 0x24: case 0x4C: case 0x44: case 0x0F: case 0xE8: case 0x8D: case 0x83: case 0x01:
        case 0xC0: case 0x05: case 0x10: case 0x08: case 0x20: case 0x40: case 0x45: case 0x49: case 0x41: case 0x85: case 0x74:
        case 0x75: case 0xC3: case 0x33: case 0xC7: case 0x4D: case 0xE9: case 0x02: case 0x04: case 0x0C: case 0x18: case 0x28:
        case 0x30: case 0x38: case 0x50: case 0x54: case 0x5C: case 0x15: case 0x0D: case 0x1D: case 0xEB: case 0x3B: case 0xC8:
            return 2;
        default:
            return 0;
    }
}

/// Offset of the fixed byte to look for first, or length if none is fixed.
static size_t AnchorOf(const BYTE* bytes, const char* fixed, size_t length) {
    size_t best = length;
    for (size_t i = 0; i < length; i++) {
        if (fixed[i] && (best == length || Commonness(bytes[i]) < Commonness(bytes[best]))) {
            best = i;
        }
    }
    return best;
}

/// Calls found(c) for every c in [begin, end) with c[anchor] == value, until it returns true.
template <typename Found>
static void ForEachByte(BYTE* begin, BYTE* end, size_t anchor, BYTE value, Found found) {
    for (BYTE* c = begin; c < end;) {
        BYTE* hit = (BYTE*)memchr(c + anchor, value, end - c);
        if (!hit) {
            return;
        }
        c = hit - anchor;
        if (found(c)) {
            return;
        }
        c++;
    }
}

std::vector<BYTE*> FindAllPatterns(HMODULE module, const char* pattern) {
    std::vector<BYTE> bytes;
    std::vector<char> fixed;
    for (const char* p = pattern; *p;) {
        if (*p == ' ') {
            p++;
        } else if (*p == '?') {
            bytes.push_back(0);
            fixed.push_back(0);
            p += p[1] == '?' ? 2 : 1;
        } else {
            bytes.push_back((BYTE)strtoul(p, (char**)&p, 16));
            fixed.push_back(1);
        }
    }
    size_t length = bytes.size();
    BYTE* begin = (BYTE*)module;
    BYTE* end = begin + ModuleImageSize(module) - length;
    std::vector<BYTE*> found;
    size_t anchor = AnchorOf(bytes.data(), fixed.data(), length);
    if (anchor == length) {
        return found;
    }
    ForEachByte(begin, end, anchor, bytes[anchor], [&](BYTE* c) {
        size_t i = 0;
        while (i < length && (!fixed[i] || c[i] == bytes[i])) {
            i++;
        }
        if (i == length) {
            found.push_back(c);
        }
        return false;
    });
    return found;
}

BYTE* FindPattern(HMODULE module, const char* pattern) {
    std::vector<BYTE*> found = FindAllPatterns(module, pattern);
    return found.size() == 1 ? found[0] : nullptr;
}

bool InModule(HMODULE module, const void* address, size_t size) {
    return (const BYTE*)address >= (const BYTE*)module && (const BYTE*)address + size <= (const BYTE*)module + ModuleImageSize(module);
}

BYTE* FindBytes(HMODULE module, const void* bytes, size_t length) {
    BYTE* begin = (BYTE*)module;
    BYTE* end = begin + ModuleImageSize(module) - length;
    std::vector<char> fixed(length, 1);
    size_t anchor = AnchorOf((const BYTE*)bytes, fixed.data(), length);
    BYTE* result = nullptr;
    ForEachByte(begin, end, anchor, ((const BYTE*)bytes)[anchor], [&](BYTE* c) {
        if (memcmp(c, bytes, length) == 0) {
            result = c;
        }
        return result != nullptr;
    });
    return result;
}

BYTE* FindRipReference(HMODULE module, const char* opcode, int length, const void* target) {
    BYTE code[4];
    int codeLength = 0;
    for (const char* p = opcode; *p && codeLength < 4;) {
        if (*p == ' ') {
            p++;
        } else {
            code[codeLength++] = (BYTE)strtoul(p, (char**)&p, 16);
        }
    }
    BYTE* begin = (BYTE*)module;
    BYTE* end = begin + ModuleImageSize(module) - length;
    BYTE* result = nullptr;
    ForEachByte(begin, end, codeLength - 1, code[codeLength - 1], [&](BYTE* c) {
        if (memcmp(c, code, codeLength) == 0 && ResolveRip(c + length - 4, c + length) == target) {
            result = c;
        }
        return result != nullptr;
    });
    return result;
}

BYTE* FindLeaTo(HMODULE module, const void* target) {
    BYTE* begin = (BYTE*)module;
    BYTE* end = begin + ModuleImageSize(module) - 7;
    BYTE* result = nullptr;
    ForEachByte(begin, end, 1, 0x8D, [&](BYTE* c) {
        if ((c[0] == 0x48 || c[0] == 0x4C) && (c[2] & 0xC7) == 0x05 && ResolveRip(c + 3, c + 7) == target) {
            result = c;
        }
        return result != nullptr;
    });
    return result;
}

std::vector<BYTE*> FindAllLeasTo(HMODULE module, const void* target) {
    BYTE* begin = (BYTE*)module;
    BYTE* end = begin + ModuleImageSize(module) - 7;
    std::vector<BYTE*> found;
    ForEachByte(begin, end, 1, 0x8D, [&](BYTE* c) {
        if ((c[0] == 0x48 || c[0] == 0x4C) && (c[2] & 0xC7) == 0x05 && ResolveRip(c + 3, c + 7) == target) {
            found.push_back(c);
        }
        return false;
    });
    return found;
}

BYTE* FindCallTo(HMODULE module, const void* target) {
    BYTE* begin = (BYTE*)module;
    BYTE* end = begin + ModuleImageSize(module) - 5;
    BYTE* result = nullptr;
    ForEachByte(begin, end, 0, 0xE8, [&](BYTE* c) {
        if (ResolveRip(c + 1, c + 5) == target) {
            result = c;
        }
        return result != nullptr;
    });
    return result;
}

std::vector<BYTE*> FindAllCallsTo(HMODULE module, const void* target) {
    BYTE* begin = (BYTE*)module;
    BYTE* end = begin + ModuleImageSize(module) - 5;
    std::vector<BYTE*> found;
    ForEachByte(begin, end, 0, 0xE8, [&](BYTE* c) {
        if (ResolveRip(c + 1, c + 5) == target) {
            found.push_back(c);
        }
        return false;
    });
    return found;
}

BYTE* FunctionStart(const void* address) {
    DWORD64 imageBase;
    PRUNTIME_FUNCTION function = RtlLookupFunctionEntry((DWORD64)address, &imageBase, nullptr);
    for (int depth = 0; function && depth < 32; depth++) {
        BYTE* unwind = (BYTE*)imageBase + function->UnwindData;
        if (!((unwind[0] >> 3) & UNW_FLAG_CHAININFO)) {
            return (BYTE*)imageBase + function->BeginAddress;
        }
        BYTE codes = unwind[2];
        function = (PRUNTIME_FUNCTION)(unwind + 4 + ((codes + 1) & ~1) * 2);
    }
    return nullptr;
}

BYTE* FunctionEnd(const void* start) {
    DWORD64 imageBase;
    PRUNTIME_FUNCTION function = RtlLookupFunctionEntry((DWORD64)start, &imageBase, nullptr);
    return function ? (BYTE*)imageBase + function->EndAddress : nullptr;
}

BYTE* FunctionBodyEnd(BYTE* start, size_t maxSize) {
    for (BYTE* p = start + 1; p < start + maxSize; p++) {
        BYTE* owner = FunctionStart(p);
        if (owner && owner != start) {
            return p;
        }
    }
    return start + maxSize;
}

bool PatchMemory(void* at, const void* value, size_t size) {
    DWORD oldProtect;
    // code and data share one section in some clients
    if (!VirtualProtect(at, size, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        return false;
    }
    memcpy(at, value, size);
    VirtualProtect(at, size, oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), at, size);
    return true;
}
