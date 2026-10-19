#include "chat_log_fix.h"
#include "module_load.h"
#include <stdio.h>
#include <string>
#include <detours/detours.h>

// The client writes each line of Chat.log in the code page of the current keyboard layout, so letters the layout's code page lacks
// turn into question marks, and cuts it at 255 bytes. Its writer is replaced: it checks the g_chatlog console variable the way the
// client does (the system's console, the variable by name, its integer value) and appends the line in UTF-8 with milliseconds.

static const char CHAT_LOG[] = "Chat.log";
static const char LINE_FORMAT[] = "%.4d.%.2d.%.2d %.2d:%.2d:%.2d : %s \n";

#if defined(_M_AMD64)
typedef __int64(__fastcall* ChatLogWrite_t)(void* chat, const wchar_t* text, __int64 flag);
#else
// __thiscall with the text on the stack, which __fastcall with an unused edx matches
typedef void(__fastcall* ChatLogWrite_t)(void* chat, void* unused, const wchar_t* text);
#endif
typedef void*(__thiscall* GetConsole_t)(void* system);
typedef void*(__thiscall* GetCVar_t)(void* console, const char* name, bool caseSensitive);
typedef int(__thiscall* GetIVal_t)(void* cvar);
typedef BOOL(WINAPI* DeleteFileA_t)(LPCSTR path);

static ChatLogWrite_t real_ChatLogWrite = nullptr;
static DeleteFileA_t real_DeleteFileA = DeleteFileA;
static ChatLogFilter_t s_filter = nullptr;

static void** s_system = nullptr;
static const char* s_cvarName = nullptr;
// the virtual calls the writer makes, in order: the system's console, the variable by name, its integer value
static DWORD s_calls[3];

void SetChatLogFilter(ChatLogFilter_t filter) {
    s_filter = filter;
}

template <typename T> static T Virtual(void* object, DWORD offset) {
    return *(T*)(*(BYTE**)object + offset);
}

static bool ChatLogEnabled() {
    void* system = *s_system;
    void* console = system ? Virtual<GetConsole_t>(system, s_calls[0])(system) : nullptr;
    void* cvar = console ? Virtual<GetCVar_t>(console, s_calls[1])(console, s_cvarName, true) : nullptr;
    return cvar && Virtual<GetIVal_t>(cvar, s_calls[2])(cvar);
}

static void AppendLine(const wchar_t* message) {
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
    line += " \r\n";
    HANDLE file = CreateFileA(CHAT_LOG, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        DWORD written;
        WriteFile(file, line.data(), (DWORD)line.size(), &written, nullptr);
        CloseHandle(file);
    }
}

static void WriteLine(const wchar_t* text) {
    if (text && s_filter) {
        text = s_filter(text);
    }
    if (text && ChatLogEnabled()) {
        AppendLine(text);
    }
}

#if defined(_M_AMD64)
static __int64 __fastcall zzChatLogWrite(void* chat, const wchar_t* text, __int64 flag) {
    WriteLine(text);
    return 0;
}
#else
static void __fastcall zzChatLogWrite(void* chat, void* unused, const wchar_t* text) {
    WriteLine(text);
}
#endif

/// Departs from the client: 5.x clients delete Chat.log at start when chat logging is off at that moment, and the in-game option
/// turns it on only later, so the log of every earlier session is lost. Chat.log is kept; drop this hook to get the client's behaviour back.
static BOOL WINAPI zzDeleteFileA(LPCSTR path) {
    if (path && _stricmp(path, CHAT_LOG) == 0) {
        return TRUE;
    }
    return real_DeleteFileA(path);
}

/// Calls a function for each readable page range of the image; packers can leave some pages without access.
template <typename F> static BYTE* ScanImage(HMODULE module, size_t length, F matches) {
    BYTE* image = (BYTE*)module;
    BYTE* imageEnd = image + ModuleImageSize(module);
    MEMORY_BASIC_INFORMATION region;
    for (BYTE* begin = image; begin < imageEnd && VirtualQuery(begin, &region, sizeof(region)); begin = (BYTE*)region.BaseAddress + region.RegionSize) {
        bool readable = region.State == MEM_COMMIT && !(region.Protect & (PAGE_NOACCESS | PAGE_GUARD)) && (region.Protect & 0xEE);
        BYTE* end = min(imageEnd, (BYTE*)region.BaseAddress + region.RegionSize);
        for (BYTE* p = (BYTE*)region.BaseAddress; readable && p + length <= end; p++) {
            if (matches(p)) {
                return p;
            }
        }
    }
    return nullptr;
}

static BYTE* FindString(HMODULE module, const char* text, size_t length) {
    return ScanImage(module, length, [&](BYTE* p) { return *p == (BYTE)*text && memcmp(p, text, length) == 0; });
}

#if defined(_M_AMD64)

/// lea reg, [rip+text]
static BYTE* FindReference(HMODULE module, const void* text) {
    return ScanImage(module, 7, [&](BYTE* p) {
        return (p[0] == 0x48 || p[0] == 0x4C) && p[1] == 0x8D && (p[2] & 0xC7) == 0x05 && p + 7 + *(INT32*)(p + 3) == text;
    });
}

/// The function containing address, following chained unwind entries back to the primary one.
static BYTE* FunctionStart(BYTE* address) {
    DWORD64 imageBase;
    PRUNTIME_FUNCTION function = RtlLookupFunctionEntry((DWORD64)address, &imageBase, nullptr);
    for (int depth = 0; function && depth < 32; depth++) {
        BYTE* unwind = (BYTE*)imageBase + function->UnwindData;
        if (!((unwind[0] >> 3) & UNW_FLAG_CHAININFO)) {
            return (BYTE*)imageBase + function->BeginAddress;
        }
        function = (PRUNTIME_FUNCTION)(unwind + 4 + ((unwind[2] + 1) & ~1) * 2);
    }
    return nullptr;
}

/// mov rcx, [rip+system]; then the virtual calls: call [reg+offset]
static void ReadWriter(BYTE* writer, BYTE* end) {
    int calls = 0;
    for (BYTE* p = writer; p + 7 <= end && calls < 3; p++) {
        int mod = p[1] >> 6, reg = (p[1] >> 3) & 7, rm = p[1] & 7;
        if (!s_system && p[0] == 0x48 && p[1] == 0x8B && p[2] == 0x0D) {
            s_system = (void**)(p + 7 + *(INT32*)(p + 3));
        } else if (p[0] == 0xFF && reg == 2 && rm != 4 && (mod == 1 || mod == 2)) {
            s_calls[calls++] = mod == 1 ? p[2] : *(DWORD*)(p + 2);
        }
    }
}

#else

/// push offset text
static BYTE* FindReference(HMODULE module, const void* text) {
    return ScanImage(module, 5, [&](BYTE* p) { return p[0] == 0x68 && *(const void**)(p + 1) == text; });
}

/// push ebp; mov ebp, esp after the padding or the return of the function before
static BYTE* FunctionStart(BYTE* address) {
    for (BYTE* p = address; p > address - 0x400; p--) {
        if (p[0] == 0x55 && p[1] == 0x8B && p[2] == 0xEC && (p[-1] == 0xCC || p[-1] == 0x90 || p[-1] == 0xC3)) {
            return p;
        }
    }
    return nullptr;
}

/// mov ecx, [system]; then the virtual calls: mov reg, [reg2+offset]; ...; call reg
static void ReadWriter(BYTE* writer, BYTE* end) {
    int calls = 0;
    for (BYTE* p = writer; p + 6 <= end && calls < 3; p++) {
        int mod = p[1] >> 6, reg = (p[1] >> 3) & 7, rm = p[1] & 7;
        if (!s_system && p[0] == 0x8B && p[1] == 0x0D) {
            s_system = *(void***)(p + 2);
        } else if (p[0] == 0x8B && rm != 4 && (mod == 1 || mod == 2)) {
            BYTE* next = p + (mod == 1 ? 3 : 6);
            for (BYTE* q = next; q < next + 8 && q + 2 <= end; q++) {
                if (q[0] == 0xFF && q[1] == 0xD0 + reg) {
                    s_calls[calls++] = mod == 1 ? p[2] : *(DWORD*)(p + 2);
                    break;
                }
            }
        }
    }
}

#endif

/// The writer formats the line with the Chat.log format, and before it asks for the variable.
static void InstallWriter(HMODULE game) {
    s_cvarName = (const char*)FindString(game, "g_chatlog", 10);
    BYTE* format = FindString(game, LINE_FORMAT, sizeof(LINE_FORMAT));
    BYTE* reference = format ? FindReference(game, format) : nullptr;
    BYTE* writer = reference ? FunctionStart(reference) : nullptr;
    if (writer) {
        ReadWriter(writer, reference);
    }
    MODULE_LOG("chat log: writer=%p system=%p calls=%X,%X,%X", writer, s_system, s_calls[0], s_calls[1], s_calls[2]);
    if (!writer || !s_cvarName || !s_system || !s_calls[2]) {
        return;
    }
    real_ChatLogWrite = (ChatLogWrite_t)writer;
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&)real_ChatLogWrite, zzChatLogWrite);
    DetourTransactionCommit();
}

void InstallChatLogFix() {
    DetourAttach(&(PVOID&)real_DeleteFileA, zzDeleteFileA);
    OnModuleLoad(L"Game.dll", InstallWriter, true);
}
