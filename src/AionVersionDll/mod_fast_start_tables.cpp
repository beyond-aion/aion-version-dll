#include "mods.h"
#include "module_load.h"
#include <unordered_map>
#include <vector>
#include "install_report.h"
#include <memory>
#include "mod_fast_start.h"

// A table row sets each of its XML attributes through one function, which looks the attribute up in the field list of the table by
// comparing its name with every field name in turn. The fields are entries of 32 bytes ending with a type of 0: type at +0, name at
// +8; the function reads everything else from the entry it found, so starting the list at that entry gives the same result. The
// index of every name is kept per list, for lists in the image of Game.dll with plain ASCII names; anything else takes the long way.

typedef __int64(__fastcall* SetField_t)(const DWORD* fields, __int64 row, const wchar_t* name, const wchar_t* value);

static SetField_t real_SetField = nullptr;

static HMODULE s_game = nullptr;
static BYTE* s_gameEnd = nullptr;
static SRWLOCK s_fieldLock = SRWLOCK_INIT;

/// Open addressing over the field numbers of one list, by the hash of the folded name; the names stay in the list itself.
struct FieldIndex {
    bool handled = false;
    std::vector<int> slots; // field number or -1, a power of two in size
    std::vector<DWORD> hashes;
};

static std::unordered_map<const DWORD*, std::unique_ptr<FieldIndex>>* s_fieldIndex;

static const wchar_t* FieldName(const DWORD* fields, int i) {
    return *(const wchar_t* const*)(fields + 8 * i + 2);
}

/// FNV-1a over the name with A-Z folded; false at the first character outside ASCII.
static bool FoldedHash(const wchar_t* text, DWORD& hash) {
    hash = 2166136261u;
    for (; *text; text++) {
        unsigned c = *text;
        if (c >= 0x80) {
            return false;
        }
        hash = (hash ^ (c - 'A' < 26 ? c + 32 : c)) * 16777619u;
    }
    return true;
}

static bool EqualFolded(const wchar_t* a, const wchar_t* b) {
    for (;; a++, b++) {
        unsigned c = *a, d = *b;
        c = c - 'A' < 26 ? c + 32 : c;
        d = d - 'A' < 26 ? d + 32 : d;
        if (c != d) {
            return false;
        }
        if (!c) {
            return true;
        }
    }
}

/// Slot of the name, holding its field number, or the empty slot where it would go.
static size_t FindSlot(const DWORD* fields, const FieldIndex& index, const wchar_t* name, DWORD hash) {
    size_t mask = index.slots.size() - 1;
    for (size_t slot = hash & mask;; slot = (slot + 1) & mask) {
        int i = index.slots[slot];
        if (i < 0 || (index.hashes[slot] == hash && EqualFolded(FieldName(fields, i), name))) {
            return slot;
        }
    }
}

static std::unique_ptr<FieldIndex> IndexFields(const DWORD* fields) {
    auto index = std::make_unique<FieldIndex>();
    int count = 0;
    while (fields[8 * count]) {
        count++;
    }
    size_t size = 16;
    while (size < 2 * (size_t)count) {
        size *= 2;
    }
    index->slots.assign(size, -1);
    index->hashes.assign(size, 0);
    for (int i = 0; i < count; i++) {
        const wchar_t* name = FieldName(fields, i);
        DWORD hash;
        if (!name || !FoldedHash(name, hash)) {
            return index;
        }
        size_t slot = FindSlot(fields, *index, name, hash);
        // the first field of a name is the one the client finds
        if (index->slots[slot] < 0) {
            index->slots[slot] = i;
            index->hashes[slot] = hash;
        }
    }
    index->handled = true;
    return index;
}

static __int64 __fastcall zzSetField(const DWORD* fields, __int64 row, const wchar_t* name, const wchar_t* value) {
    DWORD hash;
    if (!value || !name || (BYTE*)fields < (BYTE*)s_game || (BYTE*)fields + 32 > s_gameEnd || !FoldedHash(name, hash)) {
        return real_SetField(fields, row, name, value);
    }
    // the tables load one after another, so the last list is nearly always the one asked again
    static thread_local const DWORD* lastFields = nullptr;
    static thread_local FieldIndex* lastIndex = nullptr;
    FieldIndex* index = lastFields == fields ? lastIndex : nullptr;
    if (!index) {
        AcquireSRWLockShared(&s_fieldLock);
        auto list = s_fieldIndex->find(fields);
        index = list != s_fieldIndex->end() ? list->second.get() : nullptr;
        ReleaseSRWLockShared(&s_fieldLock);
        if (!index) {
            AcquireSRWLockExclusive(&s_fieldLock);
            std::unique_ptr<FieldIndex>& entry = (*s_fieldIndex)[fields];
            if (!entry) {
                entry = IndexFields(fields);
            }
            index = entry.get();
            ReleaseSRWLockExclusive(&s_fieldLock);
        }
        lastFields = fields;
        lastIndex = index;
    }
    if (!index->handled) {
        return real_SetField(fields, row, name, value);
    }
    int found = index->slots[FindSlot(fields, *index, name, hash)];
    // a name the list does not have: the function would compare it with all fields and return 0
    return found < 0 ? 0 : real_SetField(fields + 8 * found, row, name, value);
}

/// What the searches of the faster loading in Game.dll found: each function to hook, null if the client lacks it, and the token tables
/// with their indexes by hash, built during the search.
struct FastLoadPlace {
    BYTE* fieldLookup = nullptr;
    BYTE* questTokens = nullptr;
    const wchar_t** questTokenTable = nullptr;
    const wchar_t** questTokenEnd = nullptr;
    std::unordered_map<DWORD, int> questTokenIndex;
    // the first token a hash cannot index, -1 if none
    int questBadToken = -1;
    BYTE* xmlTokens = nullptr;
    const wchar_t* const* xmlTokenTable = nullptr;
    const wchar_t* const* xmlTokenEnd = nullptr;
    std::unordered_map<DWORD, int> xmlTokenIndex;
    int xmlBadToken = -1;
    BYTE* heapReport = nullptr;
    BYTE* mapLowerBound = nullptr;
};

/// The lookup loop: mov rdx, [rax+8]; mov rcx, (name); call [_wcsicmp]; test eax, eax; jz found; ...; shl rax, 5; ...; cmp dword ptr [rax], 0; jnz loop
static void FindFieldLookup(HMODULE game, FastLoadPlace& place) {
    for (BYTE* loop : FindAllPatterns(game, "48 8B 50 08 ?? 8B ?? FF 15 ?? ?? ?? ?? 85 C0 74")) {
        BYTE* shift = nullptr;
        for (BYTE* p = loop + 17; p < loop + 0x30; p++) {
            if (p[0] == 0x48 && p[1] == 0xC1 && p[2] == 0xE0 && p[3] == 0x05) {
                shift = p;
                break;
            }
        }
        BYTE* end = nullptr;
        for (BYTE* p = shift; p && p < shift + 12; p++) {
            if (p[0] == 0x83 && p[1] == 0x38 && p[2] == 0x00 && p[3] == 0x75) {
                end = p;
                break;
            }
        }
        BYTE* function = end ? FunctionStart(loop) : nullptr;
        if (function) {
            place.fieldLookup = function;
            return;
        }
    }
}

static void ApplyFieldLookup(HMODULE game, const FastLoadPlace& place) {
    if (!place.fieldLookup) {
        ModsLog("fast start: field lookup not in this client, only 4.x has it");
        ReportStatus(InstallStatus::NotApplicable);
        return;
    }
    s_game = game;
    s_gameEnd = (BYTE*)game + ModuleImageSize(game);
    s_fieldIndex = new std::unordered_map<const DWORD*, std::unique_ptr<FieldIndex>>(); // never freed: no destructors at exit
    real_SetField = (SetField_t)place.fieldLookup;
    AttachHook(&(PVOID&)real_SetField, zzSetField);
    ModsLog("fast start: field lookup at %p", place.fieldLookup);
}

// 5.x clients write a report of the table heap after loading each table: they walk the whole heap with HeapWalk and print every
// region and large block. Without a file to write to (memstat.txt is written only on request), the lines go to the client's log
// system, which drops them, and the walks take about 40 % of loading all tables. A report without a file is skipped.

typedef void(__fastcall* HeapReport_t)(HANDLE heap, HANDLE file, char flag, const char* name);

static HeapReport_t real_HeapReport = nullptr;

static void __fastcall zzHeapReport(HANDLE heap, HANDLE file, char flag, const char* name) {
    static HANDLE logged = nullptr;
    if (file != logged) {
        logged = file;
        ModsLog("fast start: heap report to %p, file type %lu", file, GetFileType(file));
    }
    if (GetFileType(file) != FILE_TYPE_UNKNOWN) {
        real_HeapReport(heap, file, flag, name);
    }
}

/// lea rdx, "\t+ LBlock %03d : %p ~ %p (%d k)" in the report function
static void FindHeapReport(HMODULE game, FastLoadPlace& place) {
    static const char LINE[] = "\t+ LBlock %03d : %p ~ %p (%d k)";
    place.heapReport = FindFunctionByString(game, LINE);
}

static void ApplyHeapReport(const FastLoadPlace& place) {
    if (!place.heapReport) {
        ModsLog("fast start: heap report not found");
        return;
    }
    real_HeapReport = (HeapReport_t)place.heapReport;
    AttachHook(&(PVOID&)real_HeapReport, zzHeapReport);
    ModsLog("fast start: heap report at %p", place.heapReport);
}

// 5.x clients look keys up in a std::map<int, ...> while loading the tables with std::lower_bound over its iterators, which finds
// each middle by stepping through the tree node by node, so a lookup walks half the map. Over the whole map the result is the
// node a descent from the root finds; any other range takes the long way.

struct MapNode {
    MapNode* left;
    MapNode* parent;
    MapNode* right;
    char color;
    char isNil;
    int key;
};

typedef MapNode** (__fastcall* LowerBound_t)(MapNode** result, MapNode* first, MapNode* last, const int* key);

static LowerBound_t real_LowerBound = nullptr;

static MapNode** __fastcall zzLowerBound(MapNode** result, MapNode* first, MapNode* last, const int* key) {
    // the head of the map is the end; its left is the first node and its parent the root
    if (!last->isNil || first != last->left) {
        return real_LowerBound(result, first, last, key);
    }
    MapNode* found = last;
    for (MapNode* node = last->parent; !node->isNil;) {
        if (node->key < *key) {
            node = node->right;
        } else {
            found = node;
            node = node->left;
        }
    }
    *result = found;
    return result;
}

/// The middle of the range: call (advance); mov rcx, [rsp+40h]; cmp [rcx+1Ch], ebp; jge
static void FindMapLowerBound(HMODULE game, FastLoadPlace& place) {
    BYTE* middle = FindPattern(game, "E8 ?? ?? ?? ?? 48 8B 4C 24 40 39 69 1C 7D");
    place.mapLowerBound = middle ? FunctionStart(middle) : nullptr;
}

static void ApplyMapLowerBound(const FastLoadPlace& place) {
    if (!place.mapLowerBound) {
        ModsLog("fast start: map lookup not in this client, only 5.x has it");
        ReportStatus(InstallStatus::NotApplicable);
        return;
    }
    real_LowerBound = (LowerBound_t)place.mapLowerBound;
    AttachHook(&(PVOID&)real_LowerBound, zzLowerBound);
    ModsLog("fast start: map lookup at %p", place.mapLowerBound);
}

/// Once Game.dll is loaded, which needs the runtime, and before it loads the game data.
// 4.x clients look the name of every attribute of every quest up in a table of some 3700 tokens, comparing it with one token after
// another: a fifth of the compares while the game data loads. The loop now starts at the token of that name, found by a hash.
extern "C" {
UINT64 g_questTokenTable = 0;
UINT64 g_questTokenEnd = 0;
UINT64 g_questTokenLoop = 0;
void QuestTokenStub();
}
static std::unordered_map<DWORD, int> s_questTokens;

/// Where the loop starts: at the token of that name, at the last token when there is none (it differs, so the loop ends as without
/// a match), or at the first when the hash cannot tell.
extern "C" int QuestTokenIndex(const wchar_t* name) {
    const wchar_t** tokens = (const wchar_t**)g_questTokenTable;
    int count = (int)((g_questTokenEnd - g_questTokenTable) / sizeof(void*));
    DWORD hash;
    if (!name || !FoldedHash(name, hash)) {
        return 0;
    }
    auto found = s_questTokens.find(hash);
    if (found == s_questTokens.end()) {
        return count - 1;
    }
    return EqualFolded(name, tokens[found->second]) ? found->second : 0;
}

/// mov ebx, ebp; lea r12, <tokens>; lea r14, <end>; nops; loop: mov rdx, [r12]; mov rcx, r13; call [_wcsicmp]; test eax, eax; jz found;
/// add r12, 8; add ebx, 1; cmp r12, r14; jl loop
static void FindQuestTokens(HMODULE game, FastLoadPlace& place) {
    BYTE* p = FindPattern(game, "8B DD 4C 8D 25 ?? ?? ?? ?? 4C 8D 35 ?? ?? ?? ?? 66 66 90 66 66 66 90 49 8B 14 24 49 8B CD FF 15 ?? ?? ?? ?? 85 C0 74 ?? 49 83 C4 08 83 C3 01 4D 3B E6 7C E3");
    if (!p) {
        return;
    }
    place.questTokens = p;
    const wchar_t** tokens = place.questTokenTable = (const wchar_t**)ResolveRip(p + 5, p + 9);
    const wchar_t** end = place.questTokenEnd = (const wchar_t**)ResolveRip(p + 12, p + 16);
    for (const wchar_t** token = tokens; token < end; token++) {
        DWORD hash;
        if (!*token || !FoldedHash(*token, hash)) {
            place.questBadToken = (int)(token - tokens);
            place.questTokenIndex.clear();
            return;
        }
        // the first of equal names, as the loop finds it
        place.questTokenIndex.emplace(hash, (int)(token - tokens));
    }
}

static void ApplyQuestTokens(FastLoadPlace& place) {
    BYTE* p = place.questTokens;
    if (!p) {
        ModsLog("fast start: quest tokens not in this client, only 4.x has them");
        ReportStatus(InstallStatus::NotApplicable);
        return;
    }
    if (place.questBadToken >= 0) {
        ModsLog("fast start: quest token %d not in ASCII", place.questBadToken);
        return;
    }
    s_questTokens = std::move(place.questTokenIndex);
    g_questTokenTable = (UINT64)place.questTokenTable;
    g_questTokenEnd = (UINT64)place.questTokenEnd;
    g_questTokenLoop = (UINT64)(p + 23);
    static PVOID patched = p + 2;
    AttachHook(&patched, QuestTokenStub);
    ModsLog("fast start: quest tokens at %p, %d of them", p, (int)(place.questTokenEnd - place.questTokenTable));
}

// 5.x clients turn every name in the XML of a table into its number with XML_GetToken, which compares it with one of some 4500 tokens
// after another: a quarter of rebuilding the tables. The number of a name is found by a hash; a name the hash does not find goes to
// the function, which reports it.
typedef int(__fastcall* XmlToken_t)(const wchar_t* name);
static XmlToken_t real_XmlToken = nullptr;
static const wchar_t* const* s_xmlTokens;
static std::unordered_map<DWORD, int> s_xmlTokenIndex;

static int __fastcall zzXmlToken(const wchar_t* name) {
    DWORD hash;
    if (!name || !FoldedHash(name, hash)) {
        return real_XmlToken(name);
    }
    auto found = s_xmlTokenIndex.find(hash);
    return found != s_xmlTokenIndex.end() && EqualFolded(name, s_xmlTokens[found->second]) ? found->second : real_XmlToken(name);
}

/// push rsi; sub rsp, 20h; mov rsi, rcx; test rcx, rcx; jnz ...; ...; lea rbx, <tokens>; lea rbp, <end>; loop: mov rdx, [rbx];
/// mov rcx, rsi; call [_wcsicmp]; ...; the report of an unknown token uses its text (5.x: only there)
static void FindXmlTokens(HMODULE game, FastLoadPlace& place) {
    static const wchar_t TEXT[] = L"%s, XML_GetToken() : undefined token \"%s\"";
    // the first function that loads the text, checked below: 4.x clients load it in three
    const void* text = FindBytes(game, TEXT, sizeof(TEXT));
    BYTE* lea = text ? FindLeaTo(game, text) : nullptr;
    BYTE* p = lea ? FunctionStart(lea) : nullptr;
    if (!p || memcmp(p, "\x40\x56\x48\x83\xEC\x20\x48\x8B\xF1\x48\x85\xC9", 12) != 0 || memcmp(p + 0x2A, "\x48\x8D\x1D", 3) != 0
        || memcmp(p + 0x31, "\x48\x8D\x2D", 3) != 0) {
        return;
    }
    place.xmlTokens = p;
    const wchar_t* const* tokens = place.xmlTokenTable = (const wchar_t* const*)ResolveRip(p + 0x2D, p + 0x31);
    const wchar_t* const* end = place.xmlTokenEnd = (const wchar_t* const*)ResolveRip(p + 0x34, p + 0x38);
    for (const wchar_t* const* token = tokens; token < end; token++) {
        DWORD hash;
        if (!*token || !FoldedHash(*token, hash)) {
            place.xmlBadToken = (int)(token - tokens);
            place.xmlTokenIndex.clear();
            return;
        }
        // the first of equal names, as the loop finds it
        place.xmlTokenIndex.emplace(hash, (int)(token - tokens));
    }
}

static void ApplyXmlTokens(FastLoadPlace& place) {
    BYTE* p = place.xmlTokens;
    if (!p) {
        ModsLog("fast start: XML tokens not in this client, only 5.x has them");
        ReportStatus(InstallStatus::NotApplicable);
        return;
    }
    if (place.xmlBadToken >= 0) {
        ModsLog("fast start: XML token %d not in ASCII", place.xmlBadToken);
        return;
    }
    s_xmlTokenIndex = std::move(place.xmlTokenIndex);
    s_xmlTokens = place.xmlTokenTable;
    real_XmlToken = (XmlToken_t)p;
    AttachHook(&(PVOID&)real_XmlToken, zzXmlToken);
    ModsLog("fast start: XML tokens at %p, %d of them", p, (int)(place.xmlTokenEnd - place.xmlTokenTable));
}

void InstallFastLoad(HMODULE game) {
    FastLoadPlace place;
    {
        LocatePhase phase;
        FindFieldLookup(game, place);
        FindQuestTokens(game, place);
        FindXmlTokens(game, place);
        FindHeapReport(game, place);
        FindMapLowerBound(game, place);
    }
    ApplyFieldLookup(game, place);
    ApplyQuestTokens(place);
    ApplyXmlTokens(place);
    ApplyHeapReport(place);
    ApplyMapLowerBound(place);
}
