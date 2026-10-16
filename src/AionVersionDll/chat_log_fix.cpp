#include "module_load.h"
#include <stdio.h>
#include <string>
#include "detours.h"
#if defined(_M_AMD64)
#include "mods.h"
#endif

typedef BOOL(WINAPI* DeleteFileA_t)(LPCSTR path);
static DeleteFileA_t real_DeleteFileA = DeleteFileA;

static const char CHAT_LOG[] = "Chat.log";
static const char LINE_FORMAT[] = "%.4d.%.2d.%.2d %.2d:%.2d:%.2d : %s \n";

/// A line of Chat.log in UTF-8 with milliseconds: the client converts the message with the code page of the current keyboard layout,
/// so letters the layout's code page lacks turn into question marks, and cuts it at 255 bytes.
static std::string ChatLogLine(const wchar_t* message, const char* lineEnd) {
    std::string line;
    WIN32_FILE_ATTRIBUTE_DATA attributes;
    // a byte order mark only at the start of a new file, older lines of an existing one are in the code page of their time
    if (GetFileAttributesExA(CHAT_LOG, GetFileExInfoStandard, &attributes) && !attributes.nFileSizeLow && !attributes.nFileSizeHigh) {
        line = "\xEF\xBB\xBF";
    }
    SYSTEMTIME t;
    GetLocalTime(&t);
    char time[40];
    sprintf_s(time, "%04d.%02d.%02d %02d:%02d:%02d.%03d : ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    line += time;
    int length = WideCharToMultiByte(CP_UTF8, 0, message, -1, nullptr, 0, nullptr, nullptr);
    if (length > 1) {
        size_t at = line.size();
        line.resize(at + length - 1);
        WideCharToMultiByte(CP_UTF8, 0, message, -1, &line[at], length, nullptr, nullptr);
    }
    line += lineEnd;
    return line;
}

/// Departs from the client: 5.x clients delete Chat.log at start when chat logging is off at that moment, and the in-game option
/// turns it on only later, so the log of every earlier session is lost. Chat.log is kept; drop this hook to get the client's behaviour back.
static BOOL WINAPI zzDeleteFileA(LPCSTR path) {
    if (path && _stricmp(path, CHAT_LOG) == 0) {
        return TRUE;
    }
    return real_DeleteFileA(path);
}

#if defined(_M_AMD64)

typedef __int64(__fastcall* ChatLogWrite_t)(void* chat, const wchar_t* text, __int64 flag);
typedef int(__cdecl* Fputs_t)(const char* text, FILE* file);

static ChatLogWrite_t real_ChatLogWrite = nullptr;
static Fputs_t real_Fputs = nullptr;

// the message of the line the writer is about to put into the file
static thread_local const wchar_t* t_message = nullptr;

/// Writes the line the client has just formatted as UTF-8 with milliseconds instead.
static int __cdecl zzFputs(const char* text, FILE* file) {
    const wchar_t* message = t_message;
    if (!message) {
        return real_Fputs(text, file);
    }
    t_message = nullptr;
    return real_Fputs(ChatLogLine(message, " \n").c_str(), file);
}

static __int64 __fastcall zzChatLogWrite(void* chat, const wchar_t* text, __int64 flag) {
    text = SkipChatTime(text);
    if (!real_Fputs || !text) {
        return real_ChatLogWrite(chat, text, flag);
    }
    t_message = text;
    __int64 result = real_ChatLogWrite(chat, text, flag);
    t_message = nullptr;
    return result;
}

/// The writer opens the file and writes the line through imported functions: lea rcx, [Chat.log]; call [fopen]; ...; call [fputs]
static void** FindFputsSlot(HMODULE game, BYTE* writer) {
    BYTE* end = FunctionBodyEnd(writer, 0x1000);
    int calls = 0;
    for (BYTE* p = writer; p + 7 < end; p++) {
        BYTE* name = !calls && p[0] == 0x48 && p[1] == 0x8D && p[2] == 0x0D ? ResolveRip(p + 3, p + 7) : nullptr;
        if (name && InModule(game, name, sizeof(CHAT_LOG)) && memcmp(name, CHAT_LOG, sizeof(CHAT_LOG)) == 0) {
            calls = 1;
            p += 6;
        } else if (calls && p[0] == 0xFF && p[1] == 0x15 && ++calls == 3) {
            return (void**)ResolveRip(p + 2, p + 6);
        }
    }
    return nullptr;
}

void InstallChatLogFix(HMODULE game) {
    const void* format = FindBytes(game, LINE_FORMAT, sizeof(LINE_FORMAT));
    BYTE* lea = format ? FindLeaTo(game, format) : nullptr;
    real_ChatLogWrite = lea ? (ChatLogWrite_t)FunctionStart(lea) : nullptr;
    void** slot = real_ChatLogWrite ? FindFputsSlot(game, (BYTE*)real_ChatLogWrite) : nullptr;
    ModsLog("chat log: writer=%p fputs=%p", real_ChatLogWrite, slot);
    if (!real_ChatLogWrite) {
        return;
    }
    Fputs_t replacement = zzFputs;
    Fputs_t original = slot ? (Fputs_t)*slot : nullptr;
    if (original && PatchMemory(slot, &replacement, sizeof(replacement))) {
        real_Fputs = original;
    }
    DetourAttach(&(PVOID&)real_ChatLogWrite, zzChatLogWrite);
    DetourAttach(&(PVOID&)real_DeleteFileA, zzDeleteFileA);
}

#else

// The 32-bit clients reach fopen and fputs through stubs of their packer, so the writer is replaced as a whole. It checks the
// g_chatlog console variable the same way the client does: the system's console, the variable by name, its integer value.
typedef void(__fastcall* ChatLogWrite_t)(void* chat, void* unused, const wchar_t* text);
typedef void*(__thiscall* GetConsole_t)(void* system);
typedef void*(__thiscall* GetCVar_t)(void* console, const char* name, int caseSensitive);
typedef int(__thiscall* GetIVal_t)(void* cvar);

static ChatLogWrite_t real_ChatLogWrite = nullptr;
static void** s_system = nullptr;
static DWORD s_getConsole = 0;
static DWORD s_getCVar = 0;
static DWORD s_getIVal = 0;
static const char* s_cvarName = nullptr;

template <typename T> static T Virtual(void* object, DWORD offset) {
    return *(T*)(*(BYTE**)object + offset);
}

static bool ChatLogEnabled() {
    void* system = *s_system;
    void* console = system ? Virtual<GetConsole_t>(system, s_getConsole)(system) : nullptr;
    void* cvar = console ? Virtual<GetCVar_t>(console, s_getCVar)(console, s_cvarName, 1) : nullptr;
    return cvar && Virtual<GetIVal_t>(cvar, s_getIVal)(cvar);
}

static void __fastcall zzChatLogWrite(void* chat, void* unused, const wchar_t* text) {
    if (!text || !ChatLogEnabled()) {
        return;
    }
    std::string line = ChatLogLine(text, " \r\n");
    HANDLE file = CreateFileA(CHAT_LOG, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        DWORD written;
        WriteFile(file, line.data(), (DWORD)line.size(), &written, nullptr);
        CloseHandle(file);
    }
}

/// Finds bytes in the readable pages of the image; packers can leave some without access.
static BYTE* FindInImage(HMODULE module, const void* bytes, size_t length) {
    BYTE* image = (BYTE*)module;
    BYTE* imageEnd = image + ModuleImageSize(module);
    MEMORY_BASIC_INFORMATION region;
    for (BYTE* begin = image; begin < imageEnd && VirtualQuery(begin, &region, sizeof(region)); begin = (BYTE*)region.BaseAddress + region.RegionSize) {
        bool readable = region.State == MEM_COMMIT && !(region.Protect & (PAGE_NOACCESS | PAGE_GUARD)) && (region.Protect & 0xEE);
        BYTE* end = min(imageEnd, (BYTE*)region.BaseAddress + region.RegionSize);
        for (BYTE* p = (BYTE*)region.BaseAddress; readable && p + length <= end; p++) {
            if (*p == *(const BYTE*)bytes && memcmp(p, bytes, length) == 0) {
                return p;
            }
        }
    }
    return nullptr;
}

static BYTE* FindPush(HMODULE module, const void* address) {
    BYTE push[5] = { 0x68 };
    *(const void**)(push + 1) = address;
    return FindInImage(module, push, sizeof(push));
}

/// The writer formats the line with push offset format; it starts with push ebp; mov ebp, esp after the padding of the one before.
/// Before the format it gets the variable: mov ecx, [system]; ...; mov edx, [eax+console]; call edx; ...; mov eax, [edx+getCVar];
/// push offset g_chatlog; call eax; ...; mov eax, [edx+getIVal]; call eax
static bool FindWriter(HMODULE game) {
    const char* name = (const char*)FindInImage(game, "g_chatlog", 10);
    const void* format = FindInImage(game, LINE_FORMAT, sizeof(LINE_FORMAT));
    BYTE* push = format ? FindPush(game, format) : nullptr;
    BYTE* writer = nullptr;
    for (BYTE* p = push; p && p > push - 0x400 && !writer; p--) {
        if (p[0] == 0x55 && p[1] == 0x8B && p[2] == 0xEC && (p[-1] == 0xCC || p[-1] == 0x90 || p[-1] == 0xC3)) {
            writer = p;
        }
    }
    for (BYTE* p = writer; p && p < push && name; p++) {
        if (!s_system && p[0] == 0x8B && p[1] == 0x0D) {
            s_system = *(void***)(p + 2);
        } else if (!s_getConsole && p[0] == 0x8B && p[1] == 0x90 && p[6] == 0xFF && p[7] == 0xD2) {
            s_getConsole = *(DWORD*)(p + 2);
        } else if (!s_getConsole && p[0] == 0x8B && p[1] == 0x50 && p[3] == 0xFF && p[4] == 0xD2) {
            s_getConsole = p[2];
        } else if (!s_getCVar && p[0] == 0x8B && p[1] == 0x42 && p[3] == 0x68 && *(const char**)(p + 4) == name) {
            s_getCVar = p[2];
        } else if (s_getCVar && !s_getIVal && p[0] == 0x8B && p[1] == 0x42 && p[3] == 0xFF && p[4] == 0xD0) {
            s_getIVal = p[2];
        }
    }
    s_cvarName = name;
    real_ChatLogWrite = (ChatLogWrite_t)writer;
    return writer && s_system && s_getConsole && s_getCVar && s_getIVal;
}

static void InstallWriter(HMODULE game) {
    if (!FindWriter(game)) {
        return;
    }
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&)real_ChatLogWrite, zzChatLogWrite);
    DetourTransactionCommit();
}

/// Must be called inside an open Detours transaction.
void InstallChatLogFix() {
    DetourAttach(&(PVOID&)real_DeleteFileA, zzDeleteFileA);
    OnModuleLoad(L"Game.dll", InstallWriter, true);
}

#endif
