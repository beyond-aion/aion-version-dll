#include "mods.h"
#include <unordered_map>
#include <unordered_set>

// The packer of the 4.6 clients turned each call through the import table of CrySystem into a call to a stub of its own, in the 6 bytes
// of the call: push reg; call stub, or call stub and a byte the stub steps over. A stub is some 25 instructions of the packer, a jump
// from one to the next across megabytes, which works out the address of the function and goes there; the code before the client window
// shows spends a third of its time in them. Each stub is run here once, an instruction at a time, until it reaches code outside the
// packer: when that is a function another module exports and the stack and registers are as the call left them, the 6 bytes call that
// function through a table of addresses again.

namespace {

struct Range {
    BYTE* from;
    BYTE* to;
};

std::vector<Range> s_packer;

/// What one thread needs to run stubs: the stack they run on and where the handler sends it back to.
struct Tracer {
    BYTE* stack;
    CONTEXT back;
    CONTEXT landed;
    volatile LONG phase;
    int steps;
};

thread_local Tracer* t_tracer = nullptr;

bool InPacker(const BYTE* p) {
    for (const Range& r : s_packer) {
        if (p >= r.from && p < r.to) {
            return true;
        }
    }
    return false;
}

LONG CALLBACK TraceHandler(EXCEPTION_POINTERS* e) {
    Tracer* t = t_tracer;
    if (!t || t->phase != 1) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    CONTEXT* c = e->ContextRecord;
    bool step = e->ExceptionRecord->ExceptionCode == EXCEPTION_SINGLE_STEP;
    if (step && InPacker((BYTE*)c->Rip) && ++t->steps < 200) {
        c->EFlags |= 0x100;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    t->landed = *c;
    t->landed.Rip = step ? c->Rip : 0;
    t->phase = 2;
    *c = t->back;
    c->EFlags &= ~0x100;
    return EXCEPTION_CONTINUE_EXECUTION;
}

DWORD64& Reg(CONTEXT& c, int i) {
    return (&c.Rax)[i];
}

DWORD64 Marker(int i) {
    return 0x5150000000000000ull + i * 0x10000;
}

/// Runs the stub as the call at site would, returns where it went, or nullptr when that is not where the original call went.
__declspec(noinline) BYTE* TraceStub(Tracer* t, BYTE* site, BYTE* stub, bool* pushed) {
    // rsp holds the return address, and the register pushed before the call, if it was
    DWORD64* stack = (DWORD64*)(t->stack + 0x8000);
    t->phase = 0;
    t->steps = 0;
    RtlCaptureContext(&t->back);
    if (t->phase == 0) {
        t->phase = 1;
        CONTEXT start = t->back;
        for (int i = 0; i < 16; i++) {
            Reg(start, i) = Marker(i);
        }
        stack[0] = (DWORD64)(site + 5);
        stack[1] = Marker(site[-1] & 7);
        start.Rsp = (DWORD64)stack;
        start.Rip = (DWORD64)stub;
        start.EFlags |= 0x100;
        RtlRestoreContext(&start, nullptr);
    }
    t->phase = 0;
    CONTEXT& landed = t->landed;
    if (!landed.Rip) {
        return nullptr;
    }
    if (landed.Rsp == (DWORD64)(stack + 1) && stack[1] == (DWORD64)(site + 5) && site[-1] >= 0x50 && site[-1] <= 0x57) {
        *pushed = true;
    } else if (landed.Rsp == (DWORD64)stack && stack[0] == (DWORD64)(site + 6)) {
        *pushed = false;
    } else {
        return nullptr;
    }
    for (int i = 0; i < 16; i++) {
        if (i != 4 && Reg(landed, i) != Marker(i)) {
            return nullptr;
        }
    }
    return (BYTE*)landed.Rip;
}

struct Site {
    BYTE* at;
    BYTE* stub;
    BYTE* target;
    bool pushed;
};

struct Work {
    Site* from;
    Site* to;
};

DWORD WINAPI TraceSites(void* param) {
    Work* work = (Work*)param;
    Tracer t = {};
    t.stack = (BYTE*)VirtualAlloc(nullptr, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!t.stack) {
        return 0;
    }
    t_tracer = &t;
    for (Site* s = work->from; s < work->to; s++) {
        s->target = TraceStub(&t, s->at, s->stub, &s->pushed);
    }
    t_tracer = nullptr;
    VirtualFree(t.stack, 0, MEM_RELEASE);
    return 0;
}

/// Whether target is the address of a function its module exports.
bool IsExport(BYTE* target) {
    static std::unordered_map<HMODULE, std::unordered_set<BYTE*>> exports;
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)target, &module)) {
        return false;
    }
    auto found = exports.find(module);
    if (found == exports.end()) {
        std::unordered_set<BYTE*>& set = exports[module];
        BYTE* base = (BYTE*)module;
        IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
        IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        if (dir.Size) {
            IMAGE_EXPORT_DIRECTORY* ed = (IMAGE_EXPORT_DIRECTORY*)(base + dir.VirtualAddress);
            DWORD* functions = (DWORD*)(base + ed->AddressOfFunctions);
            for (DWORD i = 0; i < ed->NumberOfFunctions; i++) {
                set.insert(base + functions[i]);
            }
        }
        found = exports.find(module);
    }
    return found->second.count(target) != 0;
}

/// A page for the table of addresses within reach of a 32-bit displacement from all of the module, above it or else below it.
BYTE* AllocateNear(BYTE* module, BYTE* moduleEnd, size_t size) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    DWORD64 granularity = si.dwAllocationGranularity;
    DWORD64 above = ((DWORD64)moduleEnd + granularity - 1) & ~(granularity - 1);
    for (DWORD64 at = above; at + size < (DWORD64)module + 0x7FF00000; at += granularity) {
        if (BYTE* p = (BYTE*)VirtualAlloc((void*)at, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)) {
            return p;
        }
    }
    for (DWORD64 at = ((DWORD64)module - size) & ~(granularity - 1); at > granularity && at + 0x7FF00000 > (DWORD64)moduleEnd; at -= granularity) {
        if (BYTE* p = (BYTE*)VirtualAlloc((void*)at, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)) {
            return p;
        }
    }
    return nullptr;
}

/// Finds the packer's own code: in the sections that can be both written and run, past the code, which is in the first one and still named
/// .text. The packer of the 4.8 and 5.x clients leaves no names and calls code of its own virtual machine instead, which must not be
/// stepped into.
bool FindPacker(BYTE* base) {
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    IMAGE_SECTION_HEADER* sections = IMAGE_FIRST_SECTION(nt);
    WORD count = nt->FileHeader.NumberOfSections;
    s_packer.clear();
    if (count < 2 || memcmp(sections[0].Name, ".text", 6) || !(sections[0].Characteristics & IMAGE_SCN_MEM_EXECUTE)) {
        return false;
    }
    for (WORD i = 1; i < count; i++) {
        DWORD flags = sections[i].Characteristics;
        if ((flags & IMAGE_SCN_MEM_EXECUTE) && (flags & IMAGE_SCN_MEM_WRITE)) {
            s_packer.push_back({ base + sections[i].VirtualAddress, base + sections[i].VirtualAddress + sections[i].Misc.VirtualSize });
        }
    }
    return !s_packer.empty();
}

}

bool HasImportStubs(HMODULE module) {
    return FindPacker((BYTE*)module);
}

void RestoreImportCalls(HMODULE module) {
    BYTE* base = (BYTE*)module;
    if (!FindPacker(base)) {
        return;
    }
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    IMAGE_SECTION_HEADER* sections = IMAGE_FIRST_SECTION(nt);
    BYTE* text = base + sections[0].VirtualAddress;
    BYTE* textEnd = text + sections[0].Misc.VirtualSize;
    std::vector<Site> sites;
    for (BYTE* p = text + 1; p + 6 <= textEnd; p++) {
        if (*p == 0xE8) {
            BYTE* target = p + 5 + *(INT32*)(p + 1);
            if (InPacker(target)) {
                sites.push_back({ p, target, nullptr, false });
            }
        }
    }
    if (sites.empty()) {
        return;
    }
    LARGE_INTEGER frequency, started, ended;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&started);
    BYTE* table = AllocateNear(base, base + nt->OptionalHeader.SizeOfImage, sites.size() * sizeof(void*));
    if (!table) {
        ModsLog("fast start: no memory for the import calls");
        return;
    }
    // a single step takes a trip through the kernel, so the stubs are shared out among the processors
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    size_t threads = min(max(si.dwNumberOfProcessors, 1ul), 16ul);
    std::vector<Work> work(threads);
    std::vector<HANDLE> handles;
    PVOID handler = AddVectoredExceptionHandler(1, TraceHandler);
    for (size_t i = 0; i < threads; i++) {
        work[i] = { sites.data() + sites.size() * i / threads, sites.data() + sites.size() * (i + 1) / threads };
        HANDLE h = i + 1 < threads ? CreateThread(nullptr, 0, TraceSites, &work[i], 0, nullptr) : nullptr;
        if (h) {
            handles.push_back(h);
        } else {
            TraceSites(&work[i]);
        }
    }
    WaitForMultipleObjects((DWORD)handles.size(), handles.data(), TRUE, INFINITE);
    for (HANDLE h : handles) {
        CloseHandle(h);
    }
    RemoveVectoredExceptionHandler(handler);
    struct Patch {
        BYTE* at;
        BYTE* slot;
    };
    std::vector<Patch> patches;
    std::unordered_map<BYTE*, BYTE*> slots;
    size_t used = 0;
    for (const Site& s : sites) {
        if (!s.target || InModule(module, s.target, 1) || !IsExport(s.target)) {
            continue;
        }
        BYTE*& slot = slots[s.target];
        if (!slot) {
            slot = table + used++ * sizeof(void*);
            *(BYTE**)slot = s.target;
        }
        patches.push_back({ s.pushed ? s.at - 1 : s.at, slot });
    }
    DWORD old;
    VirtualProtect(table, sites.size() * sizeof(void*), PAGE_READONLY, &old);
    VirtualProtect(text, textEnd - text, PAGE_EXECUTE_READWRITE, &old);
    for (const Patch& patch : patches) {
        // call qword ptr [rip + slot]
        patch.at[0] = 0xFF;
        patch.at[1] = 0x15;
        *(INT32*)(patch.at + 2) = (INT32)(patch.slot - (patch.at + 6));
    }
    VirtualProtect(text, textEnd - text, old, &old);
    FlushInstructionCache(GetCurrentProcess(), text, textEnd - text);
    QueryPerformanceCounter(&ended);
    ModsLog("fast start: %zu of %zu import calls restored, %zu functions, %.0f ms", patches.size(), sites.size(), used,
        (ended.QuadPart - started.QuadPart) * 1000.0 / frequency.QuadPart);
}
