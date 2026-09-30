#include "mods.h"
#include "ping.h"
#include "detours.h"

// A ping that got no answer within this time no longer holds back the next one.
static constexpr ULONGLONG PING_TIMEOUT_MS = 10000;
static constexpr int MAX_OUTSTANDING = 3;

typedef void(__fastcall* OnPingResponse_t)(void* router, unsigned char serverType);
typedef void(__fastcall* SendPingRequest_t)(void* owner, int force);
typedef char(__fastcall* DispatchPacket_t)(void* router, void* packet, void* a3, void* a4);

static OnPingResponse_t real_OnPingResponse = nullptr;
static SendPingRequest_t sendPingRequest = nullptr;
static DispatchPacket_t real_DispatchPacket = nullptr;

// 5.x builds the request on an object that also counts the player's /ping requests, which the mod's requests must not change
static void* s_pingOwner = nullptr;
static int s_pingStateOffset = -1;

static volatile DWORD s_gameThreadId = 0;
static volatile int s_outstanding = 0;
static ULONGLONG s_lastSendTick = 0;
static LARGE_INTEGER s_sentAt;
static LARGE_INTEGER s_frequency;

static volatile LONG s_lastPingMs = -1;

static double MillisSince(const LARGE_INTEGER& start) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (now.QuadPart - start.QuadPart) * 1000.0 / s_frequency.QuadPart;
}

/// Sends the next ping request once the interval has passed. Only runs on the thread that handles incoming packets,
/// because sending shares the connection's cipher state with that thread.
static void PingTick() {
    if (!sendPingRequest || GetCurrentThreadId() != s_gameThreadId) {
        return;
    }
    if (!IsInWorld()) {
        s_lastPingMs = -1;
        return;
    }
    ULONGLONG now = GetTickCount64();
    if (s_outstanding > 0 && (now - s_lastSendTick < PING_TIMEOUT_MS || s_outstanding >= MAX_OUTSTANDING)) {
        return;
    }
    if (now - s_lastSendTick < (ULONGLONG)g_modsConfig.pingInterval) {
        return;
    }
    s_lastSendTick = now;
    s_outstanding++;
    QueryPerformanceCounter(&s_sentAt);
    if (s_pingOwner && s_pingStateOffset >= 0) {
        UINT64* state = (UINT64*)((BYTE*)s_pingOwner + s_pingStateOffset);
        UINT64 saved = *state;
        sendPingRequest(s_pingOwner, 1);
        *state = saved;
    } else {
        sendPingRequest(nullptr, 1);
    }
}

static void __fastcall zzOnPingResponse(void* router, unsigned char serverType) {
    if (s_outstanding <= 0) {
        // an answer to the player's own /ping command
        real_OnPingResponse(router, serverType);
        return;
    }
    // answers come in order, so only the last one belongs to the latest send time
    if (--s_outstanding == 0) {
        s_lastPingMs = (LONG)(MillisSince(s_sentAt) + 0.5);
    }
}

static char __fastcall zzDispatchPacket(void* router, void* packet, void* a3, void* a4) {
    s_gameThreadId = GetCurrentThreadId();
    char result = real_DispatchPacket(router, packet, a3, a4);
    PingTick();
    return result;
}

static decltype(PeekMessageW)* real_PeekMessageW = PeekMessageW;
static BOOL WINAPI zzPeekMessageW(_Out_ LPMSG lpMsg, _In_opt_ HWND hWnd, _In_ UINT wMsgFilterMin, _In_ UINT wMsgFilterMax, _In_ UINT wRemoveMsg) {
    PingTick();
    return real_PeekMessageW(lpMsg, hWnd, wMsgFilterMin, wMsgFilterMax, wRemoveMsg);
}

static decltype(PeekMessageA)* real_PeekMessageA = PeekMessageA;
static BOOL WINAPI zzPeekMessageA(_Out_ LPMSG lpMsg, _In_opt_ HWND hWnd, _In_ UINT wMsgFilterMin, _In_ UINT wMsgFilterMax, _In_ UINT wRemoveMsg) {
    PingTick();
    return real_PeekMessageA(lpMsg, hWnd, wMsgFilterMin, wMsgFilterMax, wRemoveMsg);
}

int GetDisplayedPing() {
    if (!IsInWorld() || s_lastPingMs < 0) {
        return -1;
    }
    // while an answer is overdue, show how long it has been missing so that a lag spike is visible right away
    if (s_outstanding > 0) {
        int waiting = (int)MillisSince(s_sentAt);
        if (waiting > s_lastPingMs) {
            return waiting;
        }
    }
    return s_lastPingMs;
}

// push rbx; sub rsp, imm32
static const BYTE BUILDER_START[] = { 0x40, 0x53, 0x48, 0x81, 0xEC };
static const wchar_t PING_RESULT[] = L"ping result for %s(%d) : %d milisecond";

/// The request builder is the call in the response handler whose target has the shape of a builder for a packet without a body:
/// push rbx; sub rsp, imm32; ... the packet size 7 stored directly (mov word ptr [rsp+x], 7) or through a register (mov r32, 7)
static SendPingRequest_t FindRequestBuilder(HMODULE game, BYTE* handler) {
    for (BYTE* p = handler; p < handler + 0x400; p++) {
        if (p[0] != 0xE8) {
            continue;
        }
        BYTE* target = ResolveRip(p + 1, p + 5);
        if (!InModule(game, target, 0x80) || memcmp(target, BUILDER_START, sizeof(BUILDER_START)) != 0) {
            continue;
        }
        for (BYTE* q = target; q < target + 0x80; q++) {
            bool storesSize = q[0] == 0x66 && q[1] == 0xC7 && q[2] == 0x44 && q[3] == 0x24 && q[5] == 7 && q[6] == 0;
            bool loadsSize = q[0] >= 0xB8 && q[0] <= 0xBF && q[1] == 7 && q[2] == 0 && q[3] == 0 && q[4] == 0;
            if (storesSize || loadsSize) {
                // 5.x passes the object that holds the request state: lea rcx, [owner]
                if (p[-7] == 0x48 && p[-6] == 0x8D && p[-5] == 0x0D) {
                    s_pingOwner = ResolveRip(p - 4, p);
                    // cmp [rcx+count], edx; the send time sits right before the count
                    for (BYTE* r = target; r < target + 0x20; r++) {
                        if (r[0] == 0x39 && r[1] == 0x91) {
                            s_pingStateOffset = *(INT32*)(r + 2) - 4;
                            break;
                        }
                    }
                }
                return (SendPingRequest_t)target;
            }
        }
    }
    return nullptr;
}

void InstallPing(HMODULE game) {
    QueryPerformanceFrequency(&s_frequency);
    const void* resultText = FindBytes(game, PING_RESULT, sizeof(PING_RESULT));
    BYTE* resultLea = resultText ? FindLeaTo(game, resultText) : nullptr;
    real_OnPingResponse = resultLea ? (OnPingResponse_t)FunctionStart(resultLea) : nullptr;
    sendPingRequest = real_OnPingResponse ? FindRequestBuilder(game, (BYTE*)real_OnPingResponse) : nullptr;
    BYTE* dispatchCall = real_OnPingResponse ? FindCallTo(game, real_OnPingResponse) : nullptr;
    real_DispatchPacket = dispatchCall ? (DispatchPacket_t)FunctionStart(dispatchCall) : nullptr;
    ModsLog("ping: response=%p request=%p owner=%p+%d dispatch=%p", real_OnPingResponse, sendPingRequest, s_pingOwner, s_pingStateOffset,
        real_DispatchPacket);
    if (!real_OnPingResponse || !sendPingRequest || !real_DispatchPacket || !g_gameState) {
        sendPingRequest = nullptr;
        return;
    }
    DetourAttach(&(PVOID&)real_OnPingResponse, zzOnPingResponse);
    DetourAttach(&(PVOID&)real_DispatchPacket, zzDispatchPacket);
    DetourAttach(&(PVOID&)real_PeekMessageW, zzPeekMessageW);
    DetourAttach(&(PVOID&)real_PeekMessageA, zzPeekMessageA);
}
