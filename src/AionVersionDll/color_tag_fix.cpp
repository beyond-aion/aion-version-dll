#include "code_patch.h"
#include "mods_log.h"

namespace {

struct ColorTagPlace {
    // the parser's compare of a tag with the name color:
    BYTE* compare = nullptr;
    // the limit of the text, in the clients that have one
    CodePatch limit;
};

/// The parser compares a tag with each name through a pointer to it: mov rdx, [name]; ...; call [_wcsnicmp]. Older clients (4.6) then
/// cut the text of a color tag to the length of the name "color:" minus one, so 5 characters: lea r9d, [r12-1] with r12 the name length,
/// which becomes lea r9d, [r12+79h] = 127. Later ones (4.8, 5.x) take the text as it is and have no such limit.
ColorTagPlace LocateColorTag(HMODULE game) {
    ColorTagPlace place;
    static const wchar_t NAME[] = L"color:";
    const void* name = FindBytes(game, NAME, sizeof(NAME));
    UINT64 nameAt = (UINT64)name;
    const void* pointer = name ? FindBytes(game, &nameAt, sizeof(nameAt)) : nullptr;
    place.compare = pointer ? FindRipReference(game, "48 8B 15", 7, pointer) : nullptr;
    for (BYTE* p = place.compare; p && p < place.compare + 0x60 && !place.limit; p++) {
        if (memcmp(p, "\x45\x8D\x4C\x24\xFF", 5) == 0) {
            static const BYTE DISPLACEMENT = 0x79;
            place.limit = PlanPatch(p + 4, &DISPLACEMENT, 1);
        }
    }
    return place;
}

void ApplyColorTag(const ColorTagPlace& place) {
    if (!place.compare) {
        ModsLog("color tag: compare not found");
        return;
    }
    if (!place.limit) {
        ModsLog("color tag: text not cut in this client, only 4.6 cuts it");
        ReportStatus(InstallStatus::NotApplicable);
        return;
    }
    if (ApplyPatch(place.limit)) {
        ModsLog("color tag text length fixed at %p", place.limit.at - 4);
    }
}

} // namespace

void InstallColorTagFix(HMODULE game) {
    LocateThenApply(game, LocateColorTag, ApplyColorTag);
}
