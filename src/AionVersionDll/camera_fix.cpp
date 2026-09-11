#include <windows.h>
#include <windowsx.h>
#include "detours.h"

#pragma warning(disable: 28159)

static bool cursorHidden = false;
static bool insertMessage = false;
static int insertMessageCount = 0;
static MSG hiddenMouseMoveMsg = { nullptr, WM_MOUSEMOVE };
static DWORD fixCoordsExpirationTime = 0;
static POINT lastVisibleCursorPos = {};
static POINT hiddenCursorPos = {};


static bool IsRightLeftOrMiddleMouseButtonDown();
static decltype(SetCursor)* real_SetCursor = SetCursor;
static HCURSOR WINAPI zzSetCursor(_In_opt_ HCURSOR hCursor) {
    if (!cursorHidden && !hCursor && IsRightLeftOrMiddleMouseButtonDown()) {
        fixCoordsExpirationTime = GetTickCount() + 200;
        GetCursorPos(&lastVisibleCursorPos);
        insertMessageCount = 0;
        cursorHidden = true;
    } else if (cursorHidden && hCursor) {
        cursorHidden = false;
        SetCursorPos(lastVisibleCursorPos.x, lastVisibleCursorPos.y);
    }
    return real_SetCursor(hCursor);
}

static decltype(GetCursorPos)* real_GetCursorPos = GetCursorPos;
static BOOL WINAPI zzGetCursorPos(_Out_ LPPOINT lpPoint) {
    if (cursorHidden) {
        *lpPoint = hiddenCursorPos;
        return TRUE;
    }
    return real_GetCursorPos(lpPoint);
}

static WPARAM MakeMouseMoveWParam();
static decltype(SetCursorPos)* real_SetCursorPos = SetCursorPos;
static BOOL WINAPI zzSetCursorPos(_In_ int X, _In_ int Y) {
    BOOL result = real_SetCursorPos(X, Y);
    if (cursorHidden) {
        hiddenCursorPos.x = X;
        hiddenCursorPos.y = Y;
        if (hiddenMouseMoveMsg.pt.x != X || hiddenMouseMoveMsg.pt.y != Y) {
            POINT pt = { X, Y };
            ScreenToClient(hiddenMouseMoveMsg.hwnd, &pt);
            hiddenMouseMoveMsg.lParam = MAKELPARAM(pt.x, pt.y);
            hiddenMouseMoveMsg.pt.x = X;
            hiddenMouseMoveMsg.pt.y = Y;
        }
        hiddenMouseMoveMsg.wParam = MakeMouseMoveWParam();
        hiddenMouseMoveMsg.time = GetTickCount();
        insertMessage = true;
    }
    return result;
}

static void FixCoords(LPMSG lpMsg);
static decltype(PeekMessageA)* real_PeekMessageA = PeekMessageA;
static BOOL WINAPI zzPeekMessageA(_Out_ LPMSG lpMsg, _In_opt_ HWND hWnd, _In_ UINT wMsgFilterMin, _In_ UINT wMsgFilterMax, _In_ UINT wRemoveMsg) {
    if (insertMessage) {
        if (insertMessageCount++ == 0) {
            // drop queued messages when starting to move the camera, as they are often relative to the last visible cursor position instead of the hidden position
            while (real_PeekMessageA(lpMsg, hWnd, WM_MOUSEMOVE, WM_MOUSEMOVE, PM_REMOVE)) {
            }
        }
        *lpMsg = hiddenMouseMoveMsg;
        insertMessage = false;
        return TRUE;
    }
    BOOL result = real_PeekMessageA(lpMsg, hWnd, wMsgFilterMin, wMsgFilterMax, wRemoveMsg);
    if (result && cursorHidden && lpMsg->message == WM_MOUSEMOVE) {
        if (lpMsg->time < fixCoordsExpirationTime) {
            FixCoords(lpMsg);
        }
        hiddenCursorPos = lpMsg->pt;
    }
    return result;
}

static bool IsRightLeftOrMiddleMouseButtonDown() {
    return (GetKeyState(VK_LBUTTON) & 0x8000) != 0 || (GetKeyState(VK_RBUTTON) & 0x8000) != 0 || (GetKeyState(VK_MBUTTON) & 0x8000) != 0;
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

/// For the first few frames after the cursor is hidden, the client may randomly receive movement relative to the last visible cursor position rather than the SetCursorPos position.
/// This function detects this and translates the coordinates relative to the hidden cursor position to prevent initial camera jumps.
static void FixCoords(LPMSG lpMsg) {
    bool hiddenNearLastVisiblePos = abs(lastVisibleCursorPos.x - hiddenMouseMoveMsg.pt.x) <= 100 && abs(lastVisibleCursorPos.y - hiddenMouseMoveMsg.pt.y) <= 100;
	// return if the cursor was hidden near the last visible cursor position, as the detection is unreliable in this case and false positives can cause jumping or inverted camera movement
    if (hiddenNearLastVisiblePos) {
        return;
    }
    long lastVisibleCursorPosDistX = lastVisibleCursorPos.x - lpMsg->pt.x;
    long lastVisibleCursorPosDistY = lastVisibleCursorPos.y - lpMsg->pt.y;
    long lastVisibleCursorPosDistSum = abs(lastVisibleCursorPosDistX) + abs(lastVisibleCursorPosDistY);
    long hiddenCursorPosDistX = hiddenMouseMoveMsg.pt.x - lpMsg->pt.x;
    long hiddenCursorPosDistY = hiddenMouseMoveMsg.pt.y - lpMsg->pt.y;
    long hiddenCursorPosDistSum = abs(hiddenCursorPosDistX) + abs(hiddenCursorPosDistY);
    if (lastVisibleCursorPosDistSum >= hiddenCursorPosDistSum) {
        return;
    }
    long cursorPosLParamX = GET_X_LPARAM(hiddenMouseMoveMsg.lParam);
    long cursorPosLParamY = GET_Y_LPARAM(hiddenMouseMoveMsg.lParam);
    lpMsg->lParam = MAKELPARAM(cursorPosLParamX + lastVisibleCursorPosDistX, cursorPosLParamY + lastVisibleCursorPosDistY);
    lpMsg->pt.x = hiddenMouseMoveMsg.pt.x + lastVisibleCursorPosDistX;
    lpMsg->pt.y = hiddenMouseMoveMsg.pt.y + lastVisibleCursorPosDistY;
}

static bool IsWindowsVersionOrLater(int major, int minor, int build) {
    HMODULE hNtdll = GetModuleHandle(L"ntdll.dll");
    if (!hNtdll) return false;

    auto pRtlGetVersion = reinterpret_cast<LONG(WINAPI*)(RTL_OSVERSIONINFOW*)>(GetProcAddress(hNtdll, "RtlGetVersion"));
    if (!pRtlGetVersion) return false;

    RTL_OSVERSIONINFOW verInfo = { sizeof(RTL_OSVERSIONINFOW) };
    if (pRtlGetVersion(&verInfo) >= 0) {
        int majorVersion = verInfo.dwMajorVersion;
        int minorVersion = verInfo.dwMinorVersion;
        int buildNumber = verInfo.dwBuildNumber;
        return majorVersion > major || majorVersion == major && (minorVersion > minor || minorVersion == minor && buildNumber >= build);
    }
    return false;
}

static bool IsWindows10FallCreatorsUpdateOrLater() {
    return IsWindowsVersionOrLater(10, 0, 16299); // update version 1709 (Fall Creators Update) broke the WM_MOUSEMOVE event when dragging the mouse
}

/// Fixes stuttering camera movement on Windows 10 and later
void InstallOrUpdateCameraFix(HWND hWnd) {
    hiddenMouseMoveMsg.hwnd = hWnd;
    if (real_SetCursor == SetCursor && IsWindows10FallCreatorsUpdateOrLater()) {
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourAttach(&(PVOID&)real_SetCursor, zzSetCursor);
        DetourAttach(&(PVOID&)real_SetCursorPos, zzSetCursorPos);
        DetourAttach(&(PVOID&)real_GetCursorPos, zzGetCursorPos);
        DetourAttach(&(PVOID&)real_PeekMessageA, zzPeekMessageA);
        DetourTransactionCommit();
    }
}