#include <windows.h>
#include <windowsx.h>
#include <intrin.h>
#include <psapi.h>
#include <cmath>
#include <detours/detours.h>
#include "install_report.h"
#include "module_load.h"

#pragma warning(disable: 28159)

void UpdatePrintScreenHotKey(HWND window, bool foreground);
bool TranslatePrintScreen(MSG* msg, bool removed);

// The client turns the camera while a mouse button is held by putting the cursor back to the screen centre after every mouse move and turning
// by how far the next WM_MOUSEMOVE lands from it. Since the Windows 10 Fall Creators Update the move messages after SetCursorPos are often
// still relative to the old cursor position, which makes the camera jump, and while Bandicam captures the window SetCursorPos fails
// altogether, which makes it spin. So while the camera turns, the real cursor stays where it was and the client gets a virtual one instead,
// moved by raw mouse input. Pointing devices that report absolute positions (pen tablets, remote desktop, virtual machines) are never put
// back to the centre either; the virtual cursor moves by how far their position went from the last report.

static HWND s_window;
static DWORD s_windowThread;
static ULONG_PTR s_gameStart, s_gameSize;
static bool s_rawInput;
// the last position a device with absolute positions reported, while no relative input came after it
static bool s_haveAbsolute;
static POINT s_absolute;
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
// A click with a hand that trembles a little stays a click: after a mouse button goes down, the client sees no move until the cursor has gone
// SM_CXDRAG or SM_CYDRAG pixels from where it was pressed, as Windows does before a drag, and the button goes up where it went down.
static bool s_deadZone;
static POINT s_pressedAt;
// Alt+Tab in the middle of a turn: the button is still held, so the client turns on by the real cursor, puts it to the centre of the screen
// in the other program and turns by every move there. Until its window is in front again, the client keeps the cursor where it put it last
// and sees no move, and the cursor of the other program stays where it is.
static bool s_turnLeft;
// The game and renderer can both restore the old click during activation, before the message loop sees the foreground again.
static bool s_skipInterruptedRestore;

/// Pointer speed multipliers of the mouse settings, by the speed 1..20, so that the camera turns as fast as it did with cursor moves.
static const double POINTER_SPEEDS[] = { 1 / 32.0, 1 / 16.0, 1 / 8.0, 2 / 8.0, 3 / 8.0, 4 / 8.0, 5 / 8.0, 6 / 8.0, 7 / 8.0, 1, 1.25, 1.5, 1.75, 2,
    2.25, 2.5, 2.75, 3, 3.25, 3.5 };

// Enhance pointer precision: Windows moves the pointer farther per mouse count the faster the mouse goes. Raw input carries the counts before
// that, so the curve is applied here the way win32kbase of Windows 10 and 11 applies it (CDeviceAcceleration::Accelerate): the speed of a
// report is the larger of |dx| and |dy| plus half the smaller one, in counts; the curve of HKCU\Control Panel\Mouse goes through the points
// (SmoothMouseXCurve x 3.5, SmoothMouseYCurve x DPI / 120 x pointer speed / 10) and goes on along its last segment; a report moves the pointer
// by the curve's value at its speed, shared between x and y as the counts are; when the speed climbs onto a higher segment than the last
// report's, the gain is the mean of both segments'. The pointer speed is linear here, the table above is only for the pointer without it. The
// client without this DLL was not DPI aware and saw the pointer in pixels of 96 DPI whatever the scale of the screen, so the curve is built
// for 96 DPI. Wine keeps no curve in the registry and does not accelerate this way, so without one the camera turns by the counts alone.
static bool s_ballistic;
static double s_ballisticX[5]; // speeds in counts per report
static double s_ballisticSlope[4], s_ballisticIntercept[4];
static int s_ballisticSegment;
static bool s_ballisticLogged;

/// The five 16.16 fixed point values of one SmoothMouse curve, each first in its 8-byte point.
static bool ReadSmoothCurve(const wchar_t* name, double* points) {
    BYTE data[40] = {};
    DWORD size = sizeof(data);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Control Panel\\Mouse", name, RRF_RT_REG_BINARY, nullptr, data, &size) != ERROR_SUCCESS
        || size < sizeof(data)) {
        return false;
    }
    for (int i = 0; i < 5; i++) {
        points[i] = *(const DWORD*)(data + i * 8) / 65536.0;
    }
    return true;
}

/// Builds the curve from the mouse settings as they are now, at the start of every turn; MouseSpeed 0 is the setting turned off.
static void ReadBallistic(int pointerSpeed) {
    int accel[3] = {};
    SystemParametersInfoW(SPI_GETMOUSE, 0, accel, 0);
    double x[5], y[5];
    s_ballistic = accel[2] && ReadSmoothCurve(L"SmoothMouseXCurve", x)
        && ReadSmoothCurve(L"SmoothMouseYCurve", y);
    if (!s_ballistic) {
        if (!s_ballisticLogged) {
            s_ballisticLogged = true;
            MODULE_LOG("camera fix: pointer acceleration off");
        }
        return;
    }
    const double DPI = 96;
    double scale = DPI / 120 * pointerSpeed / 10;
    for (int i = 0; i < 5; i++) {
        s_ballisticX[i] = x[i] * 3.5;
        y[i] *= scale;
    }
    for (int i = 0; i < 4; i++) {
        double width = s_ballisticX[i + 1] - s_ballisticX[i];
        // a segment without width moves nothing, as in Windows
        s_ballisticSlope[i] = width != 0 ? (y[i + 1] - y[i]) / width : 0;
        s_ballisticIntercept[i] = width != 0 ? y[i] - s_ballisticX[i] * s_ballisticSlope[i] : 0;
    }
    s_ballisticSegment = 0;
    if (!s_ballisticLogged) {
        s_ballisticLogged = true;
        MODULE_LOG("camera fix: pointer acceleration on, speed %d, curve %.2f/%.2f %.2f/%.2f %.2f/%.2f %.2f/%.2f", pointerSpeed, x[1], y[1] / scale,
            x[2], y[2] / scale, x[3], y[3] / scale, x[4], y[4] / scale);
    }
}

/// How far the pointer moves per count of a report with these counts.
static double BallisticGain(LONG dx, LONG dy) {
    double ax = fabs((double)dx), ay = fabs((double)dy);
    double speed = max(ax, ay) + min(ax, ay) / 2;
    if (speed <= 0) {
        return 0;
    }
    int above = 0;
    while (above < 4 && speed > s_ballisticX[above]) {
        above++;
    }
    int segment = max(above - 1, 0);
    double gain = s_ballisticSlope[segment] + s_ballisticIntercept[segment] / speed;
    if (s_ballisticSegment < segment) {
        gain = (gain + s_ballisticSlope[s_ballisticSegment] + s_ballisticIntercept[s_ballisticSegment] / speed) / 2;
    }
    s_ballisticSegment = segment;
    return gain;
}

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
    speed = max(1, min(20, speed));
    s_speed = POINTER_SPEEDS[speed - 1];
    ReadBallistic(speed);
    s_restX = s_restY = 0;
    s_virtual = { x, y };
    s_movePending = false;
    GetClipCursor(&s_previousClip);
    RECT pin = { cursor.x, cursor.y, cursor.x + 1, cursor.y + 1 };
    s_clipped = ClipCursor(&pin) != FALSE;
    s_pinned = cursor;
    s_releasedMidTurn = false;
    s_deadZone = false;
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

/// Where an absolute position report puts the pointer, in pixels of the virtual desktop or of the primary monitor.
static POINT AbsolutePosition(const RAWMOUSE& mouse) {
    bool virtualDesktop = (mouse.usFlags & MOUSE_VIRTUAL_DESKTOP) != 0;
    int left = virtualDesktop ? GetSystemMetrics(SM_XVIRTUALSCREEN) : 0;
    int top = virtualDesktop ? GetSystemMetrics(SM_YVIRTUALSCREEN) : 0;
    int width = GetSystemMetrics(virtualDesktop ? SM_CXVIRTUALSCREEN : SM_CXSCREEN);
    int height = GetSystemMetrics(virtualDesktop ? SM_CYVIRTUALSCREEN : SM_CYSCREEN);
    return { left + MulDiv(mouse.lLastX, width, 65536), top + MulDiv(mouse.lLastY, height, 65536) };
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
    LONG dx, dy;
    if (input.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) {
        // the position has been through the pointer speed already
        POINT at = AbsolutePosition(input.data.mouse);
        bool known = s_haveAbsolute;
        dx = at.x - s_absolute.x;
        dy = at.y - s_absolute.y;
        s_absolute = at;
        s_haveAbsolute = true;
        if (!s_turning || !known) {
            return;
        }
    } else {
        s_haveAbsolute = false;
        if (!s_turning) {
            return;
        }
        LONG rawX = input.data.mouse.lLastX;
        LONG rawY = input.data.mouse.lLastY;
        double factor = s_ballistic ? BallisticGain(rawX, rawY) : s_speed;
        s_restX += rawX * factor;
        s_restY += rawY * factor;
        dx = (LONG)s_restX;
        dy = (LONG)s_restY;
        s_restX -= dx;
        s_restY -= dy;
    }
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

/// Whether the client lost its window in the middle of a turn and is still behind another one.
static bool TurnLeft() {
    if (s_turnLeft && GetForegroundWindow() == s_window) {
        s_turnLeft = false;
    }
    return s_turnLeft;
}

static bool FromD3D9(const void* address) {
    HMODULE module = nullptr;
    char name[MAX_PATH];
    return GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)address, &module)
        && K32GetModuleBaseNameA(GetCurrentProcess(), module, name, _countof(name)) && _stricmp(name, "d3d9.dll") == 0;
}

static BOOL WINAPI zzSetCursorPos(_In_ int X, _In_ int Y) {
    void* caller = _ReturnAddress();
    bool onWindowThread = OnWindowThread();
    bool turnLeft = TurnLeft();
    bool fromGame = (turnLeft || onWindowThread) && FromGame(caller);
    if (turnLeft && fromGame) {
        s_warp = { X, Y };
        return TRUE;
    }
    if (onWindowThread && s_skipInterruptedRestore && GetForegroundWindow() == s_window && (fromGame || FromD3D9(caller))) {
        return TRUE;
    }
    if (s_rawInput && onWindowThread && fromGame) {
        if (!s_turning && AnyButtonHeld() && (GetCapture() == s_window || (s_releasedMidTurn && X == s_warp.x && Y == s_warp.y))) {
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
    if (s_deadZone && OnWindowThread()) {
        *lpPoint = s_pressedAt;
        return TRUE;
    }
    if (TurnLeft()) {
        *lpPoint = s_warp;
        return TRUE;
    }
    return real_GetCursorPos(lpPoint);
}

static BOOL WINAPI zzReleaseCapture() {
    if (s_turning && FromGame(_ReturnAddress())) {
        StopTurning();
        if (GetForegroundWindow() != s_window) {
            s_turnLeft = true;
            s_skipInterruptedRestore = true;
        } else if (AnyButtonHeld()) {
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
    if ((!down && msg->message != WM_KEYUP && msg->message != WM_SYSKEYUP) || msg->wParam >= 256) {
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

/// Whether a move of the real cursor to pt stays in the dead zone after a press; the first one out of it ends the zone.
static bool StaysInDeadZone(POINT pt) {
    if (!s_deadZone) {
        return false;
    }
    if (abs(pt.x - s_pressedAt.x) < GetSystemMetrics(SM_CXDRAG) && abs(pt.y - s_pressedAt.y) < GetSystemMetrics(SM_CYDRAG)) {
        return true;
    }
    s_deadZone = false;
    return false;
}

/// Starts the dead zone when the left or right button goes down and puts the button back up where it went down while the zone lasts.
static void OnButton(MSG* msg, bool removed) {
    switch (msg->message) {
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONDBLCLK:
        if (!removed) {
            break;
        }
        s_skipInterruptedRestore = false;
        if (!s_turning && !s_releasedMidTurn && !s_deadZone) {
            s_pressedAt = msg->pt;
            s_deadZone = true;
        }
        break;
    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
        if (s_deadZone) {
            POINT client = s_pressedAt;
            ScreenToClient(s_window, &client);
            msg->lParam = MAKELPARAM(client.x, client.y);
            msg->pt = s_pressedAt;
            if (removed) {
                s_deadZone = false;
            }
        }
        break;
    }
}

static BOOL WINAPI zzPeekMessageA(_Out_ LPMSG lpMsg, _In_opt_ HWND hWnd, _In_ UINT wMsgFilterMin, _In_ UINT wMsgFilterMax, _In_ UINT wRemoveMsg) {
    if (OnWindowThread()) {
        bool foreground = GetForegroundWindow() == s_window;
        if (foreground) {
            s_skipInterruptedRestore = false;
        } else {
            memset(s_keysDown, 0, sizeof(s_keysDown)); // the key ups go elsewhere
        }
        UpdatePrintScreenHotKey(s_window, foreground);
    }
    if (s_turning && OnWindowThread() && (!AnyButtonHeld() || GetForegroundWindow() != s_window)) {
        s_turnLeft = GetForegroundWindow() != s_window;
        if (s_turnLeft) {
            s_skipInterruptedRestore = true;
        }
        StopTurning();
    }
    if (s_releasedMidTurn && OnWindowThread() && !AnyButtonHeld()) {
        s_releasedMidTurn = false;
    }
    if (s_deadZone && OnWindowThread() && (!AnyButtonHeld() || GetForegroundWindow() != s_window)) {
        s_deadZone = false;
    }
    bool movesWanted = (!hWnd || hWnd == s_window)
        && ((wMsgFilterMin == 0 && wMsgFilterMax == 0) || (wMsgFilterMin <= WM_MOUSEMOVE && WM_MOUSEMOVE <= wMsgFilterMax));
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
        if (lpMsg->hwnd == s_window) {
            OnButton(lpMsg, wRemoveMsg & PM_REMOVE);
        }
        // positions of the real cursor, which stays put while turning
        bool drop = (lpMsg->message == WM_MOUSEMOVE && lpMsg->hwnd == s_window
                        && (s_turning || TurnLeft() || (s_releasedMidTurn && lpMsg->pt.x == s_pinned.x && lpMsg->pt.y == s_pinned.y) || StaysInDeadZone(lpMsg->pt)))
            || !TranslatePrintScreen(lpMsg, wRemoveMsg & PM_REMOVE);
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
        AttachHook(&(PVOID&)real_SetCursorPos, zzSetCursorPos);
        AttachHook(&(PVOID&)real_GetCursorPos, zzGetCursorPos);
        AttachHook(&(PVOID&)real_PeekMessageA, zzPeekMessageA);
        AttachHook(&(PVOID&)real_ReleaseCapture, zzReleaseCapture);
        AttachHook(&(PVOID&)real_GetAsyncKeyState, zzGetAsyncKeyState);
        AttachHook(&(PVOID&)real_GetKeyboardState, zzGetKeyboardState);
        LONG error = CommitHooks();
        MODULE_LOG("camera fix: hooks %s (%ld)", error ? "failed" : "installed", error);
    }
    if (hWnd == s_window || !IsClientWindow(hWnd)) {
        return;
    }
    RAWINPUTDEVICE mouse = { 0x01, 0x02, 0, hWnd }; // generic desktop, mouse
    s_rawInput = RegisterRawInputDevices(&mouse, 1, sizeof(mouse)) != FALSE;
    MODULE_LOG("camera fix: window %p, raw input %s (error %lu)", hWnd, s_rawInput ? "on" : "off", s_rawInput ? 0 : GetLastError());
    s_windowThread = GetWindowThreadProcessId(hWnd, nullptr);
    s_window = hWnd;
}
