#include <algorithm>
#include <list>
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

/// Allows connecting to unofficial game servers by spoofing the game server IP as an official one
void InstallIpFix() {
    DetourAttach(&(PVOID&)real_inet_ntoa, zzinet_ntoa);
    DetourAttach(&(PVOID&)real_lstrcpynA, zzlstrcpynA);
    DetourAttach(&(PVOID&)real_connect, zzconnect);
}