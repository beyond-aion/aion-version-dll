#include "mods.h"
#include <algorithm>
#include <map>

namespace {

struct Place {
    // the two compares of the macro count with the limit, their limit byte kept
    CodePatch checks[2];
    int oldLimit = 0;
    bool ambiguous = false;
    // mov r32, <slots> in the macro window, its immediate kept; nullptr if not found
    CodePatch slots;
};

/// The macro window creates one list slot per allowed macro when it opens; macros beyond them would not show:
/// lea rdx, "macro_list"; ...; mov dword ptr [list+x], 1; mov r32, <slots>
CodePatch FindMacroSlots(HMODULE game, int oldLimit) {
    static const char MACRO_LIST[] = "macro_list";
    BYTE* lookup = FindStringLea(game, MACRO_LIST);
    for (BYTE* p = lookup; p && p < lookup + 0x50; p++) {
        if (p[0] == 0xC7 && (p[1] & 0xF8) == 0x80 && *(DWORD*)(p + 6) == 1 && (p[10] & 0xF8) == 0xB8 && *(DWORD*)(p + 11) == (DWORD)oldLimit) {
            return ExpectBytes(p + 11, sizeof(DWORD));
        }
    }
    return CodePatch();
}

/// The client compares its macro count against the limit twice: when a macro is created (right at the start of that function)
/// and in the macro window (to disable the add button). Both compare the same field: cmp qword ptr [reg+count], limit
Place Locate(HMODULE game) {
    Place place;
    std::map<DWORD, std::vector<BYTE*>> byField;
    // 48/49 83 ?? ?? ?? 00 00 0C/18, in one pass; the sites of a field in the order of 48 0C, 49 0C, 48 18, 49 18
    std::vector<BYTE*> compares;
    for (BYTE* p : FindAllPatterns(game, "83 ?? ?? ?? 00 00")) {
        if ((p[-1] == 0x48 || p[-1] == 0x49) && (p[6] == 0x0C || p[6] == 0x18) && (p[1] & 0xF8) == 0xB8 && p[1] != 0xBC) {
            compares.push_back(p - 1);
        }
    }
    std::stable_sort(compares.begin(), compares.end(), [](BYTE* a, BYTE* b) { return (a[7] == 0x18) * 2 + (a[0] == 0x49) < (b[7] == 0x18) * 2 + (b[0] == 0x49); });
    for (BYTE* p : compares) {
        byField[*(DWORD*)(p + 3) | (DWORD)p[7] << 24].push_back(p);
    }
    std::vector<BYTE*> found;
    for (auto& entry : byField) {
        std::vector<BYTE*>& sites = entry.second;
        if (sites.size() != 2) {
            continue;
        }
        // both in code, which data has no unwind entries for, and only the one in the creation right at the start of its function
        int inCode = 0, atStart = 0;
        for (BYTE* site : sites) {
            BYTE* function = FunctionStart(site);
            inCode += function != nullptr;
            atStart += function && site - function < 0x20;
        }
        if (inCode == 2 && atStart == 1) {
            if (!found.empty()) {
                place.ambiguous = true;
                return place;
            }
            found = sites;
        }
    }
    if (found.empty()) {
        return place;
    }
    place.oldLimit = found[0][7];
    for (int i = 0; i < 2; i++) {
        place.checks[i] = ExpectBytes(found[i] + 7, 1);
    }
    place.slots = FindMacroSlots(game, place.oldLimit);
    return place;
}

/// Raises the number of macros the client lets the player create. The server has to accept the higher slot numbers as well.
void Apply(const Place& place) {
    if (place.ambiguous) {
        ModsLog("macro limit: more than one candidate");
        ReportStatus(InstallStatus::Ambiguous);
        return;
    }
    if (!place.checks[0]) {
        ModsLog("macro limit: checks not found");
        return;
    }
    ModsLog("macro limit: checks at %p and %p, limit %d -> %d", place.checks[0].at - 7, place.checks[1].at - 7, place.oldLimit,
        g_modsConfig.macroLimit);
    if (g_modsConfig.macroLimit <= place.oldLimit) {
        return;
    }
    BYTE limit = (BYTE)g_modsConfig.macroLimit;
    for (const CodePatch& check : place.checks) {
        ApplyPatch(check, &limit);
    }
    if (!place.slots) {
        ModsLog("macro limit: window slots not found");
        return;
    }
    DWORD slots = g_modsConfig.macroLimit;
    ApplyPatch(place.slots, &slots);
    ModsLog("macro limit: window slots at %p", place.slots.at - 1);
}

} // namespace

void InstallMacroLimit(HMODULE game) {
    LocateThenApply(game, Locate, Apply);
}
