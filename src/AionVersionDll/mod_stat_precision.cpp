#include "mods.h"
#include "module_load.h"
#include <math.h>
#include <detours/detours.h>
#include "install_report.h"

// The character window fills attack speed with a direct call right after looking up this widget.
static const char ATTACK_SPEED_WIDGET[] = "atk_delay";
// In 5.x the tooltip of these stats is a function of its own, printing the difference to the base value with this format.
static const wchar_t SEPARATE_TOOLTIP_DIFFERENCE[] = L"<font color=\"%s\">(%+.1f)</font>";

static constexpr int MAX_DECIMALS = 3;
static constexpr int MAX_FORMATS = 8;
static constexpr int FORMAT_LENGTH = 256;

/// Formats and rounding constants the stat functions read instead of their own, rewritten before every call.
struct PrecisionData {
    float roundOffset;
    float roundScale;
    int formatCount;
    wchar_t formats[MAX_FORMATS][FORMAT_LENGTH];
};

static PrecisionData* s_data = nullptr;

typedef void*(__fastcall* SetStatValue_t)(void* widget, float current, float base, __int64 a4, int invertColors);
typedef void*(__fastcall* SetStatTooltip_t)(void* tooltip, void* widget, float current, float base, int title, int invertColors);
static SetStatValue_t real_SetStatValue = nullptr;
static SetStatTooltip_t real_SetStatTooltip = nullptr;

/// Points a RIP-relative displacement, kept by the search, at target; the instruction ends 4 bytes after it.
static bool Redirect(const CodePatch& displacement, const void* target) {
    INT64 disp = (BYTE*)target - (displacement.at + 4);
    if (disp < INT_MIN || disp > INT_MAX) {
        return false;
    }
    INT32 value = (INT32)disp;
    return ApplyPatch(displacement, &value);
}

/// Fewest decimals (at least one) that show the value without losing digits.
static int DecimalsFor(float value) {
    double scaled = fabs((double)value);
    for (int decimals = 1; decimals < MAX_DECIMALS; decimals++) {
        scaled *= 10;
        if (fabs(scaled - floor(scaled + 0.5)) < 0.01) {
            return decimals;
        }
    }
    return MAX_DECIMALS;
}

static void SetDecimals(float current, float base) {
    int decimals = max(DecimalsFor(current), max(DecimalsFor(base), DecimalsFor(current - base)));
    wchar_t digit = (wchar_t)(L'0' + decimals);
    for (int i = 0; i < s_data->formatCount; i++) {
        // every "%.Nf" and "%+.Nf" placeholder
        for (wchar_t* f = s_data->formats[i]; (f = wcschr(f, L'%')) != nullptr; f++) {
            wchar_t* p = f[1] == L'+' ? f + 2 : f + 1;
            if (p[0] == L'.' && p[1] && p[2] == L'f') {
                p[1] = digit;
            }
        }
    }
    float scale = 1;
    for (int i = 0; i < decimals; i++) {
        scale *= 10;
    }
    s_data->roundScale = scale;
    s_data->roundOffset = 0.5f / scale;
}

static void* __fastcall zzSetStatValue(void* widget, float current, float base, __int64 a4, int invertColors) {
    SetDecimals(current, base);
    return real_SetStatValue(widget, current, base, a4, invertColors);
}

static void* __fastcall zzSetStatTooltip(void* tooltip, void* widget, float current, float base, int title, int invertColors) {
    SetDecimals(current, base);
    return real_SetStatTooltip(tooltip, widget, current, base, title, invertColors);
}

namespace {

/// An operand of a stat function to point at the copies the hooks change.
struct Operand {
    enum Kind { Format, Scale, Offset } kind;
    CodePatch displacement;
    const wchar_t* text; // the format, for Format
};

struct Place {
    HMODULE game = nullptr;
    BYTE* value = nullptr;   // the function that fills a stat in the character window
    BYTE* tooltip = nullptr; // the 5.x tooltip function, or nullptr
    std::vector<Operand> valueOperands;
    std::vector<Operand> tooltipOperands;
    std::vector<CodePatch> increments; // the inc eax of the attack delay, to become a nop
};

/// Every string with a one-decimal float placeholder, and every use of the rounding constants 10 and 0.05, in the function.
std::vector<Operand> FindOperands(HMODULE game, BYTE* function, int& formats) {
    std::vector<Operand> operands;
    BYTE* end = FunctionBodyEnd(function, 0x1000);
    for (BYTE* p = function; p < end - 8; p++) {
        // lea reg, [rip+string]
        if ((p[0] == 0x48 || p[0] == 0x4C) && p[1] == 0x8D && (p[2] & 0xC7) == 0x05) {
            auto text = (const wchar_t*)ResolveRip(p + 3, p + 7);
            if (!InModule(game, text, FORMAT_LENGTH * sizeof(wchar_t)) || formats >= MAX_FORMATS) {
                continue;
            }
            size_t length = wcsnlen(text, FORMAT_LENGTH);
            if (length < FORMAT_LENGTH && (wcsstr(text, L"%.1f") || wcsstr(text, L"%+.1f"))) {
                operands.push_back({ Operand::Format, ExpectBytes(p + 3, sizeof(INT32)), text });
                formats++;
            }
            continue;
        }
        // movss/addss/mulss xmm, [rip+constant], with or without REX
        BYTE* op = p[1] >= 0x40 && p[1] <= 0x4F ? p + 2 : p + 1;
        if (p[0] == 0xF3 && op[0] == 0x0F && (op[1] == 0x10 || op[1] == 0x58 || op[1] == 0x59) && (op[2] & 0xC7) == 0x05) {
            auto value = (const float*)ResolveRip(op + 3, op + 7);
            if (!InModule(game, value, sizeof(float))) {
                continue;
            }
            if (*value == 10.0f) {
                operands.push_back({ Operand::Scale, ExpectBytes(op + 3, sizeof(INT32)), nullptr });
            } else if (*value == 0.05f) {
                operands.push_back({ Operand::Offset, ExpectBytes(op + 3, sizeof(INT32)), nullptr });
            }
        }
    }
    return operands;
}

/// 5.x shows attack speed as (delay + 1) / 1000, which one decimal rounds away but more show as 2.801 for a delay of 2800 ms. The
/// increment to drop: movzx eax, word [stat]; ...; inc eax; ...; movd xmm, eax.
std::vector<CodePatch> FindAttackDelayIncrements(HMODULE game, const void* widgetName) {
    static const BYTE NOP2[] = { 0x66, 0x90 };
    std::vector<CodePatch> increments;
    BYTE* end = (BYTE*)game + ModuleImageSize(game) - 0x40;
    for (BYTE* c : FindAllLeasTo(game, widgetName)) {
        if (c < (BYTE*)game + 0x20 || c > end) {
            continue;
        }
        for (BYTE* p = c - 0x20; p < c + 0x40; p++) {
            if (p[0] != 0xFF || p[1] != 0xC0) {
                continue;
            }
            bool loaded = false, moved = false;
            for (BYTE* q = p - 0x20; q < p; q++) {
                loaded |= q[0] == 0x0F && q[1] == 0xB7 && (q[2] & 0x38) == 0;
            }
            for (BYTE* q = p + 2; q < p + 18; q++) {
                moved |= q[0] == 0x66 && q[1] == 0x0F && q[2] == 0x6E && (q[3] & 0xC7) == 0xC0;
            }
            if (loaded && moved) {
                increments.push_back(PlanPatch(p, NOP2, sizeof(NOP2)));
            }
        }
    }
    return increments;
}

Place Locate(HMODULE game) {
    Place place;
    place.game = game;
    // the widget's name is loaded in several places; the first is its lookup, the rest FindAttackDelayIncrements walks
    const void* widgetName = FindBytes(game, ATTACK_SPEED_WIDGET, sizeof(ATTACK_SPEED_WIDGET));
    BYTE* lookup = widgetName ? FindLeaTo(game, widgetName) : nullptr;
    for (BYTE* p = lookup; p && p < lookup + 0x80; p++) {
        if (p[0] == 0xE8) {
            place.value = ResolveRip(p + 1, p + 5);
            break;
        }
    }
    place.tooltip = FindFunctionByString(game, SEPARATE_TOOLTIP_DIFFERENCE);
    if (!place.value || !InModule(game, place.value, 16)) {
        return place;
    }
    int formats = 0;
    place.valueOperands = FindOperands(game, place.value, formats);
    if (place.tooltip) {
        place.tooltipOperands = FindOperands(game, place.tooltip, formats);
    }
    if (!place.valueOperands.empty()) {
        place.increments = FindAttackDelayIncrements(game, widgetName);
    }
    return place;
}

/// Points the operands at the copies in s_data. Returns the number of places changed.
int RedirectOperands(const std::vector<Operand>& operands) {
    int changed = 0;
    for (const Operand& operand : operands) {
        switch (operand.kind) {
            case Operand::Format: {
                wchar_t* copy = s_data->formats[s_data->formatCount++];
                wcscpy_s(copy, FORMAT_LENGTH, operand.text);
                changed += Redirect(operand.displacement, copy);
                break;
            }
            case Operand::Scale: changed += Redirect(operand.displacement, &s_data->roundScale); break;
            case Operand::Offset: changed += Redirect(operand.displacement, &s_data->roundOffset); break;
        }
    }
    return changed;
}

/// Shows attack, casting and movement speed in the character window with as many decimals as the value has, up to three.
void Apply(const Place& place) {
    if (!place.value || !InModule(place.game, place.value, 16)) {
        ModsLog("stat precision: value function not found");
        return;
    }
    real_SetStatValue = (SetStatValue_t)place.value;
    real_SetStatTooltip = (SetStatTooltip_t)place.tooltip;
    DWORD size = 0;
    s_data = (PrecisionData*)DetourAllocateRegionWithinJumpBounds(real_SetStatValue, &size);
    if (!s_data || size < sizeof(PrecisionData)) {
        ModsLog("stat precision: no memory near the game module");
        return;
    }
    s_data->roundScale = 10;
    s_data->roundOffset = 0.05f;
    int valueChanges = RedirectOperands(place.valueOperands);
    int tooltipChanges = RedirectOperands(place.tooltipOperands);
    int incrementChanges = 0;
    if (valueChanges) {
        for (const CodePatch& increment : place.increments) {
            incrementChanges += ApplyPatch(increment);
        }
    }
    ModsLog("stat precision: value=%p (%d changes) tooltip=%p (%d changes) attack delay increments=%d", real_SetStatValue, valueChanges,
        real_SetStatTooltip, tooltipChanges, incrementChanges);
    if (valueChanges) {
        AttachHook(&(PVOID&)real_SetStatValue, zzSetStatValue);
    }
    if (tooltipChanges) {
        AttachHook(&(PVOID&)real_SetStatTooltip, zzSetStatTooltip);
    }
}

} // namespace

void InstallStatPrecision(HMODULE game) {
    LocateThenApply(game, Locate, Apply);
}
