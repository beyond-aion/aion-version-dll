#include "mods.h"
#include <map>

/// The macro window creates one list slot per allowed macro when it opens; macros beyond them would not show:
/// lea rdx, "macro_list"; ...; mov dword ptr [list+x], 1; mov r32, <slots>
static void MakeMacroSlots(HMODULE game, int oldLimit) {
    static const char MACRO_LIST[] = "macro_list";
    const void* name = FindBytes(game, MACRO_LIST, sizeof(MACRO_LIST));
    BYTE* lookup = name ? FindLeaTo(game, name) : nullptr;
    for (BYTE* p = lookup; p && p < lookup + 0x50; p++) {
        if (p[0] == 0xC7 && (p[1] & 0xF8) == 0x80 && *(DWORD*)(p + 6) == 1 && (p[10] & 0xF8) == 0xB8 && *(DWORD*)(p + 11) == (DWORD)oldLimit) {
            DWORD slots = g_modsConfig.macroLimit;
            PatchMemory(p + 11, &slots, sizeof(slots));
            ModsLog("macro limit: window slots at %p", p + 10);
            return;
        }
    }
    ModsLog("macro limit: window slots not found");
}

/// Raises the number of macros the client lets the player create. The server has to accept the higher slot numbers as well.
/// The client compares its macro count against the limit twice: when a macro is created (right at the start of that function)
/// and in the macro window (to disable the add button). Both compare the same field: cmp qword ptr [reg+count], limit
void InstallMacroLimit(HMODULE game) {
    std::map<DWORD, std::vector<BYTE*>> byField;
    for (const char* pattern : { "48 83 ?? ?? ?? 00 00 0C", "49 83 ?? ?? ?? 00 00 0C", "48 83 ?? ?? ?? 00 00 18", "49 83 ?? ?? ?? 00 00 18" }) {
        for (BYTE* p : FindAllPatterns(game, pattern)) {
            if ((p[2] & 0xF8) == 0xB8 && p[2] != 0xBC) {
                byField[*(DWORD*)(p + 3) | (DWORD)p[7] << 24].push_back(p);
            }
        }
    }
    std::vector<BYTE*> found;
    for (auto& entry : byField) {
        std::vector<BYTE*>& sites = entry.second;
        if (sites.size() != 2) {
            continue;
        }
        int atStart = 0;
        for (BYTE* site : sites) {
            BYTE* function = FunctionStart(site);
            atStart += function && site - function < 0x20;
        }
        if (atStart == 1) {
            if (!found.empty()) {
                ModsLog("macro limit: more than one candidate");
                return;
            }
            found = sites;
        }
    }
    if (found.empty()) {
        ModsLog("macro limit: checks not found");
        return;
    }
    int oldLimit = found[0][7];
    ModsLog("macro limit: checks at %p and %p, limit %d -> %d", found[0], found[1], oldLimit, g_modsConfig.macroLimit);
    if (g_modsConfig.macroLimit <= oldLimit) {
        return;
    }
    BYTE limit = (BYTE)g_modsConfig.macroLimit;
    for (BYTE* site : found) {
        PatchMemory(site + 7, &limit, 1);
    }
    MakeMacroSlots(game, oldLimit);
}
