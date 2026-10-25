#include "module_load.h"

// With the Windows 11 setting "Use the Print screen key to open screen capture", PrintScreen opens the Snipping Tool over the client on top
// of the client's own screenshot. Windows leaves the key alone while a program holds it as a hot key, so the client window holds it while it
// is in the foreground, and gets the hot key as the key up it takes its screenshots on.

static const int HOT_KEY_ID = 0x5C01;
static HWND s_holder;
static HWND s_refused; // another program already holds the key

/// Called from the client's message loop, on the window's thread.
void UpdatePrintScreenHotKey(HWND window, bool foreground) {
    if (foreground) {
        if (!s_holder && s_refused != window) {
            if (RegisterHotKey(window, HOT_KEY_ID, MOD_NOREPEAT, VK_SNAPSHOT)) {
                s_holder = window;
            } else {
                static bool logged = false;
                if (!logged) {
                    logged = true;
                    MODULE_LOG("print screen fix: another program holds the key (error %lu)", GetLastError());
                }
                s_refused = window;
            }
        }
    } else {
        if (s_holder) {
            UnregisterHotKey(s_holder, HOT_KEY_ID);
            s_holder = nullptr;
        }
        s_refused = nullptr;
    }
}

/// Turns the hot key into the key up the client expects and drops a real key up meanwhile, so that a press takes one screenshot. False for a
/// message to drop.
bool TranslatePrintScreen(MSG* msg) {
    if (!s_holder || msg->hwnd != s_holder) {
        return true;
    }
    if (msg->message == WM_HOTKEY && msg->wParam == HOT_KEY_ID) {
        msg->message = WM_KEYUP;
        msg->wParam = VK_SNAPSHOT;
        msg->lParam = 0xC1370001; // scan code 37h, extended, released
        return true;
    }
    return !((msg->message == WM_KEYUP || msg->message == WM_SYSKEYUP) && msg->wParam == VK_SNAPSHOT);
}
