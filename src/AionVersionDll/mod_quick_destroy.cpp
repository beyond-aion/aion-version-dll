#include "mods.h"
#include <algorithm>
#include <stdio.h>

// Some items ask the player to type a word before the usual yes-or-no question: those of the Black Cloud Merchants and the ones the
// client marks as rare. Both typed questions are replaced by the usual one, so one click is enough; the item is still destroyed only
// after that, and nothing changes for the items that never asked to type anything.
namespace {

// The strings the three questions are built from, in client_strings_ui.xml: the same ids from 3.5 to 5.8.
constexpr DWORD CASH_ITEM_QUESTION = 901688; // STR_DESTROY_CASH_ITEM
constexpr DWORD RARE_ITEM_QUESTION = 902039; // STR_DESTROY_RARE_ITEM_CONFIRM
constexpr DWORD USUAL_QUESTION = 900394;     // STR_MSG_DESTROY_ITEM
constexpr DWORD YES_BUTTON = 900378;         // the button of the usual question

/// Every place in the image that holds the value.
std::vector<BYTE*> FindValues(HMODULE module, DWORD value) {
    char pattern[12];
    sprintf_s(pattern, "%02X %02X %02X %02X", value & 0xFF, (value >> 8) & 0xFF, (value >> 16) & 0xFF, value >> 24);
    return FindAllPatterns(module, pattern);
}

/// The function that shows the question built from the string with the given id. Other code can hold the same number by chance,
/// so the function that also uses the button of the usual question is the one.
BYTE* FindQuestion(HMODULE game, DWORD question) {
    for (BYTE* value : FindValues(game, question)) {
        BYTE* function = FunctionStart(value);
        if (!function) {
            continue;
        }
        BYTE* end = FunctionBodyEnd(function, 0x1000);
        for (BYTE* button : FindValues(game, YES_BUTTON)) {
            if (button >= function && button < end) {
                return function;
            }
        }
    }
    return nullptr;
}

/// Points a direct call at another function.
bool Retarget(BYTE* call, BYTE* function) {
    INT32 distance = (INT32)(function - (call + 5));
    return PatchMemory(call + 1, &distance, sizeof(distance));
}

} // namespace

void InstallQuickDestroy(HMODULE game) {
    BYTE* usual = FindQuestion(game, USUAL_QUESTION);
    BYTE* cash = FindQuestion(game, CASH_ITEM_QUESTION);
    BYTE* rare = FindQuestion(game, RARE_ITEM_QUESTION);
    if (!usual || (!cash && !rare)) {
        ModsLog("quick destroy: usual=%p cash=%p rare=%p", usual, cash, rare);
        return;
    }
    // The windows that destroy an item are the ones that show the typed questions; a call from anywhere else keeps its question.
    std::vector<BYTE*> questions;
    if (cash) {
        questions.push_back(cash);
    }
    if (rare) {
        questions.push_back(rare);
    }
    std::vector<BYTE*> askers;
    std::vector<std::pair<BYTE*, BYTE*>> calls;
    for (BYTE* question : questions) {
        for (BYTE* call : FindAllCallsTo(game, question)) {
            BYTE* function = FunctionStart(call);
            if (function && std::find(askers.begin(), askers.end(), function) == askers.end()) {
                askers.push_back(function);
            }
            calls.emplace_back(call, function);
        }
    }
    int retargeted = 0;
    for (const auto& [call, asker] : calls) {
        if (asker && std::find(askers.begin(), askers.end(), asker) != askers.end() && Retarget(call, usual)) {
            retargeted++;
        }
    }
    ModsLog("quick destroy: usual=%p cash=%p rare=%p, %d calls", usual, cash, rare, retargeted);
}
