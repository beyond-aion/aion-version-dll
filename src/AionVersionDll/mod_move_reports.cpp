#include "mods.h"
#include <cmath>
#include <intrin.h>
#include <detours/detours.h>
#include "install_report.h"

/// Memory for values that RIP-relative operands in the client read, which has to lie within 2 GB of the module.
static float* AllocateNear(HMODULE module, size_t size) {
    const ULONG_PTR granularity = 0x10000;
    ULONG_PTR base = (ULONG_PTR)module & ~(granularity - 1);
    for (ULONG_PTR distance = granularity; distance < 0x70000000; distance += granularity) {
        void* memory = VirtualAlloc((void*)(base - distance), size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (memory) {
            return (float*)memory;
        }
    }
    return nullptr;
}

/// The displacement of a RIP-relative operand pointed at value, for the instruction ending 4 bytes after the displacement.
static bool Redirect(const CodePatch& displacement, const float* value) {
    INT64 distance = (BYTE*)value - (displacement.at + 4);
    if (distance != (INT32)distance) {
        return false;
    }
    INT32 disp32 = (INT32)distance;
    return ApplyPatch(displacement, &disp32);
}

namespace {

struct ReportPlaces {
    HMODULE game = nullptr;
    BYTE* window = nullptr;
    BYTE* distance = nullptr;
    BYTE* turn = nullptr;
    CodePatch windowLimit;     // the immediate of cmp edx, 150
    CodePatch distanceOperand; // the displacement of comiss xmm3, [1.0]
    CodePatch turnOperand;     // the displacement of movss xmm0, [0.866]
    bool client5 = false;      // not found in a 5.x client, which the mod is not made for yet
};

/// The client reports the movement of the player's character from its per-frame update. Between reports the server carries
/// the character on along the last reported direction, and the client predicts the same position every frame. A report is
/// due when the character is further than a distance from that prediction, when the movement direction turned further than
/// an angle or the speed changed, when it starts or stops, and 667 ms after the last one. A report due while moving is held
/// back for a while and replaced by later ones, so the server receives at most one of them per window.
ReportPlaces LocateReports(HMODULE game) {
    ReportPlaces places;
    places.game = game;
    // sub edx, [rbx+firstHeldBack]; cmp edx, 150; jbe
    BYTE* window = FindPattern(game, "2B 93 ?? ?? 00 00 81 FA 96 00 00 00 0F 86");
    // squared distance from the predicted position: mulss/addss of the three axes; comiss xmm3, [1.0]; jbe
    BYTE* distance = FindPattern(game, "F3 0F 59 DB F3 0F 59 ED F3 0F 59 E4 F3 0F 58 DD F3 0F 58 DC 0F 2F 1D ?? ?? ?? ?? 76");
    // cosine between the movement direction and the reported one: movss xmm0, [0.866]; addss; addss; comiss xmm0, xmm7; ja
    BYTE* turn = FindPattern(game, "F3 0F 10 05 ?? ?? ?? ?? F3 41 0F 58 FD F3 41 0F 58 FC 0F 2F C7 0F 87");
    places.window = window;
    places.distance = distance;
    places.turn = turn;
    if (window && distance && turn) {
        places.windowLimit = ExpectBytes(window + 8, sizeof(DWORD));
        places.distanceOperand = ExpectBytes(distance + 23, sizeof(INT32));
        places.turnOperand = ExpectBytes(turn + 4, sizeof(INT32));
    } else {
        places.client5 = IsClient5x(game);
    }
    return places;
}

/// Running freely the prediction stays close and nothing changes. Running into a wall or along a pillar it runs away at the
/// running speed: at 8.6 m/s, 1 m and 150 ms give one report per 0.23 s and let the reported path stray up to 1 m from the
/// real one; 0.5 m and 50 ms give one per 0.06 s.
void ApplyReports(const ReportPlaces& places) {
    if (!places.windowLimit && places.client5) {
        ModsLog("move reports: made for 4.x, not for 5.x clients yet");
        ReportStatus(InstallStatus::Unsupported);
        return;
    }
    if (!places.windowLimit) {
        ModsLog("move reports: not found (window %p, distance %p, turn %p)", places.window, places.distance, places.turn);
        return;
    }
    float* values = AllocateNear(places.game, 2 * sizeof(float));
    if (!values) {
        ModsLog("move reports: no memory near Game.dll");
        return;
    }
    values[0] = g_modsConfig.moveReportDistance * g_modsConfig.moveReportDistance;
    values[1] = cosf(g_modsConfig.moveReportTurnAngle * 3.14159265f / 180);
    DWORD windowMs = g_modsConfig.moveReportWindow;
    bool ok = ApplyPatch(places.windowLimit, &windowMs);
    ok &= Redirect(places.distanceOperand, &values[0]);
    ok &= Redirect(places.turnOperand, &values[1]);
    ModsLog("move reports: window at %p, distance at %p, turn at %p: %d ms, %g m, %g degrees%s", places.window, places.distance, places.turn,
        g_modsConfig.moveReportWindow, g_modsConfig.moveReportDistance, g_modsConfig.moveReportTurnAngle, ok ? "" : ", patch failed");
}

} // namespace

void InstallMoveReports(HMODULE game) {
    LocateThenApply(game, LocateReports, ApplyReports);
}

typedef INT64(__fastcall* SnapToServerPos_t)(void* entity, float* reported, float* position, float maxDistance, char keepHeight);
static SnapToServerPos_t real_SnapToServerPos;
/// Where the calls of the move packet handler return to, so the snap distance of other packets stays as it is.
static std::vector<BYTE*> s_moveSnapReturns;
static float s_snapScale = 1;

static INT64 __fastcall zzSnapToServerPos(void* entity, float* reported, float* position, float maxDistance, char keepHeight) {
    BYTE* caller = (BYTE*)_ReturnAddress();
    for (BYTE* ret : s_moveSnapReturns) {
        if (ret == caller) {
            maxDistance *= s_snapScale;
            break;
        }
    }
    return real_SnapToServerPos(entity, reported, position, maxDistance, keepHeight);
}

namespace {

struct SnapPlaces {
    BYTE* snap = nullptr; // Char::SnapToServerPos
    size_t inAirChecks = 0;
    // where the calls of the move packet handler return to
    std::vector<BYTE*> moveReturns;
    // the jumps that keep the height in the air, to become nops
    std::vector<CodePatch> heightJumps;
    // not found in a 5.x client, which the mod is not made for yet
    bool client5 = false;
};

/// The client carries another character towards where its last move packet sends it and only puts it at the position a new packet
/// reports when it got further away than half a second of its speed (3-4 m running), or, unless it is in the air, more than 1 m off in
/// height. So others can be shown meters from where the server has them, and one that fell off a ledge in the client stays down until it
/// moves on (in the air the height is never corrected).
SnapPlaces LocateSnap(HMODULE game) {
    SnapPlaces places;
    // in Char::SnapToServerPos: unless keepHeight, fabs(reported z - z) compared with 1.0
    BYTE* body = FindPattern(game, "80 BC 24 80 00 00 00 00 75 1E F3 0F 5C 7B 08 F3 0F 5A C7 66 0F 54 05 ?? ?? ?? ?? F2 0F 5A C0 0F 2F 05 ?? ?? ?? ?? 77 04");
    // the move packet handler decides keepHeight: mov eax, [rdi+2DCh]; cmp eax, 3; je; cmp eax, 4; je (in the air or falling), at the stop,
    // the move and the move to a point
    std::vector<BYTE*> inAir = FindAllPatterns(game, "8B 87 DC 02 00 00 83 F8 03 74 ?? 83 F8 04 74 ??");
    places.snap = body ? FunctionStart(body) : nullptr;
    places.inAirChecks = inAir.size();
    if (!places.snap || inAir.size() != 3) {
        places.client5 = IsClient5x(game);
        return places;
    }
    BYTE* handler = FunctionStart(inAir[0]);
    BYTE* handlerEnd = FunctionBodyEnd(handler, 0x10000);
    for (BYTE* call : FindAllCallsTo(game, places.snap)) {
        if (call >= handler && call < handlerEnd) {
            places.moveReturns.push_back(call + 5);
        }
    }
    static const BYTE NOPS[2] = { 0x90, 0x90 };
    for (BYTE* check : inAir) {
        places.heightJumps.push_back(PlanPatch(check + 9, NOPS, 2));
        places.heightJumps.push_back(PlanPatch(check + 14, NOPS, 2));
        // the move also keeps the height at the start of a jump: test ebx, ebx; jne (4.8) or test r14d, r14d; jne (4.6)
        if (check[16] == 0x85 && check[17] == 0xDB && check[18] == 0x75) {
            places.heightJumps.push_back(PlanPatch(check + 18, NOPS, 2));
        } else if (check[16] == 0x45 && check[17] == 0x85 && check[18] == 0xF6 && check[19] == 0x75) {
            places.heightJumps.push_back(PlanPatch(check + 19, NOPS, 2));
        }
    }
    return places;
}

/// SnapTime replaces the half second; HeightInAir corrects the height in the air too.
void ApplySnap(const SnapPlaces& places) {
    real_SnapToServerPos = (SnapToServerPos_t)places.snap;
    if ((!places.snap || places.inAirChecks != 3) && places.client5) {
        ModsLog("others snap: made for 4.x, not for 5.x clients yet");
        ReportStatus(InstallStatus::Unsupported);
        return;
    }
    if (!places.snap || places.inAirChecks != 3) {
        ModsLog("others snap: not found (snap %p, %zu in-air checks)", places.snap, places.inAirChecks);
        return;
    }
    s_moveSnapReturns = places.moveReturns;
    bool ok = true;
    if (g_modsConfig.othersHeightInAir) {
        for (const CodePatch& jump : places.heightJumps) {
            ok &= ApplyPatch(jump);
        }
    }
    if (g_modsConfig.othersSnapTime != 0.5f && s_moveSnapReturns.size() == 3) {
        s_snapScale = g_modsConfig.othersSnapTime / 0.5f;
        AttachHook(&(PVOID&)real_SnapToServerPos, zzSnapToServerPos);
    }
    ModsLog("others snap: snap at %p, %zu calls from the move handler, %g s, height in the air %d%s", real_SnapToServerPos,
        s_moveSnapReturns.size(), g_modsConfig.othersSnapTime, g_modsConfig.othersHeightInAir, ok ? "" : ", patch failed");
}

} // namespace

void InstallOthersSnap(HMODULE game) {
    LocateThenApply(game, LocateSnap, ApplySnap);
}
