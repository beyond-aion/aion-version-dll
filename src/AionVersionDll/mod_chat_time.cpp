#include "mods.h"
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include "detours.h"

// Message types below 73 are chat channels, which go through a text filter first; from 200 on they are GM alerts that also go
// to the screen center, and debug output.
static constexpr int FIRST_SYSTEM_TYPE = 73;
static constexpr int FIRST_NON_CHAT_TYPE = 200;
static constexpr size_t MAX_CHAT_TEXT = 4096;

typedef __int64(__fastcall* ChatAddMessage_t)(void* chat, int type, const wchar_t* text);
typedef __int64(__fastcall* ChatLogWrite_t)(void* chat, const wchar_t* text, __int64 flag);
typedef __int64(__fastcall* FilterChatText_t)(void* filter, wchar_t* text, int size);

static ChatAddMessage_t real_ChatAddMessage = nullptr;
static ChatLogWrite_t real_ChatLogWrite = nullptr;
static FilterChatText_t real_FilterChatText = nullptr;

static thread_local wchar_t s_prefix[160];
static thread_local size_t s_prefixLength = 0;
// the channel message waiting for the filter, which would mangle the markup of the time if it came before
static thread_local bool s_prefixPending = false;
// older clients take at most 5 characters of text in a color tag, so the time is split over several tags there
static bool s_shortColorText = false;
static constexpr int SHORT_COLOR_TEXT = 5;

/// Grey time in brackets, the way later clients show it.
static void MakePrefix() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t time[16];
    int timeLength = swprintf_s(time, L"(%02d:%02d:%02d)", t.wHour, t.wMinute, t.wSecond);
    int chunk = s_shortColorText ? SHORT_COLOR_TEXT : timeLength;
    s_prefixLength = 0;
    for (int i = 0; i < timeLength; i += chunk) {
        int length = swprintf_s(s_prefix + s_prefixLength, _countof(s_prefix) - s_prefixLength, L"[color:%.*s;0.678 0.678 0.678]",
            min(chunk, timeLength - i), time + i);
        s_prefixLength += length > 0 ? length : 0;
    }
    s_prefix[s_prefixLength++] = L' ';
    s_prefix[s_prefixLength] = 0;
}

static __int64 __fastcall zzChatAddMessage(void* chat, int type, const wchar_t* text) {
    if (!text || !*text || type >= FIRST_NON_CHAT_TYPE) {
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

void InstallChatTime(HMODULE game) {
    // 5.x clients can show the time themselves, set per chat tab
    static const wchar_t BUILT_IN_TIME[] = L"[color:(%s);%f %f %f] %s";
    if (FindBytes(game, BUILT_IN_TIME, sizeof(BUILT_IN_TIME))) {
        ModsLog("chat time: the client has its own");
        return;
    }
    // the quest target effect came with the clients that fixed the text length of color tags
    static const char QUEST_TARGET_EFFECT[] = "sys_UIfx.Quest.target";
    s_shortColorText = !FindBytes(game, QUEST_TARGET_EFFECT, sizeof(QUEST_TARGET_EFFECT));
    // the message entry point also shows GM alerts on screen, the log writer formats the Chat.log lines
    real_ChatAddMessage = (ChatAddMessage_t)FunctionUsing(game, "v3_system_gm_alert");
    real_ChatLogWrite = (ChatLogWrite_t)FunctionUsing(game, "%.4d.%.2d.%.2d %.2d:%.2d:%.2d : %s \n");
    real_FilterChatText = real_ChatAddMessage ? FindChatFilter((BYTE*)real_ChatAddMessage) : nullptr;
    ModsLog("chat time: add=%p log=%p filter=%p short color text=%d", real_ChatAddMessage, real_ChatLogWrite, real_FilterChatText, s_shortColorText);
    if (!real_ChatAddMessage) {
        return;
    }
    DetourAttach(&(PVOID&)real_ChatAddMessage, zzChatAddMessage);
    if (real_ChatLogWrite) {
        DetourAttach(&(PVOID&)real_ChatLogWrite, zzChatLogWrite);
    }
    if (real_FilterChatText) {
        DetourAttach(&(PVOID&)real_FilterChatText, zzFilterChatText);
    }
}
