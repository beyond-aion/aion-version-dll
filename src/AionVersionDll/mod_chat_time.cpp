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

static thread_local wchar_t s_prefix[64];
static thread_local size_t s_prefixLength = 0;
// the channel message waiting for the filter, which would mangle the markup of the time if it came before
static thread_local bool s_prefixPending = false;

/// Grey time in brackets, the way later clients show it.
static void MakePrefix() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    int length = swprintf_s(s_prefix, L"[color:(%02d:%02d:%02d);0.678 0.678 0.678] ", t.wHour, t.wMinute, t.wSecond);
    s_prefixLength = length > 0 ? length : 0;
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

/// Older clients (4.6) cut the text of a color tag to the length of the tag name "color:" minus one, so 5 characters; later
/// ones take 127. The limit is set up before the copy loop as lea r9d, [r12-1] with r12 the name length (6), and turned into
/// lea r9d, [r12+79h] = 127.
static void FixColorTextLength(HMODULE game) {
    BYTE* p = FindPattern(game, "45 8D 4C 24 FF 48 89 B4 24 ?? ?? ?? ?? 48 8D 74 7B 02 33 FF 45 85 C9 8B D7 8B DF 7E ?? 4C 8B C5 48 8B CE "
                                "4C 2B C6 0F B7 01 66 85 C0 74 ?? 66 3D 3B 00 74 ?? 66 3D 5D 00");
    if (!p) {
        return;
    }
    BYTE displacement = 0x79;
    PatchMemory(p + 4, &displacement, 1);
    ModsLog("chat time: color text length fixed at %p", p);
}

void InstallChatTime(HMODULE game) {
    // 5.x clients can show the time themselves, set per chat tab
    static const wchar_t BUILT_IN_TIME[] = L"[color:(%s);%f %f %f] %s";
    if (FindBytes(game, BUILT_IN_TIME, sizeof(BUILT_IN_TIME))) {
        ModsLog("chat time: the client has its own");
        return;
    }
    FixColorTextLength(game);
    // the message entry point also shows GM alerts on screen, the log writer formats the Chat.log lines
    real_ChatAddMessage = (ChatAddMessage_t)FunctionUsing(game, "v3_system_gm_alert");
    real_ChatLogWrite = (ChatLogWrite_t)FunctionUsing(game, "%.4d.%.2d.%.2d %.2d:%.2d:%.2d : %s \n");
    real_FilterChatText = real_ChatAddMessage ? FindChatFilter((BYTE*)real_ChatAddMessage) : nullptr;
    ModsLog("chat time: add=%p log=%p filter=%p", real_ChatAddMessage, real_ChatLogWrite, real_FilterChatText);
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
