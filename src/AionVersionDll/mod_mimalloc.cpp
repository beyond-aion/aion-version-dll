#include "mods.h"
#include "module_load.h"
#include <detours/detours.h>
#include <mimalloc.h>

// The clients allocate through two allocators of CrySystem, both slow on a busy thread: the pool behind CryMalloc, whose locks run in
// code the packer virtualised, and the per-thread pool behind ISystem, which walks a whole free list whenever one of its pages empties.
// mimalloc takes over both for the whole session. Where the code of CrySystem is not packed (4.8) the hooks go in before its entry point,
// before it allocates anything; otherwise right after it, and a block either pool handed out before goes back to it.

typedef void*(__cdecl* CryMalloc_t)(size_t size);
typedef void(__cdecl* CryFree_t)(void* p);
typedef void*(__cdecl* CryRealloc_t)(void* p, size_t size);
typedef void*(__cdecl* CryReallocSize_t)(void* p, size_t oldSize, size_t size);
typedef void(__cdecl* CryFreeSize_t)(void* p, size_t size);
typedef void*(__cdecl* CryCalloc_t)(size_t count, size_t size);
/// The pool for an allocator object, which takes the blocks too large for the pool.
typedef void*(__fastcall* PoolAlloc_t)(void* owner, size_t size);
typedef void(__fastcall* PoolFree_t)(void* owner, void* p);
/// The per-thread pool keeps the size in the 4 bytes before each block.
typedef void*(__fastcall* ThreadAlloc_t)(size_t size);
typedef void(__fastcall* ThreadFree_t)(void* p);

static CryMalloc_t real_CryMalloc = nullptr;
static CryFree_t real_CryFree = nullptr;
static CryRealloc_t real_CryRealloc = nullptr;
static CryReallocSize_t real_CryReallocSize = nullptr;
static CryFreeSize_t real_CryFreeSize = nullptr;
static CryCalloc_t real_CryCalloc = nullptr;
static PoolAlloc_t real_PoolAlloc = nullptr;
static PoolFree_t real_PoolFree = nullptr;
static ThreadAlloc_t real_ThreadAlloc = nullptr;
static ThreadFree_t real_ThreadFree = nullptr;
/// The functions hooked before the entry point of CrySystem, which must still jump to the hooks after it.
static BYTE* s_hookedEarly[10];

static void* __cdecl zzCryMalloc(size_t size) {
    return mi_malloc(size);
}

static void __cdecl zzCryFree(void* p) {
    if (mi_is_in_heap_region(p)) {
        mi_free(p);
    } else {
        real_CryFree(p);
    }
}

static void* __cdecl zzCryRealloc(void* p, size_t size) {
    // the client's own realloc moves a block of its pool into a new one, which comes from mimalloc
    return !p || mi_is_in_heap_region(p) ? mi_realloc(p, size) : real_CryRealloc(p, size);
}

static void* __cdecl zzCryReallocSize(void* p, size_t oldSize, size_t size) {
    return !p || mi_is_in_heap_region(p) ? mi_realloc(p, size) : real_CryReallocSize(p, oldSize, size);
}

static void __cdecl zzCryFreeSize(void* p, size_t size) {
    if (mi_is_in_heap_region(p)) {
        mi_free(p);
    } else {
        real_CryFreeSize(p, size);
    }
}

static void* __cdecl zzCryCalloc(size_t count, size_t size) {
    return mi_calloc(count, size);
}

static void* __fastcall zzPoolAlloc(void*, size_t size) {
    return mi_malloc(size);
}

static void __fastcall zzPoolFree(void* owner, void* p) {
    if (mi_is_in_heap_region(p)) {
        mi_free(p);
    } else {
        real_PoolFree(owner, p);
    }
}

static void* __fastcall zzThreadAlloc(size_t size) {
    DWORD* block = (DWORD*)mi_malloc((size_t)(DWORD)size + 4);
    if (!block) {
        return nullptr;
    }
    *block = (DWORD)size;
    return block + 1;
}

static void __fastcall zzThreadFree(void* p) {
    if (p && mi_is_in_heap_region(p)) {
        mi_free((DWORD*)p - 1);
    } else {
        real_ThreadFree(p);
    }
}

/// The pool functions for an allocator object: mov r9, rcx; lea rcx, pool; jmp alloc, and test rdx, rdx; jz; mov r8, rcx; lea rcx, pool;
/// then call free in 4.x, with a frame of its own, or jmp free in 5.x. The per-thread pool: both start by reading its TLS index and
/// leaving when it is -1. Returns the pool.
static BYTE* FindAllocators(HMODULE crySystem, bool quiet) {
    BYTE* poolAlloc = FindPattern(crySystem, "4C 8B C9 48 8D 0D ?? ?? ?? ?? E9");
    BYTE* poolFree = FindPattern(crySystem, "48 83 EC 28 48 85 D2 74 ?? 4C 8B C1 48 8D 0D ?? ?? ?? ?? E8");
    BYTE* poolFreePool = poolFree ? ResolveRip(poolFree + 15, poolFree + 19) : nullptr;
    if (!poolFree && (poolFree = FindPattern(crySystem, "48 85 D2 74 ?? 4C 8B C1 48 8D 0D ?? ?? ?? ?? E9"))) {
        poolFreePool = ResolveRip(poolFree + 11, poolFree + 15);
    }
    BYTE* threadAlloc = FindPattern(crySystem, "40 53 48 83 EC 20 48 8B D9 8B 0D ?? ?? ?? ?? 83 F9 FF 75 ?? 33 C0");
    BYTE* threadFree = FindPattern(crySystem, "40 53 48 83 EC 20 48 8B D9 8B 0D ?? ?? ?? ?? 83 F9 FF 0F 84");
    threadFree = threadFree ? threadFree : FindPattern(crySystem, "40 53 48 83 EC 20 48 8B D9 8B 0D ?? ?? ?? ?? 83 F9 FF 74");
    BYTE* pool = poolAlloc ? ResolveRip(poolAlloc + 6, poolAlloc + 10) : nullptr;
    if (!pool || pool != poolFreePool || !threadAlloc || !threadFree ||
        ResolveRip(threadAlloc + 11, threadAlloc + 15) != ResolveRip(threadFree + 11, threadFree + 15)) {
        if (!quiet) {
            ModsLog("mimalloc: allocators not found (%p %p %p %p)", poolAlloc, poolFree, threadAlloc, threadFree);
        }
        return nullptr;
    }
    real_PoolAlloc = (PoolAlloc_t)poolAlloc;
    real_PoolFree = (PoolFree_t)poolFree;
    real_ThreadAlloc = (ThreadAlloc_t)threadAlloc;
    real_ThreadFree = (ThreadFree_t)threadFree;
    return pool;
}

static void HookAllocators(HMODULE crySystem, bool beforeEntry) {
    if (real_CryMalloc) {
        return;
    }
    real_CryMalloc = (CryMalloc_t)GetProcAddress(crySystem, "CryMalloc");
    real_CryFree = (CryFree_t)GetProcAddress(crySystem, "CryFree");
    real_CryRealloc = (CryRealloc_t)GetProcAddress(crySystem, "CryRealloc");
    real_CryReallocSize = (CryReallocSize_t)GetProcAddress(crySystem, "CryReallocSize");
    real_CryFreeSize = (CryFreeSize_t)GetProcAddress(crySystem, "CryFreeSize");
    real_CryCalloc = (CryCalloc_t)GetProcAddress(crySystem, "CryCalloc");
    BYTE* pool = real_CryMalloc && real_CryFree && real_CryRealloc && real_CryReallocSize && real_CryFreeSize && real_CryCalloc
        ? FindAllocators(crySystem, beforeEntry) : nullptr;
    if (!pool) {
        real_CryMalloc = nullptr;
        return;
    }
    if (beforeEntry) {
        BYTE* targets[] = { (BYTE*)real_CryMalloc, (BYTE*)real_CryFree, (BYTE*)real_CryRealloc, (BYTE*)real_CryReallocSize, (BYTE*)real_CryFreeSize,
            (BYTE*)real_CryCalloc, (BYTE*)real_PoolAlloc, (BYTE*)real_PoolFree, (BYTE*)real_ThreadAlloc, (BYTE*)real_ThreadFree };
        memcpy(s_hookedEarly, targets, sizeof(targets));
    }
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&)real_CryMalloc, zzCryMalloc);
    DetourAttach(&(PVOID&)real_CryFree, zzCryFree);
    DetourAttach(&(PVOID&)real_CryRealloc, zzCryRealloc);
    DetourAttach(&(PVOID&)real_CryReallocSize, zzCryReallocSize);
    DetourAttach(&(PVOID&)real_CryFreeSize, zzCryFreeSize);
    DetourAttach(&(PVOID&)real_CryCalloc, zzCryCalloc);
    DetourAttach(&(PVOID&)real_PoolAlloc, zzPoolAlloc);
    DetourAttach(&(PVOID&)real_PoolFree, zzPoolFree);
    DetourAttach(&(PVOID&)real_ThreadAlloc, zzThreadAlloc);
    DetourAttach(&(PVOID&)real_ThreadFree, zzThreadFree);
    LONG error = DetourTransactionCommit();
    // The pool sets up its own growable heap with 296 MB committed at once, when CrySystem starts and in CryFlushAll: lea rcx, pool;
    // xor r8d, r8d; mov edx, size; call init. It keeps only its own records there now, so it starts with 1 MB; hooked too late for that,
    // what it has not used of the heap goes back.
    int heaps = 0;
    if (error == NO_ERROR) {
        for (BYTE* init : FindAllPatterns(crySystem, "48 8D 0D ?? ?? ?? ?? 45 33 C0 BA ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8D 15")) {
            DWORD initial = 1 << 20;
            if (ResolveRip(init + 3, init + 7) == pool && PatchMemory(init + 11, &initial, sizeof(initial))) {
                heaps++;
            }
        }
        HANDLE heap = beforeEntry ? nullptr : *(HANDLE*)(pool + 8);
        if (heap) {
            HeapCompact(heap, 0);
        }
    }
    ModsLog("mimalloc: allocators at %p %s its entry point: %ld, %d pool heaps", crySystem, beforeEntry ? "before" : "after", error, heaps);
}

static void HookBeforeEntry(HMODULE crySystem) {
    HookAllocators(crySystem, true);
}

static void HookAfterEntry(HMODULE crySystem) {
    if (s_hookedEarly[0]) {
        int lost = 0;
        for (BYTE* target : s_hookedEarly) {
            lost += *target != 0xE9;
        }
        if (lost) {
            ModsLog("mimalloc: %d of the hooks were overwritten by the entry point", lost);
        }
        // the client loads CrySystem once more when it closes, after this one is gone
        s_hookedEarly[0] = nullptr;
        return;
    }
    HookAllocators(crySystem, false);
}

void InstallMimalloc() {
    OnModuleLoad(L"CrySystem.dll", HookBeforeEntry, false);
    OnModuleLoad(L"CrySystem.dll", HookAfterEntry, true);
}
