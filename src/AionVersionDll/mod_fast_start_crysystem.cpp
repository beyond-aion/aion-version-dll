#include "mods.h"
#include <algorithm>
#include <array>
#include <map>
#include <memory_resource>
#include <string>
#include <string_view>
#include <vector>
#include <detours/detours.h>
#include "install_report.h"
#include <libdeflate.h>
#include "mod_fast_start.h"

typedef double(__fastcall* ClockMeasure_t)(double* secondsPerTick);
static ClockMeasure_t real_ClockMeasure = nullptr;
static double s_clockMhz = 0;
static double s_secondsPerTick = 0;

static double __fastcall zzClockMeasure(double* secondsPerTick) {
    if (!s_clockMhz) {
        s_clockMhz = real_ClockMeasure(secondsPerTick);
        s_secondsPerTick = *secondsPerTick;
        return s_clockMhz;
    }
    *secondsPerTick = s_secondsPerTick;
    return s_clockMhz;
}

/// CrySystem measures the clock of every logical processor one after another, on a thread bound to it, each over Sleep(100): 1.6 seconds
/// with 16 of them. rdtsc; shl rdx, 20h; mov ecx, 100 becomes 10 ms, which still gives the clock to about a percent, but the timer of
/// Windows makes that 15 ms. The time stamp counter runs at one rate on every processor, so only the first one is measured and the
/// others get its clock in MHz and seconds per tick.
struct ClockPlace {
    // the client loads CrySystem.dll once more when it closes, already shortened then
    bool shortened = false;
    CodePatch duration;
    BYTE* function = nullptr;
};

static ClockPlace LocateClockMeasure(HMODULE crySystem) {
    ClockPlace place;
    static const DWORD MS = 10;
    // 5.x clients have an xorps between the shift and the mov
    for (BYTE* rdtsc : FindAllPatterns(crySystem, "0F 31 48 C1 E2 20")) {
        for (BYTE* p = rdtsc + 6; p < rdtsc + 12; p++) {
            if (p[0] == 0xB9 && *(DWORD*)(p + 1) == MS) {
                place.shortened = true;
                return place;
            }
            if (p[0] == 0xB9 && *(DWORD*)(p + 1) == 100) {
                place.duration = PlanPatch(p + 1, &MS, sizeof(MS));
                DWORD64 base = 0;
                PRUNTIME_FUNCTION function = RtlLookupFunctionEntry((DWORD64)p, &base, nullptr);
                place.function = function ? (BYTE*)(base + function->BeginAddress) : nullptr;
                return place;
            }
        }
    }
    return place;
}

static void ApplyClockMeasure(const ClockPlace& place) {
    if (place.shortened) {
        return;
    }
    if (!place.duration) {
        ModsLog("fast start: clock measure not found");
        return;
    }
    ApplyPatch(place.duration);
    LONG error = -1;
    if (place.function && !real_ClockMeasure) {
        real_ClockMeasure = (ClockMeasure_t)place.function;
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        AttachHook(&(PVOID&)real_ClockMeasure, zzClockMeasure);
        error = CommitHooks();
    }
    ModsLog("fast start: clock measure at %p, once: %ld", place.duration.at - 1, error);
}

static void ShortenClockMeasure(HMODULE crySystem) {
    LocateThenApply(crySystem, LocateClockMeasure, ApplyClockMeasure);
}

// 4.x clients keep much of their game data in memory as zlib blocks of 16 KB, which CrySystem compresses with the zlib 1.1.4 it
// carries while the data loads (a sixth of the loading time) and decompresses when it is used; 5.x clients, with zlib 1.2.8, do the
// same when they build their table dump. libdeflate does both several times
// faster; its blocks are plain zlib streams the client's own code reads as well. Whatever libdeflate cannot do (a block that does not
// fit, data it rejects) goes to the client's zlib, which then gives its own result.

typedef int(__cdecl* Compress2_t)(BYTE* dest, unsigned long* destLen, const BYTE* source, unsigned long sourceLen, int level);
typedef int(__cdecl* Uncompress_t)(BYTE* dest, unsigned long* destLen, const BYTE* source, unsigned long sourceLen);

static Compress2_t real_compress2 = nullptr;
static Uncompress_t real_uncompress = nullptr;
static bool s_blocksInMemory = false;

static int __cdecl zz_compress2(BYTE* dest, unsigned long* destLen, const BYTE* source, unsigned long sourceLen, int level) {
    // zlib's levels 0 to 9 mean the same in libdeflate. 4.x clients ask for level 6, the default, for blocks that stay in memory;
    // level 1 takes a quarter second less in 4.8 and 1 MB more for its 220 MB of data. 5.x clients store theirs in the table dump.
    int deflateLevel = level == -1 ? 6 : level;
    if (s_blocksInMemory && deflateLevel == 6) {
        deflateLevel = 1;
    }
    static thread_local libdeflate_compressor* compressors[10] = {};
    if (deflateLevel < 0 || deflateLevel > 9) {
        return real_compress2(dest, destLen, source, sourceLen, level);
    }
    libdeflate_compressor*& compressor = compressors[deflateLevel];
    if (!compressor) {
        compressor = libdeflate_alloc_compressor(deflateLevel);
    }
    size_t size = compressor ? libdeflate_zlib_compress(compressor, source, sourceLen, dest, *destLen) : 0;
    if (!size) {
        return real_compress2(dest, destLen, source, sourceLen, level);
    }
    *destLen = (unsigned long)size;
    return 0;
}

static int __cdecl zz_uncompress(BYTE* dest, unsigned long* destLen, const BYTE* source, unsigned long sourceLen) {
    static thread_local libdeflate_decompressor* decompressor = nullptr;
    if (!decompressor) {
        decompressor = libdeflate_alloc_decompressor();
    }
    size_t read, written;
    if (!decompressor || libdeflate_zlib_decompress_ex(decompressor, source, sourceLen, dest, *destLen, &read, &written) != LIBDEFLATE_SUCCESS) {
        return real_uncompress(dest, destLen, source, sourceLen);
    }
    *destLen = (unsigned long)written;
    return 0;
}

/// compress2 and uncompress of zlib 1.1.4 as the 4.x clients build them, both starting by filling a z_stream on the stack and then
/// passing the version "1.1.4" to the init function: ...; lea r8/rdx, "1.1.4"
static BYTE* FindZlibFunction(HMODULE crySystem, const char* pattern, const void* version) {
    BYTE* function = FindPattern(crySystem, pattern);
    return function && ResolveRip(function + strlen(pattern) / 3 + 1, function + strlen(pattern) / 3 + 5) == version ? function : nullptr;
}

struct ZlibPlace {
    BYTE* compress = nullptr;
    BYTE* uncompress = nullptr;
    // 4.x: zlib 1.1.4, which keeps the game data in memory as blocks
    bool blocksInMemory = false;
};

static ZlibPlace LocateZlib(HMODULE crySystem) {
    static const char VERSION4[] = "1.1.4", VERSION5[] = "1.2.8";
    ZlibPlace place;
    BYTE*& compress = place.compress;
    BYTE*& uncompress = place.uncompress;
    if (const void* version = FindBytes(crySystem, VERSION4, sizeof(VERSION4))) {
        compress = FindZlibFunction(crySystem, "4C 8B DC 48 81 EC 98 00 00 00 8B 02 4D 89 43 88 45 89 4B 90 49 89 4B 98 89 44 24 38 33 C0 49 89 "
            "7B F0 48 8B FA 8B 94 24 C0 00 00 00 44 8D 48 58 4C 8D 05", version);
        uncompress = FindZlibFunction(crySystem, "4C 8B DC 48 81 EC 88 00 00 00 8B 02 4D 89 43 98 49 89 4B A8 45 89 4B A0 89 44 24 38 33 C0 49 89 "
            "7B 20 48 8B FA 49 8D 4B 98 44 8D 40 58 48 8D 15", version);
        place.blocksInMemory = true;
    } else if (const void* version = FindBytes(crySystem, VERSION5, sizeof(VERSION5))) {
        compress = FindZlibFunction(crySystem, "4C 8B DC 57 48 81 EC 80 00 00 00 8B 02 4D 89 43 98 45 89 4B A0 49 89 4B A8 89 44 24 38 33 C0 48 8B "
            "FA 8B 94 24 B0 00 00 00 4C 8D 05", version);
        uncompress = FindZlibFunction(crySystem, "40 57 48 81 EC 80 00 00 00 8B 02 48 8B FA 4C 89 44 24 20 89 44 24 38 33 C0 48 89 4C 24 30 48 8D 15",
            version);
    }
    return place;
}

static void ApplyZlib(const ZlibPlace& place) {
    s_blocksInMemory |= place.blocksInMemory;
    BYTE* compress = place.compress;
    BYTE* uncompress = place.uncompress;
    if (!compress || !uncompress) {
        ModsLog("fast start: zlib not found (compress2 %p, uncompress %p)", compress, uncompress);
        return;
    }
    real_compress2 = (Compress2_t)compress;
    real_uncompress = (Uncompress_t)uncompress;
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    AttachHook(&(PVOID&)real_compress2, zz_compress2);
    AttachHook(&(PVOID&)real_uncompress, zz_uncompress);
    ModsLog("fast start: zlib at %p and %p: %ld", compress, uncompress, CommitHooks());
}

static void HookZlib(HMODULE crySystem) {
    if (!real_compress2) {
        LocateThenApply(crySystem, LocateZlib, ApplyZlib);
    }
}

// The files packed in the .pak archives are raw deflate streams, which CrySystem inflates with its zlib as it reads them, on the main
// thread: about a third of the work of 5.8 before the login screen. libdeflate inflates a whole stream that fits the output at once;
// a stream that does not fit goes to the client's zlib.

/// 4.x: inflates src into dest, setting *destLen to the size written; 0 when done.
typedef int(__cdecl* RawInflate4_t)(void* allocator, BYTE* dest, unsigned long* destLen, const BYTE* src, unsigned long srcLen);

/// 5.x: inflates into a chain of output blocks, reducing the size of each to what it got and setting *total; 0 when done.
struct OutputBlock {
    OutputBlock* next;
    unsigned long size;
    BYTE data[1];
};
typedef int(__cdecl* RawInflate5_t)(void* allocator, OutputBlock* out, unsigned long* total, const BYTE* src, unsigned long srcLen);

static RawInflate4_t real_RawInflate4 = nullptr;
static RawInflate5_t real_RawInflate5 = nullptr;

static bool RawInflate(const BYTE* src, unsigned long srcLen, BYTE* dest, unsigned long destSize, unsigned long& written) {
    static thread_local libdeflate_decompressor* decompressor = nullptr;
    if (!decompressor) {
        decompressor = libdeflate_alloc_decompressor();
    }
    size_t read, size;
    if (!decompressor || libdeflate_deflate_decompress_ex(decompressor, src, srcLen, dest, destSize, &read, &size) != LIBDEFLATE_SUCCESS) {
        return false;
    }
    written = (unsigned long)size;
    return true;
}

static int __cdecl zzRawInflate4(void* allocator, BYTE* dest, unsigned long* destLen, const BYTE* src, unsigned long srcLen) {
    unsigned long written;
    if (!RawInflate(src, srcLen, dest, *destLen, written)) {
        return real_RawInflate4(allocator, dest, destLen, src, srcLen);
    }
    *destLen = written;
    return 0;
}

static int __cdecl zzRawInflate5(void* allocator, OutputBlock* out, unsigned long* total, const BYTE* src, unsigned long srcLen) {
    unsigned long written;
    if (!out->next) {
        if (!RawInflate(src, srcLen, out->data, out->size, written)) {
            return real_RawInflate5(allocator, out, total, src, srcLen);
        }
        out->size = written;
        *total = written;
        return 0;
    }
    // a chain (large files such as textures) is inflated at once and then cut up as the client's zlib fills it: every block up to the
    // one the stream ends in is full, that one is cut to what it got, and the blocks after it are left as they are
    static thread_local std::vector<BYTE> buffer;
    size_t capacity = 0;
    for (OutputBlock* block = out; block; block = block->next) {
        capacity += block->size;
    }
    if (capacity > ULONG_MAX) {
        return real_RawInflate5(allocator, out, total, src, srcLen);
    }
    if (buffer.size() < capacity) {
        buffer.resize(capacity);
    }
    if (!RawInflate(src, srcLen, buffer.data(), (unsigned long)capacity, written)) {
        return real_RawInflate5(allocator, out, total, src, srcLen);
    }
    const BYTE* from = buffer.data();
    unsigned long left = written;
    OutputBlock* block = out;
    for (; left > block->size; block = block->next) {
        memcpy(block->data, from, block->size);
        from += block->size;
        left -= block->size;
    }
    memcpy(block->data, from, left);
    block->size = left;
    *total = written;
    return 0;
}

struct InflatePlace {
    BYTE* inflate4 = nullptr;
    BYTE* inflate5 = nullptr;
};

static InflatePlace LocateRawInflate(HMODULE crySystem) {
    static const char VERSION4[] = "1.1.4", VERSION5[] = "1.2.8";
    const void* version4 = FindBytes(crySystem, VERSION4, sizeof(VERSION4));
    const void* version5 = FindBytes(crySystem, VERSION5, sizeof(VERSION5));
    BYTE* inflate4 = version4 ? FindZlibFunction(crySystem, "4C 8B DC 48 81 EC 98 00 00 00 8B 84 24 C0 00 00 00 4D 89 4B 88 41 B9 58 00 00 00 89 44 24 "
        "28 41 8B 00 49 89 53 98 89 44 24 38 48 8D 05 ?? ?? ?? ?? 49 89 7B F0 49 89 43 B8 49 8B F8 49 89 4B C8 48 8D 05 ?? ?? ?? ?? 4C 8D 05", version4) : nullptr;
    BYTE* inflate5 = version5 ? FindZlibFunction(crySystem, "4C 8B DC 49 89 5B 10 56 48 81 EC 80 00 00 00 8B 84 24 B0 00 00 00 4D 89 4B 98 41 B9 58 00 00 "
        "00 89 44 24 28 48 8D 42 0C 49 8B F0 49 89 43 A8 8B 42 08 48 8B DA 89 44 24 38 48 8D 05 ?? ?? ?? ?? 49 89 4B D8 49 89 43 C8 48 8D 05 ?? ?? ?? ?? 4C "
        "8D 05", version5) : nullptr;
    return { inflate4, inflate5 };
}

static void ApplyRawInflate(const InflatePlace& place) {
    BYTE* inflate4 = place.inflate4;
    BYTE* inflate5 = place.inflate5;
    if (!inflate4 && !inflate5) {
        ModsLog("fast start: pak inflate not found");
        return;
    }
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    if (inflate4) {
        real_RawInflate4 = (RawInflate4_t)inflate4;
        AttachHook(&(PVOID&)real_RawInflate4, zzRawInflate4);
    } else {
        real_RawInflate5 = (RawInflate5_t)inflate5;
        AttachHook(&(PVOID&)real_RawInflate5, zzRawInflate5);
    }
    ModsLog("fast start: pak inflate at %p: %ld", inflate4 ? inflate4 : inflate5, CommitHooks());
}

static void HookRawInflate(HMODULE crySystem) {
    if (!real_RawInflate4 && !real_RawInflate5) {
        LocateThenApply(crySystem, LocateRawInflate, ApplyRawInflate);
    }
}

// Opening a .pak archive, CrySystem puts the name of every file into a tree of folders (a std::map of std::string per folder, a node
// and a string for every name, with the checked iterators of the runtime) and then writes the tree out as the flat directory it
// keeps for the archive. While it makes that directory, the names go into a tree of our own, which writes the same bytes. A tree the
// client keeps to write to an archive stays its own.
//
// The flat directory of a folder: WORD folders, WORD files; per folder { DWORD to its directory from this field, DWORD to its name
// from the first name }; the file entries, each with the offset of its name at PAK_NAME_AT; the names, zero-terminated, the folders
// first, each list in the order of std::map; the directories of the folders, in the same order, from the next 4-byte boundary.

static const size_t PAK_ENTRY_SIZE = 40;
static const size_t PAK_NAME_AT = 24;
static const size_t PAK_HEADER_AT = 12; // -1 in an entry that was never set

struct PakFolder {
    explicit PakFolder(std::pmr::memory_resource* arena) : folders(arena), files(arena) {}
    std::pmr::map<std::string_view, PakFolder*> folders;
    std::pmr::map<std::string_view, std::array<BYTE, PAK_ENTRY_SIZE>> files;
};

/// Folders, names and nodes all live in the arena, which frees them at once.
struct PakTree {
    const void* tree; // the client's tree this one stands in for
    std::pmr::monotonic_buffer_resource arena;
    PakFolder root{ &arena };
    // the files of a folder mostly follow one another
    std::string_view lastFolderPath;
    PakFolder* lastFolder = nullptr;
    DWORD files = 0;
};

/// The tree adds a file under its path; 0 when added, 22 when the file is there already.
typedef int(__fastcall* TreeAdd_t)(void* tree, const char* path, const BYTE* entry);
/// The number of files in the tree.
typedef DWORD(__fastcall* TreeCount_t)(void* tree);
/// The size of the flat directory of the tree.
typedef size_t(__fastcall* TreeSize_t)(void* tree);
/// Writes the flat directory, returns its size.
typedef size_t(__fastcall* TreeWrite_t)(void* tree, BYTE* out);
/// Opens the archive and makes its directory.
typedef void*(__fastcall* MakeCache_t)(void* factory, void* cache, void* path, void* arg4);

static TreeAdd_t real_TreeAdd = nullptr;
static TreeCount_t real_TreeCount = nullptr;
static TreeSize_t real_TreeSize = nullptr;
static TreeWrite_t real_TreeWrite = nullptr;
static MakeCache_t real_MakeCache = nullptr;
static int s_pakTreeAt = 0; // of the tree in the factory
static thread_local PakTree* s_pakTree = nullptr;

static PakTree* OwnPakTree(const void* tree) {
    PakTree* own = s_pakTree;
    return own && own->tree == tree ? own : nullptr;
}

static int AddPakFile(PakTree* own, const char* path, const BYTE* entry) {
    size_t length = strlen(path);
    char* copy = (char*)own->arena.allocate(length + 1, 1);
    memcpy(copy, path, length + 1);
    std::string_view rest(copy, length);
    PakFolder* folder = &own->root;
    size_t last = rest.find_last_of("/\\");
    if (last != std::string_view::npos && rest.substr(0, last + 1) == own->lastFolderPath) {
        folder = own->lastFolder;
        rest.remove_prefix(last + 1);
    } else if (last != std::string_view::npos) {
        own->lastFolderPath = rest.substr(0, last + 1);
        for (size_t slash; (slash = rest.find_first_of("/\\")) != std::string_view::npos; rest.remove_prefix(slash + 1)) {
            PakFolder*& sub = folder->folders[rest.substr(0, slash)];
            if (!sub) {
                sub = new (own->arena.allocate(sizeof(PakFolder), alignof(PakFolder))) PakFolder(&own->arena);
            }
            folder = sub;
        }
        own->lastFolder = folder;
    }
    auto [file, added] = folder->files.try_emplace(rest);
    if (!added && *(const DWORD*)(file->second.data() + PAK_HEADER_AT) != 0xFFFFFFFF) {
        return 22;
    }
    memcpy(file->second.data(), entry, PAK_ENTRY_SIZE);
    own->files += added;
    return 0;
}

static int __fastcall zzTreeAdd(void* tree, const char* path, const BYTE* entry) {
    PakTree* own = OwnPakTree(tree);
    if (!own) {
        return real_TreeAdd(tree, path, entry);
    }
    return AddPakFile(own, path, entry);
}

static DWORD __fastcall zzTreeCount(void* tree) {
    PakTree* own = OwnPakTree(tree);
    return own ? own->files : real_TreeCount(tree);
}

static size_t FolderSize(const PakFolder& folder) {
    size_t names = 0;
    size_t size = 4 + 8 * folder.folders.size() + PAK_ENTRY_SIZE * folder.files.size();
    for (const auto& [name, sub] : folder.folders) {
        names += name.size() + 1;
        size += FolderSize(*sub);
    }
    for (const auto& [name, entry] : folder.files) {
        names += name.size() + 1;
    }
    return size + ((names + 3) & ~(size_t)3);
}

static BYTE* WriteName(BYTE* at, std::string_view name) {
    memcpy(at, name.data(), name.size());
    at[name.size()] = 0;
    return at + name.size() + 1;
}

static size_t WriteFolder(const PakFolder& folder, BYTE* out) {
    *(WORD*)out = (WORD)folder.folders.size();
    *(WORD*)(out + 2) = (WORD)folder.files.size();
    BYTE* folders = out + 4;
    BYTE* files = folders + 8 * folder.folders.size();
    BYTE* names = files + PAK_ENTRY_SIZE * folder.files.size();
    BYTE* name = names;
    BYTE* at = folders;
    for (const auto& [text, sub] : folder.folders) {
        *(DWORD*)(at + 4) = (DWORD)(name - names);
        name = WriteName(name, text);
        at += 8;
    }
    at = files;
    for (const auto& [text, entry] : folder.files) {
        memcpy(at, entry.data(), PAK_ENTRY_SIZE);
        *(DWORD*)(at + PAK_NAME_AT) = (DWORD)(name - names);
        name = WriteName(name, text);
        at += PAK_ENTRY_SIZE;
    }
    BYTE* next = (BYTE*)(((ULONG_PTR)name + 3) & ~(ULONG_PTR)3);
    at = folders;
    for (const auto& [text, sub] : folder.folders) {
        *(DWORD*)at = (DWORD)(next - at);
        next += WriteFolder(*sub, next);
        at += 8;
    }
    return next - out;
}

static size_t __fastcall zzTreeSize(void* tree) {
    PakTree* own = OwnPakTree(tree);
    return own ? FolderSize(own->root) : real_TreeSize(tree);
}

static size_t __fastcall zzTreeWrite(void* tree, BYTE* out) {
    PakTree* own = OwnPakTree(tree);
    return own ? WriteFolder(own->root, out) : real_TreeWrite(tree, out);
}

static void* __fastcall zzMakeCache(void* factory, void* cache, void* path, void* arg4) {
    if (s_pakTree) {
        return real_MakeCache(factory, cache, path, arg4);
    }
    PakTree own;
    own.tree = (BYTE*)factory + s_pakTreeAt;
    s_pakTree = &own;
    void* result = real_MakeCache(factory, cache, path, arg4);
    s_pakTree = nullptr;
    return result;
}

struct CallSite {
    BYTE* target;
    int thisAt; // the offset from a register that the last lea rcx before the call added, -1 for anything else in rcx
};

/// Whether the instruction writes rcx otherwise than by a lea from a register (only the forms the compiler uses for it).
static bool WritesRcx(const BYTE* ip) {
    BYTE rex = (ip[0] & 0xF0) == 0x40 ? ip[0] : 0;
    const BYTE* op = rex ? ip + 1 : ip;
    BYTE reg = (op[1] >> 3) & 7;
    if ((op[0] == 0x8B || op[0] == 0x8D) && reg == 1 && !(rex & 4)) {
        return true;
    }
    if ((op[0] == 0x89 || op[0] == 0x33 || op[0] == 0x31) && (op[1] >> 6) == 3 && (op[1] & 7) == 1 && !(rex & 1)) {
        return true;
    }
    return op[0] == 0xB9 && !(rex & 1);
}

/// The direct calls of a function, with what rcx holds for each.
static std::vector<CallSite> CallsOf(BYTE* function) {
    std::vector<CallSite> calls;
    int thisAt = -1;
    BYTE* end = FunctionBodyEnd(function, 0x2000);
    for (BYTE* ip = function; ip < end;) {
        BYTE* next = (BYTE*)DetourCopyInstruction(nullptr, nullptr, ip, nullptr, nullptr);
        if (!next || next <= ip) {
            break;
        }
        bool rex = ip[0] == 0x48 || ip[0] == 0x49;
        if (rex && ip[1] == 0x8D && ((ip[2] >> 3) & 7) == 1 && (ip[2] & 7) != 4) {
            BYTE mod = ip[2] >> 6;
            thisAt = mod == 1 ? (signed char)ip[3] : mod == 2 ? *(INT32*)(ip + 3) : mod == 0 && (ip[2] & 7) != 5 ? 0 : -1;
        } else if (ip[0] == 0xE8) {
            calls.push_back({ ResolveRip(ip + 1, ip + 5), thisAt });
            thisAt = -1;
        } else if ((ip[0] == 0xFF && ((ip[1] >> 3) & 7) == 2) || ((ip[0] & 0xF0) == 0x40 && ip[1] == 0xFF && ((ip[2] >> 3) & 7) == 2)
            || WritesRcx(ip)) {
            thisAt = -1;
        }
        ip = next;
    }
    return calls;
}

static bool Calls(BYTE* function, BYTE* target) {
    for (const CallSite& call : CallsOf(function)) {
        if (call.target == target) {
            return true;
        }
    }
    return false;
}

/// The function that names itself in its messages.
static BYTE* NamedFunction(HMODULE module, const char* name) {
    // the first function that loads the name: in 4.8 and 4.9 "ZipDir::CacheFactory::Seek" is loaded by two
    const void* text = FindBytes(module, name, strlen(name) + 1);
    BYTE* lea = text ? FindLeaTo(module, text) : nullptr;
    return lea ? FunctionStart(lea) : nullptr;
}

// 4.x clients lower the case of every name in the central directory of an archive character by character, through the checked
// iterators of a std::string, before adding it (5.x clients do it inline). While a directory is made, the central directory is read
// here instead, with the same checks; anything out of the ordinary goes to the client's function, which reports it.

/// Reads the central directory and adds its files; false on any error, which it reports.
typedef bool(__fastcall* BuildFileEntryMap_t)(BYTE* factory);
typedef bool(__fastcall* FactorySeek_t)(BYTE* factory, DWORD offset, int origin);
typedef bool(__fastcall* FactoryRead_t)(BYTE* factory, void* buffer, DWORD size);
typedef void*(__fastcall* PakAlloc_t)(size_t size);
typedef void(__fastcall* PakFree_t)(void* block);
typedef int(__cdecl* Tolower_t)(int c);

static BuildFileEntryMap_t real_BuildFileEntryMap = nullptr;
static FactorySeek_t s_factorySeek = nullptr;
static FactoryRead_t s_factoryRead = nullptr;
static PakAlloc_t s_pakAlloc = nullptr;
static PakFree_t s_pakFree = nullptr;
static Tolower_t s_tolower = nullptr;

// the factory of 4.x
static const BYTE FACTORY_INIT_METHOD = 0x10; // from 1 on, the local header of every file is read and checked as well
static const BYTE FACTORY_CDR_SIZE = 0x24;
static const BYTE FACTORY_CDR_OFFSET = 0x28;
static const BYTE FACTORY_WANTS_MAP = 0x88; // a std::map of the names besides the tree
static const DWORD CDR_SIGNATURE = 0x02014B50;

/// The file entry of a record of the central directory, as the client makes it.
static void FileEntryOf(const BYTE* record, BYTE* entry) {
    memset(entry, 0, PAK_ENTRY_SIZE);
    *(UINT64*)entry = *(const UINT64*)(record + 16); // CRC and compressed size
    *(DWORD*)(entry + 8) = *(const DWORD*)(record + 24);
    *(DWORD*)(entry + PAK_HEADER_AT) = *(const DWORD*)(record + 42);
    *(DWORD*)(entry + 16) = 0xFFFFFFFF; // the data offset, not known yet
    *(WORD*)(entry + 20) = *(const WORD*)(record + 10);
    *(DWORD*)(entry + 28) = *(const DWORD*)record;
    *(WORD*)(entry + 32) = *(const WORD*)(record + 12);
    *(WORD*)(entry + 34) = *(const WORD*)(record + 14);
    *(DWORD*)(entry + 36) = *(const DWORD*)(record + 20) + *(const WORD*)(record + 28) + *(const DWORD*)(record + 42) + 30;
}

/// Adds the files of the central directory; false when a record is out of the ordinary.
template <typename Add> static bool AddCentralDirectory(const BYTE* factory, const BYTE* cdr, DWORD size, Add add) {
    static thread_local std::string name;
    const BYTE* end = cdr + size;
    DWORD archiveEnd = *(const DWORD*)(factory + FACTORY_CDR_OFFSET);
    for (const BYTE* record = cdr; record + 46 <= end && (*(const DWORD*)record == CDR_SIGNATURE || *(const DWORD*)record == ~CDR_SIGNATURE);) {
        WORD nameLength = *(const WORD*)(record + 28);
        const BYTE* next = record + 46 + nameLength + *(const WORD*)(record + 30) + *(const WORD*)(record + 32);
        bool stored = *(const WORD*)(record + 10) == 0;
        if (next > end || *(const DWORD*)(record + 42) > archiveEnd || (stored && *(const DWORD*)(record + 24) != *(const DWORD*)(record + 20))) {
            return false;
        }
        name.assign((const char*)record + 46, nameLength);
        for (char& c : name) {
            c = (BYTE)c < 0x80 ? (c >= 'A' && c <= 'Z' ? c + 32 : c) : (char)s_tolower(c);
        }
        BYTE entry[PAK_ENTRY_SIZE];
        FileEntryOf(record, entry);
        add(name.c_str(), entry);
        record = next;
    }
    return true;
}

static bool __fastcall zzBuildFileEntryMap(BYTE* factory) {
    PakTree* own = OwnPakTree(factory + s_pakTreeAt);
    if (!own || *(const int*)(factory + FACTORY_INIT_METHOD) >= 1 || factory[FACTORY_WANTS_MAP]) {
        return real_BuildFileEntryMap(factory);
    }
    if (!s_factorySeek(factory, *(const DWORD*)(factory + FACTORY_CDR_OFFSET), 0)) {
        return false;
    }
    DWORD size = *(const DWORD*)(factory + FACTORY_CDR_SIZE);
    if (!size) {
        return true;
    }
    BYTE* cdr = (BYTE*)s_pakAlloc(size);
    if (!cdr) {
        return real_BuildFileEntryMap(factory);
    }
    if (!s_factoryRead(factory, cdr, size)) {
        s_pakFree(cdr);
        return false;
    }
    bool plain = AddCentralDirectory(factory, cdr, size, [own](const char* path, const BYTE* entry) { AddPakFile(own, path, entry); });
    s_pakFree(cdr);
    if (plain) {
        return true;
    }
    // the client's function starts over and reports the record
    own->root.folders.clear();
    own->root.files.clear();
    own->lastFolderPath = {};
    own->lastFolder = nullptr;
    own->files = 0;
    return real_BuildFileEntryMap(factory);
}

/// What the search of the pak tree found: the tree's functions, and the function filling the names in 4.x with the calls it makes.
struct PakTreePlace {
    BYTE* addFile = nullptr;
    BYTE* prepare = nullptr;
    BYTE* add = nullptr;
    BYTE* count = nullptr;
    BYTE* size = nullptr;
    BYTE* write = nullptr;
    BYTE* makeCache = nullptr;
    int treeAt = 0;
    // whether the directory maker was found and the size and write functions walk the tree
    bool found = false;
    // the names: not the 4.x function (5.x lowers the case inline), its helpers missing, or not reading as expected
    enum { NamesNone, NamesMissing, NamesUnexpected, NamesFound } names = NamesNone;
    BYTE* build = nullptr;
    BYTE* seek = nullptr;
    BYTE* read = nullptr;
    BYTE* alloc = nullptr;
    BYTE* free = nullptr;
};

/// The 4.x function reads the size and offset of the central directory at FACTORY_CDR_SIZE and FACTORY_CDR_OFFSET; its first calls
/// are Seek, the allocator, Read, the release of the block and AddFileEntry, which checks FACTORY_INIT_METHOD and FACTORY_WANTS_MAP.
static void FindBuildFileEntryMap(HMODULE crySystem, PakTreePlace& place) {
    static const BYTE PROLOGUE[] = { 0x48, 0x81, 0xEC, 0xA8, 0x00, 0x00, 0x00, 0x8B, 0x51, FACTORY_CDR_OFFSET, 0x45, 0x33, 0xC0 };
    static const BYTE SIZE_READ[] = { 0x8B, 0x45, FACTORY_CDR_SIZE, 0x85, 0xC0 };
    static const BYTE INIT_CHECK[] = { 0x83, 0x7F, FACTORY_INIT_METHOD, 0x01 };
    static const BYTE MAP_CHECK[] = { 0x80, 0xBF, FACTORY_WANTS_MAP, 0x00, 0x00, 0x00, 0x00 };
    BYTE* build = place.build = NamedFunction(crySystem, "ZipDir::CacheFactory::BuildFileEntryMap");
    BYTE* seek = place.seek = NamedFunction(crySystem, "ZipDir::CacheFactory::Seek");
    BYTE* read = place.read = NamedFunction(crySystem, "ZipDir::CacheFactory::Read");
    if (!build || memcmp(build, PROLOGUE, sizeof(PROLOGUE)) || memcmp(build + 0x31, SIZE_READ, sizeof(SIZE_READ))) {
        return;
    }
    if (!seek || !read || !s_tolower) {
        place.names = PakTreePlace::NamesMissing;
        return;
    }
    BYTE* addFile = place.addFile;
    BYTE* addEnd = FunctionBodyEnd(addFile, 0x1000);
    auto inAdd = [&](const BYTE* bytes, size_t length) { return std::search(addFile, addEnd, bytes, bytes + length) != addEnd; };
    std::vector<CallSite> calls = CallsOf(build);
    if (calls.size() < 6 || calls[0].target != seek || calls[2].target != read || calls[4].target != addFile || calls[3].target != calls[5].target
        || !inAdd(INIT_CHECK, sizeof(INIT_CHECK)) || !inAdd(MAP_CHECK, sizeof(MAP_CHECK))) {
        place.names = PakTreePlace::NamesUnexpected;
        return;
    }
    place.names = PakTreePlace::NamesFound;
    place.alloc = calls[1].target;
    place.free = calls[3].target;
}

/// Must be called inside an open Detours transaction; returns the function when it is hooked.
static BYTE* ApplyBuildFileEntryMap(const PakTreePlace& place) {
    switch (place.names) {
        case PakTreePlace::NamesNone:
            // 5.x clients lower the case inline
            return nullptr;
        case PakTreePlace::NamesMissing:
            ModsLog("fast start: pak names not found (Seek %p, Read %p, tolower %p)", place.seek, place.read, s_tolower);
            return nullptr;
        case PakTreePlace::NamesUnexpected:
            ModsLog("fast start: pak names at %p do not read as expected", place.build);
            ReportStatus(InstallStatus::PatchFailed);
            return nullptr;
        case PakTreePlace::NamesFound:
            break;
    }
    s_factorySeek = (FactorySeek_t)place.seek;
    s_factoryRead = (FactoryRead_t)place.read;
    s_pakAlloc = (PakAlloc_t)place.alloc;
    s_pakFree = (PakFree_t)place.free;
    real_BuildFileEntryMap = (BuildFileEntryMap_t)place.build;
    AttachHook(&(PVOID&)real_BuildFileEntryMap, zzBuildFileEntryMap);
    return place.build;
}

/// Finds the functions of the tree from the factory that fills it: AddFileEntry adds to it and Prepare counts its files, both with the
/// tree at the same offset in the factory; of the callers of Prepare, the one making the directory then asks the tree for its size and
/// has it written, the one reading an archive for writing does neither.
static PakTreePlace LocatePakTree(HMODULE crySystem) {
    PakTreePlace place;
    BYTE* addFile = place.addFile = NamedFunction(crySystem, "ZipDir::CacheFactory::AddFileEntry");
    BYTE* prepare = place.prepare = NamedFunction(crySystem, "ZipDir::CacheFactory::Prepare");
    if (!addFile || !prepare) {
        return place;
    }
    std::vector<CallSite> adds = CallsOf(addFile);
    for (const CallSite& counting : CallsOf(prepare)) {
        for (const CallSite& adding : adds) {
            if (counting.thisAt > 0 && adding.thisAt == counting.thisAt && adding.target != counting.target) {
                place.treeAt = counting.thisAt;
                place.add = adding.target;
                place.count = counting.target;
            }
        }
    }
    for (BYTE* call : FindAllCallsTo(crySystem, prepare)) {
        BYTE* caller = FunctionStart(call);
        std::vector<BYTE*> treeCalls;
        for (const CallSite& site : caller ? CallsOf(caller) : std::vector<CallSite>()) {
            if (place.add && site.thisAt == place.treeAt) {
                treeCalls.push_back(site.target);
            }
        }
        if (treeCalls.size() == 2 && treeCalls[0] != treeCalls[1]) {
            place.makeCache = caller;
            place.size = treeCalls[0];
            place.write = treeCalls[1];
        }
    }
    // both walk the tree by calling themselves for every folder
    place.found = place.makeCache && Calls(place.size, place.size) && Calls(place.write, place.write);
    if (place.found) {
        FindBuildFileEntryMap(crySystem, place);
    }
    return place;
}

static void ApplyPakTree(const PakTreePlace& place) {
    if (!place.addFile || !place.prepare) {
        ModsLog("fast start: pak tree not found (AddFileEntry %p, Prepare %p)", place.addFile, place.prepare);
        return;
    }
    s_pakTreeAt = place.treeAt;
    if (!place.found) {
        ModsLog("fast start: pak tree not found (add %p, count %p at +%x, size %p, write %p, directory %p)", place.add, place.count, s_pakTreeAt,
            place.size, place.write, place.makeCache);
        return;
    }
    real_TreeAdd = (TreeAdd_t)place.add;
    real_TreeCount = (TreeCount_t)place.count;
    real_TreeSize = (TreeSize_t)place.size;
    real_TreeWrite = (TreeWrite_t)place.write;
    real_MakeCache = (MakeCache_t)place.makeCache;
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    AttachHook(&(PVOID&)real_TreeAdd, zzTreeAdd);
    AttachHook(&(PVOID&)real_TreeCount, zzTreeCount);
    AttachHook(&(PVOID&)real_TreeSize, zzTreeSize);
    AttachHook(&(PVOID&)real_TreeWrite, zzTreeWrite);
    AttachHook(&(PVOID&)real_MakeCache, zzMakeCache);
    BYTE* names = ApplyBuildFileEntryMap(place);
    ModsLog("fast start: pak tree at %p, %p, %p, %p (+%x), directory at %p, names at %p: %ld", place.add, place.count, place.size, place.write,
        s_pakTreeAt, place.makeCache, names, CommitHooks());
}

static void HookPakTree(HMODULE crySystem) {
    if (!real_MakeCache) {
        LocateThenApply(crySystem, LocatePakTree, ApplyPakTree);
    }
}

/// 5.x clients show their logos in CrySystem while the game data loads, in phases timed by the milliseconds since each began: shown
/// for 2000, faded in over 750, shown for 2000, then faded out over 750 once the loading thread is done (cmp ebx, 7D0h / 2EEh), 5.5
/// seconds even when the data is long loaded. All six limits become 0, so the logos end as soon as the loading does.
struct IntroLogosPlace {
    BYTE* function = nullptr;
    std::vector<CodePatch> limits;
};

static IntroLogosPlace LocateIntroLogos(HMODULE crySystem) {
    IntroLogosPlace place;
    std::vector<BYTE*> limits = FindAllPatterns(crySystem, "81 FB D0 07 00 00");
    std::vector<BYTE*> fades = FindAllPatterns(crySystem, "81 FB EE 02 00 00");
    limits.insert(limits.end(), fades.begin(), fades.end());
    BYTE* function = limits.size() == 6 ? FunctionStart(limits[0]) : nullptr;
    for (BYTE* limit : limits) {
        function = function && FunctionStart(limit) == function ? function : nullptr;
    }
    if (!function) {
        return place;
    }
    static const DWORD ZERO = 0;
    for (BYTE* limit : limits) {
        place.limits.push_back(PlanPatch(limit + 2, &ZERO, sizeof(ZERO)));
    }
    place.function = function;
    return place;
}

static void ApplyIntroLogos(const IntroLogosPlace& place) {
    if (!place.function) {
        // the client loads CrySystem.dll once more when it closes, without them
        return;
    }
    for (const CodePatch& limit : place.limits) {
        ApplyPatch(limit);
    }
    ModsLog("fast start: intro logos at %p", place.function);
}

static void SkipIntroLogos(HMODULE crySystem) {
    LocateThenApply(crySystem, LocateIntroLogos, ApplyIntroLogos);
}

typedef void*(__cdecl* CreateSystemInterface_t)(void* params);
static CreateSystemInterface_t real_CreateSystemInterface = nullptr;
static HMODULE s_crySystem = nullptr;

static void* __cdecl zzCreateSystemInterface(void* params) {
    RestoreImportCalls(s_crySystem);
    // the stubs of the packer leave a byte of junk after each call, so the calls of the functions read right only now
    HookPakTree(s_crySystem);
    return real_CreateSystemInterface(params);
}

/// The client calls CreateSystemInterface right after loading CrySystem, out of the loader lock and before anything else of it runs.
static void RestoreImportCallsBeforeUse(HMODULE crySystem) {
    if (real_CreateSystemInterface || !HasImportStubs(crySystem)) {
        return;
    }
    real_CreateSystemInterface = (CreateSystemInterface_t)FindExport(crySystem, "CreateSystemInterface");
    if (real_CreateSystemInterface) {
        s_crySystem = crySystem;
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        AttachHook(&(PVOID&)real_CreateSystemInterface, zzCreateSystemInterface);
        CommitHooks();
    }
}

void FastStartOnCrySystem(HMODULE crySystem) {
    InstallScope scope("fast_start");
    // the runtime is a side-by-side assembly, found by name only while CrySystem loads
    if (HMODULE runtime = GetModuleHandleW(L"MSVCR80.dll")) {
        s_tolower = (Tolower_t)GetProcAddress(runtime, "tolower");
    }
    RestoreImportCallsBeforeUse(crySystem);
    FastStartHookRuntime();
    ShortenClockMeasure(crySystem);
    HookZlib(crySystem);
    HookRawInflate(crySystem);
    if (!HasImportStubs(crySystem)) {
        HookPakTree(crySystem);
    }
    // only 5.x shows logos here; the search finds them by their six limits in one function
    if (g_modsConfig.fastStart == FAST_START_FULL) {
        SkipIntroLogos(crySystem);
    }
}
