#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <algorithm>
#include <unordered_map>
#include <vector>
#include "code_patch.h"
#include "mods.h"
#include "mods_log.h"

#pragma comment(lib, "winmm.lib")

// [Debug] Profile=N in mods.ini: a sampling profiler for the start of the client. A thread of its own suspends every thread of the process
// once a millisecond and takes its instruction pointer, whether it runs or waits, so a thread's samples are its wall-clock time. When the
// client reaches the login screen (game state 2), or after N seconds, it writes for the busiest threads the modules and the functions that
// took the most samples to version.log, with the module and the RVA of their start (from the unwind data). Waiting shows as ntdll, win32u
// and the like. It works under wine as well as on Windows, where GetThreadTimes is too coarse to tell running threads apart.
//
// [Debug] ProfileWorld=M with ProfileWorldDelay=D: the same in the world, for the frame. It starts D seconds after the character enters
// the world (game state 14) and samples M seconds. It also walks each sample's stack with the unwind data of the loaded modules, read once
// before sampling, so that nothing takes a lock the suspended thread may hold, and reports the functions by the samples they are on the
// stack of (inclusive) and the callers of the hottest ones.
//
// [Debug] FrameStat=N: the frame time from Present to Present, written to version.log every N seconds: frames, the mean and the 50th,
// 90th and 99th percentiles and the longest frame, and how long the client waited inside Present (for the GPU or the frame limit), so that
// the rest is the time the client's own thread worked on the frame. It needs the overlay's hook of Present, which it turns on.

namespace {

struct ThreadState {
    HANDLE handle = nullptr;
    size_t samples = 0;
    std::unordered_map<DWORD64, size_t> byAddress;
    std::vector<std::pair<DWORD, DWORD64>> timeline; // ms since the start, address
    std::unordered_map<DWORD64, size_t> inclusive; // function start, samples it is on the stack of
    std::unordered_map<DWORD64, std::unordered_map<DWORD64, size_t>> callers; // function start, its caller's start, samples
};

bool s_world; // ProfileWorld: in the world, with stacks
DWORD s_worldDelay;

// The unwind data of the modules loaded when the world sampling starts, so that a stack is walked without RtlLookupFunctionEntry, which
// takes the lock of the dynamic function tables.
struct ModuleUnwind {
    DWORD64 base, end;
    const RUNTIME_FUNCTION* functions;
    size_t count;
};
std::vector<ModuleUnwind> s_modules;

void ReadModules() {
    HMODULE modules[1024];
    DWORD needed = 0;
    if (!EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &needed)) {
        return;
    }
    for (DWORD i = 0; i < needed / sizeof(HMODULE) && i < 1024; i++) {
        BYTE* base = (BYTE*)modules[i];
        auto dos = (IMAGE_DOS_HEADER*)base;
        auto nt = (IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
        const IMAGE_DATA_DIRECTORY& pdata = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
        if (pdata.VirtualAddress && pdata.Size) {
            s_modules.push_back({ (DWORD64)base, (DWORD64)base + nt->OptionalHeader.SizeOfImage,
                (const RUNTIME_FUNCTION*)(base + pdata.VirtualAddress), pdata.Size / sizeof(RUNTIME_FUNCTION) });
        }
    }
    std::sort(s_modules.begin(), s_modules.end(), [](const auto& a, const auto& b) { return a.base < b.base; });
}

const ModuleUnwind* FindModule(DWORD64 pc) {
    auto it = std::upper_bound(s_modules.begin(), s_modules.end(), pc, [](DWORD64 v, const ModuleUnwind& m) { return v < m.base; });
    if (it == s_modules.begin()) {
        return nullptr;
    }
    --it;
    return pc < it->end ? &*it : nullptr;
}

const RUNTIME_FUNCTION* FindFunction(const ModuleUnwind& module, DWORD64 pc) {
    DWORD rva = (DWORD)(pc - module.base);
    size_t lo = 0, hi = module.count;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (rva < module.functions[mid].BeginAddress) {
            hi = mid;
        } else if (rva >= module.functions[mid].EndAddress) {
            lo = mid + 1;
        } else {
            return &module.functions[mid];
        }
    }
    return nullptr;
}

/// The start of the function a primary or chained entry belongs to.
DWORD64 PrimaryStart(DWORD64 base, const RUNTIME_FUNCTION* function) {
    for (int depth = 0; function && depth < 32; depth++) {
        BYTE* unwind = (BYTE*)base + function->UnwindData;
        if (!((unwind[0] >> 3) & UNW_FLAG_CHAININFO)) {
            return base + function->BeginAddress;
        }
        BYTE codes = unwind[2];
        function = (const RUNTIME_FUNCTION*)(unwind + 4 + ((codes + 1) & ~1) * 2);
    }
    return base;
}

constexpr int MAX_FRAMES = 48;

/// Walks the stack of a suspended thread; the function starts, innermost first. Stops at code without unwind data that is not a leaf.
int WalkStack(CONTEXT context, DWORD64* starts) {
    int frames = 0;
    for (int i = 0; i < MAX_FRAMES; i++) {
        DWORD64 pc = context.Rip;
        const ModuleUnwind* module = FindModule(pc);
        if (!module) {
            break;
        }
        const RUNTIME_FUNCTION* function = FindFunction(*module, pc);
        starts[frames++] = function ? PrimaryStart(module->base, function) : pc;
        DWORD64 rsp = context.Rsp;
        if (function) {
            void* handlerData;
            DWORD64 establisher;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, module->base, pc, (PRUNTIME_FUNCTION)function, &context, &handlerData, &establisher, nullptr);
        } else if (i == 0) {
            context.Rip = *(DWORD64*)context.Rsp; // a leaf function
            context.Rsp += 8;
        } else {
            break;
        }
        if (context.Rsp <= rsp || context.Rsp - rsp > (1 << 20) || !context.Rip) {
            break;
        }
    }
    return frames;
}

std::wstring ModuleName(DWORD64 address) {
    HMODULE module = nullptr;
    wchar_t path[MAX_PATH] = L"?";
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)address, &module)) {
        GetModuleFileNameW(module, path, MAX_PATH);
    }
    const wchar_t* name = wcsrchr(path, L'\\');
    return name ? name + 1 : path;
}

size_t s_total;
DWORD s_seconds;

void Describe(DWORD64 address, char* text, size_t size) {
    HMODULE module = nullptr;
    wchar_t path[MAX_PATH];
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)address, &module)
        && GetModuleFileNameW(module, path, MAX_PATH)) {
        const wchar_t* name = wcsrchr(path, L'\\');
        sprintf_s(text, size, "%ls+%llX", name ? name + 1 : path, address - (DWORD64)module);
    } else {
        sprintf_s(text, size, "%016llX", address);
    }
}

void ReportThread(DWORD id, const ThreadState& thread) {
    // samples by function: the start the unwind data gives, or the address itself where there is none
    std::unordered_map<DWORD64, size_t> byFunction;
    std::unordered_map<std::wstring, size_t> byModule;
    size_t total = thread.samples;
    for (const auto& [address, count] : thread.byAddress) {
        BYTE* start = FunctionStart((void*)address);
        byFunction[start ? (DWORD64)start : address] += count;
        HMODULE module = nullptr;
        wchar_t path[MAX_PATH] = L"?";
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)address, &module)) {
            GetModuleFileNameW(module, path, MAX_PATH);
        }
        const wchar_t* name = wcsrchr(path, L'\\');
        byModule[name ? name + 1 : path] += count;
    }
    ModsLog("profile: thread %lu, %zu samples", id, total);
    std::vector<std::pair<std::wstring, size_t>> modules(byModule.begin(), byModule.end());
    std::sort(modules.begin(), modules.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    for (size_t i = 0; i < modules.size() && i < 15; i++) {
        ModsLog("profile: module %-24ls %6zu  %5.1f %%", modules[i].first.c_str(), modules[i].second, 100.0 * modules[i].second / max(total, (size_t)1));
    }
    std::vector<std::pair<DWORD64, size_t>> functions(byFunction.begin(), byFunction.end());
    std::sort(functions.begin(), functions.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    // where the time went, by half seconds: the three modules with the most samples in each
    for (DWORD from = 0; !thread.timeline.empty() && from <= thread.timeline.back().first; from += 500) {
        std::unordered_map<std::wstring, size_t> slice;
        size_t count = 0;
        for (const auto& [ms, address] : thread.timeline) {
            if (ms >= from && ms < from + 500) {
                slice[ModuleName(address)]++;
                count++;
            }
        }
        std::vector<std::pair<std::wstring, size_t>> top(slice.begin(), slice.end());
        std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        char line[256] = "";
        for (size_t i = 0; i < top.size() && i < 3; i++) {
            char part[80];
            sprintf_s(part, "%ls %zu%%  ", top[i].first.c_str(), top[i].second * 100 / max(count, (size_t)1));
            strcat_s(line, part);
        }
        ModsLog("profile: %5lu ms: %s", from, line);
    }
    for (size_t i = 0; i < functions.size() && i < 40; i++) {
        char where[MAX_PATH + 32];
        Describe(functions[i].first, where, sizeof(where));
        ModsLog("profile: %-40s %6zu  %5.1f %%", where, functions[i].second, 100.0 * functions[i].second / max(total, (size_t)1));
    }
    if (thread.inclusive.empty()) {
        return;
    }
    std::vector<std::pair<DWORD64, size_t>> inclusive(thread.inclusive.begin(), thread.inclusive.end());
    std::sort(inclusive.begin(), inclusive.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    ModsLog("profile: on the stack (inclusive), with the callers of each");
    for (size_t i = 0; i < inclusive.size() && i < 120; i++) {
        char where[MAX_PATH + 32];
        Describe(inclusive[i].first, where, sizeof(where));
        char line[512] = "";
        auto found = thread.callers.find(inclusive[i].first);
        if (found != thread.callers.end()) {
            std::vector<std::pair<DWORD64, size_t>> callers(found->second.begin(), found->second.end());
            std::sort(callers.begin(), callers.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
            for (size_t j = 0; j < callers.size() && j < 3; j++) {
                char caller[MAX_PATH + 32], part[MAX_PATH + 48];
                Describe(callers[j].first, caller, sizeof(caller));
                sprintf_s(part, " < %s %zu%%", caller, callers[j].second * 100 / max(inclusive[i].second, (size_t)1));
                strcat_s(line, part);
            }
        }
        ModsLog("profile: %-40s %6zu  %5.1f %%%s", where, inclusive[i].second, 100.0 * inclusive[i].second / max(total, (size_t)1), line);
    }
}

void Report(const char* why, std::unordered_map<DWORD, ThreadState>& threads) {
    ModsLog("profile: %s, %zu samples of 1 ms", why, s_total);
    // the threads that did not just wait the whole time: most samples outside the wait functions first
    std::vector<std::pair<DWORD, size_t>> busy;
    for (auto& [id, state] : threads) {
        size_t working = 0;
        for (const auto& [address, count] : state.byAddress) {
            HMODULE module = nullptr;
            wchar_t path[MAX_PATH] = L"";
            if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)address, &module)) {
                GetModuleFileNameW(module, path, MAX_PATH);
            }
            if (!wcsstr(path, L"ntdll") && !wcsstr(path, L"win32u") && !wcsstr(path, L"kernelbase") && !wcsstr(path, L"KERNELBASE")) {
                working += count;
            }
        }
        busy.push_back({ id, working });
    }
    std::sort(busy.begin(), busy.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    for (size_t i = 0; i < busy.size() && i < 4; i++) {
        if (busy[i].second * 20 >= threads[busy[0].first].samples) {
            ModsLog("profile: thread %lu works %zu of its samples", busy[i].first, busy[i].second);
            ReportThread(busy[i].first, threads[busy[i].first]);
        }
    }
}

DWORD WINAPI Sampler(LPVOID) {
    DWORD self = GetCurrentThreadId();
    DWORD process = GetCurrentProcessId();
    std::unordered_map<DWORD, ThreadState> threads;
    if (s_world) {
        while (!g_gameState || *g_gameState != 14) {
            Sleep(200);
        }
        ModsLog("profile: in the world, sampling in %lu s for %lu s", s_worldDelay, s_seconds);
        Sleep(s_worldDelay * 1000);
        ReadModules();
        ModsLog("profile: sampling with the unwind data of %zu modules", s_modules.size());
    }
    ULONGLONG start = GetTickCount64(), lastScan = 0;
    timeBeginPeriod(1);
    for (;;) {
        ULONGLONG now = GetTickCount64();
        if (now - lastScan >= 100) {
            lastScan = now;
            HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            THREADENTRY32 entry = { sizeof(entry) };
            for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry)) {
                if (entry.th32OwnerProcessID == process && entry.th32ThreadID != self && !threads.count(entry.th32ThreadID)) {
                    HANDLE handle = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, entry.th32ThreadID);
                    if (handle) {
                        threads[entry.th32ThreadID].handle = handle;
                    }
                }
            }
            CloseHandle(snapshot);
        }
        for (auto& [id, state] : threads) {
            if (SuspendThread(state.handle) == (DWORD)-1) {
                continue;
            }
            CONTEXT context = {};
            context.ContextFlags = s_world ? CONTEXT_CONTROL | CONTEXT_INTEGER : CONTEXT_CONTROL;
            BOOL ok = GetThreadContext(state.handle, &context);
            DWORD64 starts[MAX_FRAMES];
            int frames = ok && s_world ? WalkStack(context, starts) : 0;
            ResumeThread(state.handle); // before anything that may take a lock the suspended thread holds
            for (int i = 0; i < frames; i++) {
                bool seen = false; // recursion counts once
                for (int j = 0; j < i && !seen; j++) {
                    seen = starts[j] == starts[i];
                }
                if (!seen) {
                    state.inclusive[starts[i]]++;
                    if (i + 1 < frames) {
                        state.callers[starts[i]][starts[i + 1]]++;
                    }
                }
            }
            if (ok) {
                state.byAddress[context.Rip]++;
                if (!s_world) {
                    state.timeline.push_back({ (DWORD)(now - start), context.Rip });
                }
                s_total++;
                state.samples++;
            }
        }
        bool login = !s_world && g_gameState && (*g_gameState == 2 || *g_gameState == 3); // 5.x goes from 0 straight to 3
        if (login || now - start >= s_seconds * 1000ULL) {
            Report(login ? "login screen" : "time up", threads);
            break;
        }
        Sleep(1);
    }
    timeEndPeriod(1);
    for (auto& [id, state] : threads) {
        CloseHandle(state.handle);
    }
    return 0;
}

} // namespace

int g_frameStatSeconds;

namespace {

LARGE_INTEGER s_frequency, s_lastPresent, s_windowStart;
std::vector<float> s_frames, s_waits; // ms

} // namespace

void FrameStatBeforePresent(LARGE_INTEGER& before) {
    QueryPerformanceCounter(&before);
}

void FrameStatAfterPresent(const LARGE_INTEGER& before) {
    LARGE_INTEGER after;
    QueryPerformanceCounter(&after);
    if (!s_frequency.QuadPart) {
        QueryPerformanceFrequency(&s_frequency);
    }
    double toMs = 1000.0 / s_frequency.QuadPart;
    if (s_lastPresent.QuadPart) {
        s_frames.push_back((float)((after.QuadPart - s_lastPresent.QuadPart) * toMs));
        s_waits.push_back((float)((after.QuadPart - before.QuadPart) * toMs));
    } else {
        s_windowStart = after;
    }
    s_lastPresent = after;
    if ((after.QuadPart - s_windowStart.QuadPart) * toMs < g_frameStatSeconds * 1000.0 || s_frames.empty()) {
        return;
    }
    double sum = 0, waits = 0;
    for (size_t i = 0; i < s_frames.size(); i++) {
        sum += s_frames[i];
        waits += s_waits[i];
    }
    size_t n = s_frames.size();
    std::sort(s_frames.begin(), s_frames.end());
    int state = g_gameState ? *g_gameState : -1;
    ModsLog("frames: state %d, %zu frames, mean %.2f ms, p50 %.2f, p90 %.2f, p99 %.2f, max %.2f, in Present %.2f, client %.2f", state, n,
        sum / n, s_frames[n / 2], s_frames[n * 9 / 10], s_frames[n * 99 / 100], s_frames[n - 1], waits / n, (sum - waits) / n);
    s_frames.clear();
    s_waits.clear();
    s_windowStart = after;
}

void InstallProfiler() {
    ReadSetting({ L"Debug", L"FrameStat", Setting::Int, &g_frameStatSeconds, 0, 0, 600 });
    int seconds = 0, worldSeconds = 0, worldDelay = 30;
    ReadSetting({ L"Debug", L"Profile", Setting::Int, &seconds, 0, 0, 600 });
    ReadSetting({ L"Debug", L"ProfileWorld", Setting::Int, &worldSeconds, 0, 0, 600 });
    ReadSetting({ L"Debug", L"ProfileWorldDelay", Setting::Int, &worldDelay, 30, 0, 3600 });
    if (worldSeconds > 0) {
        s_world = true;
        s_worldDelay = worldDelay;
        s_seconds = worldSeconds;
        CloseHandle(CreateThread(nullptr, 0, Sampler, nullptr, 0, nullptr));
        ModsLog("profile: sampling for %d s from %d s after entering the world", worldSeconds, worldDelay);
        return;
    }
    if (seconds <= 0) {
        return;
    }
    s_seconds = seconds;
    CloseHandle(CreateThread(nullptr, 0, Sampler, nullptr, 0, nullptr));
    ModsLog("profile: sampling for up to %d s or until the login screen", seconds);
}
