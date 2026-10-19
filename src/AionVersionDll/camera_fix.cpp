#include <windows.h>
#include <windowsx.h>
#include <intrin.h>
#include <psapi.h>
#include <detours/detours.h>

#pragma warning(disable: 28159)

void UpdatePrintScreenHotKey(HWND window, bool foreground);
bool TranslatePrintScreen(MSG* msg);

// The client turns the camera while a mouse button is held by putting the cursor back to the screen centre after every mouse move and turning
// by how far the next WM_MOUSEMOVE lands from it. Since the Windows 10 Fall Creators Update the move messages after SetCursorPos are often
// still relative to the old cursor position, which makes the camera jump, and while Bandicam captures the window SetCursorPos fails
// altogether, which makes it spin. So while the camera turns, the real cursor stays where it was and the client gets a virtual one instead,
// moved by raw mouse input.

static HWND s_window;
static DWORD s_windowThread;
static ULONG_PTR s_gameStart, s_gameSize;
static bool s_rawInput;
static bool s_absoluteInput; // the mouse reports absolute positions (remote desktop, tablets), there are no deltas to turn by
static bool s_turning;
static POINT s_virtual;
static POINT s_warp;
static bool s_warpPending, s_movePending;
static RECT s_previousClip;
static bool s_clipped;
static double s_speed, s_restX, s_restY;
static HRAWINPUT s_lastRawInput;
// After a hitch the client may let go of the mouse capture in the middle of a turn and still turn on without it, by the real cursor. That
// cursor is then put where the client put it back last, the moves from where it was pinned are dropped, and the turn goes on as soon as the
// client puts it back there again.
static bool s_releasedMidTurn;
static POINT s_pinned;

/// Pointer speed multipliers of the mouse settings, by the speed 1..20, so that the camera turns as fast as it did with cursor moves.
static const double POINTER_SPEEDS[] = { 1 / 32.0, 1 / 16.0, 1 / 8.0, 2 / 8.0, 3 / 8.0, 4 / 8.0, 5 / 8.0, 6 / 8.0, 7 / 8.0, 1, 1.25, 1.5, 1.75, 2,
    2.25, 2.5, 2.75, 3, 3.25, 3.5 };

static decltype(SetCursorPos)* real_SetCursorPos = SetCursorPos;
static decltype(GetCursorPos)* real_GetCursorPos = GetCursorPos;
static decltype(PeekMessageA)* real_PeekMessageA = PeekMessageA;
static decltype(ReleaseCapture)* real_ReleaseCapture = ReleaseCapture;
static decltype(GetAsyncKeyState)* real_GetAsyncKeyState = GetAsyncKeyState;

static bool FromGame(const void* address) {
    if (!s_gameStart) {
        HMODULE game = GetModuleHandleW(L"Game.dll");
        MODULEINFO info;
        if (!game || !K32GetModuleInformation(GetCurrentProcess(), game, &info, sizeof(info))) {
            return false;
        }
        s_gameSize = info.SizeOfImage;
        s_gameStart = (ULONG_PTR)game;
    }
    return (ULONG_PTR)address - s_gameStart < s_gameSize;
}

static bool OnWindowThread() {
    return s_window && GetCurrentThreadId() == s_windowThread;
}

/// The mouse button state of the window's thread, as of the last message it took: unlike GetAsyncKeyState, it stays right while Bandicam captures
/// the window. Physical mouse buttons, like GetAsyncKeyState reports them.
static bool ButtonHeld(int vKey) {
    if (GetSystemMetrics(SM_SWAPBUTTON)) {
        vKey = vKey == VK_LBUTTON ? VK_RBUTTON : vKey == VK_RBUTTON ? VK_LBUTTON : vKey;
    }
    return (GetKeyState(vKey) & 0x8000) != 0;
}

static bool AnyButtonHeld() {
    return ButtonHeld(VK_LBUTTON) || ButtonHeld(VK_RBUTTON) || ButtonHeld(VK_MBUTTON);
}

static WPARAM MakeMouseMoveWParam() {
    return ((GetKeyState(VK_LBUTTON) & 0x8000) ? MK_LBUTTON : 0) |
        ((GetKeyState(VK_RBUTTON) & 0x8000) ? MK_RBUTTON : 0) |
        ((GetKeyState(VK_SHIFT) & 0x8000) ? MK_SHIFT : 0) |
        ((GetKeyState(VK_CONTROL) & 0x8000) ? MK_CONTROL : 0) |
        ((GetKeyState(VK_MBUTTON) & 0x8000) ? MK_MBUTTON : 0) |
        ((GetKeyState(VK_XBUTTON1) & 0x8000) ? MK_XBUTTON1 : 0) |
        ((GetKeyState(VK_XBUTTON2) & 0x8000) ? MK_XBUTTON2 : 0);
}

static void StartTurning(int x, int y) {
    POINT cursor;
    real_GetCursorPos(&cursor);
    int speed = 10;
    SystemParametersInfoW(SPI_GETMOUSESPEED, 0, &speed, 0);
    s_speed = POINTER_SPEEDS[max(1, min(20, speed)) - 1];
    s_restX = s_restY = 0;
    s_virtual = { x, y };
    s_movePending = false;
    GetClipCursor(&s_previousClip);
    RECT pin = { cursor.x, cursor.y, cursor.x + 1, cursor.y + 1 };
    s_clipped = ClipCursor(&pin) != FALSE;
    s_pinned = cursor;
    s_releasedMidTurn = false;
    s_turning = true;
}

static void StopTurning() {
    s_turning = false;
    s_warpPending = s_movePending = false;
    if (s_clipped) {
        RECT screen = { GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN) };
        screen.right = screen.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
        screen.bottom = screen.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
        ClipCursor(EqualRect(&screen, &s_previousClip) ? nullptr : &s_previousClip);
        s_clipped = false;
    }
}

static void OnRawInput(HRAWINPUT handle) {
    if (handle == s_lastRawInput) {
        return; // peeked before it was taken
    }
    s_lastRawInput = handle;
    RAWINPUT input;
    UINT size = sizeof(input);
    if (GetRawInputData(handle, RID_INPUT, &input, &size, sizeof(RAWINPUTHEADER)) == (UINT)-1 || input.header.dwType != RIM_TYPEMOUSE) {
        return;
    }
    if (input.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) {
        s_absoluteInput = true;
        if (s_turning) {
            StopTurning();
        }
        return;
    }
    if (!s_turning) {
        return;
    }
    s_restX += input.data.mouse.lLastX * s_speed;
    s_restY += input.data.mouse.lLastY * s_speed;
    LONG dx = (LONG)s_restX, dy = (LONG)s_restY;
    s_restX -= dx;
    s_restY -= dy;
    if (dx || dy) {
        s_virtual.x += dx;
        s_virtual.y += dy;
        s_movePending = true;
    }
}

static MSG MakeMouseMove(POINT at) {
    MSG msg = {};
    msg.hwnd = s_window;
    msg.message = WM_MOUSEMOVE;
    POINT client = at;
    ScreenToClient(s_window, &client);
    msg.lParam = MAKELPARAM(client.x, client.y);
    msg.wParam = MakeMouseMoveWParam();
    msg.time = GetTickCount();
    msg.pt = at;
    return msg;
}

static BOOL WINAPI zzSetCursorPos(_In_ int X, _In_ int Y) {
    if (s_rawInput && !s_absoluteInput && OnWindowThread() && FromGame(_ReturnAddress())) {
        if (!s_turning && AnyButtonHeld() && (GetCapture() == s_window || s_releasedMidTurn && X == s_warp.x && Y == s_warp.y)) {
            StartTurning(X, Y);
        }
        s_releasedMidTurn = false;
        if (s_turning) {
            // the client reads where the cursor went from the next mouse move, as Windows used to send one
            s_virtual = { X, Y };
            s_warp = s_virtual;
            s_warpPending = true;
            s_movePending = false;
            return TRUE;
        }
    }
    return real_SetCursorPos(X, Y);
}

static BOOL WINAPI zzGetCursorPos(_Out_ LPPOINT lpPoint) {
    if (s_turning) {
        *lpPoint = s_virtual;
        return TRUE;
    }
    return real_GetCursorPos(lpPoint);
}

static BOOL WINAPI zzReleaseCapture() {
    if (s_turning && FromGame(_ReturnAddress())) {
        StopTurning();
        if (AnyButtonHeld()) {
            real_SetCursorPos(s_warp.x, s_warp.y);
            s_releasedMidTurn = true;
        }
    }
    return real_ReleaseCapture();
}

// While Bandicam captures the window, the key messages the window gets update neither the asynchronous key state nor the window's own,
// and the mouse button messages only the window's own. The client checks both before it moves, jumps or turns the camera, so the keys are
// tracked here from the messages the window takes.
static bool s_keysDown[256];

static void TrackKey(const MSG* msg) {
    bool down = msg->message == WM_KEYDOWN || msg->message == WM_SYSKEYDOWN;
    if (!down && msg->message != WM_KEYUP && msg->message != WM_SYSKEYUP || msg->wParam >= 256) {
        return;
    }
    int vk = (int)msg->wParam;
    s_keysDown[vk] = down;
    bool extended = (msg->lParam & (1 << 24)) != 0;
    switch (vk) {
    case VK_SHIFT:
        s_keysDown[((msg->lParam >> 16) & 0xFF) == 0x36 ? VK_RSHIFT : VK_LSHIFT] = down;
        break;
    case VK_CONTROL:
        s_keysDown[extended ? VK_RCONTROL : VK_LCONTROL] = down;
        break;
    case VK_MENU:
        s_keysDown[extended ? VK_RMENU : VK_LMENU] = down;
        break;
    }
}

static bool KeyHeld(int vKey) {
    return s_keysDown[vKey] || ButtonHeld(vKey);
}

static bool StandInForKeyState() {
    return OnWindowThread() && GetForegroundWindow() == s_window;
}

static SHORT WINAPI zzGetAsyncKeyState(_In_ int vKey) {
    SHORT state = real_GetAsyncKeyState(vKey);
    if (!(state & 0x8000) && vKey > 0 && vKey < 256 && StandInForKeyState() && KeyHeld(vKey)) {
        state |= (SHORT)0x8000;
    }
    return state;
}

static decltype(GetKeyboardState)* real_GetKeyboardState = GetKeyboardState;
static BOOL WINAPI zzGetKeyboardState(_Out_writes_(256) PBYTE lpKeyState) {
    BOOL result = real_GetKeyboardState(lpKeyState);
    if (result && StandInForKeyState()) {
        for (int vk = 1; vk < 256; vk++) {
            if (s_keysDown[vk]) {
                lpKeyState[vk] |= 0x80;
            }
        }
    }
    return result;
}

static BOOL WINAPI zzPeekMessageA(_Out_ LPMSG lpMsg, _In_opt_ HWND hWnd, _In_ UINT wMsgFilterMin, _In_ UINT wMsgFilterMax, _In_ UINT wRemoveMsg) {
    if (OnWindowThread()) {
        bool foreground = GetForegroundWindow() == s_window;
        if (!foreground) {
            memset(s_keysDown, 0, sizeof(s_keysDown)); // the key ups go elsewhere
        }
        UpdatePrintScreenHotKey(s_window, foreground);
    }
    if (s_turning && OnWindowThread() && (!AnyButtonHeld() || GetForegroundWindow() != s_window)) {
        StopTurning();
    }
    if (s_releasedMidTurn && OnWindowThread() && !AnyButtonHeld()) {
        s_releasedMidTurn = false;
    }
    bool movesWanted = (!hWnd || hWnd == s_window)
        && (wMsgFilterMin == 0 && wMsgFilterMax == 0 || wMsgFilterMin <= WM_MOUSEMOVE && WM_MOUSEMOVE <= wMsgFilterMax);
    for (;;) {
        if (s_turning && movesWanted && (s_warpPending || s_movePending)) {
            *lpMsg = MakeMouseMove(s_warpPending ? s_warp : s_virtual);
            if (wRemoveMsg & PM_REMOVE) {
                if (s_warpPending) {
                    s_warpPending = false;
                } else {
                    s_movePending = false;
                }
            }
            return TRUE;
        }
        BOOL result = real_PeekMessageA(lpMsg, hWnd, wMsgFilterMin, wMsgFilterMax, wRemoveMsg);
        if (!result) {
            return FALSE;
        }
        if (lpMsg->message == WM_INPUT && s_rawInput) {
            OnRawInput((HRAWINPUT)lpMsg->lParam);
        }
        // positions of the real cursor, which stays put while turning
        bool drop = lpMsg->message == WM_MOUSEMOVE && lpMsg->hwnd == s_window
            && (s_turning || s_releasedMidTurn && lpMsg->pt.x == s_pinned.x && lpMsg->pt.y == s_pinned.y) || !TranslatePrintScreen(lpMsg);
        if (drop) {
            if (!(wRemoveMsg & PM_REMOVE)) {
                MSG dropped;
                real_PeekMessageA(&dropped, lpMsg->hwnd, lpMsg->message, lpMsg->message, PM_REMOVE);
            }
            continue;
        }
        if (wRemoveMsg & PM_REMOVE) {
            if (lpMsg->hwnd == s_window) {
                TrackKey(lpMsg);
            }
        }
        return result;
    }
}

static bool IsClientWindow(HWND hWnd) {
    char className[64] = {};
    GetClassNameA(hWnd, className, sizeof(className));
    return strncmp(className, "AIONClientWndClass", 18) == 0;
}

/// Lets the camera turn by raw mouse input instead of the cursor position; called with the window of every SetWindowLongA of the client.
void InstallOrUpdateCameraFix(HWND hWnd) {
    if (real_SetCursorPos == SetCursorPos) {
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourAttach(&(PVOID&)real_SetCursorPos, zzSetCursorPos);
        DetourAttach(&(PVOID&)real_GetCursorPos, zzGetCursorPos);
        DetourAttach(&(PVOID&)real_PeekMessageA, zzPeekMessageA);
        DetourAttach(&(PVOID&)real_ReleaseCapture, zzReleaseCapture);
        DetourAttach(&(PVOID&)real_GetAsyncKeyState, zzGetAsyncKeyState);
        DetourAttach(&(PVOID&)real_GetKeyboardState, zzGetKeyboardState);
        DetourTransactionCommit();
    }
    if (hWnd == s_window || !IsClientWindow(hWnd)) {
        return;
    }
    RAWINPUTDEVICE mouse = { 0x01, 0x02, 0, hWnd }; // generic desktop, mouse
    s_rawInput = RegisterRawInputDevices(&mouse, 1, sizeof(mouse)) != FALSE;
    s_windowThread = GetWindowThreadProcessId(hWnd, nullptr);
    s_window = hWnd;
}
