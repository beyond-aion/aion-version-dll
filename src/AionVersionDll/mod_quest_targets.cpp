#include "mods.h"
#include <map>
#include <stdio.h>
#include "detours.h"

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
static constexpr int MARKER_STATE_OFFSET = 0x1D0;
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

typedef void(__fastcall* SetQuestMarker_t)(BYTE* npc, int questId, int state, int kind);
typedef void*(__fastcall* FindEffect_t)(void* effects, const char* name);

static SetQuestMarker_t real_SetQuestMarker = nullptr;
static FindEffect_t s_findEffect = nullptr;
static const char** s_markerEffects = nullptr;
static INT32 s_effectsOffset = 0;

extern "C" void* g_questIconResume = nullptr;
extern "C" INT64 g_questIconLines = 0;
extern "C" void QuestIconStubR15R12();
extern "C" void QuestIconStubR14Rbp();

/// The quest in progress whose current step needs the NPC (to kill, loot or gather), from the client's quest monster table.
static int FindQuestNeeding(int npcId) {
    int count = *s_questCount;
    BYTE* quests = (BYTE*)(s_questCount + 1);
    BYTE* buffer[MAX_ENTRIES];
    for (int i = 0; i < count; i++) {
        int questId = *(int*)(quests + 16 * i);
        int step = *(int*)(quests + 16 * i + 5);
        EntryList entries = { nullptr, buffer, buffer, buffer + MAX_ENTRIES };
        s_queryQuestMonsters(s_questMonsterTable, questId, step, &entries);
        for (BYTE** entry = entries.first; entry < entries.last; entry++) {
            int* npc = *(int**)(*entry + 32);
            int* npcEnd = *(int**)(*entry + 40);
            for (; npc && npc < npcEnd; npc++) {
                if (*npc == npcId) {
                    return questId;
                }
            }
        }
    }
    return 0;
}

/// Puts the icon of the quest kind, as the quest window shows it, in front of the name of an NPC a quest in progress needs.
extern "C" int AppendQuestIcon(BYTE* owner, wchar_t* line, int written) {
    if (!owner || written < 0 || written >= LINE_LENGTH - 8) {
        return written;
    }
    int questId = FindQuestNeeding(*(int*)(owner + 0x34));
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
    bool matches = length == strlen(TARGET_POINT_EFFECT) && memcmp(name, TARGET_POINT_EFFECT, length) == 0 && *(int*)params < 0;
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
    int npcId = npc ? *(int*)(npc + 0x34) : 0;
    if (IsMarkerState(state) || npcId < FIRST_GATHERABLE || npcId > LAST_GATHERABLE || !FindQuestNeeding(npcId)) {
        return;
    }
    void* effects = *(void**)(npc + s_effectsOffset);
    if (!effects || !s_findEffect(effects, g_modsConfig.glowEffect)) {
        return;
    }
    if (strcmp(g_modsConfig.glowEffect, QUEST_TARGET_EFFECT) == 0) {
        TurnTargetPoint(effects);
    }
    const char** slot = s_markerEffects + BORROWED_STATE;
    if (!*slot) {
        // the table is filled on the first marker shown
        real_SetQuestMarker(npc, questId, BORROWED_STATE, 0);
    }
    const char* marker = *slot;
    *slot = g_modsConfig.glowEffect;
    real_SetQuestMarker(npc, questId, BORROWED_STATE, 0);
    *slot = marker;
    int* markerState = (int*)(npc + MARKER_STATE_OFFSET);
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
    ModsLog("quest target effect: marker function=%p lookup=%p", function, lookup);
    if (!lookup || !s_queryQuestMonsters || !s_questCount) {
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
    for (BYTE* c = (BYTE*)game; InModule(game, c, 8); c++) {
        if ((c[0] == 0x48 || c[0] == 0x4C) && (c[1] == 0x8B || c[1] == 0x8D) && (c[2] & 0xC7) == 0x05 && ResolveRip(c + 3, c + 7) == target) {
            return FunctionStart(c);
        }
    }
    return nullptr;
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

void InstallQuestTargets(HMODULE game) {
    // the query walks the table's tree first: mov r10, [table + 16]
    BYTE* query = FindPattern(game, "48 89 4C 24 08 48 83 EC 78 4C 8B 15 ?? ?? ?? ?? 48 89 6C 24 68 4C 89 7C 24 38 49 8B 42 08 49 8B E9 45 8B F8 80 78 29 00");
    if (query) {
        s_queryQuestMonsters = (QueryQuestMonsters_t)query;
        s_questMonsterTable = ResolveRip(query + 12, query + 16) - 16;
    }
    s_questCount = FindQuestCount(game);
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
    ModsLog("quest targets: query=%p table=%p count=%p type=%p icon=%p join=%p stub=%p", s_queryQuestMonsters, s_questMonsterTable,
        s_questCount, s_questIconType, s_questIcon, join, stub);
    if (!s_queryQuestMonsters || !s_questCount || !s_questIconType || !s_questIcon || !stub) {
        return;
    }
    g_questIconLines = *(INT32*)(join - 9);
    g_questIconResume = join + 8;
    DetourAttach(&g_questIconResume, stub);
}
