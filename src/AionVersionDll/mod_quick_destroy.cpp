#include "mods.h"
#include <algorithm>
#include <stdio.h>

// Some items ask the player to type a word instead of the usual yes-or-no question: those of the Black Cloud Merchants and the ones
// the client marks as rare. The calls of both typed questions are pointed at the usual one, so one click is enough; the item is still
// destroyed only after it. All three questions take the same arguments (the window and the item), from 3.5 to 5.8.
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
/// so the function that also holds the button of the usual question is the one.
BYTE* FindQuestion(HMODULE game, DWORD question, const std::vector<BYTE*>& buttons) {
    for (BYTE* value : FindValues(game, question)) {
        BYTE* function = FunctionStart(value);
        if (!function) {
            continue;
        }
        BYTE* end = FunctionBodyEnd(function, 0x1000);
        for (BYTE* button : buttons) {
            if (button >= function && button < end) {
                return function;
            }
        }
    }
    return nullptr;
}

/// The functions with a direct call to target.
std::vector<BYTE*> Callers(HMODULE game, BYTE* target) {
    std::vector<BYTE*> callers;
    for (BYTE* call : FindAllCallsTo(game, target)) {
        if (BYTE* function = FunctionStart(call)) {
            callers.push_back(function);
        }
    }
    return callers;
}

struct Place {
    BYTE* usual = nullptr;
    BYTE* cash = nullptr;
    BYTE* rare = nullptr;
    // the calls of the typed questions from the windows that destroy an item, pointed at the usual question
    std::vector<CodePatch> calls;
};

/// A direct call pointed at another function.
CodePatch Retarget(BYTE* call, BYTE* function) {
    INT32 distance = (INT32)(function - (call + 5));
    return PlanPatch(call + 1, &distance, sizeof(distance));
}

Place Locate(HMODULE game) {
    Place place;
    std::vector<BYTE*> buttons = FindValues(game, YES_BUTTON);
    place.usual = FindQuestion(game, USUAL_QUESTION, buttons);
    place.cash = FindQuestion(game, CASH_ITEM_QUESTION, buttons);
    place.rare = FindQuestion(game, RARE_ITEM_QUESTION, buttons);
    if (!place.usual || (!place.cash && !place.rare)) {
        return place;
    }
    // The windows that destroy an item pick one of the three questions, so a call is changed only in a function that also calls the
    // usual one; any other caller keeps its question.
    std::vector<BYTE*> windows = Callers(game, place.usual);
    for (BYTE* question : { place.cash, place.rare }) {
        if (!question) {
            continue;
        }
        for (BYTE* call : FindAllCallsTo(game, question)) {
            BYTE* function = FunctionStart(call);
            if (function && std::find(windows.begin(), windows.end(), function) != windows.end()) {
                place.calls.push_back(Retarget(call, place.usual));
            }
        }
    }
    return place;
}

void Apply(const Place& place) {
    if (!place.usual || (!place.cash && !place.rare)) {
        ModsLog("quick destroy: usual=%p cash=%p rare=%p", place.usual, place.cash, place.rare);
        return;
    }
    int retargeted = 0;
    for (const CodePatch& call : place.calls) {
        retargeted += ApplyPatch(call);
    }
    ModsLog("quick destroy: usual=%p cash=%p rare=%p, %d calls", place.usual, place.cash, place.rare, retargeted);
}

} // namespace

void InstallQuickDestroy(HMODULE game) {
    LocateThenApply(game, Locate, Apply);
}
