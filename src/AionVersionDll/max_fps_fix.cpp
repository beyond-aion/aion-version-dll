#include "code_patch.h"
#include "mods_log.h"
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <sstream>
#include <string_view>
#include <string.h>

// The client registers g_maxfps with the default "60" (4.x, "120" in 5.x), so that without the line in system.cfg it sleeps away every
// frame above that. The default becomes "0", no limit, while a value in system.cfg is still loaded and saved normally.
// The string is the default of other cvars as well, so the registration of g_maxfps is pointed at a "0" of its own module instead of
// changing the string.
namespace {

HMODULE s_dxvkD3d9 = nullptr;

struct DxvkRates {
    std::optional<int> d3d9;
    std::optional<int> global;

    int Effective(bool supportsGlobal) const {
        int fallback = d3d9.value_or(0);
        return supportsGlobal ? global.value_or(fallback) : fallback;
    }
};

bool HasFrameLimit(int rate) {
    return rate != 0 && rate != -1;
}

std::optional<int> ParseDxvkRate(std::string_view value) {
    value.remove_prefix(min(value.find_first_not_of(" \t\r"), value.size()));
    std::string number;
    bool quoted = false;
    for (char ch : value) {
        if (ch == '"') {
            quoted = !quoted;
        } else if (!quoted && (ch == ' ' || ch == '\t' || ch == '\r')) {
            break;
        } else {
            number += ch;
        }
    }
    if (number == "-") {
        return 0;
    }
    int rate;
    const char* end = number.data() + number.size();
    auto parsed = std::from_chars(number.data(), end, rate);
    if (parsed.ec != std::errc() || parsed.ptr != end) {
        return std::nullopt;
    }
    return rate;
}

void ReadDxvkRates(std::istream& stream, char delimiter, std::string_view exe, DxvkRates& rates) {
    bool active = true;
    for (std::string line; std::getline(stream, line, delimiter);) {
        std::string_view entry(line);
        entry.remove_prefix(min(entry.find_first_not_of(" \t\r"), entry.size()));
        if (entry.empty()) {
            continue;
        }
        if (entry.front() == '[') {
            size_t end = entry.rfind(']');
            active = end != std::string_view::npos && entry.substr(1, end - 1) == exe;
            continue;
        }
        size_t equals = entry.find('=');
        if (!active || equals == std::string_view::npos) {
            continue;
        }
        std::string_view key = entry.substr(0, equals);
        key = key.substr(0, key.find_last_not_of(" \t\r") + 1);
        if (key == "d3d9.maxFrameRate") {
            rates.d3d9 = ParseDxvkRate(entry.substr(equals + 1));
        } else if (key == "dxvk.maxFrameRate") {
            rates.global = ParseDxvkRate(entry.substr(equals + 1));
        }
    }
}

int DetectDxvkFrameLimit() {
    if (!s_dxvkD3d9 || GetModuleHandleW(L"d3d9.dll") != s_dxvkD3d9) {
        return 0;
    }
    static const char D3D9_RATE[] = "d3d9.maxFrameRate";
    static const char GLOBAL_RATE[] = "dxvk.maxFrameRate";
    static const char LEGACY_RATE_ENV[] = "DXVK_FRAME_RATE";
    if (!FindBytes(s_dxvkD3d9, D3D9_RATE, sizeof(D3D9_RATE))) {
        return 0;
    }
    bool supportsGlobal = FindBytes(s_dxvkD3d9, GLOBAL_RATE, sizeof(GLOBAL_RATE)) != nullptr;

    char exePath[MAX_PATH];
    DWORD length = GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    std::string_view exe = length && length < MAX_PATH ? std::string_view(exePath, length) : std::string_view();
    exe.remove_prefix(exe.find_last_of("\\/") + 1);

    wchar_t* path = nullptr;
    _wdupenv_s(&path, nullptr, L"DXVK_CONFIG_FILE");
    std::ifstream config(path && *path ? path : L"dxvk.conf");
    free(path);
    DxvkRates rates;
    ReadDxvkRates(config, '\n', exe, rates);
    if (!HasFrameLimit(rates.Effective(supportsGlobal))) {
        return 0;
    }

    // An older DXVK can let this environment variable control its limiter independently of the file.
    if (GetEnvironmentVariableA(LEGACY_RATE_ENV, nullptr, 0)
        && FindBytes(s_dxvkD3d9, LEGACY_RATE_ENV, sizeof(LEGACY_RATE_ENV))) {
        return 0;
    }
    char* overrides = nullptr;
    _dupenv_s(&overrides, nullptr, "DXVK_CONFIG");
    if (overrides) {
        std::istringstream stream(overrides);
        free(overrides);
        ReadDxvkRates(stream, ';', exe, rates);
    }
    return rates.Effective(supportsGlobal);
}

/// A frame rate of two or three digits, as the default string of the cvar.
bool IsLimit(const char* value) {
    size_t length = strspn(value, "0123456789");
    return value[length] == 0 && (length == 2 || length == 3) && value[0] != '0';
}

/// lea rdx, "g_maxfps" names the cvar; the lea r8 of its default string comes a few instructions before it.
CodePatch Locate(HMODULE game) {
    static const char NAME[] = "g_maxfps";
    static const char ZERO[] = "\0" "0";
    BYTE* lea = FindStringLea(game, NAME);
    const BYTE* zero = FindBytes(game, ZERO, sizeof(ZERO));
    if (!lea || !zero) {
        ModsLog("max fps fix: the registration of g_maxfps %p, a string \"0\" %p", lea, zero);
        return {};
    }
    zero++;
    for (BYTE* p = lea - 7; p >= lea - 0x30; p--) {
        if (p[0] != 0x4C || p[1] != 0x8D || p[2] != 0x05) {
            continue;
        }
        const char* value = (const char*)ResolveRip(p + 3, p + 7);
        if (!InModule(game, value, 4) || !IsLimit(value)) {
            continue;
        }
        LONGLONG disp = zero - (p + 7);
        if (disp != (LONG)disp) {
            return {};
        }
        LONG newDisp = (LONG)disp;
        return PlanPatch(p + 3, &newDisp, sizeof(newDisp));
    }
    ModsLog("max fps fix: no default frame rate before the registration of g_maxfps at %p", lea);
    return {};
}

void Apply(const CodePatch& patch) {
    ApplyPatch(patch);
}

/// Use the member saved by g_maxfps registration to find its float getter in the frame loop, separately from the cvar itself.
CodePatch LocateFrameLimiter(HMODULE game) {
    BYTE* registration = FindStringLea(game, "g_maxfps");
    if (!registration || !InModule(game, registration, 37)) {
        return {};
    }
    // The registration call is followed by the next cvar's description and default, then mov [rdi+disp32], rax.
    if (memcmp(registration + 7, "\x45\x33\xC9\x48\x8B\xCB\xFF\x50\x18\x48\x8D\x0D", 12) != 0
        || memcmp(registration + 23, "\x4C\x8D\x05", 3) != 0
        || memcmp(registration + 30, "\x48\x89\x87", 3) != 0) {
        ReportStatus(InstallStatus::NotFound);
        return {};
    }
    const BYTE* field = registration + 33;
    char pattern[48];
    sprintf_s(pattern, "%02X %02X %02X %02X 48 8B ?? FF ?? 10", field[0], field[1], field[2], field[3]);

    BYTE* getter = nullptr;
    size_t count = 0;
    for (BYTE* at : FindAllPatterns(game, pattern)) {
        if (!InModule(game, at - 4, 14)) {
            continue;
        }
        // mov rcx, [this+disp32], with the R12 form using a SIB byte.
        bool fieldLoad = ((at[-3] == 0x48 || at[-3] == 0x49) && at[-2] == 0x8B && (at[-1] & 0xF8) == 0x88 && at[-1] != 0x8C)
            || (at[-4] == 0x49 && at[-3] == 0x8B && at[-2] == 0x8C && at[-1] == 0x24);
        // The vtable load and the virtual call must use the same register.
        bool floatGetter = (at[6] == 0x01 && at[8] == 0x50) || (at[6] == 0x11 && at[8] == 0x52);
        if (fieldLoad && floatGetter) {
            getter = at + 7;
            count++;
        }
    }
    ReportSearch(count);
    // The frame loop should take its normal no-limit path without discarding the configured cvar.
    static const BYTE ZERO_FLOAT[] = { 0x0F, 0x57, 0xC0 }; // xorps xmm0, xmm0
    return count == 1 ? PlanPatch(getter, ZERO_FLOAT, sizeof(ZERO_FLOAT)) : CodePatch{};
}

} // namespace

void InstallMaxFpsFix(HMODULE game) {
    LocateThenApply(game, Locate, Apply);
}

void SetDxvkFrameLimitModule(HMODULE d3d9) {
    s_dxvkD3d9 = d3d9;
}

void InstallDxvkFrameLimit(HMODULE game) {
    int rate = g_dryRun ? 0 : DetectDxvkFrameLimit();
    if (!g_dryRun && !HasFrameLimit(rate)) {
        ReportStatus(InstallStatus::NotApplicable);
        return;
    }
    CodePatch patch;
    {
        LocatePhase phase;
        patch = LocateFrameLimiter(game);
    }
    if (ApplyPatch(patch) && !g_dryRun) {
        ModsLog("dxvk: frame rate limit %d overrides g_maxfps", rate);
    }
}
