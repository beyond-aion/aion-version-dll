#include "mods.h"
#include <algorithm>
#include <array>
#include <stdio.h>
#include <string.h>
#include <string>
#include <utility>

// 4.x clients can keep their largest tables (items, skills, NPCs, spawns and more) as binary dumps in Data/Dump and load them from there on
// the next start instead of parsing the XML. The switch is a developer flag of the game object (CXGame) that no code sets; with it, the table
// loop calls table->vt[4](dump file) and falls back to the XML (vt[0]) when that fails, writes the dump with vt[3] after loading, and the exit
// marks the dumps as complete (NormalStop in Data/Dump/TableDump.ini). A dump records the sizes of its source files and is dropped when they
// change, as after a patch. The mod sets the flag. In 4.8 the writers look for their source files in the region folder (data\USA\items\...),
// where the client has none, so the item, skill and NPC dumps are never written; while a dump method runs, a file of data\<region>\ that
// CryPak cannot open is tried without the region folder, which is where the tables load it from.
// The client only compares the sizes of the source files, so an edit that keeps a file's size (a 10 made 20) would leave the old table in
// use. The mod therefore keeps a stamp of the size and time of every .pak under Data and L10N next to the dumps and deletes the dumps
// when it changes, so that they are written anew.

namespace {

struct DumpPlaces {
    CodePatch flag;            // the developer flag, CXGame+0x53C0 in 4.8 (4.6 +0x44B8, 4.9 +0x5488)
    void** system = nullptr;   // where the game keeps ISystem*
    std::vector<void*> methods; // the dump writers and readers (vt[3], vt[4]) of the tables that have a dump file
    std::vector<bool> isWriter; // whether the method at the same index is a writer (vt[3]) or a reader (vt[4])
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
                places.isWriter.push_back(slot == 3);
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

bool s_dumpActive = false;
volatile bool s_writerSeen = false;
bool s_isWriter[MAX_METHODS] = {};
std::wstring s_flagsIni;   // Data\Dump\TableDump.ini in 4.x, data\dump\config.ini in 5.x
bool s_digitFlags = false; // 5.x writes 1/0, 4.x writes true/false
void WriterStarted();

template <int N>
__int64 __fastcall zzDumpMethod(__int64 table, const char* file) {
    HookPak(true);
    __int64 result = s_real[N](table, file);
    HookPak(false);
    if (s_isWriter[N]) {
        WriterStarted();
    }
    ModsLog("table dump: %s %s", file ? file : "?", result ? "done" : "failed");
    return result;
}

template <int... N>
constexpr std::array<PVOID, sizeof...(N)> MakeHooks(std::integer_sequence<int, N...>) {
    return { (PVOID)&zzDumpMethod<N>... };
}

/// FNV-1a over the relative path, size and last write time of every .pak below folder.
void StampPaks(const std::wstring& folder, const std::wstring& relative, unsigned long long& hash, int& count) {
    WIN32_FIND_DATAW found;
    HANDLE search = FindFirstFileW((folder + L"\\*").c_str(), &found);
    if (search == INVALID_HANDLE_VALUE) {
        return;
    }
    do {
        std::wstring name = found.cFileName;
        if (name == L"." || name == L"..") {
            continue;
        }
        if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            StampPaks(folder + L"\\" + name, relative + name + L"\\", hash, count);
            continue;
        }
        if (name.size() < 4 || _wcsicmp(name.c_str() + name.size() - 4, L".pak") != 0) {
            continue;
        }
        std::wstring key = relative + name;
        auto mix = [&](const void* data, size_t size) {
            for (size_t i = 0; i < size; i++) {
                hash = (hash ^ ((const BYTE*)data)[i]) * 0x100000001B3ull;
            }
        };
        mix(key.data(), key.size() * sizeof(wchar_t));
        mix(&found.nFileSizeLow, sizeof(found.nFileSizeLow));
        mix(&found.nFileSizeHigh, sizeof(found.nFileSizeHigh));
        mix(&found.ftLastWriteTime, sizeof(found.ftLastWriteTime));
        count++;
    } while (FindNextFileW(search, &found));
    FindClose(search);
}

/// The folder of the client, the parent of bin64.
std::wstring ClientRoot() {
    wchar_t path[MAX_PATH];
    DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring root(path, length);
    for (int up = 0; up < 2 && root.rfind(L'\\') != std::wstring::npos; up++) {
        root.resize(root.rfind(L'\\'));
    }
    return root;
}

/// Data\Dump\TableDump.ini (4.x) and data\dump\config.ini (5.x): where the client marks whether the table dumps on disk are complete.
std::wstring FourXIniPath() {
    return ClientRoot() + L"\\Data\\Dump\\TableDump.ini";
}

std::wstring FiveXIniPath() {
    return ClientRoot() + L"\\data\\dump\\config.ini";
}

/// The value of a key in the flags ini, as text: "true"/"1", "8" and so on.
std::string ReadIniValue(const char* name) {
    if (s_flagsIni.empty()) {
        return std::string();
    }
    HANDLE file = CreateFileW(s_flagsIni.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return std::string();
    }
    char text[1024] = {};
    DWORD read = 0;
    ReadFile(file, text, sizeof(text) - 1, &read, nullptr);
    CloseHandle(file);
    const char* key = strstr(text, name);
    if (!key) {
        return std::string();
    }
    key += strlen(name);
    while (*key == ' ' || *key == '\t') {
        key++;
    }
    if (*key != '=') {
        return std::string();
    }
    key++;
    while (*key == ' ' || *key == '\t') {
        key++;
    }
    const char* end = key;
    while (*end && *end != '\r' && *end != '\n' && *end != ' ' && *end != '\t') {
        end++;
    }
    return std::string(key, end);
}

bool ReadNormalStop() {
    std::string value = ReadIniValue("NormalStop");
    return _stricmp(value.c_str(), "true") == 0 || value == "1";
}

void WriteNormalStop(bool complete) {
    if (s_flagsIni.empty()) {
        return;
    }
    const wchar_t* text = s_digitFlags ? (complete ? L"1" : L"0") : (complete ? L"true" : L"false");
    WritePrivateProfileStringW(L"Game", L"NormalStop", text, s_flagsIni.c_str());
    // passing a null section flushes the cache of the profile API to the file
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, s_flagsIni.c_str());
}

/// A dump writer runs only for a dump that was missing or rejected, so the flag has to stay false until the loop is over.
void WriterStarted() {
    s_writerSeen = true;
    if (s_dumpActive) {
        WriteNormalStop(false);
    }
}

/// The client writes NormalStop=false when it starts and true only on a normal exit, so a client killed early makes the next start
/// rebuild the dumps even though they were complete. Once the dumps of the last run are known to be complete, put the flag back
/// right after the client clears it; a writer, should one run after all, takes the flag down again until the loop is over.
DWORD WINAPI NormalStopWatch(LPVOID) {
    for (int i = 0; i < 300; i++) {
        if (s_writerSeen) {
            return 0;
        }
        if (!ReadNormalStop()) {
            WriteNormalStop(true);
            ModsLog("table dump: put NormalStop back after the client started, the dumps were complete");
            return 0;
        }
        Sleep(50);
    }
    return 0;
}

/// Deletes the dumps in Data\Dump when the game data changed since they were written. Returns the number of deleted dumps.
int DropStaleDumps() {
    std::wstring root = ClientRoot();
    unsigned long long hash = 0xCBF29CE484222325ull;
    int count = 0;
    StampPaks(root + L"\\Data", L"Data\\", hash, count);
    StampPaks(root + L"\\L10N", L"L10N\\", hash, count);
    std::wstring dump = root + L"\\Data\\Dump";
    std::wstring stampFile = dump + L"\\version.dll.stamp";
    char stamp[32];
    sprintf_s(stamp, "%016llX %d", hash, count);
    char old[32] = {};
    HANDLE file = CreateFileW(stampFile.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        DWORD read = 0;
        ReadFile(file, old, sizeof(old) - 1, &read, nullptr);
        CloseHandle(file);
    }
    if (strcmp(old, stamp) == 0) {
        return 0;
    }
    int deleted = 0;
    WIN32_FIND_DATAW found;
    HANDLE search = FindFirstFileW((dump + L"\\*.dat").c_str(), &found);
    if (search != INVALID_HANDLE_VALUE) {
        do {
            deleted += DeleteFileW((dump + L"\\" + found.cFileName).c_str()) ? 1 : 0;
        } while (FindNextFileW(search, &found));
        FindClose(search);
    }
    CreateDirectoryW(dump.c_str(), nullptr);
    file = CreateFileW(stampFile.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(file, stamp, (DWORD)strlen(stamp), &written, nullptr);
        CloseHandle(file);
    }
    ModsLog("table dump: %d .pak files changed since the dumps were written (stamp %s, was %s), %d dumps deleted", count, stamp, old[0] ? old : "none",
        deleted);
    return deleted;
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
    bool trusted = false;
    if (!g_dryRun) {
        s_flagsIni = FourXIniPath();
        s_digitFlags = false;
        // read the flag before the client's own start writes it: true here means the dumps of the last run are complete
        trusted = ReadNormalStop();
        int deleted = DropStaleDumps();
        trusted = trusted && deleted == 0;
        s_dumpActive = true;
    }
    static const auto HOOKS = MakeHooks(std::make_integer_sequence<int, MAX_METHODS>());
    s_system = places.system;
    size_t count = min(places.methods.size(), (size_t)MAX_METHODS);
    for (size_t i = 0; i < count; i++) {
        s_real[i] = (DumpMethod_t)places.methods[i];
        s_isWriter[i] = i < places.isWriter.size() && places.isWriter[i];
        AttachHook(&(PVOID&)s_real[i], HOOKS[i]);
    }
    bool ok = ApplyPatch(places.flag);
    ModsLog("table dump: flag at %p, ISystem at %p, %zu dump methods%s", places.flag.at, places.system, count, ok ? "" : ", flag not set");
    if (s_dumpActive && trusted) {
        CreateThread(nullptr, 0, NormalStopWatch, nullptr, 0, nullptr);
        ModsLog("table dump: the dumps were complete, keeping NormalStop true after the client's start");
    }
}

} // namespace

/// The table loop is over (the client is at its login screen): the dumps on disk are complete, so a later early exit or crash must not
/// make the next start rebuild them.
void MarkDumpsComplete() {
    if (!s_dumpActive) {
        return;
    }
    if (!ReadNormalStop()) {
        WriteNormalStop(true);
        ModsLog("table dump: the table loop is over, NormalStop=true");
    }
}

void InstallTableDump(HMODULE game) {
    LocateThenApply(game, Locate, Apply);
    if (g_dryRun) {
        return;
    }
    // 4.x went through Apply above; 5.x keeps its tables in data\dump\tables.dmp and has its own flags ini
    std::wstring four = FourXIniPath();
    std::wstring five = FiveXIniPath();
    if (GetFileAttributesW(four.c_str()) != INVALID_FILE_ATTRIBUTES || GetFileAttributesW(five.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return;
    }
    s_flagsIni = five;
    s_digitFlags = true;
    s_dumpActive = true;
    bool normalStop = ReadNormalStop();
    bool valid = ReadIniValue("ValidTableDump") == "1";
    bool bits = ReadIniValue("BitSize") == "8";
    WIN32_FILE_ATTRIBUTE_DATA dump = {};
    bool hasDump = GetFileAttributesExW((ClientRoot() + L"\\data\\dump\\tables.dmp").c_str(), GetFileExInfoStandard, &dump) != FALSE
        && (dump.nFileSizeHigh > 0 || dump.nFileSizeLow > 0);
    ModsLog("table dump: 5.x tables.dmp, NormalStop=%d ValidTableDump=%d BitSize=%s tables.dmp=%d", normalStop, valid,
        ReadIniValue("BitSize").c_str(), hasDump);
    if (normalStop && valid && bits && hasDump) {
        CreateThread(nullptr, 0, NormalStopWatch, nullptr, 0, nullptr);
        ModsLog("table dump: the 5.x dump was complete, keeping NormalStop=1 after the client's start");
    }
}
