#include "code_patch.h"
#include "module_load.h"
#include "install_report.h"
#include <stdlib.h>
#include <string.h>
#include <intrin.h>

// The searches below go through images of up to 30 MB some hundred times while Game.dll loads, so each one checks 16 places at a time
// for two bytes first: the two bytes of the pattern least common in x64 code, or those of the instruction it looks for.

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

/// The offset of the least common fixed byte, other than the one at skip; length if none is fixed.
static size_t AnchorOf(const BYTE* bytes, const char* fixed, size_t length, size_t skip = SIZE_MAX) {
    size_t best = length;
    for (size_t i = 0; i < length; i++) {
        if (fixed[i] && i != skip && (best == length || Commonness(bytes[i]) < Commonness(bytes[best]))) {
            best = i;
        }
    }
    return best;
}

/// A byte the matches have at a given offset, after the mask.
struct Anchor {
    size_t at;
    BYTE value;
    BYTE mask = 0xFF;
};

/// The two least common fixed bytes of a pattern, or the one twice.
static void AnchorsOf(const BYTE* bytes, const char* fixed, size_t length, Anchor& first, Anchor& second) {
    size_t a = AnchorOf(bytes, fixed, length);
    size_t b = AnchorOf(bytes, fixed, length, a);
    b = b == length ? a : b;
    first = { a, bytes[a] };
    second = { b, bytes[b] };
}

/// Calls range(begin, end) for the executable sections of the module when code is searched, or once for its whole image, until it returns
/// true. An image whose headers mark no section executable is searched whole.
template <typename Range>
static void ForEachRange(HMODULE module, bool code, Range range) {
    BYTE* base = (BYTE*)module;
    size_t imageSize = ModuleImageSize(module);
    if (code) {
        auto nt = (PIMAGE_NT_HEADERS)(base + ((PIMAGE_DOS_HEADER)base)->e_lfanew);
        PIMAGE_SECTION_HEADER section = IMAGE_FIRST_SECTION(nt);
        bool any = false;
        for (WORD i = 0; i < nt->FileHeader.NumberOfSections; i++, section++) {
            size_t size = section->Misc.VirtualSize ? section->Misc.VirtualSize : section->SizeOfRawData;
            if (!(section->Characteristics & IMAGE_SCN_MEM_EXECUTE) || section->VirtualAddress >= imageSize || !size) {
                continue;
            }
            any = true;
            if (range(base + section->VirtualAddress, base + min(imageSize, (size_t)section->VirtualAddress + size))) {
                return;
            }
        }
        if (any) {
            return;
        }
    }
    range(base, base + imageSize);
}

/// Calls found(c) for every c in the searched ranges with room for length bytes that has both anchors, until it returns true. Code is
/// searched in the executable sections only. 16 places at a time are checked for both anchors, so only the rare places with both get the
/// full comparison. Given a target, the places must also hold the low byte of a RIP-relative displacement to it at dispAt, for an
/// instruction of length bytes; that byte differs from place to place, and few places hold it.
template <typename Found>
static void ForEachMatch(HMODULE module, bool code, size_t length, Anchor first, Anchor second, Found found, const void* target = nullptr,
    size_t dispAt = 0) {
    ForEachRange(module, code, [&](BYTE* begin, BYTE* rangeEnd) {
        if ((size_t)(rangeEnd - begin) < length) {
            return false;
        }
        // the last place with room for the whole pattern; the loads below end within the range, as both anchors lie in the pattern
        BYTE* end = rangeEnd - length + 1;
        const __m128i value1 = _mm_set1_epi8((char)first.value), mask1 = _mm_set1_epi8((char)first.mask);
        const __m128i value2 = _mm_set1_epi8((char)second.value), mask2 = _mm_set1_epi8((char)second.mask);
        // the low byte of the displacement at place c + i is that at c minus i
        const __m128i ramp = _mm_setr_epi8(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
        auto lowByte = [&](const BYTE* place) { return (BYTE)((ULONG_PTR)target - (ULONG_PTR)(place + length)); };
        BYTE* c = begin;
        for (; c + 16 <= end; c += 16) {
            __m128i a = _mm_and_si128(_mm_loadu_si128((const __m128i*)(c + first.at)), mask1);
            __m128i b = _mm_and_si128(_mm_loadu_si128((const __m128i*)(c + second.at)), mask2);
            __m128i both = _mm_and_si128(_mm_cmpeq_epi8(a, value1), _mm_cmpeq_epi8(b, value2));
            if (target) {
                __m128i expected = _mm_sub_epi8(_mm_set1_epi8((char)lowByte(c)), ramp);
                both = _mm_and_si128(both, _mm_cmpeq_epi8(_mm_loadu_si128((const __m128i*)(c + dispAt)), expected));
            }
            unsigned bits = (unsigned)_mm_movemask_epi8(both);
            while (bits) {
                unsigned long i;
                _BitScanForward(&i, bits);
                if (found(c + i)) {
                    return true;
                }
                bits &= bits - 1;
            }
        }
        for (; c < end; c++) {
            if ((c[first.at] & first.mask) == first.value && (c[second.at] & second.mask) == second.value
                && (!target || c[dispAt] == lowByte(c)) && found(c)) {
                return true;
            }
        }
        return false;
    });
}

static std::vector<BYTE*> MatchPattern(HMODULE module, const char* pattern) {
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
    std::vector<BYTE*> found;
    if (AnchorOf(bytes.data(), fixed.data(), length) == length) {
        return found;
    }
    Anchor first, second;
    AnchorsOf(bytes.data(), fixed.data(), length, first, second);
    ForEachMatch(module, true, length, first, second, [&](BYTE* c) {
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

std::vector<BYTE*> FindAllPatterns(HMODULE module, const char* pattern) {
    std::vector<BYTE*> found = MatchPattern(module, pattern);
    ReportSearch(found.empty() ? 0 : 1);
    return found;
}

BYTE* FindPattern(HMODULE module, const char* pattern, size_t* count) {
    std::vector<BYTE*> found = MatchPattern(module, pattern);
    ReportSearch(found.size());
    if (count) {
        *count = found.size();
    }
    if (found.size() > 1) {
        MODULE_LOG("pattern ambiguous (%zu matches): %.60s", found.size(), pattern);
    }
    return found.size() == 1 ? found[0] : nullptr;
}

bool InModule(HMODULE module, const void* address, size_t size) {
    return (const BYTE*)address >= (const BYTE*)module && (const BYTE*)address + size <= (const BYTE*)module + ModuleImageSize(module);
}

BYTE* FindBytes(HMODULE module, const void* bytes, size_t length) {
    std::vector<char> fixed(length, 1);
    Anchor first, second;
    AnchorsOf((const BYTE*)bytes, fixed.data(), length, first, second);
    BYTE* result = nullptr;
    ForEachMatch(module, false, length, first, second, [&](BYTE* c) {
        if (memcmp(c, bytes, length) == 0) {
            result = c;
        }
        return result != nullptr;
    });
    ReportSearch(result ? 1 : 0);
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
    BYTE* result = nullptr;
    Anchor last = { (size_t)codeLength - 1, code[codeLength - 1] };
    Anchor before = codeLength > 1 ? Anchor{ (size_t)codeLength - 2, code[codeLength - 2] } : last;
    ForEachMatch(module, true, length, last, before, [&](BYTE* c) {
        if (memcmp(c, code, codeLength) == 0 && ResolveRip(c + length - 4, c + length) == target) {
            result = c;
        }
        return result != nullptr;
    }, target, length - 4);
    ReportSearch(result ? 1 : 0);
    return result;
}

// lea reg, [rip+disp32] and call rel32
static const Anchor LEA = { 1, 0x8D };
static const Anchor RIP_OPERAND = { 2, 0x05, 0xC7 };
static const Anchor CALL = { 0, 0xE8 };

BYTE* FindLeaTo(HMODULE module, const void* target) {
    BYTE* result = nullptr;
    ForEachMatch(module, true, 7, LEA, RIP_OPERAND, [&](BYTE* c) {
        if ((c[0] == 0x48 || c[0] == 0x4C) && (c[2] & 0xC7) == 0x05 && ResolveRip(c + 3, c + 7) == target) {
            result = c;
        }
        return result != nullptr;
    }, target, 3);
    ReportSearch(result ? 1 : 0);
    return result;
}

/// The text a search looked for, for the log: up to 60 printable characters, narrow or wide.
static void TextForLog(const BYTE* text, char (&out)[64]) {
    size_t step = text[0] && !text[1] ? 2 : 1, n = 0;
    for (; n < 60 && text[n * step] >= 0x20 && text[n * step] < 0x7F; n++) {
        out[n] = (char)text[n * step];
    }
    out[n] = 0;
}

static std::vector<BYTE*> LeasTo(HMODULE module, const void* target) {
    std::vector<BYTE*> found;
    ForEachMatch(module, true, 7, LEA, RIP_OPERAND, [&](BYTE* c) {
        if ((c[0] == 0x48 || c[0] == 0x4C) && (c[2] & 0xC7) == 0x05 && ResolveRip(c + 3, c + 7) == target) {
            found.push_back(c);
        }
        return false;
    }, target, 3);
    return found;
}

std::vector<BYTE*> FindAllLeasTo(HMODULE module, const void* target) {
    std::vector<BYTE*> found = LeasTo(module, target);
    ReportSearch(found.empty() ? 0 : 1);
    return found;
}

BYTE* FindStringLea(HMODULE module, const void* bytes, size_t length, size_t skip) {
    BYTE* text = FindBytes(module, bytes, length);
    if (!text) {
        return nullptr;
    }
    std::vector<BYTE*> found = LeasTo(module, text + skip);
    ReportSearch(found.size());
    if (found.size() > 1) {
        char shown[64];
        TextForLog(text + skip, shown);
        MODULE_LOG("text loaded from %zu places: %s", found.size(), shown);
    }
    return found.size() == 1 ? found[0] : nullptr;
}

BYTE* FindFunctionByString(HMODULE module, const void* bytes, size_t length, size_t skip) {
    BYTE* text = FindBytes(module, bytes, length);
    if (!text) {
        return nullptr;
    }
    // a function that names itself in several of its messages loads the text more than once: all must be in it
    BYTE* function = nullptr;
    size_t functions = 0;
    for (BYTE* lea : LeasTo(module, text + skip)) {
        BYTE* start = FunctionStart(lea);
        if (start != function) {
            function = start;
            functions++;
        }
    }
    ReportSearch(functions);
    if (functions > 1) {
        char shown[64];
        TextForLog(text + skip, shown);
        MODULE_LOG("text loaded from %zu functions: %s", functions, shown);
    }
    return functions == 1 ? function : nullptr;
}

BYTE* FindCallTo(HMODULE module, const void* target) {
    BYTE* result = nullptr;
    ForEachMatch(module, true, 5, CALL, CALL, [&](BYTE* c) {
        if (ResolveRip(c + 1, c + 5) == target) {
            result = c;
        }
        return result != nullptr;
    }, target, 1);
    ReportSearch(result ? 1 : 0);
    return result;
}

std::vector<BYTE*> FindAllCallsTo(HMODULE module, const void* target) {
    std::vector<BYTE*> found;
    ForEachMatch(module, true, 5, CALL, CALL, [&](BYTE* c) {
        if (ResolveRip(c + 1, c + 5) == target) {
            found.push_back(c);
        }
        return false;
    }, target, 1);
    ReportSearch(found.empty() ? 0 : 1);
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

CodePatch ExpectBytes(BYTE* at, size_t size) {
    CodePatch patch;
    if (!at || size > CodePatch::MAX_SIZE) {
        return patch;
    }
    patch.at = at;
    patch.size = size;
    memcpy(patch.expected, at, size);
    memcpy(patch.value, at, size);
    return patch;
}

CodePatch PlanPatch(BYTE* at, const void* value, size_t size) {
    CodePatch patch = ExpectBytes(at, size);
    if (patch) {
        memcpy(patch.value, value, size);
    }
    return patch;
}

bool ApplyPatch(const CodePatch& place, const void* value) {
    CodePatch patch = place;
    if (patch) {
        memcpy(patch.value, value, patch.size);
    }
    return ApplyPatch(patch);
}

bool ApplyPatch(const CodePatch& patch) {
    if (!patch) {
        return false;
    }
    if (memcmp(patch.at, patch.expected, patch.size) != 0) {
        MODULE_LOG("patch at %p skipped: the bytes there changed since the search", patch.at);
        ReportWrite(patch.at, false);
        return false;
    }
    return PatchMemory(patch.at, patch.value, patch.size);
}

bool PatchMemory(void* at, const void* value, size_t size) {
    if (g_dryRun) {
        ReportWrite(at, true);
        return true;
    }
    DWORD oldProtect;
    // code and data share one section in some clients
    if (!VirtualProtect(at, size, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        ReportWrite(at, false);
        return false;
    }
    memcpy(at, value, size);
    VirtualProtect(at, size, oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), at, size);
    ReportWrite(at, true);
    return true;
}

BYTE* FindExport(HMODULE module, const char* name) {
    BYTE* base = (BYTE*)module;
    auto nt = (PIMAGE_NT_HEADERS)(base + ((PIMAGE_DOS_HEADER)base)->e_lfanew);
    const IMAGE_DATA_DIRECTORY& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!directory.VirtualAddress) {
        return nullptr;
    }
    auto exports = (PIMAGE_EXPORT_DIRECTORY)(base + directory.VirtualAddress);
    DWORD* names = (DWORD*)(base + exports->AddressOfNames);
    WORD* ordinals = (WORD*)(base + exports->AddressOfNameOrdinals);
    DWORD* functions = (DWORD*)(base + exports->AddressOfFunctions);
    for (DWORD i = 0; i < exports->NumberOfNames; i++) {
        if (strcmp((const char*)(base + names[i]), name) == 0) {
            DWORD rva = functions[ordinals[i]];
            // a forwarder points at a name inside the export directory
            bool forwarded = rva >= directory.VirtualAddress && rva < directory.VirtualAddress + directory.Size;
            return forwarded ? nullptr : base + rva;
        }
    }
    return nullptr;
}
