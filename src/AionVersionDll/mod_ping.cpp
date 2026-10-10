#include "mods.h"
#include "ping.h"
#include <winsock2.h>
#include "detours.h"

// A ping that got no answer within this time counts as lost.
static constexpr ULONGLONG PING_TIMEOUT_MS = 10000;
// length header, opcode, static code, inverted opcode and the one byte of the answer
static constexpr int PING_RESPONSE_LENGTH = 8;

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
// answers to pings that timed out, which may still arrive and must not reach the chat
static volatile int s_lost = 0;
static ULONGLONG s_lastSendTick = 0;
static LARGE_INTEGER s_sentAt;
static LARGE_INTEGER s_frequency;

static volatile LONG s_lastPingMs = -1;

// The client reads the game connection on its own thread, while the answer is handled on the game thread once per frame.
// The time an answer of the ping's length arrived is taken from the reading thread, so that the frame rate does not count.
static volatile bool s_sendingPing = false;
static volatile SOCKET s_gameSocket = INVALID_SOCKET;
static volatile LONGLONG s_answerArrivedAt = 0;
static SOCKET s_trackedSocket = INVALID_SOCKET;
static bool s_streamSynced = false;
static BYTE s_header[2];
static int s_headerBytes = 0;
static int s_packetLength = 0;
static int s_bodyLeft = 0;

static double MillisBetween(LONGLONG start, LONGLONG end) {
    return (end - start) * 1000.0 / s_frequency.QuadPart;
}

static double MillisSince(const LARGE_INTEGER& start) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return MillisBetween(start.QuadPart, now.QuadPart);
}

/// Splits the bytes read from the game connection into packets by their length headers. Starts with a read of a header,
/// which the client reads on its own.
static void TrackReceived(const BYTE* data, int size, int requested) {
    if (!s_streamSynced) {
        if (requested != sizeof(s_header)) {
            return;
        }
        s_streamSynced = true;
        s_headerBytes = 0;
        s_bodyLeft = 0;
    }
    while (size > 0) {
        if (s_bodyLeft == 0) {
            s_header[s_headerBytes++] = *data++;
            size--;
            if (s_headerBytes == sizeof(s_header)) {
                s_headerBytes = 0;
                s_packetLength = *(USHORT*)s_header;
                s_bodyLeft = s_packetLength - (int)sizeof(s_header);
                if (s_bodyLeft <= 0) {
                    s_streamSynced = false;
                    return;
                }
            }
            continue;
        }
        int taken = min(size, s_bodyLeft);
        data += taken;
        size -= taken;
        s_bodyLeft -= taken;
        if (s_bodyLeft == 0 && s_packetLength == PING_RESPONSE_LENGTH) {
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            s_answerArrivedAt = now.QuadPart;
        }
    }
}

static decltype(recv)* real_recv = recv;
static int WSAAPI zzrecv(_In_ SOCKET s, _Out_writes_bytes_to_(len, return) char FAR* buf, _In_ int len, _In_ int flags) {
    int result = real_recv(s, buf, len, flags);
    if (result > 0 && s == s_gameSocket && !(flags & MSG_PEEK)) {
        if (s != s_trackedSocket) {
            s_trackedSocket = s;
            s_streamSynced = false;
        }
        TrackReceived((const BYTE*)buf, result, len);
    }
    return result;
}

static decltype(send)* real_send = send;
static int WSAAPI zzsend(_In_ SOCKET s, _In_reads_bytes_(len) const char FAR* buf, _In_ int len, _In_ int flags) {
    if (s_sendingPing && GetCurrentThreadId() == s_gameThreadId) {
        s_gameSocket = s;
    }
    return real_send(s, buf, len, flags);
}

/// Whether an answer of the ping's length arrived after the last request.
static bool AnswerArrived() {
    return s_answerArrivedAt >= s_sentAt.QuadPart;
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
    if (s_outstanding > 0) {
        if (now - s_lastSendTick < PING_TIMEOUT_MS) {
            return;
        }
        s_lost += s_outstanding;
        s_outstanding = 0;
    }
    if (now - s_lastSendTick < (ULONGLONG)g_modsConfig.pingInterval) {
        return;
    }
    s_lastSendTick = now;
    s_outstanding = 1;
    QueryPerformanceCounter(&s_sentAt);
    s_sendingPing = true;
    if (s_pingOwner && s_pingStateOffset >= 0) {
        UINT64* state = (UINT64*)((BYTE*)s_pingOwner + s_pingStateOffset);
        UINT64 saved = *state;
        sendPingRequest(s_pingOwner, 1);
        *state = saved;
    } else {
        sendPingRequest(nullptr, 1);
    }
    s_sendingPing = false;
}

static void __fastcall zzOnPingResponse(void* router, unsigned char serverType) {
    if (s_outstanding <= 0) {
        if (s_lost > 0) {
            s_lost--;
        } else {
            // an answer to the player's own /ping command
            real_OnPingResponse(router, serverType);
        }
        return;
    }
    s_outstanding = 0;
    LONGLONG arrivedAt = s_answerArrivedAt;
    s_lastPingMs = (LONG)((arrivedAt >= s_sentAt.QuadPart ? MillisBetween(s_sentAt.QuadPart, arrivedAt) : MillisSince(s_sentAt)) + 0.5);}

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
    // while an answer is overdue, show how long it has been missing so that a lag spike is visible right away; in steps of
    // 100 ms, as each new value renders the text again
    if (s_outstanding > 0 && !AnswerArrived()) {
        int waiting = (int)MillisSince(s_sentAt) / 100 * 100;
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
    DetourAttach(&(PVOID&)real_recv, zzrecv);
    DetourAttach(&(PVOID&)real_send, zzsend);
}
