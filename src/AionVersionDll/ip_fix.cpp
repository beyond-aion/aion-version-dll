#include <algorithm>
#include <list>
#include <stdio.h>
#include <string.h>
#include <string>
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <winsock2.h>
#include "detours.h"

static constexpr char officialIp[] = "70.5.0.18";
static std::list<std::string> possibleConnectIps = {};

static bool contains(const std::list<std::string>& list, const char* element) {
    return !list.empty() && std::find(list.begin(), list.end(), element) != list.end();
}

static decltype(inet_ntoa)* real_inet_ntoa = inet_ntoa;
/// This hook gathers all game server IPs from the server list (unrelated calls can happen before the server list and also after login when connecting to the chat server, but that's fine)
static char* WINAPI zzinet_ntoa(_In_ struct in_addr in) {
    char* addr = real_inet_ntoa(in);
    if (!contains(possibleConnectIps, addr)) {
        possibleConnectIps.push_back(addr);
    }
    return addr;
}

static decltype(lstrcpynA)* real_lstrcpynA = lstrcpynA;
static LPSTR WINAPI zzlstrcpynA(_Out_writes_(iMaxLength) LPSTR lpString1, _In_ LPCSTR lpString2, _In_ int iMaxLength) {
    if (iMaxLength == 256 && contains(possibleConnectIps, lpString2)) {
        possibleConnectIps.clear();
        return real_lstrcpynA(lpString1, officialIp, iMaxLength);
    }
    return real_lstrcpynA(lpString1, lpString2, iMaxLength);
}

static decltype(connect)* real_connect = connect;
static int WSAAPI zzconnect(_In_ SOCKET s, _In_reads_bytes_(namelen) const struct sockaddr FAR* name, _In_ int namelen) {
    possibleConnectIps.clear();
    return real_connect(s, name, namelen);
}

static char s_commandLineIp[16] = "";
static char s_spoofedIp[16] = "";

/// The server IP given with -ip: on the command line, which 5.x clients compare against the official one.
static bool ReadCommandLineIp() {
    const char* arg = strstr(GetCommandLineA(), "-ip:");
    int a, b, c, d;
    return arg && sscanf_s(arg + 4, "%d.%d.%d.%d", &a, &b, &c, &d) == 4
        && (unsigned)snprintf(s_commandLineIp, sizeof(s_commandLineIp), "%d.%d.%d.%d", a, b, c, d) < sizeof(s_commandLineIp);
}

typedef char*(__cdecl* strtok_s_t)(char*, const char*, char**);
static strtok_s_t real_strtok_s = nullptr;
/// 5.x clients split the game server address with strtok_s instead of copying it with lstrcpynA
static char* __cdecl zzstrtok_s(char* str, const char* delimiter, char** context) {
    if (str && (contains(possibleConnectIps, str) || (*s_commandLineIp && !strcmp(str, s_commandLineIp))) && strlen(str) >= strlen(officialIp)) {
        possibleConnectIps.clear();
        strcpy_s(s_spoofedIp, str);
        strcpy_s(str, strlen(str) + 1, officialIp);
    }
    return real_strtok_s(str, delimiter, context);
}

static decltype(inet_addr)* real_inet_addr = inet_addr;
/// Turns the spoofed official IP back into the real server address when 5.x clients connect to it
static unsigned long WSAAPI zzinet_addr(_In_z_ const char* cp) {
    if (cp && !strcmp(cp, officialIp)) {
        const char* realIp = *s_spoofedIp ? s_spoofedIp : s_commandLineIp;
        if (*realIp) {
            return real_inet_addr(realIp);
        }
    }
    return real_inet_addr(cp);
}

/// Allows connecting to unofficial game servers by spoofing the game server IP as an official one
void InstallIpFix() {
    DetourAttach(&(PVOID&)real_inet_ntoa, zzinet_ntoa);
    DetourAttach(&(PVOID&)real_lstrcpynA, zzlstrcpynA);
    DetourAttach(&(PVOID&)real_connect, zzconnect);
    HMODULE msvcr120 = GetModuleHandleA("msvcr120.dll");
    if (msvcr120) {
        ReadCommandLineIp();
        real_strtok_s = (strtok_s_t)GetProcAddress(msvcr120, "strtok_s");
        if (real_strtok_s) {
            DetourAttach(&(PVOID&)real_strtok_s, zzstrtok_s);
            DetourAttach(&(PVOID&)real_inet_addr, zzinet_addr);
        }
    }
}