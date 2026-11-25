#include "code_patch.h"
#include "module_load.h"
#include "mods_log.h"
#include <detours/detours.h>
#include "install_report.h"

// CObjManager::RenderObject asks the camera whether an object's bounding box is in view (CCamera::IsAABBVisible). After the test
// against the six planes of the view frustum the camera makes an exact one against the outline of the box seen from its position, and
// that one says "not in view" for a box that is flat and thin when the camera stands above it within its outline's reach: such an
// object disappears though the planes say it is in view. Level designers lit some ponds with light cards laid flat under the water and
// scaled to 100-240 m (a pond in Cygnea, also Kaldor, Enshar, Idian Depths); the camera above the pond dropped some of them while their
// neighbours stayed, and the edge of the next one showed on the water as a straight pale slab that moved as the camera turned (on a
// stand: 4 of the 9 cards that pass the planes, all with the exact test saying no). For boxes under two meters thick and at least ten
// meters wide only the planes decide.
//
// The function is the one RenderObject calls that starts by pointing at the frustum planes of the camera: lea rax, [r10+planes].

namespace {

typedef bool(__fastcall* IsAabbVisible_t)(const BYTE* camera, const float* box);
IsAabbVisible_t g_realIsAabbVisible = nullptr;
int g_planes = 0;

bool __fastcall IsAabbVisibleHook(const BYTE* camera, const float* box) {
    float height = box[5] - box[2];
    if (height < 2.0f && (box[3] - box[0] >= 10.0f || box[4] - box[1] >= 10.0f)) {
        const float* plane = (const float*)(camera + g_planes);
        for (int i = 0; i < 6; i++, plane += 4) {
            float distance = -plane[3];
            distance += plane[0] * (plane[0] < 0.0f ? box[3] : box[0]);
            distance += plane[1] * (plane[1] < 0.0f ? box[4] : box[1]);
            distance += plane[2] * (plane[2] < 0.0f ? box[5] : box[2]);
            if (distance > 0.0f) {
                return false;
            }
        }
        return true;
    }
    return g_realIsAabbVisible(camera, box);
}

struct Place {
    BYTE* test = nullptr;
    int planes = 0;
};

Place Locate(HMODULE engine) {
    Place place;
    static const char label[] = "CObjManager::RenderObject";
    BYTE* text = FindBytes(engine, label, sizeof(label));
    std::vector<BYTE*> leas = text ? FindAllLeasTo(engine, text) : std::vector<BYTE*>();
    BYTE* render = leas.size() == 1 ? FunctionStart(leas[0]) : nullptr;
    // the call comes in the first 0x900 bytes of the function in 4.6 to 5.8; FunctionEnd would stop at the first of its chunks
    BYTE* end = render && InModule(engine, render, 0x4000) ? render + 0x4000 : nullptr;
    if (!render || !end) {
        ModsLog("flat box visibility fix: RenderObject %p", render);
        return place;
    }
    for (BYTE* p = render; p + 5 <= end; p++) {
        if (p[0] != 0xE8) {
            continue;
        }
        BYTE* target = p + 5 + *(int*)(p + 1);
        if (!InModule(engine, target, 0x40) || FunctionStart(target) != target) {
            continue;
        }
        // mov r10, rcx ... lea rax, [r10+planes] within the first 0x40 bytes
        bool camera = false;
        for (BYTE* q = target; q < target + 0x40; q++) {
            if (q[0] == 0x4C && q[1] == 0x8B && q[2] == 0xD1) {
                camera = true;
            }
            if (camera && q[0] == 0x49 && q[1] == 0x8D && q[2] == 0x82) {
                int planes = *(int*)(q + 3);
                if (planes <= 0 || planes > 0x400 || (place.test && place.test != target)) {
                    return {};
                }
                place.test = target;
                place.planes = planes;
                break;
            }
        }
    }
    return place;
}

void Apply(const Place& place) {
    if (!place.test) {
        ModsLog("flat box visibility fix: not found");
        return;
    }
    g_planes = place.planes;
    g_realIsAabbVisible = (IsAabbVisible_t)place.test;
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    if (AttachHook((PVOID*)&g_realIsAabbVisible, IsAabbVisibleHook) == NO_ERROR) {
        ModsLog("flat box visibility fix: hooked at %p (planes %X)", place.test, place.planes);
    }
    CommitHooks();
}

void InstallFlatBoxVisibilityFix(HMODULE engine) {
    InstallScope scope("flat_box_visibility_fix");
    LocateThenApply(engine, Locate, Apply);
}

} // namespace

void InstallFlatBoxVisibilityFixOnLoad() {
    OnModuleLoad(L"Cry3DEngine.dll", InstallFlatBoxVisibilityFix, true);
}
