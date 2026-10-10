#include "mods.h"
#include <intrin.h>
#include <stdio.h>
#include "detours.h"

/// The virtual table of a class, found through its RTTI: type descriptor name -> complete object locator -> table.
static BYTE* FindVtable(HMODULE module, const char* rttiName) {
    BYTE* name = FindBytes(module, rttiName, strlen(rttiName) + 1);
    if (!name) {
        return nullptr;
    }
    BYTE* begin = (BYTE*)module;
    DWORD typeRva = (DWORD)(name - 0x10 - begin);
    PIMAGE_NT_HEADERS nt = (PIMAGE_NT_HEADERS)(begin + ((PIMAGE_DOS_HEADER)begin)->e_lfanew);
    BYTE* end = begin + nt->OptionalHeader.SizeOfImage - 0x18;
    for (BYTE* p = begin; p < end; p += 4) {
        DWORD* locator = (DWORD*)p;
        // the 4.x clients write signature 0 and no self reference, so only the type and the offset of the primary table are compared
        if (locator[0] > 1 || locator[1] != 0 || locator[3] != typeRva) {
            continue;
        }
        for (BYTE* q = begin; q < end; q += 8) {
            if (*(BYTE**)q == p) {
                return q + 8;
            }
        }
    }
    return nullptr;
}

/// The client picks at most this many buffs and as many debuffs for a window.
static constexpr int CLIENT_EFFECTS = 24;
static constexpr int MAX_EFFECTS = 64;
// One picked effect: unknown, effect id, type, a 16-bit value; 5.x clients add a dword.
static constexpr int ENTRY_SIZE = 18;
static constexpr int ENTRY_SIZE_5X = 20;
// The space the picks get on the stack when the input array follows them, which is no longer read once they are picked.
static constexpr int CLIENT_PICKS_SIZE = CLIENT_EFFECTS * ENTRY_SIZE;
// Columns and rows the client's own buff and debuff windows are built with.
static constexpr int OWN_COLUMNS = 8;
static constexpr int OWN_ROWS = 3;
// The slot arrays a window gets past the end of its object: buffs, then debuffs.
static constexpr int EXTRA_SLOTS_SIZE = 2 * MAX_EFFECTS * 8;

/// A window with configured columns shows as many effects as it has slots for, the server decides which.
static int TargetCount() {
    return g_modsConfig.targetBuffColumns ? MAX_EFFECTS : CLIENT_EFFECTS;
}

static int OwnCount() {
    return g_modsConfig.ownBuffColumns ? MAX_EFFECTS : CLIENT_EFFECTS;
}

static int OwnColumns() {
    return g_modsConfig.ownBuffColumns;
}

static int RowsFor(int columns, int count) {
    return (count + columns - 1) / columns;
}

static bool s_ownWindowsReady = false;
static bool s_ownGridReady = false;
static bool s_targetWindowReady = false;
// rows the client builds the own buff and debuff windows with
static int s_ownRows[2] = { OWN_ROWS, OWN_ROWS };

/// Adds widgets prefix<from>..prefix<to> as copies of the widget named source.
static void AddSlots(XmlNode& dialog, const wchar_t* source, const wchar_t* prefix, int from, int to) {
    XmlNode* first = dialog.ChildNamed(source);
    if (!first) {
        return;
    }
    XmlNode slot = *first;
    for (int i = from; i <= to; i++) {
        std::wstring name = prefix + std::to_wstring(i);
        if (!dialog.ChildNamed(name)) {
            slot.SetAttribute(L"name", name);
            dialog.children.push_back(slot);
        }
    }
}

/// Grows the frame "x,y,width,height" of a window built for the client's grid to the configured one, keeping its margins.
static void ResizeOwnWindow(XmlNode& dialog, const wchar_t* slotName, int clientRows) {
    XmlNode* slot = dialog.ChildNamed(slotName);
    const std::wstring* frame = dialog.Attribute(L"frame");
    const std::wstring* slotFrame = slot ? slot->Attribute(L"frame") : nullptr;
    int x, y, width, height, slotX, slotY, slotWidth, slotHeight;
    if (!frame || !slotFrame || swscanf_s(frame->c_str(), L"%d,%d,%d,%d", &x, &y, &width, &height) != 4
        || swscanf_s(slotFrame->c_str(), L"%d,%d,%d,%d", &slotX, &slotY, &slotWidth, &slotHeight) != 4) {
        return;
    }
    int columns = OwnColumns();
    width += (columns - OWN_COLUMNS) * slotWidth;
    height += (RowsFor(columns, OwnCount()) - clientRows) * slotHeight;
    dialog.SetAttribute(L"frame", std::to_wstring(x) + L"," + std::to_wstring(y) + L"," + std::to_wstring(width) + L"," + std::to_wstring(height));
}

/// The target window's file has 20 slots of each kind although the window fills up to 24, or the configured count.
static void EditTargetWindow(XmlNode& dialog) {
    int count = s_targetWindowReady ? TargetCount() : CLIENT_EFFECTS;
    AddSlots(dialog, L"buff_item1", L"buff_item", 2, count);
    AddSlots(dialog, L"debuff_item1", L"debuff_item", 2, count);
}

/// The own windows get the slots of their kind up to the configured count.
static void EditBuffWindow(XmlNode& dialog) {
    if (s_ownWindowsReady) {
        AddSlots(dialog, L"buff_item1", L"buff_item", 2, OwnCount());
    }
    if (s_ownGridReady) {
        ResizeOwnWindow(dialog, L"buff_item1", s_ownRows[0]);
    }
}

static void EditDebuffWindow(XmlNode& dialog) {
    if (s_ownWindowsReady) {
        AddSlots(dialog, L"debuff_item1", L"debuff_item", 2, OwnCount());
    }
    if (s_ownGridReady) {
        ResizeOwnWindow(dialog, L"debuff_item1", s_ownRows[1]);
    }
}

// --- picking more effects ---

typedef int(__fastcall* SelectBuffs_t)(BYTE* effects, int count, BYTE* picks, __int64 limit, int mode, int withType5);
typedef int(__fastcall* SelectDebuffs_t)(BYTE* effects, int count, BYTE* picks, __int64 limit, int mode);
static SelectBuffs_t real_SelectBuffs = nullptr;
static SelectDebuffs_t real_SelectDebuffs = nullptr;

/// A call of a selector that gets the configured count: where it returns to, the buffer its window reads the picks from
/// (nullptr: the caller's own, which has room), and the count.
struct SelectRoute {
    BYTE* returnTo;
    BYTE* picks;
    int count;
};
static SelectRoute s_routes[4];
static int s_routeCount = 0;
static BYTE s_scratch[MAX_EFFECTS * ENTRY_SIZE_5X];

/// The selectors pick up to 48 once patched; every caller gets as many as it can hold: the configured count for the windows
/// routed here, 24 for the others. Mode 2 keeps the first ones, the other mode the last ones.
static int Deliver(BYTE* picks, int picked, int mode, BYTE* returnTo) {
    const SelectRoute* route = nullptr;
    for (int i = 0; i < s_routeCount; i++) {
        if (s_routes[i].returnTo == returnTo) {
            route = &s_routes[i];
        }
    }
    int kept = min(picked, route ? route->count : CLIENT_EFFECTS);
    BYTE* from = s_scratch + (mode == 2 ? 0 : picked - kept) * ENTRY_SIZE;
    BYTE* target = route && route->picks ? route->picks : picks;
    memcpy(target, from, kept * ENTRY_SIZE);
    if (target != picks) {
        memcpy(picks, from, min(kept, CLIENT_EFFECTS) * ENTRY_SIZE);
    }
    return kept;
}

static int __fastcall zzSelectBuffs(BYTE* effects, int count, BYTE* picks, __int64 limit, int mode, int withType5) {
    int picked = real_SelectBuffs(effects, count, s_scratch, limit, mode, withType5);
    return Deliver(picks, picked, mode, (BYTE*)_ReturnAddress());
}

static int __fastcall zzSelectDebuffs(BYTE* effects, int count, BYTE* picks, __int64 limit, int mode) {
    int picked = real_SelectDebuffs(effects, count, s_scratch, limit, mode);
    return Deliver(picks, picked, mode, (BYTE*)_ReturnAddress());
}

// --- finding and patching ---

struct Patch {
    BYTE* at;
    std::vector<BYTE> bytes;
};

static void Add(std::vector<Patch>& patches, BYTE* at, std::initializer_list<BYTE> bytes) {
    patches.push_back({ at, bytes });
}

static void AddDword(std::vector<Patch>& patches, BYTE* at, DWORD value) {
    BYTE* b = (BYTE*)&value;
    Add(patches, at, { b[0], b[1], b[2], b[3] });
}

static void Apply(const std::vector<Patch>& patches) {
    for (const Patch& patch : patches) {
        PatchMemory(patch.at, patch.bytes.data(), patch.bytes.size());
    }
}

/// The 24 written into a selector: cmp r9, 24 (twice), lea eax, [rdx-24] (twice), cmp ebx, 24; jl; mov ebx, 24.
static bool PatchSelectorLimit(BYTE* selector, std::vector<Patch>& patches) {
    BYTE* end = FunctionBodyEnd(selector, 0x2000);
    size_t before = patches.size();
    for (BYTE* p = selector; p + 10 <= end; p++) {
        if (p[0] == 0x49 && p[1] == 0x83 && p[2] == 0xF9 && p[3] == CLIENT_EFFECTS) {
            Add(patches, p + 3, { MAX_EFFECTS });
        } else if (p[0] == 0x8D && p[1] == 0x42 && p[2] == (BYTE)-CLIENT_EFFECTS) {
            Add(patches, p + 2, { (BYTE)-MAX_EFFECTS });
        } else if (memcmp(p, "\x83\xFB\x18\x7C\x05\xBB\x18\x00\x00\x00", 10) == 0) {
            Add(patches, p + 2, { MAX_EFFECTS });
            Add(patches, p + 6, { MAX_EFFECTS });
        }
    }
    return patches.size() - before == 6;
}

/// The direct call from the function to one of the targets, nullptr if none.
static BYTE* FindCall(BYTE* function, const void* target) {
    BYTE* end = FunctionBodyEnd(function, 0x2000);
    for (BYTE* p = function; p + 5 <= end; p++) {
        if (p[0] == 0xE8 && ResolveRip(p + 1, p + 5) == target) {
            return p;
        }
    }
    return nullptr;
}

/// The window's update: the entry of its table that calls a selector.
static BYTE* FindUpdate(HMODULE game, BYTE* vtable, BYTE* selector) {
    for (int i = 0; vtable && i < 260; i++) {
        BYTE* function = ((BYTE**)vtable)[i];
        if (!InModule(game, function, 16)) {
            break;
        }
        if (FunctionStart(function) == function && FindCall(function, selector)) {
            return function;
        }
    }
    return nullptr;
}

/// The stack offset loaded by lea reg, [rsp+disp8|disp32] at p (register in the ModRM reg field), or -1.
static int StackLea(BYTE* p, BYTE rex, BYTE reg) {
    if (p[0] != rex || p[1] != 0x8D || p[3] != 0x24 || ((p[2] >> 3) & 7) != reg || (p[2] & 7) != 4) {
        return -1;
    }
    BYTE mod = p[2] >> 6;
    return mod == 1 ? p[4] : mod == 2 ? *(INT32*)(p + 4) : -1;
}

/// The stack offsets of the picks (r8) and of the effects (rcx) set up for a selector call.
static bool CallBuffers(BYTE* call, int& picks, int& effects) {
    picks = effects = -1;
    for (BYTE* p = call - 0x30; p < call; p++) {
        int offset = StackLea(p, 0x4C, 0);
        if (offset >= 0) {
            picks = offset;
        }
        offset = StackLea(p, 0x48, 1);
        if (offset >= 0) {
            effects = offset;
        }
    }
    return picks >= 0 && effects >= 0;
}

/// cmp r32, 23 checks against the last slot: every one in the function gets the last configured slot.
static int PatchLastSlotChecks(BYTE* function, int count, std::vector<Patch>& patches) {
    BYTE* end = FunctionBodyEnd(function, 0x2000);
    int found = 0;
    for (BYTE* p = function; p + 4 <= end; p++) {
        if (p[0] == 0x41 && p[1] == 0x83 && p[2] >= 0xF8 && p[3] == CLIENT_EFFECTS - 1) {
            Add(patches, p + 3, { (BYTE)(count - 1) });
            found++;
        }
    }
    return found;
}

/// Where the displacement of lea reg, [base+disp32] at p starts (base r12 or rsp takes a SIB byte), nullptr if p holds none.
static BYTE* FieldLeaDisplacement(BYTE* p) {
    if ((p[0] & 0xF8) != 0x48 || p[1] != 0x8D || (p[2] & 0xC0) != 0x80) {
        return nullptr;
    }
    if ((p[2] & 7) != 4) {
        return p + 3;
    }
    return p[3] == 0x24 ? p + 4 : nullptr;
}

/// lea reg, [base+disp32] with the given displacement: where the displacement starts, nullptr if p holds none.
static BYTE* FieldLea(BYTE* p, DWORD offset) {
    BYTE* displacement = FieldLeaDisplacement(p);
    return displacement && *(DWORD*)displacement == offset ? displacement : nullptr;
}

/// Where an instruction sets a register to 24: mov r32, 24 (a dword) or lea r32, [r+24] (a byte).
struct SlotCount {
    BYTE* at;
    int size;
};

static SlotCount SlotCountAt(BYTE* q) {
    if (q[0] >= 0xB8 && q[0] <= 0xBF && *(DWORD*)(q + 1) == CLIENT_EFFECTS) {
        return { q + 1, 4 };
    }
    if (q[0] == 0x41 && q[1] >= 0xB8 && q[1] <= 0xBF && *(DWORD*)(q + 2) == CLIENT_EFFECTS) {
        return { q + 2, 4 };
    }
    if ((q[0] & 0xF0) == 0x40 && q[1] == 0x8D && (q[2] & 0xC0) == 0x40 && (q[2] & 7) != 4 && q[3] == CLIENT_EFFECTS) {
        return { q + 3, 1 };
    }
    return { nullptr, 0 };
}

/// The slot count of the loop over the array whose lea starts at lea and ends at next: set within a few bytes after it, or
/// with before, the closest one in front of it.
static SlotCount FindSlotCount(BYTE* lea, BYTE* next, bool before) {
    for (BYTE* q = next; q < next + 0x18; q++) {
        SlotCount count = SlotCountAt(q);
        if (count.at) {
            return count;
        }
    }
    for (BYTE* q = lea - 1; before && q > lea - 0x40; q--) {
        SlotCount count = SlotCountAt(q);
        if (count.at && count.at + count.size <= lea) {
            return count;
        }
    }
    return { nullptr, 0 };
}

static void AddSlotCount(std::vector<Patch>& patches, SlotCount count, int value) {
    if (count.size == 4) {
        AddDword(patches, count.at, value);
    } else {
        Add(patches, count.at, { (BYTE)value });
    }
}

/// The slot arrays, from the init that fills them: lea rdi, [rbx+buffs]; lea r8, "buff_item%d". The debuff array follows.
static bool FindSlotArrays(BYTE* slotNameLea, DWORD& buffs, DWORD& debuffs) {
    BYTE* displacement = FieldLeaDisplacement(slotNameLea - 7);
    if (displacement != slotNameLea - 4) {
        return false;
    }
    buffs = *(DWORD*)displacement;
    debuffs = buffs + CLIENT_EFFECTS * 8;
    return true;
}

static BYTE* ConstructorOf(HMODULE game, BYTE* vtable) {
    BYTE* store = vtable ? FindLeaTo(game, vtable) : nullptr;
    return store ? FunctionStart(store) : nullptr;
}

// --- windows with more slots ---

typedef void*(__fastcall* Construct_t)(BYTE* window);
typedef void(__fastcall* InitWindow_t)(BYTE* window);
typedef BYTE*(__fastcall* FindChild_t)(BYTE* window, const char* name, int flags);

/// A window whose object grows by a buff and a debuff slot array of MAX_EFFECTS, which its update and init use instead of the
/// client's arrays of 24.
struct ExtendedWindow {
    BYTE* vtable;
    // where the factory's call of the constructor returns to; the constructor also builds the base of other windows
    BYTE* createdAt;
    // the client's size of the object, where the arrays start
    DWORD slots;
    int count;
    bool buffs;
    bool debuffs;
    Construct_t realConstruct;
};
static ExtendedWindow s_windows[3];
static int s_windowCount = 0;
static InitWindow_t real_InitWindow = nullptr;
static DWORD s_findChildSlot = 0;
static DWORD s_slotFlagOffset = 0;
static DWORD s_slotFlag = 0;
static int s_findChildFlags = 0;
static BYTE* s_targetPicks[2] = {};

template <int I>
static __declspec(noinline) void* __fastcall zzConstruct(BYTE* window) {
    ExtendedWindow& extended = s_windows[I];
    void* result = extended.realConstruct(window);
    if ((BYTE*)_ReturnAddress() == extended.createdAt) {
        memset(window + extended.slots, 0, EXTRA_SLOTS_SIZE);
    }
    return result;
}

static void* (__fastcall* const CONSTRUCT_HOOKS[])(BYTE*) = { zzConstruct<0>, zzConstruct<1>, zzConstruct<2> };

/// Looks the slots up like the client does for its 24 of each kind, into the arrays past the end of the window.
static void __fastcall zzInitWindow(BYTE* window) {
    real_InitWindow(window);
    for (int w = 0; w < s_windowCount; w++) {
        const ExtendedWindow& extended = s_windows[w];
        if (*(BYTE**)window != extended.vtable) {
            continue;
        }
        FindChild_t findChild = *(FindChild_t*)(*(BYTE**)window + s_findChildSlot);
        BYTE** slots = (BYTE**)(window + extended.slots);
        for (int kind = 0; kind < 2; kind++) {
            if (!(kind == 0 ? extended.buffs : extended.debuffs)) {
                continue;
            }
            for (int i = 0; i < MAX_EFFECTS; i++) {
                char name[32];
                sprintf_s(name, kind == 0 ? "buff_item%d" : "debuff_item%d", i + 1);
                BYTE* slot = i < extended.count ? findChild(window, name, s_findChildFlags) : nullptr;
                if (slot) {
                    *(DWORD*)(slot + s_slotFlagOffset) = s_slotFlag;
                }
                slots[kind * MAX_EFFECTS + i] = slot;
            }
        }
        return;
    }
}

/// The lookup in the shared window init: mov r8d, flags; mov rcx, reg; call [r11+slot] (4.x) or call [rax+slot] (5.x), then
/// mov dword ptr [rax+offset], flag.
static bool ParseInit(BYTE* init) {
    BYTE* end = FunctionBodyEnd(init, 0x400);
    for (BYTE* p = init; p + 24 <= end; p++) {
        if (p[0] != 0x41 || p[1] != 0xB8 || p[6] != 0x48 || p[7] != 0x8B) {
            continue;
        }
        BYTE* call = p[9] == 0x41 ? p + 10 : p + 9;
        if (call[0] == 0xFF && (call[1] == 0x93 || call[1] == 0x90)) {
            s_findChildFlags = *(int*)(p + 2);
            s_findChildSlot = *(DWORD*)(call + 2);
            for (BYTE* q = p + 16; q < p + 0x30; q++) {
                if (q[0] == 0xC7 && q[1] == 0x80) {
                    s_slotFlagOffset = *(DWORD*)(q + 2);
                    s_slotFlag = *(DWORD*)(q + 6);
                    return true;
                }
            }
        }
    }
    return false;
}

/// The factory's call of the constructor, which makes the window with operator new: add [rip+total], size; ...; mov ecx, size;
/// call [alloc]; test rax, rax; jz; mov rcx, rax; call constructor. nullptr if not found.
static BYTE* FindFactoryCall(HMODULE game, BYTE* constructor) {
    PIMAGE_NT_HEADERS nt = (PIMAGE_NT_HEADERS)((BYTE*)game + ((PIMAGE_DOS_HEADER)game)->e_lfanew);
    BYTE* end = (BYTE*)game + nt->OptionalHeader.SizeOfImage - 5;
    for (BYTE* call = (BYTE*)game + 0x40; call < end; call++) {
        if (call[0] != 0xE8 || ResolveRip(call + 1, call + 5) != constructor) {
            continue;
        }
        if (memcmp(call - 3, "\x48\x8B\xC8", 3) == 0 && call[-5] == 0x74 && memcmp(call - 8, "\x48\x85\xC0", 3) == 0
            && call[-14] == 0xFF && call[-13] == 0x15 && call[-19] == 0xB9 && memcmp(call - 37, "\x48\x81\x05", 3) == 0
            && *(DWORD*)(call - 30) == *(DWORD*)(call - 18)) {
            return call;
        }
    }
    return nullptr;
}

/// Grows the window's object by the slot arrays and gives its table the init that fills them. false if not recognised.
static bool ExtendWindow(HMODULE game, BYTE* vtable, BYTE* init, int count, bool buffs, bool debuffs, std::vector<Patch>& patches) {
    BYTE* constructor = ConstructorOf(game, vtable);
    BYTE* call = constructor ? FindFactoryCall(game, constructor) : nullptr;
    int initSlot = -1;
    for (int i = 0; i < 64 && vtable; i++) {
        if (((BYTE**)vtable)[i] == init) {
            initSlot = i;
            break;
        }
    }
    if (!call || initSlot < 0 || s_windowCount >= _countof(s_windows)) {
        ModsLog("buffs: window factory %p or init slot %d not found", call, initSlot);
        return false;
    }
    DWORD size = *(DWORD*)(call - 18);
    AddDword(patches, call - 30, size + EXTRA_SLOTS_SIZE);
    AddDword(patches, call - 18, size + EXTRA_SLOTS_SIZE);
    void* replacement = zzInitWindow;
    Add(patches, vtable + 8 * initSlot, {});
    patches.back().bytes.assign((BYTE*)&replacement, (BYTE*)&replacement + 8);
    s_windows[s_windowCount++] = { vtable, call + 5, size, count, buffs, debuffs, (Construct_t)constructor };
    return true;
}

/// Points the update's leas of the client's slot arrays at the window's own ones and its loops over them at MAX_EFFECTS.
static void RedirectSlots(BYTE* update, DWORD buffSlots, DWORD debuffSlots, DWORD slots, bool countBefore, std::vector<Patch>& patches,
    int& leas, int& counts) {
    leas = counts = 0;
    BYTE* end = FunctionBodyEnd(update, 0x2000);
    for (BYTE* p = update; p + 8 <= end; p++) {
        BYTE* displacement = FieldLea(p, buffSlots);
        displacement = displacement ? displacement : FieldLea(p, debuffSlots);
        if (!displacement) {
            continue;
        }
        AddDword(patches, displacement, slots + (*(DWORD*)displacement == buffSlots ? 0 : MAX_EFFECTS * 8));
        leas++;
        SlotCount count = FindSlotCount(p, displacement + 4, countBefore);
        if (count.at) {
            AddSlotCount(patches, count, MAX_EFFECTS);
            counts++;
        }
    }
}

/// Makes the target window fill the configured number of buffs and debuffs: the update reads the window's own slot arrays and
/// the picks from buffers of their own, as its stack only holds 24.
static bool PrepareTargetWindow(HMODULE game, BYTE* vtable, BYTE* init, BYTE* buffSelector, BYTE* debuffSelector, DWORD buffSlots,
    DWORD debuffSlots, std::vector<Patch>& patches) {
    BYTE* update = FindUpdate(game, vtable, buffSelector);
    BYTE* buffCall = update ? FindCall(update, buffSelector) : nullptr;
    BYTE* debuffCall = update ? FindCall(update, debuffSelector) : nullptr;
    int buffPicks, debuffPicks, effects;
    if (!buffCall || !debuffCall || !CallBuffers(buffCall, buffPicks, effects) || !CallBuffers(debuffCall, debuffPicks, effects)) {
        ModsLog("buffs: target window update not recognised (%p)", update);
        return false;
    }
    DWORD regionSize = 0;
    BYTE* region = (BYTE*)DetourAllocateRegionWithinJumpBounds(update, &regionSize);
    if (!region || regionSize < 2 * MAX_EFFECTS * ENTRY_SIZE || !ExtendWindow(game, vtable, init, TargetCount(), true, true, patches)) {
        return false;
    }
    s_targetPicks[0] = region;
    s_targetPicks[1] = region + MAX_EFFECTS * ENTRY_SIZE;

    int leas, counts, pickLeas = 0;
    RedirectSlots(update, buffSlots, debuffSlots, s_windows[s_windowCount - 1].slots, false, patches, leas, counts);
    BYTE* end = FunctionBodyEnd(update, 0x2000);
    for (BYTE* p = update; p + 8 <= end; p++) {
        // lea rbp, [rsp+picks+4]: the effect id of the first pick
        int offset = StackLea(p, 0x48, 5);
        for (int kind = 0; kind < 2 && offset >= 0; kind++) {
            if (offset == (kind == 0 ? buffPicks : debuffPicks) + 4 && (p[2] >> 6) == 2) {
                INT32 rel = (INT32)(s_targetPicks[kind] + 4 - (p + 7));
                BYTE* r = (BYTE*)&rel;
                Add(patches, p, { 0x48, 0x8D, 0x2D, r[0], r[1], r[2], r[3], 0x90 });
                pickLeas++;
            }
        }
    }
    int checks = PatchLastSlotChecks(update, TargetCount(), patches);
    ModsLog("buffs: target update %p: %d slot arrays, %d loops, %d pick reads, %d slot checks", update, leas, counts, pickLeas, checks);
    if (leas < 2 || counts != 2 || pickLeas != 2 || checks != 2) {
        return false;
    }
    s_routes[s_routeCount++] = { buffCall + 5, s_targetPicks[0], TargetCount() };
    s_routes[s_routeCount++] = { debuffCall + 5, s_targetPicks[1], TargetCount() };
    return true;
}

/// Makes the own buff or debuff window fill the configured number: the picks go into its stack buffer, whose following input
/// has room, and the slots into the window's own arrays.
static bool PrepareOwnWindow(HMODULE game, BYTE* vtable, BYTE* init, BYTE* selector, bool debuffs, DWORD buffSlots,
    DWORD debuffSlots, std::vector<Patch>& patches) {
    BYTE* update = FindUpdate(game, vtable, selector);
    BYTE* call = update ? FindCall(update, selector) : nullptr;
    int picks, effects;
    if (!call || !CallBuffers(call, picks, effects) || effects != picks + CLIENT_PICKS_SIZE) {
        ModsLog("buffs: %s window update not recognised (%p)", debuffs ? "debuff" : "buff", update);
        return false;
    }
    if (!ExtendWindow(game, vtable, init, OwnCount(), !debuffs, debuffs, patches)) {
        return false;
    }
    int leas, counts;
    RedirectSlots(update, buffSlots, debuffSlots, s_windows[s_windowCount - 1].slots, true, patches, leas, counts);
    int checks = PatchLastSlotChecks(update, OwnCount(), patches);
    ModsLog("buffs: %s update %p: %d slot arrays, %d loops, %d slot checks", debuffs ? "debuff" : "buff", update, leas, counts, checks);
    if (leas != 2 || counts != 2 || checks != 1) {
        return false;
    }
    s_routes[s_routeCount++] = { call + 5, nullptr, OwnCount() };
    return true;
}

/// mov dword ptr [reg+disp32], imm32 or its qword form at p: the start of the field, nullptr if p holds none.
static BYTE* FieldStore(BYTE* p) {
    BYTE* store = p[0] == 0x48 ? p + 1 : p;
    return store[0] == 0xC7 && (store[1] & 0xF8) == 0x80 && (store[1] & 7) != 4 ? store + 2 : nullptr;
}

/// Sets the columns and rows of an own window in its constructor: mov [reg+columns], 8 and a later mov [reg+columns+rowsAt],
/// rows (3 in 4.x, 5 for the 5.x buff window, some as qword stores).
static bool PatchOwnGrid(HMODULE game, BYTE* vtable, int rowsAt, int window, std::vector<Patch>& patches) {
    BYTE* constructor = ConstructorOf(game, vtable);
    BYTE* end = constructor ? FunctionBodyEnd(constructor, 0x800) : nullptr;
    for (BYTE* p = constructor; p && p + 11 <= end; p++) {
        BYTE* columns = FieldStore(p);
        if (!columns || *(DWORD*)(columns + 4) != OWN_COLUMNS) {
            continue;
        }
        for (BYTE* q = p + 1; q < p + 0x40 && q + 11 <= end; q++) {
            BYTE* rows = FieldStore(q);
            if (rows && *(DWORD*)rows == *(DWORD*)columns + rowsAt && *(DWORD*)(rows + 4) > 0 && *(DWORD*)(rows + 4) <= 10) {
                s_ownRows[window] = *(DWORD*)(rows + 4);
                AddDword(patches, columns + 4, OwnColumns());
                AddDword(patches, rows + 4, RowsFor(OwnColumns(), OwnCount()));
                return true;
            }
        }
    }
    return false;
}

/// Lays the own buff and debuff windows out in the configured columns, with as many rows as their count needs.
static void InstallOwnGrid(HMODULE game) {
    std::vector<Patch> patches;
    BYTE* buffVtable = FindVtable(game, ".?AVDlgBuffStatus@@");
    BYTE* debuffVtable = FindVtable(game, ".?AVDlgDebuffStatus@@");
    s_ownGridReady = buffVtable && debuffVtable && PatchOwnGrid(game, buffVtable, 4, 0, patches)
        && PatchOwnGrid(game, debuffVtable, 8, 1, patches);
    ModsLog("buffs: own windows grid %dx%d %s", OwnColumns(), RowsFor(OwnColumns(), OwnCount()), s_ownGridReady ? "set" : "not found");
    if (s_ownGridReady) {
        Apply(patches);
    }
}

// --- 5.x clients ---
// 5.x windows hold 40 buff and 24 debuff slots and fill them through one function shared with the group windows, which takes
// the slot number. The windows here keep the slots past those in a table of their own; for such a slot the fill function gets
// it put in place of the first one and the layout the real number.

typedef int(__fastcall* SelectBuffs5_t)(BYTE* effects, __int64 count, BYTE* picks, __int64 a4, __int64 mode, __int64 reversed,
    __int64 a7, __int64 a8);
typedef int(__fastcall* SelectDebuffs5_t)(BYTE* effects, __int64 count, BYTE* picks, __int64 a4, __int64 mode);
typedef void(__fastcall* FillSlot_t)(BYTE* window, __int64 buffs, __int64 icon, __int64 index, __int64 a5, __int64 a6, __int64 a7,
    __int64 a8, __int64 a9);
typedef double*(__fastcall* SlotRect_t)(BYTE* window, double* rect, __int64 buffs, __int64 index);
typedef void(__fastcall* HideSlots_t)(BYTE* window);
static SelectBuffs5_t real_SelectBuffs5 = nullptr;
static SelectDebuffs5_t real_SelectDebuffs5 = nullptr;
static FillSlot_t real_FillSlot = nullptr;
static SlotRect_t real_SlotRect = nullptr;
static HideSlots_t real_HideSlots = nullptr;

/// A window with more slots than the client's: its instance once the init ran, and its slots of each kind.
struct SlotTable {
    BYTE* vtable;
    BYTE* window;
    int count;
    bool kinds[2];
    BYTE* slots[2][MAX_EFFECTS];
};
static SlotTable s_tables[3];
static int s_tableCount = 0;
// the client's slot arrays (buffs, debuffs) in the window and how many slots each has
static DWORD s_arrays[2] = {};
static int s_clientSlots[2] = {};
static int s_slotRectIndex = -1;

/// Mode 2 keeps the first picks, the other mode the last ones; reversed writes them last first.
static int Deliver5(BYTE* picks, int picked, bool takeFirst, int clientSlots, BYTE* returnTo) {
    const SelectRoute* route = nullptr;
    for (int i = 0; i < s_routeCount; i++) {
        if (s_routes[i].returnTo == returnTo) {
            route = &s_routes[i];
        }
    }
    int kept = min(picked, route ? route->count : clientSlots);
    BYTE* from = s_scratch + (takeFirst ? 0 : picked - kept) * ENTRY_SIZE_5X;
    BYTE* target = route && route->picks ? route->picks : picks;
    memcpy(target, from, kept * ENTRY_SIZE_5X);
    if (target != picks) {
        memcpy(picks, from, min(kept, clientSlots) * ENTRY_SIZE_5X);
    }
    return kept;
}

static int __fastcall zzSelectBuffs5(BYTE* effects, __int64 count, BYTE* picks, __int64 a4, __int64 mode, __int64 reversed, __int64 a7,
    __int64 a8) {
    int picked = real_SelectBuffs5(effects, count, s_scratch, a4, mode, reversed, a7, a8);
    return Deliver5(picks, picked, ((int)mode == 2) != ((char)reversed != 0), s_clientSlots[0], (BYTE*)_ReturnAddress());
}

static int __fastcall zzSelectDebuffs5(BYTE* effects, __int64 count, BYTE* picks, __int64 a4, __int64 mode) {
    int picked = real_SelectDebuffs5(effects, count, s_scratch, a4, mode);
    return Deliver5(picks, picked, (int)mode == 2, s_clientSlots[1], (BYTE*)_ReturnAddress());
}

static SlotTable* TableOf(BYTE* window) {
    for (int t = 0; t < s_tableCount; t++) {
        if (s_tables[t].window == window && *(BYTE**)window == s_tables[t].vtable) {
            return &s_tables[t];
        }
    }
    return nullptr;
}

static void __fastcall zzInitWindow5(BYTE* window) {
    real_InitWindow(window);
    for (int t = 0; t < s_tableCount; t++) {
        SlotTable& table = s_tables[t];
        if (*(BYTE**)window != table.vtable) {
            continue;
        }
        table.window = window;
        FindChild_t findChild = *(FindChild_t*)(*(BYTE**)window + s_findChildSlot);
        for (int kind = 0; kind < 2; kind++) {
            for (int i = 0; i < MAX_EFFECTS; i++) {
                char name[32];
                sprintf_s(name, kind == 0 ? "buff_item%d" : "debuff_item%d", i + 1);
                BYTE* slot = table.kinds[kind] && i < table.count ? findChild(window, name, s_findChildFlags) : nullptr;
                if (slot) {
                    *(DWORD*)(slot + s_slotFlagOffset) = s_slotFlag;
                }
                table.slots[kind][i] = slot;
            }
        }
        return;
    }
}

static void __fastcall zzFillSlot(BYTE* window, __int64 buffs, __int64 icon, __int64 index, __int64 a5, __int64 a6, __int64 a7,
    __int64 a8, __int64 a9) {
    int kind = (int)buffs ? 0 : 1;
    int i = (int)index;
    SlotTable* table = i >= s_clientSlots[kind] && i < MAX_EFFECTS ? TableOf(window) : nullptr;
    if (!table || !table->slots[kind][i]) {
        real_FillSlot(window, buffs, icon, index, a5, a6, a7, a8, a9);
        return;
    }
    BYTE** first = (BYTE**)(window + s_arrays[kind]);
    BYTE* client = *first;
    *first = table->slots[kind][i];
    s_slotRectIndex = i;
    real_FillSlot(window, buffs, icon, 0, a5, a6, a7, a8, a9);
    s_slotRectIndex = -1;
    *first = client;
}

static double* __fastcall zzSlotRect(BYTE* window, double* rect, __int64 buffs, __int64 index) {
    return real_SlotRect(window, rect, buffs, s_slotRectIndex >= 0 ? s_slotRectIndex : index);
}

/// Hides the slots past the client's ones too, by passing them in the window's arrays to the client's function.
static void __fastcall zzHideSlots(BYTE* window) {
    real_HideSlots(window);
    SlotTable* table = TableOf(window);
    if (!table) {
        return;
    }
    BYTE** arrays[2] = { (BYTE**)(window + s_arrays[0]), (BYTE**)(window + s_arrays[1]) };
    BYTE* client[2][MAX_EFFECTS];
    int next[2] = { s_clientSlots[0], s_clientSlots[1] };
    for (int kind = 0; kind < 2; kind++) {
        memcpy(client[kind], arrays[kind], s_clientSlots[kind] * sizeof(BYTE*));
    }
    while (next[0] < table->count || next[1] < table->count) {
        for (int kind = 0; kind < 2; kind++) {
            for (int i = 0; i < s_clientSlots[kind]; i++, next[kind]++) {
                arrays[kind][i] = next[kind] < table->count ? table->slots[kind][next[kind]] : nullptr;
            }
        }
        real_HideSlots(window);
    }
    for (int kind = 0; kind < 2; kind++) {
        memcpy(arrays[kind], client[kind], s_clientSlots[kind] * sizeof(BYTE*));
    }
}

/// The limit a 5.x selector writes into its code, as mov r32, n; cmp r32, n; cmp r12, n; lea eax, [rdx-n]. The number found.
static int PatchSelectorLimit5(BYTE* selector, int limit, std::vector<Patch>& patches) {
    BYTE* end = FunctionBodyEnd(selector, 0x2000);
    int found = 0;
    for (BYTE* p = selector; p + 5 <= end; p++) {
        Patch patch = {};
        if (p[0] >= 0xB8 && p[0] <= 0xBF && p[0] != 0xBC && *(DWORD*)(p + 1) == (DWORD)limit) {
            AddDword(patches, p + 1, MAX_EFFECTS);
            patch = patches.back();
            patches.pop_back();
        } else if (p[0] == 0x83 && p[1] >= 0xF8 && p[2] == limit && (p == selector || (p[-1] & 0xF0) != 0x40)) {
            patch = { p + 2, { MAX_EFFECTS } };
        } else if (p[0] == 0x49 && p[1] == 0x83 && p[2] >= 0xF8 && p[3] == limit) {
            patch = { p + 3, { MAX_EFFECTS } };
        } else if (p[0] == 0x8D && p[1] == 0x42 && p[2] == (BYTE)-limit) {
            patch = { p + 2, { (BYTE)-MAX_EFFECTS } };
        }
        // the ranges of the function are not contiguous; code between them belongs to others
        if (patch.at && FunctionStart(p) == selector) {
            patches.push_back(patch);
            found++;
        }
    }
    return found;
}

/// The function the update calls that hides every slot: lea reg, [rcx+buffs]; mov r32, n; ... lea reg, [rcx+debuffs];
/// mov r32, m. Gets the slot counts.
static bool IsHideSlots(BYTE* function, int counts[2]) {
    BYTE* end = FunctionBodyEnd(function, 0x200);
    int found = 0;
    for (BYTE* p = function; p + 12 <= end && found < 2; p++) {
        BYTE* displacement = FieldLea(p, s_arrays[found]);
        BYTE* count = displacement && displacement[4] >= 0xB8 && displacement[4] <= 0xBF ? displacement + 4 : nullptr;
        if (count && *(DWORD*)(count + 1) > 0 && *(DWORD*)(count + 1) <= 64) {
            counts[found++] = *(DWORD*)(count + 1);
        }
    }
    return found == 2;
}

/// The function that fills one slot: mov reg, [rcx+index*8+buffs] and the same for debuffs.
static bool IsFillSlot(BYTE* function) {
    BYTE* end = FunctionBodyEnd(function, 0x400);
    int found = 0;
    for (BYTE* p = function; p + 8 <= end; p++) {
        if (p[0] == 0x8B && (p[1] & 0xC7) == 0x84 && (p[2] >> 6) == 3) {
            found |= *(DWORD*)(p + 3) == s_arrays[0] ? 1 : *(DWORD*)(p + 3) == s_arrays[1] ? 2 : 0;
        }
    }
    return found == 3;
}

/// The direct calls of a function, in order.
static std::vector<BYTE*> CallsOf(HMODULE game, BYTE* function, size_t maxSize) {
    std::vector<BYTE*> calls;
    BYTE* end = FunctionBodyEnd(function, maxSize);
    for (BYTE* p = function; p + 5 <= end; p++) {
        BYTE* target = p[0] == 0xE8 ? ResolveRip(p + 1, p + 5) : nullptr;
        if (target && InModule(game, target, 16) && FunctionStart(target) == target) {
            calls.push_back(p);
        }
    }
    return calls;
}

/// lea reg, [rsp+disp8|disp32] or [rbp+disp32] at p: its length, the base and the displacement; 0 if p holds none.
static int StackLeaAt(BYTE* p, int& base, int& displacement) {
    if ((p[0] & 0xFB) != 0x48 || p[1] != 0x8D) {
        return 0;
    }
    BYTE mod = p[2] >> 6, rm = p[2] & 7;
    if (rm == 4 && p[3] == 0x24 && (mod == 1 || mod == 2)) {
        base = 4;
        displacement = mod == 1 ? (signed char)p[4] : *(INT32*)(p + 4);
        return mod == 1 ? 5 : 8;
    }
    if (rm == 5 && mod == 2) {
        base = 5;
        displacement = *(INT32*)(p + 3);
        return 7;
    }
    return 0;
}

/// Makes the lea of length length at p load target instead: lea reg, [rip+target] in place, or for a shorter lea, a jump to it
/// in stub followed by a jump back. Returns the stub space used.
static int RedirectLea(BYTE* p, int length, BYTE* target, BYTE* stub, std::vector<Patch>& patches) {
    BYTE rex = p[0], reg = (p[2] >> 3) & 7;
    if (length >= 7) {
        INT32 rel = (INT32)(target - (p + 7));
        BYTE* r = (BYTE*)&rel;
        Add(patches, p, { rex, 0x8D, (BYTE)(reg << 3 | 5), r[0], r[1], r[2], r[3] });
        for (int i = 7; i < length; i++) {
            Add(patches, p + i, { 0x90 });
        }
        return 0;
    }
    INT32 lea = (INT32)(target - (stub + 7));
    INT32 back = (INT32)(p + length - (stub + 12));
    BYTE* l = (BYTE*)&lea;
    BYTE* b = (BYTE*)&back;
    BYTE code[12] = { rex, 0x8D, (BYTE)(reg << 3 | 5), l[0], l[1], l[2], l[3], 0xE9, b[0], b[1], b[2], b[3] };
    memcpy(stub, code, sizeof(code));
    INT32 to = (INT32)(stub - (p + 5));
    BYTE* t = (BYTE*)&to;
    Add(patches, p, { 0xE9, t[0], t[1], t[2], t[3] });
    for (int i = 5; i < length; i++) {
        Add(patches, p + i, { 0x90 });
    }
    return sizeof(code);
}

/// Every lea in the function of the stack buffer a selector call passes in r8, except that one, now loads picks.
static int RedirectPicks(BYTE* update, BYTE* call, BYTE* picks, BYTE*& stub, std::vector<Patch>& patches) {
    int base = 0, displacement = 0;
    BYTE* argument = nullptr;
    for (BYTE* p = call - 0x30; p < call; p++) {
        int b, d;
        if (p[0] == 0x4C && StackLeaAt(p, b, d) && ((p[2] >> 3) & 7) == 0) {
            argument = p;
            base = b;
            displacement = d;
        }
    }
    int redirected = 0;
    BYTE* end = FunctionBodyEnd(update, 0x2000);
    for (BYTE* p = call + 5; argument && p + 8 <= end; p++) {
        int b, d, length = StackLeaAt(p, b, d);
        if (length && b == base && d == displacement && ((p[2] >> 3) & 7) != 0) {
            stub += RedirectLea(p, length, picks, stub, patches);
            redirected++;
        }
    }
    return redirected;
}

/// The window's update: the entry of its table that calls the functions hiding and filling the slots.
static BYTE* FindUpdate5(HMODULE game, BYTE* vtable, int counts[2]) {
    for (int i = 0; vtable && i < 260; i++) {
        BYTE* function = ((BYTE**)vtable)[i];
        if (!InModule(game, function, 16)) {
            break;
        }
        if (FunctionStart(function) != function) {
            continue;
        }
        bool hides = false, fills = false;
        for (BYTE* call : CallsOf(game, function, 0x800)) {
            BYTE* target = ResolveRip(call + 1, call + 5);
            hides = hides || IsHideSlots(target, counts);
            fills = fills || IsFillSlot(target);
        }
        if (hides && fills) {
            return function;
        }
    }
    return nullptr;
}

static void AddTable(BYTE* vtable, int count, bool buffs, bool debuffs) {
    SlotTable& table = s_tables[s_tableCount++];
    table = {};
    table.vtable = vtable;
    table.count = count;
    table.kinds[0] = buffs;
    table.kinds[1] = debuffs;
}

/// Gives the init that fills the slot tables to the window's table.
static bool ReplaceInit(BYTE* vtable, BYTE* init, std::vector<Patch>& patches) {
    for (int i = 0; i < 64; i++) {
        if (((BYTE**)vtable)[i] == init) {
            void* replacement = zzInitWindow5;
            Add(patches, vtable + 8 * i, {});
            patches.back().bytes.assign((BYTE*)&replacement, (BYTE*)&replacement + 8);
            return true;
        }
    }
    return false;
}

/// 5.x: shows up to MAX_EFFECTS buffs and as many debuffs in the own windows and the target window.
static void InstallBuffCount5(HMODULE game, BYTE* buffVtable, BYTE* debuffVtable, BYTE* targetVtable, BYTE* init) {
    BYTE* buffName = FindBytes(game, "\0buff_item%d", 13);
    BYTE* debuffName = FindBytes(game, "\0debuff_item%d", 15);
    BYTE* buffLea = buffName ? FindLeaTo(game, buffName + 1) : nullptr;
    BYTE* debuffLea = debuffName ? FindLeaTo(game, debuffName + 1) : nullptr;
    for (int kind = 0; kind < 2; kind++) {
        BYTE* nameLea = kind == 0 ? buffLea : debuffLea;
        for (BYTE* p = nameLea ? nameLea - 0x30 : nullptr; p && p < nameLea; p++) {
            BYTE* displacement = FieldLeaDisplacement(p);
            if (displacement && displacement + 4 <= nameLea && *(DWORD*)displacement > 0x100 && *(DWORD*)displacement < 0x2000) {
                s_arrays[kind] = *(DWORD*)displacement;
            }
        }
    }
    int counts[2];
    BYTE* buffUpdate = s_arrays[0] && s_arrays[1] ? FindUpdate5(game, buffVtable, counts) : nullptr;
    BYTE* debuffUpdate = buffUpdate ? FindUpdate5(game, debuffVtable, counts) : nullptr;
    if (!buffUpdate || !debuffUpdate || !ParseInit(init)) {
        ModsLog("buffs: 5.x slot arrays %X/%X, updates %p/%p or init not found", s_arrays[0], s_arrays[1], buffUpdate, debuffUpdate);
        return;
    }
    s_clientSlots[0] = counts[0];
    s_clientSlots[1] = counts[1];
    std::vector<Patch> selectorPatches, windowPatches;
    BYTE *buffSelector = nullptr, *debuffSelector = nullptr, *buffCall = nullptr, *debuffCall = nullptr, *fill = nullptr, *hide = nullptr;
    for (BYTE* call : CallsOf(game, buffUpdate, 0x800)) {
        BYTE* target = ResolveRip(call + 1, call + 5);
        int hideCounts[2];
        std::vector<Patch> limits;
        if (!buffSelector && PatchSelectorLimit5(target, s_clientSlots[0], limits) == 3) {
            buffSelector = target;
            buffCall = call;
            selectorPatches.insert(selectorPatches.end(), limits.begin(), limits.end());
        } else if (!fill && IsFillSlot(target)) {
            fill = target;
        } else if (!hide && IsHideSlots(target, hideCounts)) {
            hide = target;
        }
    }
    for (BYTE* call : CallsOf(game, debuffUpdate, 0x800)) {
        BYTE* target = ResolveRip(call + 1, call + 5);
        std::vector<Patch> limits;
        if (PatchSelectorLimit5(target, s_clientSlots[1], limits) == 4) {
            debuffSelector = target;
            debuffCall = call;
            selectorPatches.insert(selectorPatches.end(), limits.begin(), limits.end());
            break;
        }
    }
    // the slot rect is the fill function's last direct call, a division of the number by the columns
    std::vector<BYTE*> fillCalls = fill ? CallsOf(game, fill, 0x400) : std::vector<BYTE*>();
    BYTE* rect = fillCalls.empty() ? nullptr : ResolveRip(fillCalls.back() + 1, fillCalls.back() + 5);
    int picks, effects;
    if (!buffSelector || !debuffSelector || !fill || !hide || !rect || selectorPatches.size() != 7
        || !CallBuffers(buffCall, picks, effects) || effects != picks + s_clientSlots[0] * ENTRY_SIZE_5X
        || !CallBuffers(debuffCall, picks, effects) || effects != picks + s_clientSlots[1] * ENTRY_SIZE_5X) {
        ModsLog("buffs: 5.x selectors %p/%p, fill %p, hide %p, rect %p, %d limits not recognised", buffSelector, debuffSelector, fill,
            hide, rect, (int)selectorPatches.size());
        return;
    }

    s_ownWindowsReady = OwnCount() > s_clientSlots[1] && ReplaceInit(buffVtable, init, windowPatches)
        && ReplaceInit(debuffVtable, init, windowPatches);
    if (s_ownWindowsReady) {
        AddTable(buffVtable, OwnCount(), true, false);
        AddTable(debuffVtable, OwnCount(), false, true);
        s_routes[s_routeCount++] = { buffCall + 5, nullptr, OwnCount() };
        s_routes[s_routeCount++] = { debuffCall + 5, nullptr, OwnCount() };
    } else {
        windowPatches.clear();
    }

    BYTE* targetUpdate = FindUpdate(game, targetVtable, buffSelector);
    BYTE* targetBuffCall = targetUpdate ? FindCall(targetUpdate, buffSelector) : nullptr;
    BYTE* targetDebuffCall = targetUpdate ? FindCall(targetUpdate, debuffSelector) : nullptr;
    DWORD regionSize = 0;
    BYTE* region = targetUpdate ? (BYTE*)DetourAllocateRegionWithinJumpBounds(targetUpdate, &regionSize) : nullptr;
    if (TargetCount() > s_clientSlots[1] && targetBuffCall && targetDebuffCall && region
        && regionSize >= 2 * MAX_EFFECTS * ENTRY_SIZE_5X + 64) {
        std::vector<Patch> targetPatches;
        BYTE* buffPicks = region;
        BYTE* debuffPicks = region + MAX_EFFECTS * ENTRY_SIZE_5X;
        BYTE* stub = region + 2 * MAX_EFFECTS * ENTRY_SIZE_5X;
        int buffReads = RedirectPicks(targetUpdate, targetBuffCall, buffPicks, stub, targetPatches);
        int debuffReads = RedirectPicks(targetUpdate, targetDebuffCall, debuffPicks, stub, targetPatches);
        ModsLog("buffs: 5.x target update %p: %d buff and %d debuff pick reads", targetUpdate, buffReads, debuffReads);
        if (buffReads == 1 && debuffReads == 1 && ReplaceInit(targetVtable, init, targetPatches)) {
            AddTable(targetVtable, TargetCount(), true, true);
            s_routes[s_routeCount++] = { targetBuffCall + 5, buffPicks, TargetCount() };
            s_routes[s_routeCount++] = { targetDebuffCall + 5, debuffPicks, TargetCount() };
            windowPatches.insert(windowPatches.end(), targetPatches.begin(), targetPatches.end());
            s_targetWindowReady = true;
        }
    }
    ModsLog("buffs: 5.x client slots %d/%d, own windows %s, target window %s", s_clientSlots[0], s_clientSlots[1],
        s_ownWindowsReady ? "ready" : "unchanged", s_targetWindowReady ? "ready" : "unchanged");
    if (!s_routeCount) {
        return;
    }
    Apply(selectorPatches);
    Apply(windowPatches);
    real_InitWindow = (InitWindow_t)init;
    real_SelectBuffs5 = (SelectBuffs5_t)buffSelector;
    real_SelectDebuffs5 = (SelectDebuffs5_t)debuffSelector;
    real_FillSlot = (FillSlot_t)fill;
    real_SlotRect = (SlotRect_t)rect;
    real_HideSlots = (HideSlots_t)hide;
    DetourAttach(&(PVOID&)real_SelectBuffs5, zzSelectBuffs5);
    DetourAttach(&(PVOID&)real_SelectDebuffs5, zzSelectDebuffs5);
    DetourAttach(&(PVOID&)real_FillSlot, zzFillSlot);
    DetourAttach(&(PVOID&)real_SlotRect, zzSlotRect);
    DetourAttach(&(PVOID&)real_HideSlots, zzHideSlots);
}

/// Shows up to MAX_EFFECTS buffs and as many debuffs in the own buff and debuff windows and in the target window. Needs the UI
/// XML edits for the added slots.
static void InstallBuffCount(HMODULE game, BYTE* targetVtable) {
    std::vector<BYTE*> limits = FindAllPatterns(game, "83 FB 18 7C 05 BB 18 00 00 00");
    BYTE* buffVtable = FindVtable(game, ".?AVDlgBuffStatus@@");
    BYTE* debuffVtable = FindVtable(game, ".?AVDlgDebuffStatus@@");
    // with the zero before it, as "debuff_item%d" ends the same way
    BYTE* slotName = FindBytes(game, "\0buff_item%d", 13);
    BYTE* slotNameLea = slotName ? FindLeaTo(game, slotName + 1) : nullptr;
    BYTE* init = slotNameLea ? FunctionStart(slotNameLea) : nullptr;
    if (!buffVtable || !debuffVtable || !targetVtable || !init) {
        ModsLog("buffs: windows or init not found");
        return;
    }
    if (limits.empty()) {
        InstallBuffCount5(game, buffVtable, debuffVtable, targetVtable, init);
        return;
    }
    if (limits.size() != 2) {
        ModsLog("buffs: %d selectors found", (int)limits.size());
        return;
    }
    // which selector is which: the own buff window calls the buff one
    BYTE* selectors[2] = { FunctionStart(limits[0]), FunctionStart(limits[1]) };
    BYTE* buffSelector = FindUpdate(game, buffVtable, selectors[0]) ? selectors[0] : selectors[1];
    BYTE* debuffSelector = buffSelector == selectors[0] ? selectors[1] : selectors[0];
    DWORD buffSlots, debuffSlots;
    std::vector<Patch> selectorPatches, ownPatches, targetPatches;
    if (!PatchSelectorLimit(buffSelector, selectorPatches) || !PatchSelectorLimit(debuffSelector, selectorPatches)
        || !FindSlotArrays(slotNameLea, buffSlots, debuffSlots) || !ParseInit(init)) {
        ModsLog("buffs: selector limits, slot arrays or init not recognised");
        return;
    }
    s_ownWindowsReady = OwnCount() > CLIENT_EFFECTS
        && PrepareOwnWindow(game, buffVtable, init, buffSelector, false, buffSlots, debuffSlots, ownPatches)
        && PrepareOwnWindow(game, debuffVtable, init, debuffSelector, true, buffSlots, debuffSlots, ownPatches);
    if (!s_ownWindowsReady) {
        s_routeCount = 0;
        s_windowCount = 0;
        ownPatches.clear();
    }
    int ownRoutes = s_routeCount, ownWindows = s_windowCount;
    s_targetWindowReady = TargetCount() > CLIENT_EFFECTS && targetVtable
        && PrepareTargetWindow(game, targetVtable, init, buffSelector, debuffSelector, buffSlots, debuffSlots, targetPatches);
    if (!s_targetWindowReady) {
        s_routeCount = ownRoutes;
        s_windowCount = ownWindows;
        targetPatches.clear();
    }
    ModsLog("buffs: own windows %s (%d), target window %s (%d)", s_ownWindowsReady ? "ready" : "unchanged", OwnCount(),
        s_targetWindowReady ? "ready" : "unchanged", TargetCount());
    if (!s_routeCount) {
        return;
    }
    Apply(selectorPatches);
    Apply(ownPatches);
    Apply(targetPatches);
    real_SelectBuffs = (SelectBuffs_t)buffSelector;
    real_SelectDebuffs = (SelectDebuffs_t)debuffSelector;
    DetourAttach(&(PVOID&)real_SelectBuffs, zzSelectBuffs);
    DetourAttach(&(PVOID&)real_SelectDebuffs, zzSelectDebuffs);
    real_InitWindow = (InitWindow_t)init;
    for (int w = 0; w < s_windowCount; w++) {
        DetourAttach(&(PVOID&)s_windows[w].realConstruct, CONSTRUCT_HOOKS[w]);
    }
}

void InstallBuffLimit(HMODULE game) {
    AddUiXmlEdit(L"target_buff_status_dialog", EditTargetWindow);
    AddUiXmlEdit(L"buff_status_dialog", EditBuffWindow);
    AddUiXmlEdit(L"debuff_status_dialog", EditDebuffWindow);
}

/// How many slots of each kind the client fills in its buff windows: 40 in 5.x, whose loop ends in cmp reg, 40; jl right after
/// the slot name, 24 in 4.x.
static int ClientSlots(HMODULE game) {
    BYTE* slotName = FindBytes(game, "\0buff_item%d", 13);
    BYTE* lea = slotName ? FindLeaTo(game, slotName + 1) : nullptr;
    for (BYTE* p = lea; p && p < lea + 0x80; p++) {
        BYTE* cmp = p[0] == 0x41 ? p + 1 : p;
        if (cmp[0] == 0x83 && (cmp[1] & 0xF8) == 0xF8 && cmp[3] == 0x7C && cmp[2] > CLIENT_EFFECTS && cmp[2] <= MAX_EFFECTS) {
            return cmp[2];
        }
    }
    return CLIENT_EFFECTS;
}

/// The target window lays its buff and debuff icons out in a grid of 10 columns and 2 rows each. Its constructor sets the
/// columns, rows of buffs and rows of debuffs one after another:
/// mov dword ptr [reg+columns], 10; mov dword ptr [reg+rows], 2; mov dword ptr [reg+debuffRows], 2
/// Runs after the UI XML hooks are in place, which the added slots need.
void InstallBuffSlots(HMODULE game, bool uiXml) {
    BYTE* vtable = FindVtable(game, ".?AVDlgTargetBuffStatus@@");
    BYTE* constructor = ConstructorOf(game, vtable);
    if (uiXml && max(TargetCount(), OwnCount()) > CLIENT_EFFECTS) {
        InstallBuffCount(game, vtable);
    }
    if (uiXml && g_modsConfig.ownBuffColumns) {
        InstallOwnGrid(game);
    }
    if (!constructor) {
        ModsLog("buffs: target window constructor not found");
        return;
    }
    DWORD columns = g_modsConfig.targetBuffColumns ? g_modsConfig.targetBuffColumns : 10;
    DWORD rows = RowsFor(columns, s_targetWindowReady ? TargetCount() : ClientSlots(game));
    if (!g_modsConfig.targetBuffColumns && !s_targetWindowReady) {
        return;
    }
    BYTE* end = FunctionBodyEnd(constructor, 0x800);
    int patched = 0;
    for (BYTE* p = constructor; p + 30 <= end; p++) {
        if (p[0] == 0xC7 && (p[1] & 0xF8) == 0x80 && *(DWORD*)(p + 6) == 10
            && p[10] == 0xC7 && p[11] == p[1] && *(DWORD*)(p + 12) == *(DWORD*)(p + 2) + 4 && *(DWORD*)(p + 16) == 2
            && p[20] == 0xC7 && p[21] == p[1] && *(DWORD*)(p + 22) == *(DWORD*)(p + 2) + 8 && *(DWORD*)(p + 26) == 2) {
            PatchMemory(p + 6, &columns, sizeof(columns));
            PatchMemory(p + 16, &rows, sizeof(rows));
            PatchMemory(p + 26, &rows, sizeof(rows));
            patched++;
        }
    }
    ModsLog("buffs: target window grid 10x2 -> %dx%d at %d places in %p", columns, rows, patched, constructor);
}
