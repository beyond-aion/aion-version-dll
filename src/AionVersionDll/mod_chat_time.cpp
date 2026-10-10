#include "mods.h"
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include "detours.h"

// Message types below 73 are chat channels, which go through a text filter first; 203..205 are GM alerts that also go to the screen
// center, which gets them without the time; from 206 on they are debug output.
static constexpr int FIRST_SYSTEM_TYPE = 73;
static constexpr int FIRST_ALERT_TYPE = 203;
static constexpr int FIRST_NON_CHAT_TYPE = 206;
static constexpr size_t MAX_CHAT_TEXT = 4096;
static const char GM_ALERT_STYLE[] = "v3_system_gm_alert";

typedef __int64(__fastcall* ChatAddMessage_t)(void* chat, int type, const wchar_t* text);
typedef __int64(__fastcall* ChatLogWrite_t)(void* chat, const wchar_t* text, __int64 flag);
typedef __int64(__fastcall* FilterChatText_t)(void* filter, wchar_t* text, int size);
typedef __int64(__fastcall* ShowAlert_t)(void* alerts, const wchar_t* text, __int64 a3, const char* style, __int64 a5, __int64 a6, __int64 a7,
    __int64 a8);

static ChatAddMessage_t real_ChatAddMessage = nullptr;
static ChatLogWrite_t real_ChatLogWrite = nullptr;
static FilterChatText_t real_FilterChatText = nullptr;
static ShowAlert_t real_ShowAlert = nullptr;

static thread_local wchar_t s_prefix[64];
static thread_local size_t s_prefixLength = 0;
// the channel message waiting for the filter, which would mangle the markup of the time if it came before
static thread_local bool s_prefixPending = false;

/// Grey time in brackets, the way later clients show it.
static void MakePrefix() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    int length;
    if (g_modsConfig.chatTimeFormat == 1) {
        length = swprintf_s(s_prefix, L"[color:(%02d:%02d);0.678 0.678 0.678] ", t.wHour, t.wMinute);
    } else {
        length = swprintf_s(s_prefix, L"[color:(%02d:%02d:%02d);0.678 0.678 0.678] ", t.wHour, t.wMinute, t.wSecond);
    }
    s_prefixLength = length > 0 ? length : 0;
}

static __int64 __fastcall zzChatAddMessage(void* chat, int type, const wchar_t* text) {
    if (!text || !*text || type >= FIRST_NON_CHAT_TYPE || (type >= FIRST_ALERT_TYPE && !real_ShowAlert)) {
        return real_ChatAddMessage(chat, type, text);
    }
    MakePrefix();
    __int64 result;
    if (type < FIRST_SYSTEM_TYPE && real_FilterChatText) {
        s_prefixPending = true;
        result = real_ChatAddMessage(chat, type, text);
        s_prefixPending = false;
    } else {
        wchar_t buffer[MAX_CHAT_TEXT + 64];
        wcscpy_s(buffer, s_prefix);
        wcsncpy_s(buffer + s_prefixLength, _countof(buffer) - s_prefixLength, text, MAX_CHAT_TEXT - s_prefixLength);
        result = real_ChatAddMessage(chat, type, buffer);
    }
    s_prefixLength = 0;
    return result;
}

/// Puts the time in front of a channel message once the filter is done with it.
static __int64 __fastcall zzFilterChatText(void* filter, wchar_t* text, int size) {
    __int64 result = real_FilterChatText(filter, text, size);
    if (s_prefixPending && text && size > 0) {
        s_prefixPending = false;
        size_t length = wcsnlen(text, size);
        size_t kept = min(length, (size_t)size - 1 - s_prefixLength);
        memmove(text + s_prefixLength, text, kept * sizeof(wchar_t));
        memcpy(text, s_prefix, s_prefixLength * sizeof(wchar_t));
        text[s_prefixLength + kept] = 0;
    }
    return result;
}

/// Chat.log has its own date and time on every line, so the prefix is kept out of it.
static __int64 __fastcall zzChatLogWrite(void* chat, const wchar_t* text, __int64 flag) {
    if (s_prefixLength && text && wcsncmp(text, s_prefix, s_prefixLength) == 0) {
        text += s_prefixLength;
    }
    return real_ChatLogWrite(chat, text, flag);
}

/// The alert in the screen center shows a GM alert as it came, without the time.
static __int64 __fastcall zzShowAlert(void* alerts, const wchar_t* text, __int64 a3, const char* style, __int64 a5, __int64 a6, __int64 a7,
    __int64 a8) {
    if (s_prefixLength && text && wcsncmp(text, s_prefix, s_prefixLength) == 0) {
        text += s_prefixLength;
    }
    return real_ShowAlert(alerts, text, a3, style, a5, a6, a7, a8);
}

/// The function whose code loads the given string.
static BYTE* FunctionUsing(HMODULE game, const char* text) {
    const void* found = FindBytes(game, text, strlen(text) + 1);
    BYTE* lea = found ? FindLeaTo(game, found) : nullptr;
    return lea ? FunctionStart(lea) : nullptr;
}

/// The filter the channel function runs over a message: lea rcx, [filter]; mov r8d, 4096; ...; call
static FilterChatText_t FindChatFilter(BYTE* addMessage) {
    // the message entry point hands channel messages over: cmp edx, 73; ...; jge; call channel
    BYTE* channel = nullptr;
    for (BYTE* p = addMessage; p < addMessage + 0x30; p++) {
        if (p[0] == 0x7D && p[2] == 0xE8) {
            channel = ResolveRip(p + 3, p + 7);
            break;
        }
    }
    for (BYTE* p = channel; p && p < channel + 0x100; p++) {
        if (p[0] == 0x48 && p[1] == 0x8D && p[2] == 0x0D && memcmp(p + 7, "\x41\xB8\x00\x10\x00\x00", 6) == 0) {
            for (BYTE* q = p + 13; q < p + 0x30; q++) {
                if (q[0] == 0xE8) {
                    return (FilterChatText_t)ResolveRip(q + 1, q + 5);
                }
            }
        }
    }
    return nullptr;
}

/// The alert the message entry point shows GM alerts with: lea r9, [style]; ...; call
static ShowAlert_t FindShowAlert(HMODULE game) {
    const void* style = FindBytes(game, GM_ALERT_STYLE, sizeof(GM_ALERT_STYLE));
    BYTE* lea = style ? FindLeaTo(game, style) : nullptr;
    for (BYTE* p = lea; p && p < lea + 0x40; p++) {
        if (p[0] == 0xE8) {
            BYTE* target = ResolveRip(p + 1, p + 5);
            return InModule(game, target, 16) ? (ShowAlert_t)target : nullptr;
        }
    }
    return nullptr;
}

void InstallChatTime(HMODULE game) {
    // 5.x clients can show the time themselves, set per chat tab
    static const wchar_t BUILT_IN_TIME[] = L"[color:(%s);%f %f %f] %s";
    if (FindBytes(game, BUILT_IN_TIME, sizeof(BUILT_IN_TIME))) {
        ModsLog("chat time: the client has its own");
        return;
    }
    // the message entry point also shows GM alerts on screen, the log writer formats the Chat.log lines
    real_ChatAddMessage = (ChatAddMessage_t)FunctionUsing(game, GM_ALERT_STYLE);
    real_ChatLogWrite = (ChatLogWrite_t)FunctionUsing(game, "%.4d.%.2d.%.2d %.2d:%.2d:%.2d : %s \n");
    real_FilterChatText = real_ChatAddMessage ? FindChatFilter((BYTE*)real_ChatAddMessage) : nullptr;
    real_ShowAlert = real_ChatAddMessage ? FindShowAlert(game) : nullptr;
    ModsLog("chat time: add=%p log=%p filter=%p alert=%p", real_ChatAddMessage, real_ChatLogWrite, real_FilterChatText, real_ShowAlert);
    if (!real_ChatAddMessage) {
        return;
    }
    if (real_ShowAlert) {
        DetourAttach(&(PVOID&)real_ShowAlert, zzShowAlert);
    }
    DetourAttach(&(PVOID&)real_ChatAddMessage, zzChatAddMessage);
    if (real_ChatLogWrite) {
        DetourAttach(&(PVOID&)real_ChatLogWrite, zzChatLogWrite);
    }
    if (real_FilterChatText) {
        DetourAttach(&(PVOID&)real_FilterChatText, zzFilterChatText);
    }
}
