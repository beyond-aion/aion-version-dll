#include "mods.h"
#include <algorithm>
#include <array>
#include <stdio.h>
#include <string.h>
#include <utility>

// 4.x clients can keep their largest tables (items, skills, NPCs, spawns and more) as binary dumps in Data/Dump and load them from there on
// the next start instead of parsing the XML. The switch is a developer flag of the game object (CXGame) that no code sets; with it, the table
// loop calls table->vt[4](dump file) and falls back to the XML (vt[0]) when that fails, writes the dump with vt[3] after loading, and the exit
// marks the dumps as complete (NormalStop in Data/Dump/TableDump.ini). A dump records the sizes of its source files and is dropped when they
// change, as after a patch. The mod sets the flag. In 4.8 the writers look for their source files in the region folder (data\USA\items\...),
// where the client has none, so the item, skill and NPC dumps are never written; while a dump method runs, a file of data\<region>\ that
// CryPak cannot open is tried without the region folder, which is where the tables load it from.

namespace {

struct DumpPlaces {
    CodePatch flag;            // the developer flag, CXGame+0x53C0 in 4.8 (4.6 +0x44B8, 4.9 +0x5488)
    void** system = nullptr;   // where the game keeps ISystem*
    std::vector<void*> methods; // the dump writers and readers (vt[3], vt[4]) of the tables that have a dump file
    bool hasDumps = false;      // 5.x clients have none, they keep their tables in tables.dmp by themselves
};

/// The game object: its callers pass it to GameClient::EndTempPanning (lea rcx, [game]; call).
BYTE* FindGame(HMODULE game) {
    BYTE* end = FindPattern(game, "48 83 EC 18 83 B9 ?? ?? 00 00 00 74 ?? 8B 81 ?? ?? 00 00");
    if (!end) {
        return nullptr;
    }
    for (BYTE* call : FindAllCallsTo(game, end)) {
        if (call[-7] == 0x48 && call[-6] == 0x8D && call[-5] == 0x0D) {
            return ResolveRip(call - 4, call);
        }
    }
    return nullptr;
}

/// The offset of the flag: CXGame::Init compares it right before it loads Data/Dump/StringTable.dat (cmp [reg+flag], r8 or cmp byte [reg+flag], 0).
DWORD FindFlagOffset(HMODULE game) {
    static const char NAME[] = "Data/Dump/StringTable.dat";
    BYTE* name = FindBytes(game, NAME, sizeof(NAME));
    if (!name) {
        return 0;
    }
    for (BYTE* lea : FindAllLeasTo(game, name)) {
        for (BYTE* p = lea - 24; p < lea - 5; p++) {
            if (p[0] == 0x38 && (p[1] & 0xC0) == 0x80) {
                return *(DWORD*)(p + 2);
            }
            if (p[0] == 0x80 && (p[1] & 0xF8) == 0xB8 && p[6] == 0) {
                return *(DWORD*)(p + 2);
            }
        }
    }
    return 0;
}

/// mov rcx, [system]; ... mov rax, [rcx]; call [rax+190h] (ISystem::GetIPak) in a dump method.
void** FindSystem(BYTE* method) {
    static const BYTE GET_PAK[] = { 0x48, 0x8B, 0x01, 0xFF, 0x90, 0x90, 0x01, 0x00, 0x00 };
    for (BYTE* p = method; p < method + 0x800; p++) {
        if (p[0] != 0x48 || p[1] != 0x8B || p[2] != 0x0D) {
            continue;
        }
        for (BYTE* q = p + 7; q < p + 40; q++) {
            if (memcmp(q, GET_PAK, sizeof(GET_PAK)) == 0) {
                return (void**)ResolveRip(p + 3, p + 7);
            }
        }
    }
    return nullptr;
}

DumpPlaces Locate(HMODULE game) {
    DumpPlaces places;
    static const char TABLE_DUMP_INI[] = "Data/Dump/TableDump.ini";
    places.hasDumps = FindBytes(game, TABLE_DUMP_INI, sizeof(TABLE_DUMP_INI)) != nullptr;
    if (!places.hasDumps) {
        return places;
    }
    BYTE* gameObject = FindGame(game);
    DWORD flag = FindFlagOffset(game);
    if (!gameObject || !flag || flag > 0x10000) {
        return places;
    }
    // the records of the table loop: { table, name, dump file, ... }; the dump file names are in the image, the records point at them
    static const char* const FILES[] = { "it", "st", "nt", "rt", "spt", "rbt", "ctt", "hlt", "hopt", "hot", "hpt", "hbt", "htt", "hat", "sit", "que" };
    for (const char* file : FILES) {
        char path[32];
        sprintf_s(path, "Data/Dump/%s.dat", file);
        BYTE* name = FindBytes(game, path, strlen(path) + 1);
        if (!name) {
            continue;
        }
        BYTE* pointer = FindBytes(game, &name, sizeof(name));
        if (!pointer || !InModule(game, pointer - 0x10, sizeof(void*))) {
            continue;
        }
        BYTE* table = *(BYTE**)(pointer - 0x10);
        if (!InModule(game, table, sizeof(void*))) {
            continue;
        }
        void** vtable = *(void***)table;
        if (!InModule(game, vtable, 5 * sizeof(void*))) {
            continue;
        }
        for (int slot : { 3, 4 }) {
            void* method = vtable[slot];
            if (InModule(game, method, 1) && std::find(places.methods.begin(), places.methods.end(), method) == places.methods.end()) {
                places.methods.push_back(method);
            }
        }
        if (!places.system) {
            places.system = FindSystem((BYTE*)vtable[3]);
        }
    }
    if (!places.methods.empty() && places.system) {
        static const BYTE ON = 1;
        places.flag = PlanPatch(gameObject + flag, &ON, 1);
    }
    return places;
}

typedef __int64(__fastcall* DumpMethod_t)(__int64 table, const char* file);
typedef void*(__fastcall* FOpen_t)(void* pak, const char* name, const char* mode, unsigned flags);

void** s_system;
FOpen_t real_FOpen;
int s_retries;

void* __fastcall zzFOpen(void* pak, const char* name, const char* mode, unsigned flags) {
    void* file = real_FOpen(pak, name, mode, flags);
    if (!file && name && _strnicmp(name, "data\\", 5) == 0) {
        const char* rest = strchr(name + 5, '\\');
        if (rest) {
            char plain[MAX_PATH];
            if (sprintf_s(plain, "data%s", rest) > 0) {
                file = real_FOpen(pak, plain, mode, flags);
                if (s_retries++ < 20) {
                    ModsLog("table dump: %s opened as %s: %s", name, plain, file ? "yes" : "no");
                }
            }
        }
    }
    return file;
}

/// Puts the retrying FOpen into the vtable of CryPak (ISystem::GetIPak at vt+400, ICryPak::FOpen at vt+120) for the time of a dump method.
void HookPak(bool on) {
    void* system = *s_system;
    if (!system) {
        return;
    }
    typedef void*(__fastcall* GetIPak_t)(void*);
    void* pak = (*(GetIPak_t**)system)[400 / 8](system);
    if (!pak) {
        return;
    }
    void** slot = &(*(void***)pak)[120 / 8];
    DWORD old;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
        return;
    }
    if (on && *slot != (void*)zzFOpen) {
        real_FOpen = (FOpen_t)*slot;
        *slot = (void*)zzFOpen;
    } else if (!on && *slot == (void*)zzFOpen) {
        *slot = (void*)real_FOpen;
    }
    VirtualProtect(slot, sizeof(void*), old, &old);
}

constexpr int MAX_METHODS = 32;
DumpMethod_t s_real[MAX_METHODS];

template <int N>
__int64 __fastcall zzDumpMethod(__int64 table, const char* file) {
    HookPak(true);
    __int64 result = s_real[N](table, file);
    HookPak(false);
    ModsLog("table dump: %s %s", file ? file : "?", result ? "done" : "failed");
    return result;
}

template <int... N>
constexpr std::array<PVOID, sizeof...(N)> MakeHooks(std::integer_sequence<int, N...>) {
    return { (PVOID)&zzDumpMethod<N>... };
}

void Apply(const DumpPlaces& places) {
    if (!places.hasDumps) {
        ModsLog("table dump: the client has no table dumps of this kind (5.x keeps tables.dmp by itself)");
        ReportStatus(InstallStatus::NotApplicable);
        return;
    }
    if (!places.flag) {
        ModsLog("table dump: not found (system %p, %zu dump methods)", places.system, places.methods.size());
        ReportStatus(InstallStatus::NotFound);
        return;
    }
    static const auto HOOKS = MakeHooks(std::make_integer_sequence<int, MAX_METHODS>());
    s_system = places.system;
    size_t count = min(places.methods.size(), (size_t)MAX_METHODS);
    for (size_t i = 0; i < count; i++) {
        s_real[i] = (DumpMethod_t)places.methods[i];
        AttachHook(&(PVOID&)s_real[i], HOOKS[i]);
    }
    bool ok = ApplyPatch(places.flag);
    ModsLog("table dump: flag at %p, ISystem at %p, %zu dump methods%s", places.flag.at, places.system, count, ok ? "" : ", flag not set");
}

} // namespace

void InstallTableDump(HMODULE game) {
    LocateThenApply(game, Locate, Apply);
}
