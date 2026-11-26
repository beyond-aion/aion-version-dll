#include "mods.h"
#include <stdarg.h>
#include <detours/detours.h>
#include "install_report.h"
#include "mod_fast_start.h"

// Loading the game data spends about a third of its time in two functions of the runtime (VC 8 in 4.x, VC 12 in 5.x): the case-insensitive compare, with
// which the tables look up their fields by name, one field after another, and the wide printf, which builds field names such as
// effect%d_remain1 for every skill. Both look up the thread's locale for every character. The replacements below handle the plain
// cases themselves, with the same result, and leave everything else to the runtime.

typedef int(__cdecl* Wcsicmp_t)(const wchar_t*, const wchar_t*);
typedef int(__cdecl* Wcsnicmp_t)(const wchar_t*, const wchar_t*, size_t);
typedef int(__cdecl* Stricmp_t)(const char*, const char*);
typedef int(__cdecl* Vsnwprintf_t)(wchar_t*, size_t, size_t, const wchar_t*, va_list);
typedef int(__cdecl* Vswprintf_t)(wchar_t*, const wchar_t*, va_list);

static Wcsicmp_t real_wcsicmp = nullptr;
static Wcsnicmp_t real_wcsnicmp = nullptr;
static Stricmp_t real_stricmp = nullptr;
static Vsnwprintf_t real_vsnwprintf_s = nullptr;
static Vswprintf_t real_vswprintf = nullptr;

/// Whether 8 bytes from p can be read without leaving its page, past the end of the string as that may be.
static bool InPage(const void* p) {
    return ((ULONG_PTR)p & 0xFFF) <= 0xFF8;
}

/// Skips equal characters four at a time, as equal characters stay equal in every locale. Folds A-Z like the runtime does in every
/// locale but the Turkish one; at the first differing character outside ASCII the runtime compares the rest, which gives its own
/// result as the parts before are equal.
static int __cdecl zz_wcsicmp(const wchar_t* a, const wchar_t* b) {
    if (!a || !b) {
        return real_wcsicmp(a, b);
    }
    for (;;) {
        while (InPage(a) && InPage(b)) {
            unsigned __int64 x = *(const unsigned __int64*)a;
            if (x != *(const unsigned __int64*)b) {
                break;
            }
            // a zero character among the four ends both strings
            if ((x - 0x0001000100010001ull) & ~x & 0x8000800080008000ull) {
                return 0;
            }
            a += 4;
            b += 4;
        }
        unsigned c = *a, d = *b;
        if (c != d) {
            if (c >= 0x80 || d >= 0x80) {
                return real_wcsicmp(a, b);
            }
            if (c - 'A' < 26) {
                c += 32;
            }
            if (d - 'A' < 26) {
                d += 32;
            }
            if (c != d) {
                return (int)c - (int)d;
            }
        } else if (!c) {
            return 0;
        }
        a++;
        b++;
    }
}

/// The same for byte strings, which CrySystem compares while it opens the .pak archives.
static int __cdecl zz_stricmp(const char* a, const char* b) {
    if (!a || !b) {
        return real_stricmp(a, b);
    }
    for (;; a++, b++) {
        while (InPage(a) && InPage(b)) {
            unsigned __int64 x = *(const unsigned __int64*)a;
            if (x != *(const unsigned __int64*)b) {
                break;
            }
            // a zero byte among the eight ends both strings
            if ((x - 0x0101010101010101ull) & ~x & 0x8080808080808080ull) {
                return 0;
            }
            a += 8;
            b += 8;
        }
        unsigned c = (unsigned char)*a, d = (unsigned char)*b;
        if (c != d) {
            if (c >= 0x80 || d >= 0x80) {
                return real_stricmp(a, b);
            }
            if (c - 'A' < 26) {
                c += 32;
            }
            if (d - 'A' < 26) {
                d += 32;
            }
            if (c != d) {
                return (int)c - (int)d;
            }
        } else if (!c) {
            return 0;
        }
    }
}

/// The same for at most count characters, with which 4.6 finds the closing tag of every XML element it reads.
static int __cdecl zz_wcsnicmp(const wchar_t* a, const wchar_t* b, size_t count) {
    if (!a || !b) {
        return real_wcsnicmp(a, b, count);
    }
    for (; count; a++, b++, count--) {
        while (count >= 4 && InPage(a) && InPage(b)) {
            unsigned __int64 x = *(const unsigned __int64*)a;
            if (x != *(const unsigned __int64*)b) {
                break;
            }
            if ((x - 0x0001000100010001ull) & ~x & 0x8000800080008000ull) {
                return 0;
            }
            a += 4;
            b += 4;
            count -= 4;
        }
        if (!count) {
            return 0;
        }
        unsigned c = *a, d = *b;
        if (c != d) {
            if (c >= 0x80 || d >= 0x80) {
                return real_wcsnicmp(a, b, count);
            }
            if (c - 'A' < 26) {
                c += 32;
            }
            if (d - 'A' < 26) {
                d += 32;
            }
            if (c != d) {
                return (int)c - (int)d;
            }
        } else if (!c) {
            return 0;
        }
    }
    return 0;
}

/// Writes format into at most size - 1 characters and a terminating zero, handling only %d, %i, %u, %s and %% without flags, width or
/// precision; returns the length, -1 if it did not all fit, or -2 for anything else, having written part of it.
static int FormatPlain(wchar_t* buffer, size_t size, const wchar_t* format, va_list args) {
    va_list ap = args;
    size_t length = 0;
    bool truncated = false;
    auto put = [&](wchar_t c) {
        if (length + 1 < size) {
            buffer[length++] = c;
        } else {
            truncated = true;
        }
    };
    for (const wchar_t* f = format; *f; f++) {
        if (*f != L'%') {
            put(*f);
            continue;
        }
        wchar_t spec = *++f;
        if (spec == L'%') {
            put(L'%');
        } else if (spec == L'd' || spec == L'i' || spec == L'u') {
            int value = va_arg(ap, int);
            unsigned long long magnitude = spec == L'u' ? (unsigned)value : value < 0 ? 0ULL - (long long)value : (unsigned long long)value;
            if (spec != L'u' && value < 0) {
                put(L'-');
            }
            wchar_t digits[24];
            int n = 0;
            do {
                digits[n++] = (wchar_t)(L'0' + magnitude % 10);
                magnitude /= 10;
            } while (magnitude);
            while (n) {
                put(digits[--n]);
            }
        } else if (spec == L's') {
            const wchar_t* text = va_arg(ap, const wchar_t*);
            for (const wchar_t* t = text ? text : L"(null)"; *t; t++) {
                put(*t);
            }
        } else {
            return -2;
        }
    }
    buffer[length] = 0;
    return truncated ? -1 : (int)length;
}

/// Only with _TRUNCATE as the count, which writes what fits and returns -1 if it did not all fit. Anything else goes to the runtime,
/// which then reads the arguments from the start.
static int __cdecl zz_vsnwprintf_s(wchar_t* buffer, size_t size, size_t count, const wchar_t* format, va_list args) {
    if (count != _TRUNCATE || !buffer || !size || !format) {
        return real_vsnwprintf_s(buffer, size, count, format, args);
    }
    int length = FormatPlain(buffer, size, format, args);
    if (length == -2) {
        return real_vsnwprintf_s(buffer, size, count, format, args);
    }
    // the runtime clears the last element of the buffer as well
    buffer[size - 1] = 0;
    return length;
}

/// The legacy swprintf without a size, with which 4.6 builds the closing tag of every XML element it reads.
static int __cdecl zz_vswprintf(wchar_t* buffer, const wchar_t* format, va_list args) {
    int length = buffer && format ? FormatPlain(buffer, SIZE_MAX, format, args) : -2;
    return length == -2 ? real_vswprintf(buffer, format, args) : length;
}


/// Once CrySystem is loaded, which needs the runtime (VC 8 in 4.x, VC 12 in 5.x), before it opens the .pak archives.
void FastStartHookRuntime() {
    HMODULE crt = GetModuleHandleW(L"MSVCR80.dll");
    crt = crt ? crt : GetModuleHandleW(L"MSVCR120.dll");
    if (!crt || real_wcsicmp) {
        return;
    }
    real_wcsicmp = (Wcsicmp_t)GetProcAddress(crt, "_wcsicmp");
    real_vsnwprintf_s = (Vsnwprintf_t)GetProcAddress(crt, "_vsnwprintf_s");
    if (!real_wcsicmp || !real_vsnwprintf_s) {
        ModsLog("fast start: runtime functions not found");
        return;
    }
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    AttachHook(&(PVOID&)real_wcsicmp, zz_wcsicmp);
    AttachHook(&(PVOID&)real_vsnwprintf_s, zz_vsnwprintf_s);
    real_vswprintf = (Vswprintf_t)GetProcAddress(crt, "_vswprintf");
    if (real_vswprintf) {
        AttachHook(&(PVOID&)real_vswprintf, zz_vswprintf);
    }
    real_wcsnicmp = (Wcsnicmp_t)GetProcAddress(crt, "_wcsnicmp");
    if (real_wcsnicmp) {
        AttachHook(&(PVOID&)real_wcsnicmp, zz_wcsnicmp);
    }
    real_stricmp = (Stricmp_t)GetProcAddress(crt, "_stricmp");
    if (real_stricmp) {
        AttachHook(&(PVOID&)real_stricmp, zz_stricmp);
    }
    ModsLog("fast start: runtime string functions at %p: %ld", crt, CommitHooks());
}
