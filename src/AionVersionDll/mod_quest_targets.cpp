#include "mods.h"
#include "module_load.h"
#include <map>
#include <stdio.h>
#include <unordered_map>
#include <vector>
#include <detours/detours.h>

// First entry of the table of graphic characters for quest icons (givable, working, finished per quest kind).
static const char FIRST_QUEST_ICON[] = "quest_givable";
static constexpr int NO_ICON = 14;
static constexpr int LINE_LENGTH = 256;
static constexpr int MAX_ENTRIES = 128;

// Later clients put this effect on gatherable objects (ids 700000..729999) a quest in progress needs; older ones only have the
// quest markers, whose effect names come from a table indexed by quest kind and marker state.
static const char QUEST_TARGET_EFFECT[] = "sys_UIfx.Quest.target";
static const char FIRST_MARKER_EFFECT[] = "sys_quest.quest.bag_blue";
static constexpr int FIRST_GATHERABLE = 700000;
static constexpr int LAST_GATHERABLE = 729999;
// a marker state the marker function accepts, and the slot of the name table it reads for it with quest kind 0
static constexpr int BORROWED_STATE = 2;
// The point of the quest target effect runs along a circle drawn flat. Later effect data turns it upright by 90 degrees about Y
// in a field older clients lack, so there the point swings sideways instead of up and down.
static const char TARGET_POINT_EFFECT[] = "sys_UIfx.Quest.point";
static constexpr int EFFECT_NAME_OFFSET = 0x20;
static constexpr int EFFECT_PARAMS_OFFSET = 0x4C;
static constexpr int PARAMS_ANGLES_OFFSET = 1308;
static constexpr float TARGET_POINT_TURN = 90.0f;

/// Output of the quest monster table query: a vector the query appends table entries to while it has room.
struct EntryList {
    void* allocator;
    BYTE** first;
    BYTE** last;
    BYTE** end;
};

typedef void(__fastcall* QueryQuestMonsters_t)(void* table, int questId, int step, EntryList* entries);
typedef int(__fastcall* QuestIconType_t)(void* questManager, int questId);
typedef const wchar_t*(__fastcall* QuestIcon_t)(int type);

static QueryQuestMonsters_t s_queryQuestMonsters = nullptr;
static void* s_questMonsterTable = nullptr;
static QuestIconType_t s_questIconType = nullptr;
static QuestIcon_t s_questIcon = nullptr;
// quests in progress: their number, followed by 16 bytes per quest (id, state byte, step)
static volatile int* s_questCount = nullptr;

// The client asks two more things before it counts an NPC as needed, as it does for the glow over gatherable objects. A quest waits
// for a talk when it has nothing left to collect (the items of its collecting step are all in the inventory, or it has no such step)
// and an NPC script answers its state and step, like the NPC that takes the items: then none of its targets count. And 4.8 drops the
// entries whose item the inventory already holds enough of; 4.6 has no such check. These functions keep their quest manager in
// globals, as the icon type does, and ignore the pointer.
typedef int(__fastcall* CollectionDone_t)(void* questManager, int questId);
typedef int(__fastcall* QuestTalks_t)(int questId, int state, int step, void** scripts);
typedef int(__fastcall* ItemCollected_t)(void* questManager, int questId, int itemId);

static CollectionDone_t s_collectionDone = nullptr;
// the NPC scripts by quest, null until loaded
static void** s_questTalkTable = nullptr;
static QuestTalks_t s_questTalks = nullptr;
static ItemCollected_t s_itemCollected = nullptr;
// the scripts the talk lookup writes, at most 32 (the client gives it 264 bytes)
static constexpr int MAX_TALKS = 33;
// item counts change without a quest changing, so the conditions are asked again this often
static constexpr ULONGLONG RECHECK_MS = 500;

typedef void(__fastcall* SetQuestMarker_t)(BYTE* npc, int questId, int state, int kind);
typedef void*(__fastcall* FindEffect_t)(void* effects, const char* name);

static SetQuestMarker_t real_SetQuestMarker = nullptr;
static FindEffect_t s_findEffect = nullptr;
static const char** s_markerEffects = nullptr;
static INT32 s_effectsOffset = 0;
// where the marker function keeps the quest, the marker state and the quest kind it was given (4.x 0x1D0, 3.9 0x1C8)
static INT32 s_markerStateOffset = -1;
// where an NPC keeps its id (4.x 0x34, 3.9 0x30)
static int s_npcIdOffset = -1;

extern "C" void* g_questIconResume = nullptr;
extern "C" INT64 g_questIconLines = 0;
extern "C" void QuestIconStubR15R12();
extern "C" void QuestIconStubR14Rbp();

/// An entry of the quest monster table for the current step of a quest: the item it drops or gives, if any, and the NPCs.
struct TargetEntry {
    int item;
    std::vector<int> npcs;
};

struct LookupQuest {
    int id;
    BYTE state;
    int step;
    std::vector<TargetEntry> entries;
};

// The quests in progress the NPC lookup was built from with their table entries, which entries the conditions kept, and the lookup:
// NPC id to the first quest needing it. Names are built for every NPC on screen in every frame, so the table is only queried again
// when a quest changes, and the conditions are asked again when a quest changes or RECHECK_MS passed.
static std::vector<LookupQuest> s_lookupQuests;
static std::vector<bool> s_keptEntries;
static ULONGLONG s_checkedAt = 0;
static std::unordered_map<int, int> s_questByNpc;

static bool WaitsForTalk(const LookupQuest& quest) {
    if (!s_collectionDone || !s_questTalks || !*s_questTalkTable || !s_collectionDone(nullptr, quest.id)) {
        return false;
    }
    void* scripts[MAX_TALKS];
    return s_questTalks(quest.id, quest.state, quest.step, scripts) > 0;
}

static void RefreshQuestLookup() {
    int count = max(0, *s_questCount);
    BYTE* quests = (BYTE*)(s_questCount + 1);
    bool unchanged = count == (int)s_lookupQuests.size();
    for (int i = 0; unchanged && i < count; i++) {
        BYTE* quest = quests + 16 * i;
        unchanged = s_lookupQuests[i].id == *(int*)quest && s_lookupQuests[i].state == quest[4] && s_lookupQuests[i].step == *(int*)(quest + 5);
    }
    ULONGLONG now = GetTickCount64();
    if (unchanged && now - s_checkedAt < RECHECK_MS) {
        return;
    }
    s_checkedAt = now;
    if (!unchanged) {
        s_lookupQuests.clear();
        BYTE* buffer[MAX_ENTRIES];
        for (int i = 0; i < count; i++) {
            BYTE* quest = quests + 16 * i;
            LookupQuest& added = s_lookupQuests.emplace_back(LookupQuest{ *(int*)quest, quest[4], *(int*)(quest + 5), {} });
            EntryList entries = { nullptr, buffer, buffer, buffer + MAX_ENTRIES };
            s_queryQuestMonsters(s_questMonsterTable, added.id, added.step, &entries);
            for (BYTE** entry = entries.first; entry < entries.last; entry++) {
                int* npc = *(int**)(*entry + 32);
                int* npcEnd = *(int**)(*entry + 40);
                added.entries.push_back({ *(int*)(*entry + 16), npc && npc < npcEnd ? std::vector<int>(npc, npcEnd) : std::vector<int>() });
            }
        }
    }
    std::vector<bool> kept;
    for (const LookupQuest& quest : s_lookupQuests) {
        bool open = !WaitsForTalk(quest);
        for (const TargetEntry& entry : quest.entries) {
            kept.push_back(open && !(entry.item > 0 && s_itemCollected && s_itemCollected(nullptr, quest.id, entry.item)));
        }
    }
    if (unchanged && kept == s_keptEntries) {
        return;
    }
    s_keptEntries = std::move(kept);
    s_questByNpc.clear();
    size_t index = 0;
    for (const LookupQuest& quest : s_lookupQuests) {
        for (const TargetEntry& entry : quest.entries) {
            if (s_keptEntries[index++]) {
                for (int npc : entry.npcs) {
                    s_questByNpc.emplace(npc, quest.id);
                }
            }
        }
    }
}

/// The quest in progress whose current step needs the NPC (to kill, loot or gather), from the client's quest monster table.
static int FindQuestNeeding(int npcId) {
    RefreshQuestLookup();
    auto found = s_questByNpc.find(npcId);
    return found != s_questByNpc.end() ? found->second : 0;
}

/// Puts the icon of the quest kind, as the quest window shows it, in front of the name of an NPC a quest in progress needs.
extern "C" int AppendQuestIcon(BYTE* owner, wchar_t* line, int written) {
    if (!owner || written < 0 || written >= LINE_LENGTH - 8) {
        return written;
    }
    int questId = FindQuestNeeding(*(int*)(owner + s_npcIdOffset));
    if (!questId) {
        return written;
    }
    int type = s_questIconType(nullptr, questId);
    const wchar_t* icon = type >= 0 && type < NO_ICON ? s_questIcon(type) : nullptr;
    if (!icon || !*icon) {
        return written;
    }
    int added = swprintf_s(line + written, LINE_LENGTH - written, L"%s ", icon);
    return added > 0 ? written + added : written;
}

/// Stands the path of the quest target point upright, once the effect library is loaded. The effect keeps its name in a string
/// (text or pointer to it, then length and capacity) and its particle parameters inline: flags first, negative when following a
/// path, and the start angles further on.
static void TurnTargetPoint(void* effects) {
    static bool logged = false;
    BYTE* effect = (BYTE*)s_findEffect(effects, TARGET_POINT_EFFECT);
    if (!effect) {
        return;
    }
    size_t length = *(size_t*)(effect + EFFECT_NAME_OFFSET + 16);
    size_t capacity = *(size_t*)(effect + EFFECT_NAME_OFFSET + 24);
    const char* name = capacity >= 16 ? *(const char**)(effect + EFFECT_NAME_OFFSET) : (const char*)(effect + EFFECT_NAME_OFFSET);
    BYTE* params = effect + EFFECT_PARAMS_OFFSET;
    float* angles = (float*)(params + PARAMS_ANGLES_OFFSET);
    // the name is only read where the string fields look like a string of that length, in case the effect is laid out otherwise
    bool matches = length == strlen(TARGET_POINT_EFFECT) && capacity >= length && capacity < 0x10000
        && memcmp(name, TARGET_POINT_EFFECT, length) == 0 && *(int*)params < 0;
    bool flat = angles[0] == 0 && angles[1] == 0 && angles[2] == 0;
    if (matches && flat) {
        angles[1] = TARGET_POINT_TURN;
    }
    if (!logged) {
        logged = true;
        ModsLog("quest target effect: point %p %s", effect, matches ? (flat ? "turned upright" : "already turned") : "not recognised");
    }
}

static bool IsMarkerState(int state) {
    return state >= 2 && state <= 11 && state != 4 && state != 8;
}

/// Shows the quest target effect on a gatherable object a quest in progress needs, by letting the marker function attach it in
/// place of the marker effect it would use for a borrowed marker state.
static void __fastcall zzSetQuestMarker(BYTE* npc, int questId, int state, int kind) {
    real_SetQuestMarker(npc, questId, state, kind);
    int npcId = npc ? *(int*)(npc + s_npcIdOffset) : 0;
    if (IsMarkerState(state) || npcId < FIRST_GATHERABLE || npcId > LAST_GATHERABLE || !FindQuestNeeding(npcId)) {
        return;
    }
    void* effects = *(void**)(npc + s_effectsOffset);
    if (!effects || !s_findEffect(effects, QUEST_TARGET_EFFECT)) {
        return;
    }
    TurnTargetPoint(effects);
    const char** slot = s_markerEffects + BORROWED_STATE;
    if (!*slot) {
        // the table is filled on the first marker shown
        real_SetQuestMarker(npc, questId, BORROWED_STATE, 0);
    }
    const char* marker = *slot;
    *slot = QUEST_TARGET_EFFECT;
    real_SetQuestMarker(npc, questId, BORROWED_STATE, 0);
    *slot = marker;
    int* markerState = (int*)(npc + s_markerStateOffset);
    markerState[0] = questId;
    markerState[1] = state;
    markerState[2] = kind;
}

/// For clients that do not show the quest target effect themselves: the marker function names the marker effects, then looks
/// one up: lea rcx, [kind*3]; lea rax, [table]; lea rdx, [state+rcx*4]; mov rcx, [npc+effects]; mov rdx, [rax+rdx*8]; call
static void InstallQuestTargetEffect(HMODULE game) {
    if (FindBytes(game, QUEST_TARGET_EFFECT, sizeof(QUEST_TARGET_EFFECT))) {
        return;
    }
    const void* name = FindBytes(game, FIRST_MARKER_EFFECT, sizeof(FIRST_MARKER_EFFECT));
    BYTE* nameLea = name ? FindLeaTo(game, name) : nullptr;
    BYTE* function = nameLea ? FunctionStart(nameLea) : nullptr;
    BYTE* lookup = nullptr;
    for (BYTE* p = function; p && p < function + 0x800 && !lookup; p++) {
        if (memcmp(p, "\x48\x8D\x05", 3) == 0 && memcmp(p + 7, "\x48\x8D\x14\x8F\x48\x8B\x8B", 7) == 0 && memcmp(p + 18, "\x48\x8B\x14\xD0\xE8", 5) == 0) {
            lookup = p;
        }
    }
    // the marker function stores the quest it is given first: mov [rcx+state], edx
    for (BYTE* p = function; p && p < function + 0x100 && s_markerStateOffset < 0; p++) {
        if (p[0] == 0x89 && p[1] == 0x91) {
            s_markerStateOffset = *(INT32*)(p + 2);
        }
    }
    ModsLog("quest target effect: marker function=%p lookup=%p state=%X", function, lookup, s_markerStateOffset);
    if (!lookup || s_markerStateOffset < 0 || s_npcIdOffset < 0 || !s_queryQuestMonsters || !s_questCount) {
        return;
    }
    s_markerEffects = (const char**)ResolveRip(lookup + 3, lookup + 7);
    s_effectsOffset = *(INT32*)(lookup + 14);
    s_findEffect = (FindEffect_t)ResolveRip(lookup + 23, lookup + 27);
    real_SetQuestMarker = (SetQuestMarker_t)function;
    DetourAttach(&(PVOID&)real_SetQuestMarker, zzSetQuestMarker);
}

/// The function holding an instruction that loads the given address through a RIP-relative mov or lea.
static BYTE* FunctionLoading(HMODULE game, const void* target) {
    BYTE* end = (BYTE*)game + ModuleImageSize(game) - 8;
    for (BYTE* c = (BYTE*)game; c <= end; c++) {
        if ((c[0] == 0x48 || c[0] == 0x4C) && (c[1] == 0x8B || c[1] == 0x8D) && (c[2] & 0xC7) == 0x05 && ResolveRip(c + 3, c + 7) == target) {
            return FunctionStart(c);
        }
    }
    return nullptr;
}

/// The NPC id, as the client loads it before telling gatherable objects apart: mov r32, [reg+id]; ...; cmp r32, 700000. The most
/// common offset among those loads.
static int FindNpcIdOffset(HMODULE game) {
    std::map<int, int> votes;
    BYTE* start = (BYTE*)game + 0x40;
    BYTE* end = (BYTE*)game + ModuleImageSize(game) - 8;
    for (BYTE* c = start; c < end; c++) {
        if (c[0] != 0x81 || (c[1] & 0xF8) != 0xF8 || *(UINT32*)(c + 2) != FIRST_GATHERABLE) {
            continue;
        }
        bool extended = c[-1] == 0x41;
        int reg = c[1] & 7;
        for (BYTE* q = c - 3; q > c - 40 && q > start; q--) {
            if (q[0] == 0x8B && (q[1] & 0xC0) == 0x40 && ((q[1] >> 3) & 7) == reg && (q[1] & 7) != 4 && ((q[-1] & 0x44) == 0x44) == extended) {
                votes[q[2]]++;
                break;
            }
        }
    }
    int best = -1, bestVotes = 0;
    for (auto& vote : votes) {
        if (vote.second > bestVotes) {
            best = vote.first;
            bestVotes = vote.second;
        }
    }
    return best;
}

/// The quest list is searched in many places: movsxd r8, [count]; lea rdx, [count + 4]; mov r9d, 16 (the entry size).
static volatile int* FindQuestCount(HMODULE game) {
    std::map<BYTE*, int> hits;
    for (BYTE* p : FindAllPatterns(game, "4C 63 05")) {
        BYTE* count = ResolveRip(p + 3, p + 7);
        bool entrySize = false, array = false;
        for (BYTE* q = p + 7; q < p + 32; q++) {
            entrySize |= memcmp(q, "\x41\xB9\x10\x00\x00\x00", 6) == 0;
            array |= q[0] == 0x48 && q[1] == 0x8D && q[2] == 0x15 && ResolveRip(q + 3, q + 7) == count + 4;
        }
        if (entrySize && array) {
            hits[count]++;
        }
    }
    BYTE* best = nullptr;
    int bestHits = 0;
    for (auto& hit : hits) {
        if (hit.second > bestHits) {
            best = hit.first;
            bestHits = hit.second;
        }
    }
    return (volatile int*)best;
}

/// The conditions a quest's targets need besides its step, as the client's quest navigation asks them: call <collection done>; test
/// eax, eax; jz; cmp [talk table], r15; jz; lea r9, [rsp+x]; movzx edx, bpl; ...; call <talk lookup>. The talk lookup is found by
/// itself, as it loads the same table first: sub rsp, 58h; three register saves; mov r11d, ecx; mov rcx, [table]; xor esi, esi.
static void FindTalkConditions(HMODULE game) {
    BYTE* site = FindPattern(game, "E8 ?? ?? ?? ?? 85 C0 0F 84 ?? ?? ?? ?? 4C 39 3D ?? ?? ?? ?? 0F 84 ?? ?? ?? ?? 4C 8D 8C 24 ?? ?? ?? ?? 40 0F B6 D5");
    BYTE* talks = FindPattern(game, "48 83 EC 58 48 89 74 24 70 4C 89 6C 24 48 4C 89 74 24 40 44 8B D9 48 8B 0D ?? ?? ?? ?? 33 F6 4C 8B 51 08");
    if (!site || !talks || ResolveRip(site + 16, site + 20) != ResolveRip(talks + 25, talks + 29)) {
        return;
    }
    s_collectionDone = (CollectionDone_t)ResolveRip(site + 1, site + 5);
    s_questTalkTable = (void**)ResolveRip(site + 16, site + 20);
    s_questTalks = (QuestTalks_t)talks;
}

/// The item check, where the client skips a table entry: mov r8d, [entry+16]; test r8d, r8d; jle; ...; lea rcx, [quest manager];
/// ...; call. The function most of those places call.
static void FindItemCollected(HMODULE game) {
    std::map<BYTE*, int> votes;
    for (BYTE* p : FindAllPatterns(game, "44 8B ?? 10 45 85 C0 7E")) {
        if ((p[2] & 0xF8) != 0x40) {
            continue;
        }
        BYTE* lea = nullptr;
        for (BYTE* q = p + 9; q < p + 25 && !lea; q++) {
            lea = q[0] == 0x48 && q[1] == 0x8D && q[2] == 0x0D ? q : nullptr;
        }
        for (BYTE* q = lea ? lea + 7 : nullptr; q && q < lea + 13; q++) {
            if (*q == 0xE8) {
                votes[ResolveRip(q + 1, q + 5)]++;
                break;
            }
        }
    }
    int bestVotes = 0;
    for (auto& vote : votes) {
        if (vote.second > bestVotes) {
            s_itemCollected = (ItemCollected_t)vote.first;
            bestVotes = vote.second;
        }
    }
    if (bestVotes < 3) {
        s_itemCollected = nullptr;
    }
}

void InstallQuestTargets(HMODULE game) {
    // the query walks the table's tree first: mov r10, [table + 16]
    BYTE* query = FindPattern(game, "48 89 4C 24 08 48 83 EC 78 4C 8B 15 ?? ?? ?? ?? 48 89 6C 24 68 4C 89 7C 24 38 49 8B 42 08 49 8B E9 45 8B F8 80 78 29 00");
    if (query) {
        s_queryQuestMonsters = (QueryQuestMonsters_t)query;
        s_questMonsterTable = ResolveRip(query + 12, query + 16) - 16;
    }
    s_questCount = FindQuestCount(game);
    s_npcIdOffset = FindNpcIdOffset(game);
    FindTalkConditions(game);
    FindItemCollected(game);
    ModsLog("quest targets: collection done=%p talk table=%p talks=%p item collected=%p", s_collectionDone, s_questTalkTable, s_questTalks,
        s_itemCollected);
    // the icon of a quest in the quest window
    s_questIconType = (QuestIconType_t)FindPattern(game,
        "48 89 4C 24 08 B8 ?? ?? 00 00 E8 ?? ?? ?? ?? 48 2B E0 48 89 AC 24 ?? ?? 00 00 48 8D 4C 24 40 48 89 BC 24 ?? ?? 00 00 8B FA BD 01 00 00 00 E8");
    // the icon markup comes from a table of graphic character names starting with quest_givable
    const void* iconName = FindBytes(game, FIRST_QUEST_ICON, sizeof(FIRST_QUEST_ICON));
    UINT64 namePointer = (UINT64)iconName;
    const void* iconTable = iconName ? FindBytes(game, &namePointer, sizeof(namePointer)) : nullptr;
    s_questIcon = iconTable ? (QuestIcon_t)FunctionLoading(game, iconTable) : nullptr;

    // the name builder after its optional icon: mov ebx, eax; jmp; mov edi, [rsp+x]; test edi, edi; jnz
    BYTE* join = FindPattern(game, "8B D8 EB 04 8B 7C 24 ?? 85 FF 0F 85");
    // mov rax, <line index>; shl rax, 9; lea rcx, [rsp+rax+lines]
    static const BYTE LINE_R12[] = { 0x49, 0x8B, 0xC4, 0x48, 0xC1, 0xE0, 0x09, 0x48, 0x8D, 0x8C, 0x04 };
    static const BYTE LINE_RBP[] = { 0x48, 0x8B, 0xC5, 0x48, 0xC1, 0xE0, 0x09, 0x48, 0x8D, 0x8C, 0x04 };
    void (*stub)() = nullptr;
    BYTE* builder = join ? FunctionStart(join) : nullptr;
    for (BYTE* p = builder; p && p < builder + 0x60 && !stub; p++) {
        // the name owner is the third argument, kept in r15 or r14: mov r15, r8 / mov r14, r8
        if (p[0] == 0x4D && p[1] == 0x8B && p[2] == 0xF8 && memcmp(join - 20, LINE_R12, sizeof(LINE_R12)) == 0) {
            stub = QuestIconStubR15R12;
        } else if (p[0] == 0x4D && p[1] == 0x8B && p[2] == 0xF0 && memcmp(join - 20, LINE_RBP, sizeof(LINE_RBP)) == 0) {
            stub = QuestIconStubR14Rbp;
        }
    }
    InstallQuestTargetEffect(game);
    ModsLog("quest targets: query=%p table=%p count=%p id=%X type=%p icon=%p join=%p stub=%p", s_queryQuestMonsters, s_questMonsterTable,
        s_questCount, s_npcIdOffset, s_questIconType, s_questIcon, join, stub);
    if (!s_queryQuestMonsters || !s_questCount || s_npcIdOffset < 0 || !s_questIconType || !s_questIcon || !stub) {
        return;
    }
    g_questIconLines = *(INT32*)(join - 9);
    g_questIconResume = join + 8;
    DetourAttach(&g_questIconResume, stub);
}
