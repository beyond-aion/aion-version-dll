#ifndef AION_CHAT_LOG_FIX
#define AION_CHAT_LOG_FIX

/// Returns the part of a chat message that belongs in Chat.log.
typedef const wchar_t* (*ChatLogFilter_t)(const wchar_t* text);

/// Writes Chat.log in UTF-8 with milliseconds and keeps 5.x clients from deleting it. Must be called inside an open Detours transaction.
void InstallChatLogFix();

/// Lets a mod keep what it adds to chat messages out of Chat.log.
void SetChatLogFilter(ChatLogFilter_t filter);

#endif
