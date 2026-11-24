/* PS5: the Win32 calls host/posix/win32_posix.c leaves to the console (host/ps5/win32_ps5.h): memory, thread and fiber
   stacks, futexes, modules and the process's exit. Built into the PS5 executable only (ps5/build.py).

   Memory. The guest window [HOS_PS5_GUEST_LO, HOS_PS5_GUEST_HI) is reserved in 16 GiB pieces (the most the console was
   measured to grant at a hint: PS5_PayloadSDK platform/docs/PROBE.md, "Virtual space") and committed in 64 KiB units,
   one state byte each. A commit maps direct memory taken from blocks of 64 MiB or more (one allocation per block, not
   per unit: a run of units is one mapping), read-write for the CPU and the GPU: the Vulkan back end imports the game's
   pools from these pages (VK_EXT_external_memory_host), and RADV's import grants GPU access the same way, so a later
   protection change here must keep the GPU bits or the GPU faults on the pool. A committed unit is never committed
   again (that would be a protection change too) and never given back: discards zero in place (host/hos_main.c
   hos_posix_guest_discard). Anything VirtualAlloc reserves outside the window is the platform layer's ps5_vrange. */
#define _GNU_SOURCE
#include <windows.h>

#include <errno.h>
#include <pthread.h>
#include <pthread_np.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>
#include <sys/umtx.h>

#include <ps5platform/kernel.h>
#include <ps5platform/shm.h>
#include <ps5platform/heap.h>

#include "win32_ps5.h"

#undef dlopen
#undef dlsym
#undef dlclose

int sceSystemServiceLoadExec(const char *path, const char *const *argv);
int sceKernelUsleep(unsigned microseconds);
int32_t sceKernelMprotect(const void *address, size_t bytes, int protection);

#define UNIT 0x10000ULL                       /* commit granularity: Windows' allocation granularity */
#define PIECE (16ULL << 30)                   /* reservation step */
#define BLOCK (64ULL << 20)                   /* smallest direct-memory block taken for guest pages */
#define GPU_RW 0x30                           /* the kernel's GPU read and write protection bits (RADV's own mappings) */
#define DIRECT_TYPE 12                        /* write-back, CPU and GPU coherent: RADV's and ProsperoEden's guest type */
#define GUEST_UNITS ((HOS_PS5_GUEST_HI - HOS_PS5_GUEST_LO) / UNIT)
#define LAZY_COMMIT (64ULL << 20)

static DWORD set_error(DWORD e) { SetLastError(e); return e; }
int sceKernelDebugOutText(int channel, const char *text);
/* A line straight to klog: standard error goes through the platform layer's pipe and thread, which a crash outruns */
void hos_ps5_klog_now(const char *fmt, ...) {
    char line[320];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line - 2, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof line - 2) n = (int)sizeof line - 2;
    line[n] = '\n', line[n + 1] = 0;
    /* (2026-11-23, HOS_KLOG=0: host/hos_thread.c) quiet but for the crash, emergency and out-of-memory lines */
    extern volatile int hos_klog_quiet;
    if (hos_klog_quiet && !strstr(line, "unhandled") && !strstr(line, "emergency") && !strstr(line, "FATAL") &&
        !strstr(line, "out of memory") && !strstr(line, "crash") && !strstr(line, "ran out") &&
        !strstr(line, "gpu hang"))   /* (the GPU hang markers, RX_VK_CHECKPOINTS=1) */
        return;
    sceKernelDebugOutText(0, line);
}

/* ---- the guest window ---------------------------------------------------------------------------------------- */
enum { U_RESERVED = 0, U_COMMITTED = 1 };
static uint8_t *unit_state;                   /* GUEST_UNITS bytes, made with the reservation */
static uint8_t *unit_ahead;                   /* (HOS_PS5_ZERO_SCAN) 1: backed by hos_ps5_commit_now, not by a touch */
static int64_t *unit_phys;                    /* a committed unit's direct memory (for its decommit) */
static uint8_t *unit_pinned;                  /* 1: once given to hos_ps5_commit_now (a GPU import, a file read's buffer) */
static uint8_t *cold_watch;                   /* (the cold meter's, below: units made inaccessible, not touched since) */
static int64_t *free_units;                   /* direct memory of decommitted units, taken first by the next commits */
static uint32_t *swap_slot;                   /* (the cold store, below: a reserved unit's contents in the swap file, slot + 1) */
static int swap_restore(uint64_t u);          /* (window_lock held: the unit committed with its contents read back) */
static void swap_forget(uint64_t u);          /* (window_lock held: the game gave the unit back; its slot is free) */
static size_t free_count, free_cap;
static int window_reserved;
static pthread_mutex_t window_lock = PTHREAD_MUTEX_INITIALIZER;
static int64_t block_next, block_end;         /* direct memory of the current block not mapped yet */
static uint64_t committed_bytes;

static int in_window(uintptr_t lo, uintptr_t hi) { return lo >= HOS_PS5_GUEST_LO && hi <= HOS_PS5_GUEST_HI && lo < hi; }

static int reserve_window(void) {
    if (window_reserved) return 1;
    unit_state = calloc(GUEST_UNITS, 1);
    unit_phys = calloc(GUEST_UNITS, sizeof *unit_phys);
    unit_pinned = calloc(GUEST_UNITS, 1);
    if (!unit_state || !unit_phys) return 0;
    for (uintptr_t at = HOS_PS5_GUEST_LO; at < HOS_PS5_GUEST_HI; at += PIECE) {
        const int r = ps5_vrange_reserve_at((void *)at, PIECE);
        if (r) {
            fprintf(stderr, "[ps5] the guest window: %#llx+16 GiB not reserved (%#x)\n", (unsigned long long)at, (unsigned)r);
            for (uintptr_t back = HOS_PS5_GUEST_LO; back < at; back += PIECE) ps5_vrange_release((void *)back, PIECE);
            free(unit_state), unit_state = NULL;
            return 0;
        }
    }
    window_reserved = 1;
    fprintf(stderr, "[ps5] the guest window: %#llx..%#llx reserved\n", (unsigned long long)HOS_PS5_GUEST_LO,
            (unsigned long long)HOS_PS5_GUEST_HI);
    return 1;
}

/* n bytes of direct memory, contiguous in the current block or a new one of at least BLOCK; -1 when there is none */
static int64_t direct_take(uint64_t n, uint64_t *got) {
    if (block_next >= block_end) {
        uint64_t size = n > BLOCK ? (n + BLOCK - 1) & ~(BLOCK - 1) : BLOCK;
        if (size > (1ULL << 30)) size = 1ULL << 30;   /* (a large commit takes 1 GiB blocks, one after another) */
        int64_t at = 0;
        /* (fragmented: smaller, down to one unit. In Hyrule Castle Town 842 MiB were free in 203 pieces, the largest
           under a block: a commit of 37 MiB found "no direct memory left" and the game wrote to the page it did not get) */
        while (sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), size, UNIT, DIRECT_TYPE, &at)) {
            if (size <= UNIT) return -1;
            size = (size / 2 + UNIT - 1) & ~(UNIT - 1);
        }
        block_next = at, block_end = at + (int64_t)size;
    }
    const uint64_t left = (uint64_t)(block_end - block_next);
    *got = n < left ? n : left;
    const int64_t at = block_next;
    block_next += (int64_t)*got;
    return at;
}

static double now_ms(void);
void hos_ps5_mem_ran_out(void);
int hos_ps5_cold_touch(uintptr_t a);
/* Maps fresh zeroed memory over every reserved unit of [lo, hi); window_lock held */
static int commit_units(uintptr_t lo, uintptr_t hi) {
    const double commit_t0 = now_ms();
    uint64_t commit_bytes = 0;
    uint64_t u = (lo - HOS_PS5_GUEST_LO) / UNIT;
    const uint64_t end = (hi - HOS_PS5_GUEST_LO + UNIT - 1) / UNIT;
    while (u < end) {
        if (unit_state[u] == U_COMMITTED) { u++; continue; }
        if (swap_slot && swap_slot[u]) {   /* (a unit the cold store wrote out: its own memory, its contents back) */
            if (!swap_restore(u)) return 0;
            commit_bytes += UNIT;
            u++;
            continue;
        }
        uint64_t run = u;
        while (run < end && unit_state[run] != U_COMMITTED && !(swap_slot && swap_slot[run])) run++;
        uint64_t need = (run - u) * UNIT;
        while (need) {
            uint64_t got = 0;
            int64_t phys;
            if (free_count) {   /* (decommitted units' memory, again: as many as lie one after another, in one mapping; a run
                                   was pushed lowest first, so it comes off the top highest first) */
                uint64_t k = 1;
                while (k < need / UNIT && k < free_count && free_units[free_count - 1 - k] == free_units[free_count - k] - (int64_t)UNIT) k++;
                free_count -= k;
                phys = free_units[free_count], got = k * UNIT;
            } else phys = direct_take(need, &got);
            if (phys < 0) return 0;   /* (commit_units_wait says so) */
            void *want = (void *)(HOS_PS5_GUEST_LO + u * UNIT), *at = want;
            const int r = sceKernelMapDirectMemory(&at, got, PS5_KERNEL_PROT_CPU_READ | PS5_KERNEL_PROT_CPU_WRITE | GPU_RW,
                                                   PS5_KERNEL_MAP_FIXED, phys, UNIT);
            if (r || at != want) {
                hos_ps5_klog_now("[ps5] guest commit: mapping %#llx+%#llx failed (%#x)", (unsigned long long)(uintptr_t)want,
                                 (unsigned long long)got, (unsigned)r);
                return 0;
            }
            memset(want, 0, got);   /* (the kernel hands direct memory out as it was left: PROBE.md) */
            for (uint64_t k = 0; k < got / UNIT; k++) {
                unit_phys[u + k] = phys + (int64_t)(k * UNIT);
                __atomic_store_n(&unit_state[u + k], U_COMMITTED, __ATOMIC_RELEASE);
            }
            committed_bytes += got;
            commit_bytes += got;
            u += got / UNIT, need -= got;
        }
    }
    if (now_ms() - commit_t0 >= 20)
        hos_ps5_klog_now("[ps5] guest commit: %llu MiB backed in %.0f ms", (unsigned long long)(commit_bytes >> 20), now_ms() - commit_t0);
    return 1;
}

/* commit_units, waiting for memory when there is none: the game gives an area's pages back while it loads the next (leaving
   Hyrule Castle Town a worker's page found none with 85 MiB free, and the game was gone), the renderer's emptied blocks and
   the heap's free segments go back too. Tried again for up to 10 s before the caller is told no; window_lock held on
   entry and on return, not while waiting */
static int commit_units_wait(uintptr_t lo, uintptr_t hi) {
    for (int tries = 0;; tries++) {
        if (commit_units(lo, hi)) {
            if (tries) hos_ps5_klog_now("[ps5] guest commit: memory found after %d ms", tries * 20);
            return 1;
        }
        if (!tries || tries == 500)
            hos_ps5_klog_now("[ps5] guest commit: no direct memory left (%llu MiB committed)%s",
                             (unsigned long long)(committed_bytes >> 20), tries ? "" : ": waiting for some");
        if (tries == 500) return 0;
        pthread_mutex_unlock(&window_lock);
        if (!tries) hos_ps5_mem_ran_out();   /* (the memory guard: the parachute back at once, the renderer told) */
#ifdef PS5_HEAP_TRIM
        if (!tries) ps5_heap_trim();
#endif
        sceKernelUsleep(20000);
        pthread_mutex_lock(&window_lock);
    }
}

int hos_ps5_unit_backed(uintptr_t addr);

/* Guest memory the game gave back: each whole committed unit in [lo, hi) is unmapped and reserved again, its direct
   memory kept for the next commits; the parts of units only partly inside read as zero. Without it every scene the game
   loaded added memory it never returned: 7.1 GB of guest pages when Link's house loaded, and the next page found none.
   Pages a GPU import holds never come here (host/hos_mem.c keeps and zeroes those). */
int hos_ps5_decommit(void *addr, size_t size) {
    const uintptr_t lo = (uintptr_t)addr, hi = lo + size;
    if (!size || !window_reserved || !in_window(lo, hi)) return 0;
    const uintptr_t ulo = (lo + UNIT - 1) & ~(uintptr_t)(UNIT - 1), uhi = hi & ~(uintptr_t)(UNIT - 1);
    if (ulo >= uhi) {   /* (no whole unit) */
        if (hos_ps5_unit_backed(lo) && hos_ps5_unit_backed(hi - 1)) memset(addr, 0, size);
        return 1;
    }
    if (lo < ulo && hos_ps5_unit_backed(lo)) memset(addr, 0, ulo - lo);
    if (uhi < hi && hos_ps5_unit_backed(uhi)) memset((void *)uhi, 0, hi - uhi);
    /* (a run of committed units at a time, not a unit: two system calls a 64 KiB unit under the window's lock were 1.3 s of
       45 while a scene was up, and the game stood still for 1.8 s whenever it gave a large region back) */
    const double t0 = now_ms();
    uint64_t units = 0;
    pthread_mutex_lock(&window_lock);
    for (uintptr_t at = ulo; at < uhi;) {
        uint64_t u = (at - HOS_PS5_GUEST_LO) / UNIT;
        if (unit_state[u] != U_COMMITTED) {
            if (swap_slot && swap_slot[u]) swap_forget(u);   /* (written out, and now given back: it reads as zero again) */
            at += UNIT;
            continue;
        }
        uint64_t n = 1;
        while (at + n * UNIT < uhi && unit_state[u + n] == U_COMMITTED) n++;
        if (free_count + n > free_cap) {
            size_t cap = free_cap ? free_cap * 2 : 65536;
            while (cap < free_count + n) cap *= 2;
            int64_t *grown = realloc(free_units, cap * sizeof *grown);
            if (!grown) { memset((void *)at, 0, n * UNIT); at += n * UNIT; continue; }   /* (no room to keep them: zeroed and kept) */
            free_units = grown, free_cap = cap;
        }
        for (uint64_t k = 0; k < n; k++) __atomic_store_n(&unit_state[u + k], U_RESERVED, __ATOMIC_RELEASE);
        if (cold_watch) memset(&cold_watch[u], 0, n);   /* (the cold meter's samples there: gone with the memory) */
        void *place = (void *)at;
        if (sceKernelMunmap(place, n * UNIT) != 0 ||
            sceKernelReserveVirtualRange(&place, n * UNIT, PS5_KERNEL_MAP_FIXED, UNIT) != 0 || place != (void *)at) {
            hos_ps5_klog_now("[ps5] guest decommit: %#llx+%#llx could not be reserved again", (unsigned long long)at,
                             (unsigned long long)(n * UNIT));
            at += n * UNIT;
            continue;   /* (their memory is lost to the game; the units read as reserved and are backed afresh if touched) */
        }
        for (uint64_t k = 0; k < n; k++) free_units[free_count++] = unit_phys[u + k];
        committed_bytes -= n * UNIT;
        units += n;
        at += n * UNIT;
    }
    pthread_mutex_unlock(&window_lock);
    if (now_ms() - t0 >= 20) hos_ps5_klog_now("[ps5] guest decommit: %llu MiB given back in %.0f ms", (unsigned long long)(units * UNIT >> 20), now_ms() - t0);
    return 1;
}

static int prot_bits(DWORD protect) {
    switch (protect & 0xFF) {
    case PAGE_NOACCESS: return 0;
    case PAGE_READONLY: case PAGE_EXECUTE_READ: return PS5_KERNEL_PROT_CPU_READ;
    default: return PS5_KERNEL_PROT_CPU_READ | PS5_KERNEL_PROT_CPU_WRITE;
    }
}

/* ---- reservations outside the window: the platform layer's ranges, with their sizes for MEM_RELEASE ---- */
static struct { uintptr_t base; size_t size; } regions[256];
static pthread_mutex_t regions_lock = PTHREAD_MUTEX_INITIALIZER;

static void *reserve_elsewhere(void *addr, SIZE_T size) {
    void *base = NULL;
    const size_t n = (size + UNIT - 1) & ~(size_t)(UNIT - 1);
    if ((addr ? ps5_vrange_reserve_at(addr, n) : ps5_vrange_reserve(n, NULL, UNIT, &base)) != 0) return NULL;
    if (addr) base = addr;
    pthread_mutex_lock(&regions_lock);
    for (int i = 0; i < 256; i++)
        if (!regions[i].base) { regions[i].base = (uintptr_t)base, regions[i].size = n; break; }
    pthread_mutex_unlock(&regions_lock);
    return base;
}

void *VirtualAlloc(void *addr, SIZE_T size, DWORD type, DWORD protect) {
    const uintptr_t a = (uintptr_t)addr;
    if (type & MEM_RESET) {   /* (the contents are not wanted: they read as zero after this) */
        if (in_window(a, a + size)) hos_ps5_decommit(addr, size);
        return addr;
    }
    if (a == HOS_PS5_GUEST_LO && size == HOS_PS5_GUEST_HI - HOS_PS5_GUEST_LO && !(type & MEM_COMMIT)) {
        pthread_mutex_lock(&window_lock);
        const int ok = reserve_window();
        pthread_mutex_unlock(&window_lock);
        if (!ok) { set_error(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
        return addr;
    }
    if (addr && in_window(a, a + size)) {
        if (!(type & MEM_COMMIT)) return addr;   /* (reserved already) */
        /* A large commit (the heap's 6.9 GB at SetHeapSize, big address regions) stays reserved: its pages are backed as
           they are first touched (mkw_glue.c commit_on_touch), as Windows and Linux back untouched committed pages with
           nothing. Committed at once, the console ran out of direct memory with 9.2 GB of guest pages, most never used.
           What the kernel or the GPU must find backed is committed with hos_ps5_commit_now (ReadFile and WriteFile
           buffers, the renderer's imported pools). */
        if (size >= LAZY_COMMIT) return addr;
        pthread_mutex_lock(&window_lock);
        const int ok = window_reserved && commit_units_wait(a & ~(uintptr_t)(UNIT - 1), a + size);
        pthread_mutex_unlock(&window_lock);
        if (!ok) { set_error(ERROR_COMMITMENT_LIMIT); return NULL; }
        return addr;
    }
    void *base = addr;
    if (!addr || (type & MEM_RESERVE)) {
        base = reserve_elsewhere(addr, size);
        if (!base) { set_error(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
    }
    if (type & MEM_COMMIT) {
        const int p = (prot_bits(protect) & PS5_KERNEL_PROT_CPU_WRITE) ? PS5_SHM_READ | PS5_SHM_WRITE : PS5_SHM_READ;
        if (ps5_vrange_commit(base, size, p) != 0) { set_error(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
    }
    return base;
}

/* Whether a guest address's 64 KiB unit is backed (a signal handler's question: no lock) */
int hos_ps5_unit_backed(uintptr_t addr) {
    if (!window_reserved || !in_window(addr, addr + 1)) return 0;
    return __atomic_load_n(&unit_state[(addr - HOS_PS5_GUEST_LO) / UNIT], __ATOMIC_ACQUIRE) == U_COMMITTED;
}

/* (longshot #790, for the console) Whether a guest address's 64 KiB unit holds data: backed, or written out by the cold store.
   A unit neither (never touched, or given back by the zero reclaim) reads as zero, and reading it would back it with direct
   memory (commit_on_touch). -1: not in the window. No lock: a unit backed after this is one the game writes now, as racy as
   the read it saves */
int hos_ps5_unit_data(uintptr_t addr) {
    if (!window_reserved || !in_window(addr, addr + 1)) return -1;
    const uint64_t u = (addr - HOS_PS5_GUEST_LO) / UNIT;
    if (__atomic_load_n(&unit_state[u], __ATOMIC_ACQUIRE) == U_COMMITTED) return 1;
    return swap_slot && __atomic_load_n(&swap_slot[u], __ATOMIC_ACQUIRE) ? 1 : 0;
}

/* Guest pages backed now, whatever their size (the lazy rule above is for the runtime's own commits) */
int hos_ps5_commit_now(void *addr, size_t size) {
    const uintptr_t a = (uintptr_t)addr;
    if (!size || !in_window(a, a + size)) return 1;
    pthread_mutex_lock(&window_lock);
    if (window_reserved && unit_pinned)   /* (the zero reclaim leaves these alone for good: the GPU or the kernel writes them) */
        for (uint64_t u = (a - HOS_PS5_GUEST_LO) / UNIT; u < (a + size - HOS_PS5_GUEST_LO + UNIT - 1) / UNIT; u++) {
            unit_pinned[u] = 1;
            /* (the cold store writing it out (2): it finishes or gives up without the window's lock, which it needs) */
            while (cold_watch && __atomic_load_n(&cold_watch[u], __ATOMIC_ACQUIRE) == 2) {
                pthread_mutex_unlock(&window_lock);
                sceKernelUsleep(500);
                pthread_mutex_lock(&window_lock);
            }
            if (cold_watch && cold_watch[u]) hos_ps5_cold_touch(HOS_PS5_GUEST_LO + u * UNIT);   /* (its access back first) */
        }
    if (window_reserved && unit_ahead)   /* (HOS_PS5_ZERO_SCAN: the units backed here before the game touched them) */
        for (uint64_t u = (a - HOS_PS5_GUEST_LO) / UNIT; u < (a + size - HOS_PS5_GUEST_LO + UNIT - 1) / UNIT; u++)
            if (unit_state[u] != U_COMMITTED) unit_ahead[u] = 1;
    const int ok = window_reserved && commit_units_wait(a & ~(uintptr_t)(UNIT - 1), a + size);
    pthread_mutex_unlock(&window_lock);
    return ok;
}

BOOL VirtualFree(void *addr, SIZE_T size, DWORD type) {
    const uintptr_t a = (uintptr_t)addr;
    if (in_window(a, a + (size ? size : 1))) {
        if (type & MEM_DECOMMIT) hos_ps5_decommit(addr, size);
        return TRUE;
    }
    if (type & MEM_DECOMMIT) return ps5_vrange_decommit(addr, size) == 0;
    if (type & MEM_RELEASE) {
        pthread_mutex_lock(&regions_lock);
        for (int i = 0; i < 256; i++)
            if (regions[i].base == a) {
                ps5_vrange_release(addr, regions[i].size);
                regions[i].base = 0;
                pthread_mutex_unlock(&regions_lock);
                return TRUE;
            }
        pthread_mutex_unlock(&regions_lock);
    }
    set_error(ERROR_INVALID_PARAMETER);
    return FALSE;
}

/* Whole 16 KiB kernel pages; guest pages keep their GPU bits (see the top of the file) */
BOOL VirtualProtect(void *addr, SIZE_T size, DWORD protect, DWORD *old) {
    const uintptr_t lo = (uintptr_t)addr & ~(uintptr_t)0x3FFF, hi = ((uintptr_t)addr + size + 0x3FFF) & ~(uintptr_t)0x3FFF;
    if (old) *old = PAGE_READWRITE;
    const int gpu = in_window(lo, hi) ? GPU_RW : 0;
    return sceKernelMprotect((void *)lo, hi - lo, prot_bits(protect) | gpu) == 0 ? TRUE : (set_error(ERROR_INVALID_PARAMETER), FALSE);
}

SIZE_T VirtualQuery(const void *addr, MEMORY_BASIC_INFORMATION *mbi, SIZE_T size) {
    if (size < sizeof *mbi) return 0;
    memset(mbi, 0, sizeof *mbi);
    const uintptr_t a = (uintptr_t)addr;
    mbi->BaseAddress = (void *)(a & ~(uintptr_t)0xFFF);
    mbi->Type = 0x20000;
    if (window_reserved && in_window(a, a + 1)) {
        const uint64_t u = (a - HOS_PS5_GUEST_LO) / UNIT;
        const uint8_t s = __atomic_load_n(&unit_state[u], __ATOMIC_ACQUIRE);
        uint64_t end = u + 1;
        while (end < GUEST_UNITS && __atomic_load_n(&unit_state[end], __ATOMIC_RELAXED) == s) end++;
        mbi->AllocationBase = (void *)HOS_PS5_GUEST_LO;
        mbi->RegionSize = HOS_PS5_GUEST_LO + end * UNIT - (uintptr_t)mbi->BaseAddress;
        mbi->State = s == U_COMMITTED ? MEM_COMMIT : MEM_RESERVE;
        mbi->Protect = mbi->AllocationProtect = s == U_COMMITTED ? PAGE_READWRITE : PAGE_NOACCESS;
        return sizeof *mbi;
    }
    /* shortcut: outside the window every address reads as committed; ask the kernel (sceKernelQueryMemoryProtection)
       when a caller needs more */
    mbi->AllocationBase = mbi->BaseAddress;
    mbi->RegionSize = 0x1000;
    mbi->State = MEM_COMMIT, mbi->Protect = mbi->AllocationProtect = PAGE_READWRITE;
    return sizeof *mbi;
}

/* ---- stacks: direct memory with an unmapped guard page below (libkernel's come from flexible memory) ---------- */
static volatile int64_t stack_bytes, stack_count;   /* (the meter's: what the stacks have backed, and how many) */

/* Stacks grow as they are used. A stack reserves its whole size (and a guard unit below it) but backs only its top
   STACK_FIRST at first; a fault in the reserved part below what is backed (mc_err: page not present) backs down to the
   faulting unit and the access is retried (hos_ps5_stack_fault, from win32_posix.c's signal handler). The handler runs
   on a small alternate stack of the thread's own (hos_ps5_signal_stack): the faulting thread has no stack left to run
   it on. Each stack was 8 or 16 MiB backed whole: 1.7 GB for 96 threads that touch a fraction of it. */
#define STACK_FIRST (256u << 10)
#define STACK_SLOTS 1024
typedef struct { uintptr_t low, high, backed; } StackRange;   /* (usable [low, high), backed from `backed` up) */
static StackRange stacks[STACK_SLOTS];
static pthread_mutex_t stacks_lock = PTHREAD_MUTEX_INITIALIZER;

void *hos_ps5_stack_alloc(size_t *size) {
    const size_t n = (*size + UNIT - 1) & ~(size_t)(UNIT - 1);
    void *base = NULL;
    if (ps5_vrange_reserve(n + UNIT, NULL, UNIT, &base) != 0) return NULL;
    const uintptr_t low = (uintptr_t)base + UNIT, high = low + n;
    const size_t first = n < STACK_FIRST ? n : STACK_FIRST;
    if (ps5_vrange_commit((void *)(high - first), first, PS5_SHM_READ | PS5_SHM_WRITE) != 0) {
        ps5_vrange_release(base, n + UNIT);
        return NULL;
    }
    pthread_mutex_lock(&stacks_lock);
    int slot = -1;
    for (int i = 0; i < STACK_SLOTS && slot < 0; i++)
        if (!stacks[i].high) slot = i;
    if (slot >= 0) __atomic_store_n(&stacks[slot].low, low, __ATOMIC_RELAXED), stacks[slot].backed = high - first,
                   __atomic_store_n(&stacks[slot].high, high, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&stacks_lock);
    if (slot < 0) {   /* (no slot: backed whole, as before) */
        if (ps5_vrange_commit((void *)low, n, PS5_SHM_READ | PS5_SHM_WRITE) != 0) {
            ps5_vrange_release(base, n + UNIT);
            return NULL;
        }
        __atomic_add_fetch(&stack_bytes, (int64_t)(n - first), __ATOMIC_RELAXED);
    }
    *size = n;
    __atomic_add_fetch(&stack_bytes, (int64_t)first, __ATOMIC_RELAXED);
    __atomic_add_fetch(&stack_count, 1, __ATOMIC_RELAXED);
    return (void *)low;   /* (the unit below stays reserved: the guard) */
}
void hos_ps5_stack_free(void *stack, size_t size) {
    const uintptr_t low = (uintptr_t)stack;
    pthread_mutex_lock(&stacks_lock);
    for (int i = 0; i < STACK_SLOTS; i++)
        if (stacks[i].high && stacks[i].low == low) {
            __atomic_sub_fetch(&stack_bytes, (int64_t)(stacks[i].high - stacks[i].backed), __ATOMIC_RELAXED);
            stacks[i].high = 0;
            goto found;
        }
    __atomic_sub_fetch(&stack_bytes, (int64_t)size, __ATOMIC_RELAXED);
found:
    pthread_mutex_unlock(&stacks_lock);
    __atomic_sub_fetch(&stack_count, 1, __ATOMIC_RELAXED);
    ps5_vrange_release((char *)stack - UNIT, size + UNIT);
}
/* A page-not-present fault at addr: 1 when it is in a stack's reserved part, now backed down to it (retry the access) */
int hos_ps5_stack_fault(uintptr_t addr) {
    for (int i = 0; i < STACK_SLOTS; i++) {
        StackRange *r = &stacks[i];
        const uintptr_t high = __atomic_load_n(&r->high, __ATOMIC_ACQUIRE);
        if (!high || addr < r->low || addr >= high) continue;
        pthread_mutex_lock(&stacks_lock);
        int ok = 1;
        if (addr < r->backed) {
            const uintptr_t from = (addr & ~(uintptr_t)(UNIT - 1)) > r->low + UNIT ? (addr & ~(uintptr_t)(UNIT - 1)) - UNIT : r->low;
            ok = ps5_vrange_commit((void *)from, r->backed - from, PS5_SHM_READ | PS5_SHM_WRITE) == 0;
            if (ok) __atomic_add_fetch(&stack_bytes, (int64_t)(r->backed - from), __ATOMIC_RELAXED), r->backed = from;
        }
        pthread_mutex_unlock(&stacks_lock);
        return ok;
    }
    return 0;
}
/* The calling thread's alternate signal stack (64 KiB of direct memory), so a stack fault can be handled */
void hos_ps5_signal_stack(void) {
    stack_t cur;
    if (sigaltstack(NULL, &cur) == 0 && !(cur.ss_flags & SS_DISABLE)) return;
    void *base = NULL;
    if (ps5_vrange_reserve(64u << 10, NULL, UNIT, &base) != 0) return;
    if (ps5_vrange_commit(base, 64u << 10, PS5_SHM_READ | PS5_SHM_WRITE) != 0) return;
    stack_t ss = {.ss_sp = base, .ss_size = 64u << 10, .ss_flags = 0};
    sigaltstack(&ss, NULL);
}

/* Threads on those stacks. A Win32 thread here is detached; its stack can only go once it has ended, so it is made
   joinable and handed at its end to a reaper thread that joins it and frees the stack. Guest threads ask for 64 MiB,
   every byte of which the console commits: they get HOS_PS5_STACK_MB (16) at most. */
typedef struct Reap {
    struct Reap *next;
    pthread_t th;
    void *stack;
    size_t size;
    void *(*fn)(void *);
    void *arg;
} Reap;
static Reap *reap_list;
static pthread_mutex_t reap_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t reap_wake = PTHREAD_COND_INITIALIZER;
static pthread_once_t reaper_once = PTHREAD_ONCE_INIT;

static void *reaper_main(void *unused) {
    (void)unused;
    pthread_mutex_lock(&reap_lock);
    for (;;) {
        while (!reap_list) pthread_cond_wait(&reap_wake, &reap_lock);
        Reap *r = reap_list;
        reap_list = r->next;
        pthread_mutex_unlock(&reap_lock);
        pthread_join(r->th, NULL);
        hos_ps5_stack_free(r->stack, r->size);
        free(r);
        pthread_mutex_lock(&reap_lock);
    }
    return NULL;
}
static void reaper_start(void) {
    pthread_t t;
    if (pthread_create(&t, NULL, reaper_main, NULL) == 0) pthread_detach(t);
}

void hos_ps5_sample_register(void);
static void *thread_trampoline(void *p) {
    Reap *r = (Reap *)p;
    hos_ps5_signal_stack();
    hos_ps5_sample_register();
    void *result = r->fn(r->arg);
    pthread_mutex_lock(&reap_lock);   /* (joined once this function has returned: the stack is not freed under it) */
    r->next = reap_list, reap_list = r;
    pthread_cond_signal(&reap_wake);
    pthread_mutex_unlock(&reap_lock);
    return result;
}

static size_t stack_cap(void) {
    static size_t cap;
    if (!cap) {
        const char *e = getenv("HOS_PS5_STACK_MB");
        cap = (size_t)(e && atoi(e) > 0 ? atoi(e) : 16) << 20;
    }
    return cap;
}

int hos_ps5_thread_start(pthread_t *th, size_t stack, void *(*fn)(void *), void *arg) {
    pthread_once(&reaper_once, reaper_start);
    size_t size = stack < stack_cap() ? stack : stack_cap();
    Reap *r = (Reap *)calloc(1, sizeof *r);
    if (!r) return ENOMEM;
    r->stack = hos_ps5_stack_alloc(&size);
    if (!r->stack) { free(r); return ENOMEM; }
    r->size = size, r->fn = fn, r->arg = arg;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstack(&a, r->stack, size);
    const int e = pthread_create(&r->th, &a, thread_trampoline, r);
    pthread_attr_destroy(&a);
    if (e) {
        hos_ps5_stack_free(r->stack, size);
        free(r);
        return e;
    }
    *th = r->th;
    return 0;
}

/* ---- futexes and thread ids ---- */
long hos_ps5_futex(volatile uint32_t *word, int op, uint32_t value, const struct timespec *relative) {
    /* (the word read here first: a unit the cold meter made inaccessible gets its access back through the fault, where the
       kernel's own read of it would only answer EFAULT; and once more if it was taken again in between) */
    for (int tries = 0;; tries++) {
        (void)*word;
        long r;
        if (op == FUTEX_WAKE_PRIVATE) r = _umtx_op((void *)word, UMTX_OP_WAKE_PRIVATE, value, NULL, NULL);
        else {
            struct timespec t = relative ? *relative : (struct timespec){0};
            /* (a timespec in uaddr2 with its size in uaddr is a relative timeout; FreeBSD answers ETIMEDOUT as Linux does) */
            r = _umtx_op((void *)word, UMTX_OP_WAIT_UINT_PRIVATE, value, relative ? (void *)(uintptr_t)sizeof t : NULL,
                         relative ? &t : NULL);
        }
        if (r != -1 || errno != EFAULT || tries >= 3) return r;
    }
}
pid_t hos_ps5_tid(void) { return (pid_t)pthread_getthreadid_np(); }

/* ---- modules: the libraries the Linux build loads are linked in ---- */
#define MOD_VK ((void *)(uintptr_t)0x48530001)
#define MOD_VULKAN ((void *)(uintptr_t)0x48530002)
#define MOD_SELF ((void *)(uintptr_t)0x48530003)

void *mkwvk_init(const void *host);                                      /* (gpu/vk/vk_main.cpp) */
void mkwvk_close_asked(void);
void *vk_icdGetInstanceProcAddr(void *instance, const char *name);      /* (RADV, linked in) */
extern volatile uint8_t hos_posix_keys[256] __attribute__((weak));       /* (host/posix/display_sdl.c) */
extern volatile int hos_posix_window[4] __attribute__((weak));
int hos_posix_post_message(unsigned msg, uintptr_t wp, intptr_t lp) __attribute__((weak));
int hos_posix_guest_discard(void *p, size_t n) __attribute__((weak));   /* (host/hos_main.c) */
int hos_posix_guest_memfd(void) __attribute__((weak));

void *hos_ps5_dlopen(const char *path, int mode) {
    (void)mode;
    if (!path) return MOD_SELF;
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    if (!strncasecmp(base, "hookshot_vk.", 12) || !strncasecmp(base, "longshot_vk.", 12)) return MOD_VK;   /* (Longshot's name) */
    if (!strncasecmp(base, "libvulkan.so", 12) || !strcasecmp(base, "vulkan-1.dll")) return MOD_VULKAN;
    return NULL;   /* (SDL, X11, Wayland ...: not on the console; host/ps5/hos_sdl_ps5.c stands in for SDL) */
}

void *hos_ps5_dlsym(void *module, const char *name) {
    if (!name) return NULL;
    if (module == MOD_VK) {
        if (!strcmp(name, "mkwvk_init")) return (void *)mkwvk_init;
        if (!strcmp(name, "mkwvk_close_asked")) return (void *)mkwvk_close_asked;
        return NULL;
    }
    if (module == MOD_VULKAN) return !strcmp(name, "vkGetInstanceProcAddr") ? (void *)vk_icdGetInstanceProcAddr : NULL;
    /* (RTLD_DEFAULT, or the executable: what the Linux build finds across its .so boundary) */
    if (!strcmp(name, "hos_posix_keys")) return (void *)hos_posix_keys;
    if (!strcmp(name, "hos_posix_window")) return (void *)hos_posix_window;
    if (!strcmp(name, "hos_posix_post_message")) return (void *)hos_posix_post_message;
    if (!strcmp(name, "hos_posix_guest_discard")) return (void *)hos_posix_guest_discard;
    if (!strcmp(name, "hos_posix_guest_memfd")) return NULL;   /* (no dma-bufs: pools are host-pointer imports) */
    return NULL;
}

int hos_ps5_dlclose(void *module) { (void)module; return 0; }

uintptr_t hos_ps5_exe_base(void) {
    extern char __executable_start[] __attribute__((weak));
    return (uintptr_t)__executable_start;
}

/* ---- the end: the shell closes the title (exit() and _exit are a crash report) ---- */
void hos_ps5_exit(int code) {
    fprintf(stderr, "[ps5] exit %d: asking the shell to close the title\n", code);
    fflush(NULL);
    (void)sceSystemServiceLoadExec("exit", NULL);
    for (;;) sceKernelUsleep(100000);
}

/* ---- what host/posix/prof.c and the C library give the Linux build ---- */
/* shortcut: no sampling profiler on the console (prof.c reads Linux's perf events); HOS_PROFILE does nothing here */
void hos_prof_start(void) {}

/* timegm: the console's libc has none. UTC fields to seconds since 1970, days counted as civil-from-days does
   (Howard Hinnant's algorithm), any year, months normalised. shortcut: belongs in PS5_PayloadSDK's platform layer */
time_t timegm(struct tm *t) {
    long long y = t->tm_year + 1900LL, m = t->tm_mon;
    y += m / 12, m %= 12;
    if (m < 0) m += 12, y--;
    y -= m < 2;
    const long long era = (y >= 0 ? y : y - 399) / 400, yoe = y - era * 400;
    const long long doy = (153 * (m + (m > 1 ? -2 : 10)) + 2) / 5 + t->tm_mday - 1;
    const long long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const long long days = era * 146097 + doe - 719468;
    return (time_t)(days * 86400 + t->tm_hour * 3600LL + t->tm_min * 60LL + t->tm_sec);
}

/* ---- three calls only libraries a title does not load export (null at run time: ps5/tools/link.sh refuses them) ---- */
int isatty(int fd) { (void)fd; return 0; }   /* (no terminal on the console: standard error goes to klog) */
void hos_ps5_sample_name(pthread_t thread, const char *name);
int pthread_setname_np(pthread_t thread, const char *name) {
    pthread_set_name_np(thread, name);
    hos_ps5_sample_name(thread, name);
    return 0;
}
/* fnmatch for the Win32 FindFirstFile patterns win32_posix.c passes: '*' and '?', FNM_CASEFOLD (the only flag used) */
int fnmatch(const char *pattern, const char *s, int flags) {
    const int fold = (flags & 0x10) != 0;   /* (FNM_CASEFOLD) */
    const char *star = NULL, *resume = NULL;
    while (*s) {
        const char a = fold && *pattern >= 'A' && *pattern <= 'Z' ? (char)(*pattern + 32) : *pattern;
        const char b = fold && *s >= 'A' && *s <= 'Z' ? (char)(*s + 32) : *s;
        if (*pattern == '*') { star = ++pattern, resume = s; continue; }
        if (*pattern && (*pattern == '?' || a == b)) { pattern++, s++; continue; }
        if (!star) return 1;
        pattern = star, s = ++resume;
    }
    while (*pattern == '*') pattern++;
    return *pattern ? 1 : 0;
}

/* ---- a test run's clean stop: ps5/tools/run.py puts /app0/quit-run.txt in the title folder when its measurement is done;
   the title deletes it and leaves through the shell, as a player's quit would (never a kill: PS5_Vulkan's rule) ---- */
#include <unistd.h>
static void *quit_watch(void *unused) {
    (void)unused;
    for (;;) {
        sceKernelUsleep(500000);
        if (access("/app0/quit-run.txt", F_OK) == 0) {
            unlink("/app0/quit-run.txt");
            fprintf(stderr, "[ps5] quit-run.txt: the run is over\n");
            hos_ps5_exit(0);
        }
    }
    return NULL;
}
#ifdef PS5_HEAP_TRIM
/* The heap's free memory goes back to the title's pool every 10 s (the platform heap gives its pieces back only whole, and
   only when asked or every few thousand frees): what a burst of shader compiles took is the game's again soon after.
   Not more often: every 2 s, an area loading mapped and unmapped the same memory again and again (23 fps for 27). */
static void *heap_trim_loop(void *unused) {
    (void)unused;
    for (;;) {
        sceKernelUsleep(10000000);
        ps5_heap_trim();
    }
    return NULL;
}
#endif
void hos_ps5_quit_watch(void) {
    pthread_t t;
    if (pthread_create(&t, NULL, quit_watch, NULL) == 0) pthread_detach(t);
#ifdef PS5_HEAP_TRIM
    if (pthread_create(&t, NULL, heap_trim_loop, NULL) == 0) pthread_detach(t);
#endif
}

/* ---- HOS_PS5_TIMING=1: how long the waits the runtime builds on take on the console (each asked for the time named) ---- */
#include <sched.h>
static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}
/* The console's CPUs as the title sees them, one line at the start: which logical processors the title may run on, which
   share a core, and which share a level 3 cache (the pins below and GetLogicalProcessorInformation go by it). Read on each
   processor in turn (CPUID 0x8000001e: the core; 0x8000001d, cache 3: how many share it). */
#include <cpuid.h>

/* (base PS5) The console's model, by the direct memory the title is given: what PS5_Mesa's gpu-broker tells its clients
   (0ccdad5, "from the application's direct pool"). A base console gave this title 12,016 MiB, the Pro ~13.2 GB.
   HOS_PS5_MODEL=base or pro says it instead (a test). 1: base, 0: Pro. */
int hos_ps5_is_base(void) {
    static int known = -1;
    if (known >= 0) return known;
    const uint64_t mib = (uint64_t)sceKernelGetDirectMemorySize() >> 20;
    const char *m = getenv("HOS_PS5_MODEL");
    known = m && *m ? strcmp(m, "base") == 0 : mib < 12800;
    fprintf(stderr, "[ps5] console: %s (direct memory %llu MiB%s)\n", known ? "base PS5" : "PS5 Pro",
            (unsigned long long)mib, m && *m ? ", HOS_PS5_MODEL" : "");
    return known;
}
uint64_t hos_ps5_l3_mask[2];   /* (the two halves of the title's processors by level 3 cache; 0: not read) */
void hos_ps5_cpu_topology(void) {
    uint64_t was = 0;
    const int got = scePthreadGetaffinity(pthread_self(), &was);
    if (got != 0 || !was) {
        fprintf(stderr, "[ps5] cpus: the thread's affinity could not be read (%#x)\n", (unsigned)got);
        return;
    }
    char line[512];
    int n = 0, first_l3 = -1;
    for (int i = 0; i < 64; i++) {
        if (!(was >> i & 1)) continue;
        if (scePthreadSetaffinity(pthread_self(), 1ull << i) != 0) continue;
        sched_yield();
        unsigned a, b, c, d, share = 0;
        __cpuid_count(0x8000001e, 0, a, b, c, d);
        const unsigned apic = a, core = b & 0xff;
        __cpuid_count(0x8000001d, 3, a, b, c, d);
        if ((a & 0x1f) && ((a >> 5) & 7) == 3) share = ((a >> 14) & 0xfff) + 1;
        int l3 = (int)apic;
        for (unsigned s = share; s > 1; s >>= 1) l3 >>= 1;   /* (the processors of one cache: the APIC id less its low bits) */
        if (first_l3 < 0) first_l3 = l3;
        if (share) hos_ps5_l3_mask[l3 != first_l3] |= 1ull << i;
        if (n < (int)sizeof line - 32) n += snprintf(line + n, sizeof line - n, " %d:c%u/L%d", i, core, l3);
    }
    scePthreadSetaffinity(pthread_self(), was);
    fprintf(stderr, "[ps5] cpus: allowed %#llx; processor:core/L3%s; L3 halves %#llx %#llx\n", (unsigned long long)was, line,
            (unsigned long long)hos_ps5_l3_mask[0], (unsigned long long)hos_ps5_l3_mask[1]);
}

/* (HOS_PS5_TIMING=1) A file written in the title's folder, and what the other threads feel of it: one that only
   computes and one that opens another file there, while 24 MB are written, closed and renamed as the renderer saves its
   pipeline cache in play. Each step's time, and the longest pause each of the two threads saw. */
static volatile int file_probe_stop;
static double file_probe_gap[2];
static void *file_probe_cpu(void *unused) {
    (void)unused;
    double last = now_ms();
    while (!file_probe_stop) {
        const double t = now_ms();
        if (t - last > file_probe_gap[0]) file_probe_gap[0] = t - last;
        last = t;
    }
    return NULL;
}
static void *file_probe_open(void *unused) {
    (void)unused;
    while (!file_probe_stop) {
        const double t = now_ms();
        FILE *f = fopen("/app0/param.json", "rb");
        if (f) fclose(f);
        if (now_ms() - t > file_probe_gap[1]) file_probe_gap[1] = now_ms() - t;
        struct timespec ms = {0, 1000000};
        nanosleep(&ms, NULL);
    }
    return NULL;
}
/* QueryPerformanceCounter's clock (host/posix/windows.h): the time stamp counter's rate as the system gives it, or
   measured against CLOCK_MONOTONIC over 20 ms where it gives none; set before main, and at the first read otherwise. */
uint64_t hos_ps5_tsc0, hos_ps5_tsc_mult;
int64_t hos_ps5_ns0;
__attribute__((constructor)) void hos_ps5_clock_init(void) {
    uint64_t sceKernelGetTscFrequency(void);
    if (hos_ps5_tsc_mult) return;
    struct timespec t;
    uint64_t hz = sceKernelGetTscFrequency();
    if (hz < 100000000ull) {
        clock_gettime(CLOCK_MONOTONIC, &t);
        const int64_t a = (int64_t)t.tv_sec * 1000000000LL + t.tv_nsec;
        const uint64_t ta = __builtin_ia32_rdtsc();
        usleep(20000);
        clock_gettime(CLOCK_MONOTONIC, &t);
        hz = (uint64_t)((__builtin_ia32_rdtsc() - ta) * 1e9 / (double)((int64_t)t.tv_sec * 1000000000LL + t.tv_nsec - a));
    }
    clock_gettime(CLOCK_MONOTONIC, &t);
    hos_ps5_tsc0 = __builtin_ia32_rdtsc();
    hos_ps5_ns0 = (int64_t)t.tv_sec * 1000000000LL + t.tv_nsec;
    __atomic_store_n(&hos_ps5_tsc_mult, (uint64_t)(((unsigned __int128)1000000000ull << 32) / hz), __ATOMIC_RELEASE);
    fprintf(stderr, "[ps5] clock: the time stamp counter at %llu Hz\n", (unsigned long long)hz);
}

/* The game's package on the console: HOS_PS5_NUP, else the copy an emulator there already keeps, so one 25 GB file serves
   both: PS5_Lichii's folder, then the one it had before 2026-11-10 (when neither is there, the first, for the message). */
const char *hos_ps5_nup(void) {
    static const char *const places[] = {"/data/lichii/roms/oot/oot.nup", "/data/prosperoeden/roms/oot/oot.nup"};
    const char *set = getenv("HOS_PS5_NUP");
    if (set && *set) return set;
    for (size_t i = 0; i < sizeof places / sizeof *places; i++)
        if (access(places[i], R_OK) == 0) return places[i];
    return places[0];
}
static double file_probe_read_gap;
static void *file_probe_read(void *unused) {   /* (the game's own reads: 64 KiB pieces of its package, here and there) */
    (void)unused;
    const char *nup = hos_ps5_nup();
    FILE *f = fopen(nup, "rb");
    if (!f) return NULL;
    static char piece[65536];
    uint64_t at = 12345;
    while (!file_probe_stop) {
        at = at * 6364136223846793005ull + 1442695040888963407ull;
        const double t = now_ms();
        fseeko(f, (off_t)((at >> 20) % (20ull << 30)) & ~(off_t)0xffff, SEEK_SET);
        if (fread(piece, 1, sizeof piece, f) == 0) clearerr(f);
        if (now_ms() - t > file_probe_read_gap) file_probe_read_gap = now_ms() - t;
    }
    fclose(f);
    return NULL;
}
static void file_probe_at(const char *dir) {
    char path[256], tmp[256];
    snprintf(path, sizeof path, "%s/probe-file.bin", dir);
    snprintf(tmp, sizeof tmp, "%s/probe-file.bin.tmp", dir);
    const size_t n = 24u << 20;
    char *data = malloc(n);
    if (!data) return;
    memset(data, 0x5a, n);
    file_probe_stop = 0, file_probe_gap[0] = file_probe_gap[1] = 0;
    pthread_t a, b, c;
    file_probe_read_gap = 0;
    pthread_create(&a, NULL, file_probe_cpu, NULL);
    pthread_create(&b, NULL, file_probe_open, NULL);
    pthread_create(&c, NULL, file_probe_read, NULL);
    { struct timespec warm = {0, 300000000}; nanosleep(&warm, NULL); }
    const double idle_read = file_probe_read_gap;
    file_probe_read_gap = 0;
    const double t0 = now_ms();
    FILE *f = fopen(tmp, "wb");
    const double t1 = now_ms();
    if (f) fwrite(data, 1, n, f);
    const double t2 = now_ms();
    if (f) fclose(f);
    const double t3 = now_ms();
    rename(tmp, path);
    const double t4 = now_ms();
    unlink(path);
    const double t5 = now_ms();
    file_probe_stop = 1;
    pthread_join(a, NULL);
    pthread_join(b, NULL);
    pthread_join(c, NULL);
    free(data);
    fprintf(stderr, "[ps5] timing: a read of 64 KiB from the game's package: %.1f ms at most with nothing written, %.1f ms while "
            "the 24 MB went to %s\n", idle_read, file_probe_read_gap, dir);
    fprintf(stderr, "[ps5] timing: 24 MB to %s: open %.1f ms, write %.1f, close %.1f, rename %.1f, unlink %.1f; meanwhile a "
            "computing thread paused %.1f ms at most, one opening a file in /app0 %.1f ms\n", dir, t1 - t0, t2 - t1, t3 - t2,
            t4 - t3, t5 - t4, file_probe_gap[0], file_probe_gap[1]);
}
/* (HOS_PS5_TIMING=2) A file grown to 2 GiB in pieces of 24 MB, each piece timed: whether a write at the end of a large file
   costs more than one at the end of a small one (the program cache is one file of several GB, written at its end). */
static void file_probe_grow(void) {
    const size_t n = 24u << 20;
    char *data = malloc(n);
    if (!data) return;
    memset(data, 0x5a, n);
    FILE *f = fopen("/app0/probe-grow.bin", "wb");
    if (!f) { free(data); return; }
    setvbuf(f, NULL, _IONBF, 0);
    char line[1024];
    int at = 0;
    for (int i = 0; i < 86; i++) {
        const double t0 = now_ms();
        fwrite(data, 1, n, f);
        if (i % 6 == 0 && at < (int)sizeof line - 24) at += snprintf(line + at, sizeof line - at, " %d MB: %.0f ms;", i * 24, now_ms() - t0);
        if (i % 6 == 0) fprintf(stderr, "[ps5] timing: (growing) at %d MB a piece of 24 MB took %.0f ms\n", i * 24, now_ms() - t0);
    }
    const double t0 = now_ms();
    fclose(f);
    const double t1 = now_ms();
    unlink("/app0/probe-grow.bin");
    free(data);
    fprintf(stderr, "[ps5] timing: 24 MB written at the end of a file of%s close %.0f ms, unlink %.0f ms\n", line, t1 - t0, now_ms() - t1);
}
static void hos_ps5_file_probe(void) {
    if (getenv("HOS_PS5_TIMING") && *getenv("HOS_PS5_TIMING") == '2') file_probe_grow();
    file_probe_at("/app0");
    file_probe_at("/data/homebrew/PPSA93339");
    mkdir("/data/hookshot", 0777);
    file_probe_at("/data/hookshot");
}

void hos_ps5_timing_probe(void) {
    const char *e = getenv("HOS_PS5_TIMING");
    if (!e || (*e != '1' && *e != '2')) return;
    pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    pthread_condattr_t ca;
    pthread_cond_t cv;
    pthread_condattr_init(&ca);
    const int clock_set = pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    pthread_cond_init(&cv, &ca);
    struct timespec at;
    clock_gettime(CLOCK_MONOTONIC, &at);
    at.tv_nsec += 10000000;
    if (at.tv_nsec >= 1000000000) at.tv_sec++, at.tv_nsec -= 1000000000;
    pthread_mutex_lock(&mu);
    double t0 = now_ms();
    const int r = pthread_cond_timedwait(&cv, &mu, &at);
    const double cond_ms = now_ms() - t0;
    pthread_mutex_unlock(&mu);
    volatile uint32_t word = 0;
    struct timespec ten = {0, 10000000};
    t0 = now_ms();
    const long fr = hos_ps5_futex(&word, FUTEX_WAIT_PRIVATE, 0, &ten);
    const double futex_ms = now_ms() - t0;
    t0 = now_ms();
    for (int i = 0; i < 10; i++) Sleep(1);
    const double sleep_ms = (now_ms() - t0) / 10;
    struct timespec one = {0, 1000000};
    t0 = now_ms();
    for (int i = 0; i < 10; i++) nanosleep(&one, NULL);
    const double nano_ms = (now_ms() - t0) / 10;
    t0 = now_ms();
    for (int i = 0; i < 1000; i++) sched_yield();
    const double yield_us = (now_ms() - t0);
    {   /* (the log: hookshot_log.txt is unbuffered, a write a line) */
        FILE *f = fopen("/app0/probe-write.txt", "w");
        if (f) {
            setvbuf(f, NULL, _IONBF, 0);
            double worst = 0;
            const double w0 = now_ms();
            for (int i = 0; i < 200; i++) {
                const double s0 = now_ms();
                fputs("[hos probe] a line of about the length the runtime logs, eighty characters or so....\n", f);
                fflush(f);
                if (now_ms() - s0 > worst) worst = now_ms() - s0;
            }
            fprintf(stderr, "[ps5] timing: 200 unbuffered log lines to /app0 took %.1f ms (longest %.2f ms)\n", now_ms() - w0, worst);
            fclose(f);
            unlink("/app0/probe-write.txt");
        }
    }
    {   /* (the VSync thread's sleep: host/hos_thread.c sleep_ns, a waitable timer, 60 times 16.7 ms) */
        HANDLE timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        double worst = 0;
        const double v0 = now_ms();
        for (int i = 0; i < 60; i++) {
            LARGE_INTEGER due;
            due.QuadPart = -166666;
            const double s0 = now_ms();
            SetWaitableTimerEx(timer, &due, 0, NULL, NULL, NULL, 0);
            WaitForSingleObject(timer, INFINITE);
            if (now_ms() - s0 > worst) worst = now_ms() - s0;
        }
        fprintf(stderr, "[ps5] timing: 60 waitable-timer sleeps of 16.7 ms took %.1f ms (longest %.1f ms)\n", now_ms() - v0, worst);
    }
    hos_ps5_file_probe();
    fprintf(stderr, "[ps5] timing: cond 10 ms (monotonic clock set %d) took %.2f ms (result %d); futex 10 ms took %.2f ms "
            "(result %ld); Sleep(1) %.2f ms; nanosleep 1 ms %.2f ms; sched_yield %.2f us\n", clock_set, cond_ms, r, futex_ms,
            fr, sleep_ms, nano_ms, yield_us);
}


/* ---- HOS_PS5_SAMPLE=<start>,<seconds>: a sampling profiler. From <start> seconds after launch, for <seconds>, every
   thread started through CreateThread is interrupted (SIGUSR1) every 2 ms in turn, and the chain of calls it was in is
   counted: the instruction, then up to 10 return addresses found through the frame pointers (everything here is built
   with them; a sample in a system library starts from the word at its stack pointer, as its system-call wrappers are
   leaf functions). /app0/ps5_samples.txt gets one line per thread and chain: "<count> <thread> <eboot offset>..."
   (ps5/tools/samples.py names them). No allocation and no thread-local storage in the handler. ---- */
#include <signal.h>
#include <ucontext.h>
#include <ps5platform/context.h>   /* (the console's machine context: mc_rip where the fork's header puts it) */
#define SAMPLE_THREADS 256
#define SAMPLE_SLOTS 2048
#define SAMPLE_DEPTH 11
typedef struct { uint64_t hash; uint32_t count; uint8_t n; uintptr_t at[SAMPLE_DEPTH]; } SampleChain;
typedef struct { pthread_t th; char name[32]; volatile int used; SampleChain chain[SAMPLE_SLOTS]; uint32_t other; } SampleThread;
static SampleThread *sample_threads;
static volatile int sample_n;
static volatile int sample_on;

void hos_ps5_sample_register(void) {
    static _Thread_local int registered;   /* (once a thread: the renderer's own threads come by SetThreadPriority) */
    if (!sample_threads || registered) return;
    registered = 1;
    const int i = __atomic_fetch_add(&sample_n, 1, __ATOMIC_ACQ_REL);
    if (i >= SAMPLE_THREADS) return;
    sample_threads[i].th = pthread_self();
    snprintf(sample_threads[i].name, sizeof sample_threads[i].name, "thread%d", i);
    __atomic_store_n(&sample_threads[i].used, 1, __ATOMIC_RELEASE);
}
void hos_ps5_sample_name(pthread_t thread, const char *name) {
    if (!sample_threads) return;
    for (int i = 0; i < sample_n && i < SAMPLE_THREADS; i++)
        if (sample_threads[i].used && pthread_equal(sample_threads[i].th, thread)) {
            snprintf(sample_threads[i].name, sizeof sample_threads[i].name, "%s", name);
            for (char *c = sample_threads[i].name; *c; c++) if (*c == ' ') *c = '_';
        }
}
static void sample_handler(int sig, siginfo_t *si, void *uctx) {
    (void)sig, (void)si;
    if (!sample_on) return;
    const pthread_t me = pthread_self();
    const ucontext_t *uc = (const ucontext_t *)uctx;
    const uintptr_t base = hos_ps5_exe_base();
    uintptr_t at[SAMPLE_DEPTH];
    int n = 0;
    uintptr_t rip = (uintptr_t)uc->uc_mcontext.mc_rip - base;
    at[n++] = rip;
    const uintptr_t lo = (uintptr_t)uc->uc_mcontext.mc_rsp, hi = lo + (16u << 20);
    if (rip >= 0x7f0000000ull) at[n++] = *(const uintptr_t *)lo - base;   /* (a system-call wrapper's caller) */
    uintptr_t fp = (uintptr_t)uc->uc_mcontext.mc_rbp;
    while (n < SAMPLE_DEPTH && fp >= lo && fp < hi && !(fp & 7)) {
        at[n++] = ((const uintptr_t *)fp)[1] - base;
        const uintptr_t next = ((const uintptr_t *)fp)[0];
        if (next <= fp) break;
        fp = next;
    }
    uint64_t h = 1469598103934665603ull;
    for (int k = 0; k < n; k++) h = (h ^ at[k]) * 1099511628211ull;
    for (int i = 0; i < sample_n && i < SAMPLE_THREADS; i++) {
        SampleThread *t = &sample_threads[i];
        if (!t->used || !pthread_equal(t->th, me)) continue;
        uint32_t slot = (uint32_t)(h >> 40) & (SAMPLE_SLOTS - 1);
        for (int probe = 0; probe < 64; probe++, slot = (slot + 1) & (SAMPLE_SLOTS - 1)) {
            SampleChain *c = &t->chain[slot];
            if (c->count && c->hash != h) continue;
            if (!c->count) {
                c->hash = h, c->n = (uint8_t)n;
                for (int k = 0; k < n; k++) c->at[k] = at[k];
            }
            c->count++;
            return;
        }
        t->other++;
        return;
    }
}
static void *sampler_main(void *arg) {
    const int start = (int)(intptr_t)arg >> 16, seconds = (int)(intptr_t)arg & 0xffff;
    sceKernelUsleep((unsigned)start * 1000000u);
    sample_on = 1;
    fprintf(stderr, "[ps5] sampling %d threads for %d s\n", sample_n, seconds);
    const double end = now_ms() + seconds * 1000.0;
    while (now_ms() < end) {
        for (int i = 0; i < sample_n && i < SAMPLE_THREADS; i++)
            if (sample_threads[i].used) pthread_kill(sample_threads[i].th, SIGUSR1);
        sceKernelUsleep(2000);
    }
    sample_on = 0;
    sceKernelUsleep(50000);
    FILE *f = fopen("/app0/ps5_samples.txt", "w");
    if (!f) return NULL;
    for (int i = 0; i < sample_n && i < SAMPLE_THREADS; i++) {
        SampleThread *t = &sample_threads[i];
        for (int k = 0; k < SAMPLE_SLOTS; k++) {
            const SampleChain *c = &t->chain[k];
            if (!c->count) continue;
            fprintf(f, "%u %s", c->count, t->name);
            for (int d = 0; d < c->n; d++) fprintf(f, " 0x%lx", (unsigned long)c->at[d]);
            fputc('\n', f);
        }
        if (t->other) fprintf(f, "%u %s other\n", t->other, t->name);
    }
    fclose(f);
    fprintf(stderr, "[ps5] samples written to /app0/ps5_samples.txt\n");
    return NULL;
}
void hos_ps5_sample_start(void) {
    const char *e = getenv("HOS_PS5_SAMPLE");
    int start = 0, seconds = 0;
    if (!e || sscanf(e, "%d,%d", &start, &seconds) != 2 || seconds <= 0) return;
    sample_threads = calloc(SAMPLE_THREADS, sizeof *sample_threads);
    if (!sample_threads) return;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = sample_handler;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR1, &sa, NULL);
    pthread_t t;
    if (pthread_create(&t, NULL, sampler_main, (void *)(intptr_t)(start << 16 | (seconds & 0xffff))) == 0) pthread_detach(t);
}

/* ---- SRW locks (host/posix/windows.h): one word, zero when free. Bit 31 a writer holds it, bit 30 a thread sleeps on
   it, bits 0-29 the readers holding it. A thread that cannot take it spins a little, then sets the sleeper bit and waits
   on the word; a release that finds the bit clears it and wakes every sleeper, which try again. Readers do not come in
   while a sleeper is marked, so a waiting writer is not starved by a stream of readers. ---- */
#define SRW_W 0x80000000u
#define SRW_S 0x40000000u
#define SRW_R 0x3fffffffu
#define SRW_SPINS 100
static void srw_sleep(volatile uint32_t *w, uint32_t seen) {
    if (!(seen & SRW_S) && !__atomic_compare_exchange_n(w, &seen, seen | SRW_S, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) return;
    hos_ps5_futex(w, FUTEX_WAIT_PRIVATE, seen | SRW_S, NULL);
}
static void srw_wake(volatile uint32_t *w) {
    __atomic_fetch_and(w, ~SRW_S, __ATOMIC_RELAXED);
    hos_ps5_futex(w, FUTEX_WAKE_PRIVATE, 0x7fffffff, NULL);
}
int hos_srw_try_lock(SRWLOCK *l) {
    uint32_t s = __atomic_load_n(&l->w, __ATOMIC_RELAXED);
    while (!(s & (SRW_W | SRW_R)))
        if (__atomic_compare_exchange_n(&l->w, &s, s | SRW_W, 1, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return 1;
    return 0;
}
void hos_srw_lock(SRWLOCK *l) {
    for (int spin = 0;; spin++) {
        uint32_t s = __atomic_load_n(&l->w, __ATOMIC_RELAXED);
        if (!(s & (SRW_W | SRW_R))) {
            if (__atomic_compare_exchange_n(&l->w, &s, s | SRW_W, 1, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return;
            continue;
        }
        if (spin < SRW_SPINS) __builtin_ia32_pause();
        else srw_sleep(&l->w, s);
    }
}
void hos_srw_unlock(SRWLOCK *l) {
    if (__atomic_fetch_and(&l->w, ~SRW_W, __ATOMIC_RELEASE) & SRW_S) srw_wake(&l->w);
}
int hos_srw_try_lock_shared(SRWLOCK *l) {
    uint32_t s = __atomic_load_n(&l->w, __ATOMIC_RELAXED);
    while (!(s & (SRW_W | SRW_S)))
        if (__atomic_compare_exchange_n(&l->w, &s, s + 1, 1, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return 1;
    return 0;
}
void hos_srw_lock_shared(SRWLOCK *l) {
    for (int spin = 0;; spin++) {
        uint32_t s = __atomic_load_n(&l->w, __ATOMIC_RELAXED);
        if (!(s & (SRW_W | SRW_S))) {
            if (__atomic_compare_exchange_n(&l->w, &s, s + 1, 1, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return;
            continue;
        }
        if (spin < SRW_SPINS) __builtin_ia32_pause();
        else srw_sleep(&l->w, s);
    }
}
void hos_srw_unlock_shared(SRWLOCK *l) {
    const uint32_t s = __atomic_sub_fetch(&l->w, 1, __ATOMIC_RELEASE);
    if (!(s & SRW_R) && (s & SRW_S)) srw_wake(&l->w);
}
void hos_srw_release(SRWLOCK *l) {
    if (__atomic_load_n(&l->w, __ATOMIC_RELAXED) & SRW_W) hos_srw_unlock(l);
    else hos_srw_unlock_shared(l);
}

/* The title's free direct memory in all, whatever its pieces' sizes: every allocated range walked (~240 of them in play). */
static uint64_t direct_room_now(uint64_t *pieces) {
    struct { int64_t start, end; int type; } q;
    int sceKernelDirectMemoryQuery(int64_t offset, int flags, void *info, size_t size);
    const int64_t total = (int64_t)sceKernelGetDirectMemorySize();
    uint64_t held = 0, n = 0;
    int64_t off = 0, last_end = 0;
    while (off < total && sceKernelDirectMemoryQuery(off, 1, &q, sizeof q) == 0 && q.end > q.start) {
        if (q.start > last_end) n++;
        held += (uint64_t)(q.end - q.start), last_end = q.end, off = q.end;
    }
    if (last_end < total) n++;
    if (pieces) *pieces = n;
    return (uint64_t)total - held;
}
/* The same for the renderer, which asks at every texture it makes (gpu/vk/vk_texture.cpp): counted at most every 50 ms. */
uint64_t hos_ps5_direct_room(void) {
    static int64_t at;
    static uint64_t room;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const int64_t was = __atomic_load_n(&at, __ATOMIC_RELAXED);
    /* (250 ms, not 50: every look walks every allocated range, ~2,000 of them once memory went back in pieces, klog_12) */
    if (!was || now.QuadPart - was > 250000000LL) {
        __atomic_store_n(&room, direct_room_now(NULL), __ATOMIC_RELAXED);
        __atomic_store_n(&at, now.QuadPart, __ATOMIC_RELAXED);
    }
    return __atomic_load_n(&room, __ATOMIC_RELAXED);
}

/* HOS_PS5_ZERO_SCAN=1: how much of the game's backed memory holds nothing but zeros, by 64 KiB unit (pages it never wrote, or
   cleared: what a first-touch commit would not have backed). Read 64 MiB at a time under the window's lock, so no unit goes
   away under the read; units the game has made unreadable are left out */
static uint64_t zero_units_now(uint64_t *ahead) {
    uint64_t zero = 0;
    *ahead = 0;
    for (uint64_t u = 0; u < GUEST_UNITS; u += 1024) {
        pthread_mutex_lock(&window_lock);
        for (uint64_t k = u; k < u + 1024 && k < GUEST_UNITS; k++) {
            if (unit_state[k] != U_COMMITTED) continue;
            void *at = (void *)(HOS_PS5_GUEST_LO + k * UNIT), *start = NULL, *end = NULL;
            uint32_t prot = 0;
            if (sceKernelQueryMemoryProtection(at, &start, &end, &prot) != 0 || !(prot & PS5_KERNEL_PROT_CPU_READ) ||
                (uintptr_t)end < (uintptr_t)at + UNIT)
                continue;
            const uint64_t *p = at;
            size_t i = 0;
            while (i < UNIT / 8 && !p[i]) i++;
            zero += i == UNIT / 8;
            if (i == UNIT / 8 && unit_ahead && unit_ahead[k]) (*ahead)++;
        }
        pthread_mutex_unlock(&window_lock);
    }
    *ahead *= UNIT;
    return zero * UNIT;
}

/* ---- HOS_PS5_CPU=1: the whole process's CPU use every 5 s (every thread, the ones the sampler cannot see too) ---- */
#include <sys/resource.h>
static void *cpu_meter(void *unused) {
    (void)unused;
    if (getenv("HOS_PS5_ZERO_SCAN")) {
        uint8_t *marks = calloc(GUEST_UNITS, 1);
        pthread_mutex_lock(&window_lock);
        unit_ahead = marks;
        pthread_mutex_unlock(&window_lock);
    }
    struct rusage before;
    getrusage(RUSAGE_SELF, &before);
    double t0 = now_ms();
    for (;;) {
        sceKernelUsleep(5000000);
        struct rusage now;
        getrusage(RUSAGE_SELF, &now);
        const double t = now_ms();
        const double user = (now.ru_utime.tv_sec - before.ru_utime.tv_sec) * 1e3 + (now.ru_utime.tv_usec - before.ru_utime.tv_usec) / 1e3;
        const double sys = (now.ru_stime.tv_sec - before.ru_stime.tv_sec) * 1e3 + (now.ru_stime.tv_usec - before.ru_stime.tv_usec) / 1e3;
        int64_t at = 0;
        size_t largest = 0;
        sceKernelAvailableDirectMemorySize(0, sceKernelGetDirectMemorySize(), 0x10000, &at, &largest);
        struct ps5_heap_stats heap = {0};
        ps5_heap_stats(&heap);
        {   /* (who holds the title's direct memory: what is taken, what is free in how many pieces, and what the guest window
               keeps without using it (the units a decommit left, the rest of its current block)) */
            uint64_t pieces = 0;
            const uint64_t room = direct_room_now(&pieces), total = sceKernelGetDirectMemorySize();
            pthread_mutex_lock(&window_lock);
            const uint64_t kept = (uint64_t)free_count * UNIT, ahead = (uint64_t)(block_end - block_next);
            pthread_mutex_unlock(&window_lock);
            if (getenv("HOS_PS5_ZERO_SCAN")) {
                const double z0 = now_ms();
                uint64_t ahead = 0;
                const uint64_t zero = zero_units_now(&ahead);
                fprintf(stderr, "[ps5] guest pages: %llu MiB of %llu backed hold only zeros, %llu MiB of those backed for the "
                        "GPU or a file read before the game touched them (read in %.0f ms)\n", (unsigned long long)(zero >> 20),
                        (unsigned long long)(committed_bytes >> 20), (unsigned long long)(ahead >> 20), now_ms() - z0);
            }
            extern uint64_t radv_ps5_allocated_bytes;   /* (the driver's buffers, all of them: its winsys counts them) */
            fprintf(stderr, "[ps5] direct memory: %llu MiB taken, %llu free in %llu pieces; the driver's buffers %llu MiB; the guest "
                    "window keeps %llu MiB of decommitted units and %llu MiB of its block unused\n",
                    (unsigned long long)((total - room) >> 20), (unsigned long long)(room >> 20), (unsigned long long)pieces,
                    (unsigned long long)(__atomic_load_n(&radv_ps5_allocated_bytes, __ATOMIC_RELAXED) >> 20),
                    (unsigned long long)(kept >> 20), (unsigned long long)(ahead >> 20));
            {   /* (the title's flexible memory, param.json's kernel.flexibleMemorySize: libkernel's own stacks, the system
                   libraries' heaps. Its least free so far says how far param.json can lower it) */
                extern int32_t sceKernelAvailableFlexibleMemorySize(size_t *size) __attribute__((weak));
                static size_t least = (size_t)-1;
                size_t flex = 0;
                if (sceKernelAvailableFlexibleMemorySize && sceKernelAvailableFlexibleMemorySize(&flex) == 0) {
                    if (flex < least) least = flex;
                    fprintf(stderr, "[ps5] flexible memory: %llu MiB free (least so far %llu MiB); direct pool %llu MiB\n",
                            (unsigned long long)(flex >> 20), (unsigned long long)(least >> 20),
                            (unsigned long long)(total >> 20));
                }
            }
            /* (a driver built with its buffers counted by purpose and size, for diagnosis: PS5_Mesa's radv_ps5_bo.c) */
            extern uint64_t radv_ps5_account_bytes[6][3] __attribute__((weak)), radv_ps5_account_count[6][3] __attribute__((weak));
            if (radv_ps5_account_bytes) {
                static const char *const rows[6] = {"application", "queries", "descriptor pools", "uploads and fences",
                                                    "shader code and scratch", "command streams"};
                char line[640];
                int n = 0;
                for (int r = 0; r < 6; r++)
                    n += snprintf(line + n, sizeof line - n, "; %s %llu MiB (%llu+%llu+%llu in %llu+%llu+%llu)", rows[r],
                                  (unsigned long long)((radv_ps5_account_bytes[r][0] + radv_ps5_account_bytes[r][1] +
                                                        radv_ps5_account_bytes[r][2]) >> 20),
                                  (unsigned long long)(radv_ps5_account_bytes[r][0] >> 20),
                                  (unsigned long long)(radv_ps5_account_bytes[r][1] >> 20),
                                  (unsigned long long)(radv_ps5_account_bytes[r][2] >> 20),
                                  (unsigned long long)radv_ps5_account_count[r][0], (unsigned long long)radv_ps5_account_count[r][1],
                                  (unsigned long long)radv_ps5_account_count[r][2]);
                fprintf(stderr, "[ps5] driver buffers by purpose (under 1 MiB + under 16 + larger)%s\n", line);
            }
        }
#ifdef PS5_HEAP_TRIM   /* (a platform heap that counts what its arenas hold in blocks handed out: PS5_PayloadSDK 55a2de0) */
        fprintf(stderr, "[ps5] heap: %llu MiB mapped in %u segments, %llu MiB of it in blocks in use, %u arenas\n",
                (unsigned long long)(heap.mapped_bytes >> 20), heap.segments, (unsigned long long)(heap.in_use_bytes >> 20), heap.arenas);
#endif
        fprintf(stderr, "[ps5] cpu: %.0f%% of one core (user %.0f%%, system %.0f%%) over %.1f s; guest pages %llu MiB; heap %llu MiB "
                "(peak %llu); stacks %lld MiB in %lld; direct memory %lld MiB, largest free piece %llu MiB\n",
                100 * (user + sys) / (t - t0), 100 * user / (t - t0), 100 * sys / (t - t0), (t - t0) / 1e3,
                (unsigned long long)(committed_bytes >> 20), (unsigned long long)(heap.mapped_bytes >> 20),
                (unsigned long long)(heap.peak_bytes >> 20), (long long)(stack_bytes >> 20), (long long)stack_count,
                (long long)(sceKernelGetDirectMemorySize() >> 20), (unsigned long long)(largest >> 20));
        before = now, t0 = t;
    }
    return NULL;
}
void hos_ps5_cpu_meter(void) {
    const char *e = getenv("HOS_PS5_CPU");
    if (!e || *e != '1') return;
    pthread_t t;
    if (pthread_create(&t, NULL, cpu_meter, NULL) == 0) pthread_detach(t);
}

/* The console's largest free direct-memory piece now (the renderer's budget: gpu/vk/vk_vma.cpp ps5_render_budget) */
uint64_t hos_ps5_direct_free(void) {
    int64_t at = 0;
    size_t largest = 0;
    if (sceKernelAvailableDirectMemorySize(0, sceKernelGetDirectMemorySize(), 0x10000, &at, &largest) != 0) return 0;
    return largest;
}

/* ---- (base PS5) The memory guard: the title closes itself before the console's memory runs out ----
   Run A: the game, the renderer and the heap took the last of the title's 12,016 MiB (5 MiB free), a driver buffer the
   GPU then read was never mapped (protection fault, CPG), the graphics pipe stopped, and the system killed and restarted its
   own UI while the title waited 30 s for its GPU-hang watch. Run C: 300 MiB free in 290 pieces, the largest 14 MiB, images
   and pipelines not made, the game's loader wrote through a null pointer. So:
   - a parachute: HOS_PS5_PARACHUTE_MB (128) of direct memory taken at the start and never mapped. The first time memory
     runs out (a game page or the renderer's operator new waits) or free memory falls under HOS_PS5_MEM_FLOOR_MB, it goes back
     at once, so the wait ends and the driver has room while the rest of this happens;
   - an emergency below HOS_PS5_MEM_LOW_MB (512) free, or with the parachute out: the renderer lets every texture unused
     for a few frames go (gpu/vk/vk_texture.cpp, hos_ps5_mem_emergency);
   - the floor: still under HOS_PS5_MEM_FLOOR_MB (192) free 2 s after the parachute went, or under 64 MiB at any look,
     the caches the renderer can save are saved and the title leaves through the shell (exit 3): a title-only end, the GPU
     never left waiting on memory that is not there. HOS_PS5_MEM_GUARD=0: off.
   Looked at 5 times a second (~300 ranges walked, well under a millisecond). */
static int64_t parachute_at = -1;
static uint64_t parachute_bytes;
static volatile int mem_emergency, mem_trouble;
int hos_ps5_mem_emergency(void) { return __atomic_load_n(&mem_emergency, __ATOMIC_RELAXED); }
void (*hos_ps5_before_exit)(void);   /* (the renderer's cache saving: gpu/vk/vk_main.cpp) */
void hos_ps5_reclaim_ask(void);
static uint64_t env_mb(const char *name, uint64_t dflt) {
    const char *e = getenv(name);
    return (uint64_t)(e && *e ? strtoull(e, NULL, 10) : dflt) << 20;
}
static void parachute_take(void) {
    if (parachute_at >= 0 || !parachute_bytes) return;
    int64_t at = 0;
    if (sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), parachute_bytes, UNIT, DIRECT_TYPE, &at) == 0)
        __atomic_store_n(&parachute_at, at, __ATOMIC_RELEASE);
}
static int parachute_drop(const char *why) {
    const int64_t at = __atomic_exchange_n(&parachute_at, -1, __ATOMIC_ACQ_REL);
    if (at < 0) return 0;
    sceKernelReleaseDirectMemory(at, parachute_bytes);
    hos_ps5_klog_now("[ps5] memory guard: %s: the %llu MiB parachute given back", why, (unsigned long long)(parachute_bytes >> 20));
    return 1;
}
/* (commit_units_wait and the renderer's operator new: memory ran out just now) */
void hos_ps5_mem_ran_out(void) {
    __atomic_store_n(&mem_trouble, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&mem_emergency, 1, __ATOMIC_RELEASE);
    parachute_drop("memory ran out");
}
/* The guard's exit. klog_6 (Hyrule Field): the guard said "closes now", then the game went on playing for ~80 s while the save
   (hos_ps5_before_exit) waited for memory that was not there (operator new waits, ps5_new_waits; a play file's builders
   waiting the same way), the renderer allocating all the while, and when the exit came at last the Presentation Thread was
   still in the driver and took a fatal SIGSYS on the way out: a crash report instead of a close. Now:
   - hos_ps5_exiting() turns on first: the renderer stops at its next present (gpu/vk/vk_replay.cpp present_image) and stays
     there, and operator new no longer waits: the save's thread is answered at once (a bad_alloc its try catches), every
     other thread that asks for memory stays where it is (gpu/vk/vk_main.cpp ps5_new_waits);
   - the save runs on a thread of its own, and the exit waits for it HOS_PS5_EXIT_SAVE_SEC (10) s at most. */
static volatile int ps5_exiting, exit_saved;
static pthread_t exit_saver_thread;
static volatile int exit_saver_set;
int hos_ps5_exiting(void) { return __atomic_load_n(&ps5_exiting, __ATOMIC_ACQUIRE); }
int hos_ps5_is_exit_saver(void) {
    return __atomic_load_n(&exit_saver_set, __ATOMIC_ACQUIRE) && pthread_equal(pthread_self(), exit_saver_thread);
}
static void exit_saver_mark(void) {
    exit_saver_thread = pthread_self();
    __atomic_store_n(&exit_saver_set, 1, __ATOMIC_RELEASE);
}
static void *exit_saver(void *unused) {
    (void)unused;
    exit_saver_mark();
    if (hos_ps5_before_exit) hos_ps5_before_exit();
    __atomic_store_n(&exit_saved, 1, __ATOMIC_RELEASE);
    return NULL;
}
static void mem_floor_exit(uint64_t room, uint64_t largest, const char *why) {
    static int once;
    if (__atomic_exchange_n(&once, 1, __ATOMIC_ACQ_REL)) return;
    hos_ps5_klog_now("[ps5] memory guard: %llu MiB free (largest piece %llu MiB), %s: the title closes now, before the console's "
                     "memory runs out (guest pages %llu MiB)", (unsigned long long)(room >> 20),
                     (unsigned long long)(largest >> 20), why, (unsigned long long)(committed_bytes >> 20));
    __atomic_store_n(&ps5_exiting, 1, __ATOMIC_RELEASE);
    const double t0 = now_ms();
    if (hos_ps5_before_exit) {
        const char *e = getenv("HOS_PS5_EXIT_SAVE_SEC");
        const double limit = (e && *e ? atof(e) : 10.0) * 1000.0;
        pthread_t t;
        if (pthread_create(&t, NULL, exit_saver, NULL) == 0) {
            pthread_detach(t);
            while (!__atomic_load_n(&exit_saved, __ATOMIC_ACQUIRE) && now_ms() - t0 < limit) sceKernelUsleep(20000);
        } else {   /* (no thread to be had: the save here, unbounded, as before) */
            exit_saver_mark();
            hos_ps5_before_exit();
            exit_saved = 1;
        }
        hos_ps5_klog_now(__atomic_load_n(&exit_saved, __ATOMIC_ACQUIRE)
                             ? "[ps5] memory guard: the caches saved in %.0f ms; closing"
                             : "[ps5] memory guard: the caches' save not done after %.0f ms: closing without the rest of it",
                         now_ms() - t0);
    }
    hos_ps5_exit(3);
}
static void *mem_guard(void *unused) {
    (void)unused;
    const uint64_t low = env_mb("HOS_PS5_MEM_LOW_MB", 512), floor_b = env_mb("HOS_PS5_MEM_FLOOR_MB", 192);
    double under_since = 0, roomy_since = 0, last_log = 0;
    int was_emergency = 0;
    for (;;) {
        sceKernelUsleep(200000);
        const double t = now_ms();
        const uint64_t room = direct_room_now(NULL);
        int64_t at = 0;
        size_t largest = 0;
        sceKernelAvailableDirectMemorySize(0, sceKernelGetDirectMemorySize(), 0x10000, &at, &largest);
        const int out = __atomic_load_n(&parachute_at, __ATOMIC_ACQUIRE) < 0 && parachute_bytes;
        if (room < floor_b) parachute_drop("free memory under the floor");
        /* the emergency: on below the low mark or with the parachute out; off again a quarter above it */
        const int em = room < low || __atomic_load_n(&mem_trouble, __ATOMIC_ACQUIRE) ? 1 : room > low / 4 * 5 ? 0 : was_emergency;
        __atomic_store_n(&mem_emergency, em, __ATOMIC_RELEASE);
        if (em && !was_emergency) {
            void nup_romfs_release(void);
            hos_ps5_reclaim_ask();
            nup_romfs_release();   /* (the unpacked game files kept for reuse: host/ps5/nup_romfs.c) */
#ifdef PS5_HEAP_TRIM
            ps5_heap_trim();       /* (the heap's free segments back now, not at its next 10 s trim) */
#endif
        }
        if (em != was_emergency || (em && t - last_log > 10000)) {
            hos_ps5_klog_now("[ps5] memory guard: %llu MiB free (largest piece %llu MiB): %s", (unsigned long long)(room >> 20),
                             (unsigned long long)(largest >> 20), em ? "emergency, the renderer lets go of what it can" : "out of the emergency");
            last_log = t;
        }
        was_emergency = em;
        if (room < (64ULL << 20)) mem_floor_exit(room, largest, "under 64 MiB");
        if (room < floor_b && (out || !parachute_bytes)) {
            if (!under_since) under_since = t;
            else if (t - under_since > 2000) mem_floor_exit(room, largest, "under the floor for 2 s with the parachute out");
        } else under_since = 0;
        /* memory came back: the parachute again, after 10 s with a GiB free */
        if (out && room > (1024ULL << 20) + parachute_bytes) {
            if (!roomy_since) roomy_since = t;
            else if (t - roomy_since > 10000) {
                parachute_take();
                /* (the trouble over either way: klog_12 had 2.4 GB free in pieces too small for the parachute's 128 MiB, and the
                   emergency, which lasted until it was taken back, never ended: textures let go and made again, for minutes) */
                __atomic_store_n(&mem_trouble, 0, __ATOMIC_RELEASE);
                hos_ps5_klog_now(__atomic_load_n(&parachute_at, __ATOMIC_ACQUIRE) >= 0
                                     ? "[ps5] memory guard: %llu MiB free again: the parachute taken back"
                                     : "[ps5] memory guard: %llu MiB free again, in no piece large enough for the parachute: going on without it",
                                 (unsigned long long)(room >> 20));
                roomy_since = 0;
            }
        } else roomy_since = 0;
    }
    return NULL;
}
void hos_ps5_mem_guard_start(void) {
    const char *e = getenv("HOS_PS5_MEM_GUARD");
    if (e && *e == '0') return;
    parachute_bytes = env_mb("HOS_PS5_PARACHUTE_MB", 128);
    parachute_take();
    hos_ps5_klog_now("[ps5] memory guard: on (parachute %llu MiB %s, emergency under %llu MiB free, floor %llu MiB)",
                     (unsigned long long)(parachute_bytes >> 20), parachute_at >= 0 ? "taken" : "not taken",
                     (unsigned long long)(env_mb("HOS_PS5_MEM_LOW_MB", 512) >> 20),
                     (unsigned long long)(env_mb("HOS_PS5_MEM_FLOOR_MB", 192) >> 20));
    pthread_t t;
    if (pthread_create(&t, NULL, mem_guard, NULL) == 0) pthread_detach(t);
}

/* ---- (base PS5) The zero reclaim: guest units that hold nothing but zeros go back to the pool ----
   Run B's scan: 915 MiB of the 7.2 GB of guest pages in Kokiri Forest held only zeros. 490 MiB of them were backed for an
   import or a file read before the game touched them: those (unit_pinned, every unit ever given to hos_ps5_commit_now) are
   never touched here, as the GPU or the kernel may write them without a fault. The rest, units the game wrote and cleared or
   touched only to read, go when two looks HOS_PS5_ZERO_RECLAIM_SEC (20) apart both find them zero: under the window's lock
   the unit is made read-only, read again (a write that came before is seen; one after faults and waits in
   hos_ps5_reclaim_wait), and unmapped and reserved again when still zero, its memory kept for the next commits as a decommit's
   is; a later touch backs a fresh zeroed unit (commit_on_touch). HOS_PS5_ZERO_RECLAIM=0: off. The memory guard's emergency
   asks for a pass at once. */
static volatile uintptr_t reclaim_now_at, reclaim_recent[64];   /* (the units it last made read-only, a ring) */
static volatile unsigned reclaim_recent_n;
static void reclaim_note(uintptr_t at) { reclaim_recent[__atomic_fetch_add(&reclaim_recent_n, 1, __ATOMIC_ACQ_REL) % 64] = at; }
static volatile int reclaim_asked;
int hos_ps5_reclaim_wait(uintptr_t a) {
    const uintptr_t r = __atomic_load_n(&reclaim_now_at, __ATOMIC_ACQUIRE);
    if (r && a >= r && a < r + UNIT) {
        while (__atomic_load_n(&reclaim_now_at, __ATOMIC_ACQUIRE) == r) sceKernelUsleep(50);
        return 1;
    }
    /* (made read-only and given its write back a moment ago, so still committed: retried. Given back to the pool instead, the
       unit is reserved and the usual first-touch commit takes the fault) */
    const uintptr_t mine = a & ~(uintptr_t)(UNIT - 1);
    for (int i = 0; i < 64; i++)
        if (__atomic_load_n(&reclaim_recent[i], __ATOMIC_ACQUIRE) == mine) return hos_ps5_unit_backed(a);
    return 0;
}
static int unit_all_zero(const void *at) {
    const uint64_t *p = at;
    if (p[0] | p[UNIT / 8 - 1] | p[UNIT / 16]) return 0;
    for (size_t i = 0; i < UNIT / 8; i += 4)
        if (p[i] | p[i + 1] | p[i + 2] | p[i + 3]) return 0;
    return 1;
}
static uint8_t *zero_seen;
static uint64_t reclaim_pass(uint64_t *looked) {
    uint64_t freed = 0;
    *looked = 0;
    for (uint64_t u = 0; u < GUEST_UNITS; u += 256) {
        pthread_mutex_lock(&window_lock);
        for (uint64_t k = u; k < u + 256 && k < GUEST_UNITS; k++) {
            if (unit_state[k] != U_COMMITTED || unit_pinned[k]) { zero_seen[k] = 0; continue; }
            void *at = (void *)(HOS_PS5_GUEST_LO + k * UNIT), *start = NULL, *end = NULL;
            uint32_t prot = 0;
            if (sceKernelQueryMemoryProtection(at, &start, &end, &prot) != 0 || (prot & 3) != 3 ||
                (uintptr_t)end < (uintptr_t)at + UNIT || (uintptr_t)start > (uintptr_t)at) { zero_seen[k] = 0; continue; }
            (*looked)++;
            if (!unit_all_zero(at)) { zero_seen[k] = 0; continue; }
            if (!zero_seen[k]) { zero_seen[k] = 1; continue; }   /* (zero at two looks, not one) */
            if (free_count + 1 > free_cap) {
                size_t cap = free_cap ? free_cap * 2 : 65536;
                int64_t *grown = realloc(free_units, cap * sizeof *grown);
                if (!grown) break;
                free_units = grown, free_cap = cap;
            }
            __atomic_store_n(&reclaim_now_at, (uintptr_t)at, __ATOMIC_RELEASE);
            if (sceKernelMprotect(at, UNIT, PS5_KERNEL_PROT_CPU_READ | GPU_RW) != 0) {
                __atomic_store_n(&reclaim_now_at, 0, __ATOMIC_RELEASE);
                continue;
            }
            if (!unit_all_zero(at)) {   /* (written before it was made read-only: kept) */
                sceKernelMprotect(at, UNIT, (int)prot);
                reclaim_note((uintptr_t)at);
                __atomic_store_n(&reclaim_now_at, 0, __ATOMIC_RELEASE);
                zero_seen[k] = 0;
                continue;
            }
            __atomic_store_n(&unit_state[k], U_RESERVED, __ATOMIC_RELEASE);
            void *place = at;
            if (sceKernelMunmap(place, UNIT) != 0 || sceKernelReserveVirtualRange(&place, UNIT, PS5_KERNEL_MAP_FIXED, UNIT) != 0 ||
                place != at) {
                hos_ps5_klog_now("[ps5] zero reclaim: %#llx could not be reserved again", (unsigned long long)(uintptr_t)at);
            } else {
                free_units[free_count++] = unit_phys[k];
                committed_bytes -= UNIT;
                freed += UNIT;
            }
            zero_seen[k] = 0;
            reclaim_note((uintptr_t)at);
            __atomic_store_n(&reclaim_now_at, 0, __ATOMIC_RELEASE);
        }
        pthread_mutex_unlock(&window_lock);
        if (__atomic_load_n(&reclaim_asked, __ATOMIC_ACQUIRE) == 0) sceKernelUsleep(200);   /* (others get the lock between chunks) */
    }
    return freed;
}
void hos_ps5_reclaim_ask(void) { __atomic_store_n(&reclaim_asked, 1, __ATOMIC_RELEASE); }
static void *zero_reclaim(void *unused) {
    (void)unused;
    const char *e = getenv("HOS_PS5_ZERO_RECLAIM_SEC");
    const int every = e && atoi(e) > 0 ? atoi(e) : 20;
    uint64_t total = 0;
    for (;;) {
        for (int i = 0; i < every * 10 && !__atomic_load_n(&reclaim_asked, __ATOMIC_ACQUIRE); i++) sceKernelUsleep(100000);
        const int asked = __atomic_exchange_n(&reclaim_asked, 0, __ATOMIC_ACQ_REL);
        if (!window_reserved || !unit_pinned) continue;
        const double t0 = now_ms();
        uint64_t looked = 0;
        const uint64_t freed = reclaim_pass(&looked);
        total += freed;
        if (freed || asked)
            hos_ps5_klog_now("[ps5] zero reclaim: %llu MiB given back of %llu MiB looked at in %.0f ms%s (%llu MiB in all)",
                             (unsigned long long)(freed >> 20), (unsigned long long)(looked * UNIT >> 20), now_ms() - t0,
                             asked ? ", asked by the memory guard" : "", (unsigned long long)(total >> 20));
    }
    return NULL;
}
void hos_ps5_zero_reclaim_start(void) {
    const char *e = getenv("HOS_PS5_ZERO_RECLAIM");
    if (e && *e == '0') return;
    zero_seen = calloc(GUEST_UNITS, 1);
    if (!zero_seen) return;
    pthread_t t;
    if (pthread_create(&t, NULL, zero_reclaim, NULL) == 0) pthread_detach(t);
    hos_ps5_klog_now("[ps5] zero reclaim: on (every %s s; HOS_PS5_ZERO_RECLAIM=0: off)",
                     getenv("HOS_PS5_ZERO_RECLAIM_SEC") ? getenv("HOS_PS5_ZERO_RECLAIM_SEC") : "20");
}

/* ---- (base PS5) The cold meter: how much of the game's memory goes untouched for minutes ----
   Castle Town and Hyrule Field want more game memory than the base console has; moving what the game has not touched in
   minutes to the SSD would only help if enough of it is that cold. Measured on a sample: every 10 s up to 256 more committed
   units the game uses as it likes (not unit_pinned: no GPU import or file buffer; read and write, as the reclaim's) are made
   inaccessible. The first touch of one faults; hos_ps5_cold_touch (the signal handler, host/posix/win32_posix.c) gives it its
   access back and notes it. Every 60 s: of the samples watched for 1, 5 and 10 minutes, the share still untouched, and that
   share of the units the game holds as megabytes. A fault and an mprotect a sample, about 25 a second: nothing to notice.
   HOS_PS5_COLD=0: off. */
#define COLD_MAX 8192
static struct { uint32_t unit; float since; } cold_sample[COLD_MAX];
static int cold_n;
int hos_ps5_cold_touch(uintptr_t a) {
    if (!cold_watch || !in_window(a, a + 1)) return 0;
    const uint64_t u = (a - HOS_PS5_GUEST_LO) / UNIT;
    for (;;) {
        uint8_t v = __atomic_load_n(&cold_watch[u], __ATOMIC_ACQUIRE);
        if (!v) return 0;
        if (v == 2) {   /* (the cold store is writing it out, read-only meanwhile: a write waits, then is tried again: the unit
                           is either back as it was or written out, and then its next touch reads it back) */
            while (__atomic_load_n(&cold_watch[u], __ATOMIC_ACQUIRE) == 2) sceKernelUsleep(200);
            return 1;
        }
        if (__atomic_compare_exchange_n(&cold_watch[u], &v, 0, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) break;
    }
    sceKernelMprotect((void *)(HOS_PS5_GUEST_LO + u * UNIT), UNIT, PS5_KERNEL_PROT_CPU_READ | PS5_KERNEL_PROT_CPU_WRITE | GPU_RW);
    return 1;
}
static void *cold_meter(void *unused) {
    (void)unused;
    uint64_t cursor = 0;
    double next_report = now_ms() + 60000;
    for (;;) {
        sceKernelUsleep(10000000);
        if (!window_reserved || !unit_pinned) continue;
        const float t = (float)(now_ms() / 1000.0);
        /* new samples: the next 256 eligible units after the cursor, while there is room */
        int added = 0;
        pthread_mutex_lock(&window_lock);
        for (uint64_t looked = 0; looked < GUEST_UNITS && added < 256 && cold_n < COLD_MAX; looked++) {
            const uint64_t u = cursor;
            cursor = (cursor + 97) % GUEST_UNITS;   /* (a stride, so the sample spreads over the game's memory) */
            if (unit_state[u] != U_COMMITTED || unit_pinned[u] || cold_watch[u]) continue;
            void *at = (void *)(HOS_PS5_GUEST_LO + u * UNIT), *start = NULL, *end = NULL;
            uint32_t prot = 0;
            if (sceKernelQueryMemoryProtection(at, &start, &end, &prot) != 0 || (prot & 3) != 3 ||
                (uintptr_t)start > (uintptr_t)at || (uintptr_t)end < (uintptr_t)at + UNIT)
                continue;
            __atomic_store_n(&cold_watch[u], 1, __ATOMIC_RELEASE);
            if (sceKernelMprotect(at, UNIT, GPU_RW) != 0) { __atomic_store_n(&cold_watch[u], 0, __ATOMIC_RELEASE); continue; }
            cold_sample[cold_n].unit = (uint32_t)u, cold_sample[cold_n].since = t, cold_n++, added++;
        }
        pthread_mutex_unlock(&window_lock);
        if (now_ms() < next_report) continue;
        next_report = now_ms() + 60000;
        /* the report, and the samples done with dropped: touched ones, ones the game gave back, and ones watched 15 minutes */
        int old[3] = {0, 0, 0}, cold[3] = {0, 0, 0};
        static const float ages[3] = {60, 300, 600};
        pthread_mutex_lock(&window_lock);
        int k = 0;
        for (int i = 0; i < cold_n; i++) {
            const uint32_t u = cold_sample[i].unit;
            const int watched = __atomic_load_n(&cold_watch[u], __ATOMIC_ACQUIRE);
            const float age = t - cold_sample[i].since;
            if (unit_state[u] != U_COMMITTED) { __atomic_store_n(&cold_watch[u], 0, __ATOMIC_RELEASE); continue; }
            for (int a = 0; a < 3; a++)
                if (age >= ages[a]) old[a]++, cold[a] += watched;
            if (!watched) continue;
            if (age >= 900) {   /* (watched long enough: its access back) */
                if (__atomic_exchange_n(&cold_watch[u], 0, __ATOMIC_ACQ_REL))
                    sceKernelMprotect((void *)(HOS_PS5_GUEST_LO + (uint64_t)u * UNIT), UNIT,
                                      PS5_KERNEL_PROT_CPU_READ | PS5_KERNEL_PROT_CPU_WRITE | GPU_RW);
                continue;
            }
            cold_sample[k++] = cold_sample[i];
        }
        cold_n = k;
        uint64_t eligible = 0;
        for (uint64_t u = 0; u < GUEST_UNITS; u++) eligible += unit_state[u] == U_COMMITTED && !unit_pinned[u];
        pthread_mutex_unlock(&window_lock);
        const uint64_t mb = eligible * UNIT >> 20;
        char line[256];
        int n = snprintf(line, sizeof line, "[ps5] cold memory: of %llu MiB the game holds itself (no GPU or file buffers),",
                         (unsigned long long)mb);
        for (int a = 0; a < 3; a++)
            n += snprintf(line + n, sizeof line - n, " untouched %d min: %s", (int)(ages[a] / 60),
                          old[a] ? "" : "n/a");
        hos_ps5_klog_now("%s", line);
        for (int a = 0; a < 3; a++)
            if (old[a])
                hos_ps5_klog_now("[ps5] cold memory:   untouched for %d min: %d of %d sampled units, %.0f%%, about %llu MiB",
                                 (int)(ages[a] / 60), cold[a], old[a], 100.0 * cold[a] / old[a],
                                 (unsigned long long)(mb * (uint64_t)cold[a] / (uint64_t)old[a]));
    }
    return NULL;
}
void hos_ps5_cold_meter_start(void) {
    const char *e = getenv("HOS_PS5_COLD");
    if (e && *e == '0') return;
    const char *store = getenv("HOS_PS5_SWAP");   /* (the cold store watches the units itself: one or the other) */
    if (!(store && *store == '0')) return;
    cold_watch = calloc(GUEST_UNITS, 1);
    if (!cold_watch) return;
    pthread_t t;
    if (pthread_create(&t, NULL, cold_meter, NULL) == 0) pthread_detach(t);
    hos_ps5_klog_now("[ps5] cold memory: measured on a sample, reported every minute (HOS_PS5_COLD=0: off)");
}

/* ---- (base PS5) The cold store: game memory nothing touches for minutes goes to a file on the SSD ----
   HeyListen measured the game's memory on this very build (docs/native-engine/memory-boundary.md, sections 7, 8 and 10): of
   the ~3.6 GB in heaps outside the GPU pools 94% went untouched for 20 s (1.7 GB of it the texture files the game keeps as
   loaded, the Tex folder's .bntx files, after their texels went to the GPU's pool), and the game's code touched under 1 MB of its 3.35 GB of
   CPU-cached GPU pools in 35 s. On PC those pages leave the working set for the page file; the console has no page file, so
   this is one for the game's memory:
   - watch: units that are committed, not pinned (never a GPU import's or a kernel buffer's: hos_ps5_commit_now pins them) and
     read-write are made inaccessible to the CPU (the GPU bits kept), a run at a time, up to HOS_PS5_SWAP_RATE_MB (128) a
     second. A touch gives the unit back at once (hos_ps5_cold_touch, from the signal handler) and that unit is left alone a
     while, longer each time it comes back (30 s, 60, ... 8 minutes);
   - write out: a unit untouched for HOS_PS5_SWAP_AGE (90) s, while free memory is under HOS_PS5_SWAP_WANT_MB (2560), is made
     read-only (2: a write waits in hos_ps5_cold_touch), copied with up to 127 others and written to the file in one call
     (/app0/cache/longshot_swap.bin, 8 MB extents), then unmapped, reserved again and its direct memory given back to the
     system (else kept for the next commits). Its slot is noted (swap_slot);
   - read back: any commit of such a unit (the game's touch through commit_on_touch, a file read's or an import's
     hos_ps5_commit_now, VirtualAlloc) maps memory at a scratch address, reads the slot into it, and only then maps it at the
     unit, so no thread ever sees it half-read (swap_restore);
   - a unit the game gives back (hos_ps5_decommit) forgets its slot: it reads as zero again, as before.
   The file is at most HOS_PS5_SWAP_MB (4096). HOS_PS5_SWAP=0: off (the cold meter, HOS_PS5_COLD, measures instead). */
#include <fcntl.h>
/* (klog_11: 8 MB writes, up to 16 a second, kept the SSD busy while the game read units back: 43 commits over 200 ms, up to
   6.7 s, all a unit's read-back under the window's lock behind a write. Now 2 MB extents, at most 4 a second (16 in the
   guard's emergency), none for 3 s after a read-back) */
#define EXT_UNITS 64u   /* (klog_12: a chunk of direct memory is given back whole, CHUNK_UNITS = EXT_UNITS: 4 MiB) */
static int swap_on, swap_fd = -1;
static uint8_t *swap_back;                 /* (times a watched unit came back, for its rest) */
static uint32_t *watch_since, *watch_after;   /* (seconds of the store's clock: watched since, not watched again before) */
static uint16_t *ext_refs;                 /* (units an extent holds) */
static uint32_t *ext_free, ext_free_n, ext_next, ext_max;
static void *swap_scratch;
static uint64_t swap_out_n, swap_in_n, swap_release_failed, swap_read_failed, swap_watch_n, swap_backs, swap_released;
static double swap_in_ms, swap_in_worst;
static pthread_mutex_t ext_lock = PTHREAD_MUTEX_INITIALIZER;

static void ext_put(uint32_t e) {   /* (ext_lock held) */
    if (--ext_refs[e] == 0) ext_free[ext_free_n++] = e;
}
static void swap_forget(uint64_t u) {
    const uint32_t slot = swap_slot[u] - 1;
    swap_slot[u] = 0;
    pthread_mutex_lock(&ext_lock);
    ext_put(slot / EXT_UNITS);
    pthread_mutex_unlock(&ext_lock);
}
static void scratch_reserve(void) {
    void *at = swap_scratch;
    sceKernelReserveVirtualRange(&at, UNIT, PS5_KERNEL_MAP_FIXED, UNIT);
}
static int swap_restore(uint64_t u) {
    uint64_t got = 0;
    int64_t phys;
    if (free_count) phys = free_units[--free_count], got = UNIT;
    else phys = direct_take(UNIT, &got);
    if (phys < 0) return 0;
    const double t0 = now_ms();
    void *at = swap_scratch;
    sceKernelMunmap(swap_scratch, UNIT);
    if (sceKernelMapDirectMemory(&at, UNIT, PS5_KERNEL_PROT_CPU_READ | PS5_KERNEL_PROT_CPU_WRITE, PS5_KERNEL_MAP_FIXED, phys, UNIT) ||
        at != swap_scratch) {
        scratch_reserve();
        if (free_count < free_cap) free_units[free_count++] = phys;   /* (else that unit's memory is lost to the game) */
        hos_ps5_klog_now("[ps5] cold store: a unit's memory could not be mapped to read it back");
        return 0;
    }
    const uint32_t slot = swap_slot[u] - 1;
    if (pread(swap_fd, swap_scratch, UNIT, (off_t)slot * UNIT) != (ssize_t)UNIT) {
        memset(swap_scratch, 0, UNIT);
        if (swap_read_failed++ < 8) hos_ps5_klog_now("[ps5] cold store: slot %u NOT read back (errno %d): the unit reads as zero", slot, errno);
    }
    sceKernelMunmap(swap_scratch, UNIT);
    scratch_reserve();
    void *want = (void *)(HOS_PS5_GUEST_LO + u * UNIT);
    at = want;
    if (sceKernelMapDirectMemory(&at, UNIT, PS5_KERNEL_PROT_CPU_READ | PS5_KERNEL_PROT_CPU_WRITE | GPU_RW, PS5_KERNEL_MAP_FIXED, phys,
                                 UNIT) || at != want) {
        hos_ps5_klog_now("[ps5] cold store: unit %#llx could not be mapped back", (unsigned long long)(uintptr_t)want);
        return 0;
    }
    swap_forget(u);
    unit_phys[u] = phys;
    __atomic_store_n(&unit_state[u], U_COMMITTED, __ATOMIC_RELEASE);
    committed_bytes += UNIT;
    swap_in_n++;
    const double ms = now_ms() - t0;
    swap_in_ms += ms;
    if (ms > swap_in_worst) swap_in_worst = ms;
    return 1;
}

/* one round of watching: up to `budget` units made inaccessible from the cursor on (window_lock held) */
/* (2026-11-23, base PS5) The window's lock is let go now and then while the store watches and sorts: every guest commit
   (commit_on_touch: the game's first touch of a page), every file read's and import's buffer (hos_ps5_commit_now) and every
   decommit takes it, and the store held it for a whole pass: up to HOS_PS5_SWAP_RATE_MB (128 MB, 2,048 units) a second, a
   memory query and an mprotect each, then a sort of every unit by its memory (~10 ms) whenever free memory is under
   HOS_PS5_SWAP_WANT_MB. klog_82's Hyrule Field sat at 2.0-2.3 GB free, under both lines, with nothing written out in the end
   (0 chunks taken): the passes ran every second for nothing, the game's commits and the 0.9 MB file reads waiting behind them
   (the reads took 31-48 ms each). Watching now lets the lock go every 32 units it makes inaccessible (or 4,096 looked at), and
   the sort runs without it: the chosen chunks are checked again under the lock as before (swap_chunk). The longest hold of
   each kind is in the minute's report. HOS_PS5_SWAP_YIELD=0: held throughout, as before */
static int swap_yield_on(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("HOS_PS5_SWAP_YIELD"); v = !(e && *e == '0'); }
    return v;
}
static double swap_hold_watch_max, swap_hold_sort_max;   /* (ms: the longest single hold of the window's lock, since the report) */
static void swap_watch(uint32_t now_s, uint64_t *cursor, uint64_t budget) {
    const int yield = swap_yield_on();
    double held_since = now_ms();
    uint32_t work = 0;
    for (uint64_t looked = 0; looked < GUEST_UNITS && budget; looked++) {
        if (yield && ++work >= 4096) {   /* (window_lock held on entry and on return; let go here between two units) */
            const double t = now_ms();
            if (t - held_since > swap_hold_watch_max) swap_hold_watch_max = t - held_since;
            pthread_mutex_unlock(&window_lock);
            sched_yield();
            pthread_mutex_lock(&window_lock);
            held_since = now_ms(), work = 0;
        }
        const uint64_t u = *cursor;
        *cursor = (u + 1) % GUEST_UNITS;
        if (unit_state[u] != U_COMMITTED) continue;
        if (!cold_watch[u] && watch_since[u]) {   /* (watched and touched since: it rests, longer each time) */
            watch_since[u] = 0;
            if (swap_back[u] < 255) swap_back[u]++;
            swap_backs++;
            watch_after[u] = now_s + (30u << (swap_back[u] < 4 ? swap_back[u] : 4));
            continue;
        }
        if (unit_pinned[u] || cold_watch[u] || watch_after[u] > now_s) continue;
        void *at = (void *)(HOS_PS5_GUEST_LO + u * UNIT), *start = NULL, *end = NULL;
        uint32_t prot = 0;
        if (sceKernelQueryMemoryProtection(at, &start, &end, &prot) != 0 || (prot & 3) != 3 ||
            (uintptr_t)start > (uintptr_t)at || (uintptr_t)end < (uintptr_t)at + UNIT)
            continue;
        __atomic_store_n(&cold_watch[u], 1, __ATOMIC_RELEASE);
        if (sceKernelMprotect(at, UNIT, GPU_RW) != 0) { __atomic_store_n(&cold_watch[u], 0, __ATOMIC_RELEASE); continue; }
        watch_since[u] = now_s ? now_s : 1;
        swap_watch_n++;
        budget--;
        work += 128;   /* (2026-11-23: two system calls a unit made inaccessible: 32 of them a hold) */
    }
    const double t = now_ms();
    if (t - held_since > swap_hold_watch_max) swap_hold_watch_max = t - held_since;
}

/* one batch written out: up to EXT_UNITS units untouched for age seconds. 0: none found */
/* The direct memory of the units written out (window_lock held). Given back to the system only in runs of at least
   SWAP_RUN_UNITS one after another (2 MiB): 64 KiB pieces scattered over the pool are of no use to the renderer's blocks,
   and every piece is a range the free-memory count walks (direct_room_now). Shorter runs are kept for the game's next commits */
#define SWAP_RUN_UNITS 8u   /* (512 KiB: klog_11 gave back 803 of 3,229 MiB written out with 2 MiB runs) */
static int cmp_i64(const void *a, const void *b) {
    const int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
    return x < y ? -1 : x > y;
}
static void swap_keep(int64_t phys) {
    if (free_count + 1 > free_cap) {
        size_t cap = free_cap ? free_cap * 2 : 65536;
        int64_t *grown = realloc(free_units, cap * sizeof *grown);
        if (grown) free_units = grown, free_cap = cap;
    }
    if (free_count < free_cap) free_units[free_count++] = phys;
}
static __attribute__((unused)) void swap_give_back(int64_t *phys, uint32_t n) {
    qsort(phys, n, sizeof *phys, cmp_i64);
    for (uint32_t i = 0; i < n;) {
        uint32_t k = 1;
        while (i + k < n && phys[i + k] == phys[i] + (int64_t)(k * UNIT)) k++;
        if (k >= SWAP_RUN_UNITS && sceKernelReleaseDirectMemory(phys[i], (size_t)k * UNIT) == 0) swap_released += k;
        else {
            if (k >= SWAP_RUN_UNITS && swap_release_failed++ < 4)
                hos_ps5_klog_now("[ps5] cold store: %u units' direct memory not given back to the system: kept for the game", k);
            for (uint32_t j = k; j-- > 0;) swap_keep(phys[i + j]);   /* (highest first: commit_units takes a run off the top) */
        }
        i += k;
    }
}
/* The units kept for the game's next commits (free_units) past 256 MiB: their runs of SWAP_RUN_UNITS and more given back to the
   system (klog_11: the cold store took the game's pages from 6.9 to 4.8 GB, but most of that memory stayed kept, out of the
   renderer's reach). The array stays sorted lowest first, as commit_units takes it off the top */
static void swap_trim_kept(void) {
    pthread_mutex_lock(&window_lock);
    if ((uint64_t)free_count * UNIT <= (256ULL << 20)) { pthread_mutex_unlock(&window_lock); return; }
    qsort(free_units, free_count, sizeof *free_units, cmp_i64);
    size_t out = 0;
    for (size_t i = 0; i < free_count;) {
        size_t k = 1;
        while (i + k < free_count && free_units[i + k] == free_units[i] + (int64_t)(k * UNIT)) k++;
        const int over = (uint64_t)(free_count - i + out) * UNIT > (256ULL << 20);
        /* (only the aligned 4 MiB chunks inside the run: no small pieces, klog_12) */
        size_t j = 0;
        while (j < k) {
            const int64_t at = free_units[i + j], csz = (int64_t)EXT_UNITS * (int64_t)UNIT;
            if (over && !(at % csz) && j + EXT_UNITS <= k && sceKernelReleaseDirectMemory(at, (size_t)csz) == 0) {
                swap_released += EXT_UNITS, j += EXT_UNITS;
                continue;
            }
            free_units[out++] = at, j++;
        }
        i += k;
    }
    free_count = out;
    pthread_mutex_unlock(&window_lock);
}
/* One chunk written out: CHUNK_UNITS units (4 MiB) whose direct memory is one aligned 4 MiB piece and which are ALL cold
   (untouched for age seconds, not pinned), so that the whole piece goes back to the system in one release. klog_12 gave
   memory back in runs from 512 KiB: 2.4 GB free in 2,061 pieces, the largest 7-36 MB; a heap segment then found no room,
   the parachute went, could never be taken back in one piece, and the guard stayed in its emergency, the renderer letting
   textures go and making them again: the late hitches. A chunk with one warm unit stays as it is. 0: none found */
#define CHUNK_UNITS EXT_UNITS
typedef struct { int64_t phys; uint64_t u; } Cand;
static int cmp_cand(const void *a, const void *b) {
    const int64_t x = ((const Cand *)a)->phys, y = ((const Cand *)b)->phys;
    return x < y ? -1 : x > y;
}
static Cand *cands;
/* (klog_13: with every unit of a chunk required cold, 660 MiB went out where klog_12 wrote 2.9 GB: one warm unit kept its
   whole chunk. Now up to MOVE_MAX warm units of a chunk otherwise cold are first moved to memory elsewhere (copied while
   read-only, as a unit written out is, then mapped at their address over the new memory), and the chunk goes whole. Units
   the GPU imported (pinned) are never moved: the import holds their memory; a chunk with one stays) */
#define MOVE_MAX 32u   /* (16: klog_27 wrote 1.2 GB of a 2.5 GB aim; a unit moved is a 64 KiB copy) */
/* (why the chunks were passed over, since the last report: not all 64 units the game's or kept ones, a unit the GPU imported or
   being written, too many warm, too few cold) */
static uint64_t sw_chunks, sw_split, sw_pinned, sw_warm, sw_young, sw_taken;
static uint64_t swap_moved;
/* one unit's contents moved to direct memory outside [avoid, avoid + csize) (window_lock held). 0: not moved, as it was */
static int swap_move(uint64_t u, int64_t avoid, int64_t csize) {
    if (unit_state[u] != U_COMMITTED || unit_pinned[u]) return 0;   /* (given back, or the GPU's now: not moved) */
    void *at = (void *)(HOS_PS5_GUEST_LO + u * UNIT), *start = NULL, *end = NULL;
    uint32_t prot = 0;
    if (sceKernelQueryMemoryProtection(at, &start, &end, &prot) != 0 || (prot & 3) != 3) return 0;
    int64_t phys = -1;
    for (size_t i = free_count; i-- > 0;)   /* (a kept unit not in the chunk, else the block's next) */
        if (free_units[i] < avoid || free_units[i] >= avoid + csize) {
            phys = free_units[i];
            free_units[i] = free_units[--free_count];
            break;
        }
    if (phys < 0) {
        uint64_t got = 0;
        phys = direct_take(UNIT, &got);
        if (phys < 0) return 0;
    }
    uint8_t zero = 0;
    if (!__atomic_compare_exchange_n(&cold_watch[u], &zero, 2, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        swap_keep(phys);
        return 0;
    }
    int ok = sceKernelMprotect(at, UNIT, PS5_KERNEL_PROT_CPU_READ | GPU_RW) == 0;
    void *s2 = swap_scratch;
    if (ok) {
        sceKernelMunmap(swap_scratch, UNIT);
        ok = sceKernelMapDirectMemory(&s2, UNIT, PS5_KERNEL_PROT_CPU_READ | PS5_KERNEL_PROT_CPU_WRITE, PS5_KERNEL_MAP_FIXED, phys, UNIT) == 0 &&
             s2 == swap_scratch;
        if (ok) memcpy(swap_scratch, at, UNIT);
        sceKernelMunmap(swap_scratch, UNIT);
        scratch_reserve();
    }
    if (ok) {   /* (the copy mapped over the unit: no moment unmapped, MAP_FIXED replaces the old mapping) */
        void *w = at;
        ok = sceKernelMapDirectMemory(&w, UNIT, PS5_KERNEL_PROT_CPU_READ | PS5_KERNEL_PROT_CPU_WRITE | GPU_RW, PS5_KERNEL_MAP_FIXED,
                                      phys, UNIT) == 0 && w == at;
    }
    if (!ok) {
        sceKernelMprotect(at, UNIT, PS5_KERNEL_PROT_CPU_READ | PS5_KERNEL_PROT_CPU_WRITE | GPU_RW);
        swap_keep(phys);
    } else {
        unit_phys[u] = phys;
        swap_moved++;
    }
    __atomic_store_n(&cold_watch[u], 0, __ATOMIC_RELEASE);
    return ok;
}
/* One chosen chunk written out (cands[best .. best + CHUNK_UNITS), as sorted by the pass). window_lock held on entry, not on
   return. The pass's list may have gone stale while an earlier chunk was written: every unit is checked again here */
static int swap_chunk(size_t best, uint32_t now_s, uint32_t age, uint8_t *staging) {
    uint64_t pick[CHUNK_UNITS];
    int64_t removed[CHUNK_UNITS];   /* (the chunk's kept units, off the kept list while it is written out) */
    uint32_t n = 0, nremoved = 0;
    int64_t chunk = -1;
    const int64_t csize = (int64_t)CHUNK_UNITS * (int64_t)UNIT;
    {   /* (still all of it ours: each 64 KiB of the chunk mapped by one of the listed units, or kept. Else part of it went to a
           commit since the pass looked, and the chunk is left: given back, it would free memory in use) */
        const int64_t c0 = cands[best].phys;
        uint32_t acc = 0;
        for (uint32_t k = 0; k < CHUNK_UNITS; k++) {
            const uint64_t u = cands[best + k].u;
            if (u != UINT64_MAX && unit_state[u] == U_COMMITTED && unit_phys[u] >= c0 && unit_phys[u] < c0 + csize) acc++;
        }
        for (size_t i = 0; i < free_count; i++) acc += free_units[i] >= c0 && free_units[i] < c0 + csize;
        if (acc != CHUNK_UNITS) {
            pthread_mutex_unlock(&window_lock);
            return 0;
        }
    }
    {
        const int64_t c = cands[best].phys;
        /* the warm ones moved out first; one that will not move: the chunk is left */
        int moved_all = 1;
        for (uint32_t k = 0; k < CHUNK_UNITS && moved_all; k++) {
            const uint64_t u = cands[best + k].u;
            if (u == UINT64_MAX) continue;
            const uint8_t w = __atomic_load_n(&cold_watch[u], __ATOMIC_ACQUIRE);
            if (w == 1 && watch_since[u] && now_s - watch_since[u] >= age) continue;
            if (w == 1) {   /* (watched, not long enough: given its access back, then moved) */
                uint8_t one = 1;
                if (__atomic_compare_exchange_n(&cold_watch[u], &one, 0, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
                    sceKernelMprotect((void *)(HOS_PS5_GUEST_LO + u * UNIT), UNIT, PS5_KERNEL_PROT_CPU_READ | PS5_KERNEL_PROT_CPU_WRITE | GPU_RW);
                watch_since[u] = 0;
            }
            moved_all = swap_move(u, c, csize);
        }
        /* the cold ones taken for writing (2) and read-only; the kept ones out of the kept list */
        uint32_t k = 0, taken = 0;
        uint64_t took[CHUNK_UNITS];
        for (; moved_all && k < CHUNK_UNITS; k++) {
            const uint64_t u = cands[best + k].u;
            if (u == UINT64_MAX) continue;
            if (unit_phys[u] < c || unit_phys[u] >= c + csize) continue;   /* (moved out) */
            uint8_t one = 1;
            if (!__atomic_compare_exchange_n(&cold_watch[u], &one, 2, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) break;
            if (sceKernelMprotect((void *)(HOS_PS5_GUEST_LO + u * UNIT), UNIT, PS5_KERNEL_PROT_CPU_READ | GPU_RW) != 0) {
                __atomic_store_n(&cold_watch[u], 1, __ATOMIC_RELEASE);
                break;
            }
            took[taken++] = u;
        }
        if (!moved_all || k < CHUNK_UNITS) {   /* (back to watched, as before) */
            while (taken-- > 0) {
                sceKernelMprotect((void *)(HOS_PS5_GUEST_LO + took[taken] * UNIT), UNIT, GPU_RW);
                __atomic_store_n(&cold_watch[took[taken]], 1, __ATOMIC_RELEASE);
            }
        } else {
            /* the chunk's kept units leave the kept list: the chunk goes whole */
            size_t out = 0;
            for (size_t i = 0; i < free_count; i++) {
                if (free_units[i] < c || free_units[i] >= c + csize) free_units[out++] = free_units[i];
                else if (nremoved < CHUNK_UNITS) removed[nremoved++] = free_units[i];
            }
            free_count = out;
            chunk = c;
            for (uint32_t i = 0; i < taken; i++) {
                pick[i] = took[i];
                memcpy(staging + (size_t)i * UNIT, (void *)(HOS_PS5_GUEST_LO + took[i] * UNIT), UNIT);
            }
            n = taken;
        }
    }
    pthread_mutex_unlock(&window_lock);
    if (chunk < 0) return 0;
    uint32_t e = UINT32_MAX;
    int wrote = 1;
    if (n) {
        pthread_mutex_lock(&ext_lock);
        if (ext_free_n) e = ext_free[--ext_free_n];
        else if (ext_next < ext_max) e = ext_next++;
        pthread_mutex_unlock(&ext_lock);
        /* (the file's write, without the window's lock: the units stay readable, a write to one waits in hos_ps5_cold_touch) */
        wrote = e != UINT32_MAX &&
                pwrite(swap_fd, staging, (size_t)n * UNIT, (off_t)e * EXT_UNITS * UNIT) == (ssize_t)((size_t)n * UNIT);
        if (e != UINT32_MAX && !wrote) hos_ps5_klog_now("[ps5] cold store: the file's write failed (errno %d): the units stay in memory", errno);
    }
    pthread_mutex_lock(&window_lock);
    /* (all of the chunk's taken units still as taken, and no unit of ours newly in the chunk, or none of it goes) */
    int whole = wrote;
    for (uint32_t i = 0; i < n && whole; i++) {
        const uint64_t u = pick[i];
        whole = unit_state[u] == U_COMMITTED && !unit_pinned[u] && unit_phys[u] >= chunk && unit_phys[u] < chunk + csize &&
                __atomic_load_n(&cold_watch[u], __ATOMIC_ACQUIRE) == 2;
    }
    for (size_t i = 0; i < free_count && whole; i++) whole = free_units[i] < chunk || free_units[i] >= chunk + csize;
    uint32_t held = 0;
    for (uint32_t i = 0; i < n; i++) {
        const uint64_t u = pick[i];
        void *at = (void *)(HOS_PS5_GUEST_LO + u * UNIT);
        if (!whole) {
            if (unit_state[u] == U_COMMITTED && __atomic_load_n(&cold_watch[u], __ATOMIC_ACQUIRE) == 2)
                sceKernelMprotect(at, UNIT, PS5_KERNEL_PROT_CPU_READ | PS5_KERNEL_PROT_CPU_WRITE | GPU_RW);
            if (__atomic_load_n(&cold_watch[u], __ATOMIC_ACQUIRE) == 2) __atomic_store_n(&cold_watch[u], 0, __ATOMIC_RELEASE);
            watch_since[u] = 0;
            continue;
        }
        void *place = at;
        if (sceKernelMunmap(place, UNIT) != 0 || sceKernelReserveVirtualRange(&place, UNIT, PS5_KERNEL_MAP_FIXED, UNIT) != 0 ||
            place != at)
            hos_ps5_klog_now("[ps5] cold store: %#llx could not be reserved again", (unsigned long long)(uintptr_t)at);
        swap_slot[u] = e * EXT_UNITS + i + 1;
        __atomic_store_n(&unit_state[u], U_RESERVED, __ATOMIC_RELEASE);
        committed_bytes -= UNIT;
        watch_since[u] = 0;
        held++;
        __atomic_store_n(&cold_watch[u], 0, __ATOMIC_RELEASE);   /* (a write that waited is tried again: it reads the unit back) */
    }
    if (whole) {   /* (the chunk's memory back to the system in one piece; refused: all of it kept for the game's next commits) */
        if (sceKernelReleaseDirectMemory(chunk, (size_t)csize) == 0) swap_released += CHUNK_UNITS;
        else {
            if (swap_release_failed++ < 4) hos_ps5_klog_now("[ps5] cold store: a chunk's direct memory not given back: kept for the game");
            for (uint32_t i = CHUNK_UNITS; i-- > 0;) swap_keep(chunk + (int64_t)(i * UNIT));
        }
    } else {
        for (uint32_t i = 0; i < nremoved; i++) swap_keep(removed[i]);   /* (the chunk stays: its kept units kept again) */
    }
    pthread_mutex_unlock(&window_lock);
    if (e != UINT32_MAX) {
        pthread_mutex_lock(&ext_lock);
        ext_refs[e] = (uint16_t)held;
        if (!held) ext_free[ext_free_n++] = e;
        pthread_mutex_unlock(&ext_lock);
    }
    swap_out_n += held;
    return 1;
}

static int swap_write(uint32_t now_s, uint32_t age, uint8_t *staging, uint64_t *cursor, int want_chunks) {
    (void)cursor;
    enum { CANDS = 1 << 18 };
    if (!cands && !(cands = malloc(CANDS * sizeof *cands))) return 0;
    const int64_t csize = (int64_t)CHUNK_UNITS * (int64_t)UNIT;
    if (want_chunks > 8) want_chunks = 8;
    pthread_mutex_lock(&window_lock);
    const double held_since = now_ms();
    /* every committed unit by its direct memory, and the kept ones (u = UINT64_MAX): which chunk is cold enough */
    size_t nc = 0;
    for (uint64_t u = 0; u < GUEST_UNITS && nc < CANDS; u++)
        if (unit_state[u] == U_COMMITTED) cands[nc].phys = unit_phys[u], cands[nc].u = u, nc++;
    for (size_t i = 0; i < free_count && nc < CANDS; i++) cands[nc].phys = free_units[i], cands[nc].u = UINT64_MAX, nc++;
    /* (2026-11-23, HOS_PS5_SWAP_YIELD: the sort and the choice without the window's lock. What they read past the list is the
       store's own (watch_since) or atomic (cold_watch), or a pin a commit may set meanwhile (unit_pinned): a chunk chosen from a
       stale read is checked again under the lock, unit by unit, by swap_chunk, as a list gone stale while an earlier chunk was
       written already is) */
    const int yield = swap_yield_on();
    if (yield) {
        const double t = now_ms();
        if (t - held_since > swap_hold_sort_max) swap_hold_sort_max = t - held_since;
        pthread_mutex_unlock(&window_lock);
    }
    qsort(cands, nc, sizeof *cands, cmp_cand);
    /* (the chunks that qualify, the coldest first: up to `want_chunks` of them this pass) */
    enum { PICK = 8 };
    size_t chosen[PICK];
    uint32_t chosen_cold[PICK], nchosen = 0;
    for (size_t i = 0; i + CHUNK_UNITS <= nc;) {
        const int64_t c = cands[i].phys - cands[i].phys % csize;
        size_t j = i;
        uint32_t cold = 0, warm = 0, bad = 0;   /* (kept units, u UINT64_MAX, count as the chunk's own) */
        while (j < nc && cands[j].phys < c + csize) {
            const uint64_t u = cands[j].u;
            if (u == UINT64_MAX) {
            } else if (unit_pinned[u]) bad++;
            else {
                const uint8_t w = __atomic_load_n(&cold_watch[u], __ATOMIC_ACQUIRE);
                if (w == 1 && watch_since[u] && now_s - watch_since[u] >= age) cold++;
                else if (w == 0 || w == 1) warm++;
                else bad++;
            }
            j++;
        }
        /* (all 64 units of the chunk are ours, contiguous, none pinned, few warm) */
        sw_chunks++;
        if (!(j - i == CHUNK_UNITS && cands[i].phys == c)) sw_split++;
        else if (bad) sw_pinned++;
        else if (warm > MOVE_MAX) sw_warm++;
        else if (cold < CHUNK_UNITS / 2) sw_young++;
        if (j - i == CHUNK_UNITS && cands[i].phys == c && !bad && warm <= MOVE_MAX && cold >= CHUNK_UNITS / 2) {
            uint32_t at = nchosen < (uint32_t)want_chunks ? nchosen++ : (uint32_t)want_chunks;
            if (at == (uint32_t)want_chunks) {   /* (full: it replaces the least cold, if colder) */
                uint32_t lo = 0;
                for (uint32_t q = 1; q < nchosen; q++) if (chosen_cold[q] < chosen_cold[lo]) lo = q;
                if (cold > chosen_cold[lo]) at = lo;
            }
            if (at < (uint32_t)want_chunks) chosen[at] = i, chosen_cold[at] = cold;
        }
        i = j;
    }
    if (!nchosen) {
        if (!yield) {
            const double t = now_ms();
            if (t - held_since > swap_hold_sort_max) swap_hold_sort_max = t - held_since;
            pthread_mutex_unlock(&window_lock);
        }
        return 0;
    }
    int done = 0;
    for (uint32_t q = 0; q < nchosen; q++) {
        if (q || yield) pthread_mutex_lock(&window_lock);   /* (swap_chunk lets it go) */
        done += swap_chunk(chosen[q], now_s, age, staging);
    }
    sw_taken += nchosen;
    return done;
}

static void *cold_store(void *unused) {
    (void)unused;
    const uint64_t want = env_mb("HOS_PS5_SWAP_WANT_MB", 2560), rate = env_mb("HOS_PS5_SWAP_RATE_MB", 128) / UNIT;
    const char *ae = getenv("HOS_PS5_SWAP_AGE");
    /* (180 s, not 90: in klog_11 a fifth of what went out came back in) */
    /* (120 s: klog_15 read back 11 of 680 MiB written out at 180) */
    const uint32_t age = ae && atoi(ae) >= 10 ? (uint32_t)atoi(ae) : 120;
    uint8_t *staging = malloc((size_t)EXT_UNITS * UNIT);
    if (!staging) return NULL;
    uint64_t watch_cursor = 0, write_cursor = 0, last_in = 0;
    uint32_t quiet_until = 0;
    const double t0 = now_ms();
    double next_report = t0 + 60000;
    for (;;) {
        sceKernelUsleep(1000000);
        if (!window_reserved || !unit_pinned) continue;
        const uint32_t now_s = (uint32_t)((now_ms() - t0) / 1000) + 1;
        const uint64_t room = direct_room_now(NULL);
        if (room < want + (1024ULL << 20)) {   /* (watching costs a fault per unit touched: only when it may be needed) */
            pthread_mutex_lock(&window_lock);
            swap_watch(now_s, &watch_cursor, rate);
            pthread_mutex_unlock(&window_lock);
        }
        /* (short of the target: batches out until it is met, a quicker age and more of them in the guard's emergency; none
           while the game is reading units back, which waits behind a write) */
        const int em = hos_ps5_mem_emergency();
        const uint32_t a = em ? age / 3 : age;
        if (swap_in_n != last_in) last_in = swap_in_n, quiet_until = now_s + 3;
        if (now_s >= quiet_until || em)
            for (int b = 0; b < (em ? 4 : 1) && room < want; b++)   /* (a pass sorts every unit under the window's lock: ~10 ms) */
                if (!swap_write(now_s, a, staging, &write_cursor, em ? 8 : 4)) break;
        swap_trim_kept();
        if (now_ms() >= next_report) {
            next_report = now_ms() + 60000;
            hos_ps5_klog_now("[ps5] cold store: %llu MiB written out (%llu MiB read back, %.2f ms each, worst %.1f), %llu MiB of "
                             "watching, %llu came back, %llu moved; %llu MiB given back to the system (the rest kept for the game); file %llu MiB of %llu; "
                             "%llu MiB free (aim %llu)",
                             (unsigned long long)(swap_out_n * UNIT >> 20), (unsigned long long)(swap_in_n * UNIT >> 20),
                             swap_in_n ? swap_in_ms / swap_in_n : 0.0, swap_in_worst, (unsigned long long)(swap_watch_n * UNIT >> 20),
                             (unsigned long long)swap_backs, (unsigned long long)swap_moved, (unsigned long long)(swap_released * UNIT >> 20),
                             (unsigned long long)((uint64_t)ext_next * EXT_UNITS * UNIT >> 20),
                             (unsigned long long)((uint64_t)ext_max * EXT_UNITS * UNIT >> 20), (unsigned long long)(room >> 20),
                             (unsigned long long)(want >> 20));
            hos_ps5_klog_now("[ps5] cold store: chunks looked at %llu: %llu not whole ours, %llu with a GPU-held unit, %llu too warm, "
                             "%llu not cold enough, %llu taken", (unsigned long long)sw_chunks, (unsigned long long)sw_split,
                             (unsigned long long)sw_pinned, (unsigned long long)sw_warm, (unsigned long long)sw_young,
                             (unsigned long long)sw_taken);
            hos_ps5_klog_now("[ps5] cold store: the window's lock held at most %.1f ms at a time watching, %.1f ms choosing chunks "
                             "(HOS_PS5_SWAP_YIELD=%s)", swap_hold_watch_max, swap_hold_sort_max,
                             swap_yield_on() ? "0: held for whole passes, as before" : "0: held for whole passes");
            swap_hold_watch_max = swap_hold_sort_max = 0;
            sw_chunks = sw_split = sw_pinned = sw_warm = sw_young = sw_taken = 0;
        }
    }
    return NULL;
}

void hos_ps5_cold_store_start(void) {
    const char *e = getenv("HOS_PS5_SWAP");
    if (e && *e == '0') return;
    if (cold_watch) return;   /* (the cold meter runs: HOS_PS5_COLD) */
    const uint64_t max = env_mb("HOS_PS5_SWAP_MB", 4096);
    ext_max = (uint32_t)(max / (EXT_UNITS * UNIT));
    cold_watch = calloc(GUEST_UNITS, 1);
    swap_slot = calloc(GUEST_UNITS, sizeof *swap_slot);
    swap_back = calloc(GUEST_UNITS, 1);
    watch_since = calloc(GUEST_UNITS, sizeof *watch_since);
    watch_after = calloc(GUEST_UNITS, sizeof *watch_after);
    ext_refs = calloc(ext_max + 1, sizeof *ext_refs);
    ext_free = calloc(ext_max + 1, sizeof *ext_free);
    void *scratch = NULL;
    if (!cold_watch || !swap_slot || !swap_back || !watch_since || !watch_after || !ext_refs || !ext_free || !ext_max ||
        sceKernelReserveVirtualRange(&scratch, UNIT, 0, UNIT) != 0) {
        hos_ps5_klog_now("[ps5] cold store: off (no room for its tables)");
        free(swap_slot), swap_slot = NULL;
        free(cold_watch), cold_watch = NULL;
        return;
    }
    swap_scratch = scratch;
    swap_fd = open("/app0/cache/longshot_swap.bin", O_RDWR | O_CREAT | O_TRUNC, 0666);
    if (swap_fd < 0) {
        hos_ps5_klog_now("[ps5] cold store: off (/app0/cache/longshot_swap.bin not made, errno %d)", errno);
        free(swap_slot), swap_slot = NULL;
        return;
    }
    swap_on = 1;
    pthread_t t;
    if (pthread_create(&t, NULL, cold_store, NULL) == 0) pthread_detach(t);
    hos_ps5_klog_now("[ps5] cold store: game memory untouched for %s s goes to /app0/cache/longshot_swap.bin while free memory is under "
                     "%llu MiB (at most %llu MiB; HOS_PS5_SWAP=0: off)",
                     getenv("HOS_PS5_SWAP_AGE") ? getenv("HOS_PS5_SWAP_AGE") : "120",
                     (unsigned long long)(env_mb("HOS_PS5_SWAP_WANT_MB", 2560) >> 20), (unsigned long long)(max >> 20));
}
