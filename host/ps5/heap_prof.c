/* (base PS5) The heap by caller: what holds the title heap, sampled.

   klog_6 (Kokiri, the Deku Tree, Hyrule Field): the heap went from 0.9 GB at the start to 3.0 GB, and in the field it was the
   only thing that grew (guest pages flat at 6.66 GB): about 0.6-2 MB stayed behind for each pipeline compiled in play, against
   ~160 KB for each one the start read from the cache, ~96% of it in blocks still in use. What holds them is the question this
   answers.

   Every allocation the title makes goes through the platform heap's wraps (PS5_Vulkan tools/radv-link.sh: --wrap=malloc and
   its family; ps5platform/heap.h). ps5/tools/link.sh renames the platform's wraps ps5w_* (heap_wrap.o, out of
   libps5platform.a) and these stand in front of them: each passes the call on unchanged, and, one allocation in about every
   HOS_PS5_HEAP_PROF_KB (1024) KB a thread allocates, records who asked (the caller's stack, _Unwind_Backtrace, 10 frames) and
   keeps the pointer in a table, so that its free takes it out again. A sample stands for max(its size, the interval) bytes,
   so the live total per caller estimates what that caller holds now. Every HOS_PS5_HEAP_PROF_SEC (60) s klog gets the 12
   callers holding the most, their growth since the last report and what they allocated in all; the addresses are offsets
   into work/ps5/link/llvm-pie.elf, which ps5/tools/heap_sites.py turns into function names.
   HOS_PS5_HEAP_PROF=0: off (the wraps then cost a load and a branch). */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unwind.h>

#include <ps5platform/heap.h>

int sceKernelUsleep(unsigned microseconds);
void hos_ps5_klog_now(const char *fmt, ...);
uintptr_t hos_ps5_exe_base(void);

/* the platform's own wraps, renamed by ps5/tools/link.sh */
void *ps5w_malloc(size_t n);
void *ps5w_calloc(size_t count, size_t n);
void *ps5w_realloc(void *p, size_t n);
void ps5w_free(void *p);
int ps5w_posix_memalign(void **out, size_t alignment, size_t n);
void *ps5w_aligned_alloc(size_t alignment, size_t n);
void *ps5w_memalign(size_t alignment, size_t n);
size_t ps5w_malloc_usable_size(void *p);
void *ps5w_reallocf(void *p, size_t n);
void *ps5w_reallocarray(void *p, size_t count, size_t n);
ssize_t ps5w_getline(char **line, size_t *cap, FILE *f);
ssize_t ps5w_getdelim(char **line, size_t *cap, int delim, FILE *f);

/* (the renderer's own counts, added to each report: gpu/vk/vk_main.cpp) */
void (*hos_ps5_heap_extra)(void);

#define SLOT_BITS 16
#define SLOTS (1u << SLOT_BITS)
#define PROBE 24
#define SITES 4096
#define DEPTH 10
#define TOP 12
#define SLOT_TOMB ((uintptr_t)1)
#define SLOT_BUSY ((uintptr_t)2)
#define NO_SITE 0xffffffffu

static int prof_on;
static uint64_t interval = 1u << 20;
static int64_t live_samples;   /* (none: a free need not look) */
static uint64_t dropped_slots, dropped_sites, samples_all;

static uintptr_t slot_ptr[SLOTS];
static uint32_t slot_site[SLOTS];
static uint64_t slot_w[SLOTS];

struct site {
    uint64_t hash;
    uintptr_t pc[DEPTH];
    int64_t live;
    uint64_t made;
    uint32_t samples;
    uint8_t depth, used;
    uint64_t maxn;   /* (HF1) the largest request sampled for it */
};
static struct site sites[SITES];
static int64_t last_live[SITES];
static uint64_t last_made[SITES];   /* (HF1) */
/* (2026-11-26, HF1) every allocation by size class, not sampled: count and bytes since the last report (under 1 KB, 16 KB,
   256 KB, 4 MB, larger). The heap is dlmalloc mspaces that give back only an arena's free top and whole free segments, so
   blocks made and freed between long-lived ones leave holes held mapped: 46.log, 180 -> 480 MiB mapped but free */
#define CLASSES 5
static uint64_t class_n[CLASSES], class_bytes[CLASSES];
static inline int class_of(size_t n) { return n < 1024 ? 0 : n < (16u << 10) ? 1 : n < (256u << 10) ? 2 : n < (4u << 20) ? 3 : 4; }
static int site_lock;
static unsigned sites_n;

/* (no thread-local state here: on the console a __thread variable is emulated (__emutls_get_address), and its first use on a
   thread allocates, which re-entered these wraps without end (klog_7: the main thread's stack used up at the start). One
   countdown for the whole title, and one sample taken at a time: an allocation made while one is taken (the unwinder's own,
   or another thread's) is not sampled) */
static int64_t g_left = 1 << 20;
static int g_sampling;
static uint64_t g_rng = 0x9E3779B97F4A7C15ull;

static uint32_t slot_of(const void *p) {
    return (uint32_t)((((uintptr_t)p >> 4) * 0x9E3779B97F4A7C15ull) >> (64 - SLOT_BITS));
}
/* the next interval, 0.5 to 1.5 of it (a fixed one would keep landing on the same allocations of a repeating pattern) */
static int64_t next_interval(void) {   /* (under g_sampling) */
    g_rng ^= g_rng << 13, g_rng ^= g_rng >> 7, g_rng ^= g_rng << 17;
    return (int64_t)(interval / 2 + g_rng % (interval + 1));
}

struct bt { uintptr_t *pc; int n, skip; };
static _Unwind_Reason_Code bt_frame(struct _Unwind_Context *c, void *arg) {
    struct bt *b = arg;
    const uintptr_t ip = (uintptr_t)_Unwind_GetIP(c);
    if (!ip) return _URC_END_OF_STACK;
    if (b->skip > 0) return b->skip--, _URC_NO_REASON;
    b->pc[b->n++] = ip;
    return b->n >= DEPTH ? _URC_END_OF_STACK : _URC_NO_REASON;
}
/* (skips itself, note_alloc and the wrap: the first frame is whoever called malloc, operator new, calloc...) */
static __attribute__((noinline)) int capture(uintptr_t *pc) {
    struct bt b = {pc, 0, 3};
    _Unwind_Backtrace(bt_frame, &b);
    return b.n;
}

static void lock_sites(void) {
    while (__atomic_exchange_n(&site_lock, 1, __ATOMIC_ACQUIRE)) sceKernelUsleep(0);
}
static void unlock_sites(void) { __atomic_store_n(&site_lock, 0, __ATOMIC_RELEASE); }

static uint32_t site_for(const uintptr_t *pc, int depth) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (int i = 0; i < depth; i++) h = (h ^ pc[i]) * 0x100000001b3ull;
    h ^= (uint64_t)depth;
    lock_sites();
    uint32_t at = (uint32_t)(h % SITES), found = NO_SITE;
    for (unsigned i = 0; i < SITES; i++, at = (at + 1) % SITES) {
        struct site *s = &sites[at];
        if (!s->used) {
            if (sites_n >= SITES * 3 / 4) break;   /* (full enough: further callers are counted as dropped) */
            s->hash = h, s->depth = (uint8_t)depth, s->used = 1;
            memcpy(s->pc, pc, sizeof *pc * (size_t)depth);
            sites_n++;
            found = at;
            break;
        }
        if (s->hash == h && s->depth == depth && !memcmp(s->pc, pc, sizeof *pc * (size_t)depth)) {
            found = at;
            break;
        }
    }
    unlock_sites();
    return found;
}

static int slot_put(void *p, uint32_t site, uint64_t w) {
    const uint32_t h = slot_of(p);
    for (uint32_t i = 0; i < PROBE; i++) {
        const uint32_t at = (h + i) & (SLOTS - 1);
        uintptr_t v = __atomic_load_n(&slot_ptr[at], __ATOMIC_ACQUIRE);
        if ((v == 0 || v == SLOT_TOMB) &&
            __atomic_compare_exchange_n(&slot_ptr[at], &v, SLOT_BUSY, 0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
            slot_site[at] = site, slot_w[at] = w;
            __atomic_store_n(&slot_ptr[at], (uintptr_t)p, __ATOMIC_RELEASE);
            return 1;
        }
    }
    return 0;
}

static __attribute__((noinline)) void note_alloc(void *p, size_t n) {
    if (!p || !__atomic_load_n(&prof_on, __ATOMIC_RELAXED)) return;
    {
        const int c = class_of(n);
        __atomic_fetch_add(&class_n[c], 1, __ATOMIC_RELAXED);
        __atomic_fetch_add(&class_bytes[c], (uint64_t)n, __ATOMIC_RELAXED);
    }
    if (__atomic_sub_fetch(&g_left, (int64_t)n, __ATOMIC_RELAXED) > 0) return;
    /* (a sample under way, on this thread (the unwinder's own allocation) or another: this one is not taken) */
    if (__atomic_exchange_n(&g_sampling, 1, __ATOMIC_ACQUIRE)) return;
    __atomic_store_n(&g_left, next_interval(), __ATOMIC_RELAXED);
    const uint64_t w = n >= interval ? n : interval;
    uintptr_t pc[DEPTH];
    const int depth = capture(pc);
    const uint32_t s = depth ? site_for(pc, depth) : NO_SITE;
    __atomic_fetch_add(&samples_all, 1, __ATOMIC_RELAXED);
    if (s == NO_SITE) {
        __atomic_fetch_add(&dropped_sites, 1, __ATOMIC_RELAXED);
    } else {
        __atomic_fetch_add(&sites[s].made, w, __ATOMIC_RELAXED);
        __atomic_fetch_add(&sites[s].samples, 1, __ATOMIC_RELAXED);
        {
            uint64_t m = __atomic_load_n(&sites[s].maxn, __ATOMIC_RELAXED);
            while (n > m && !__atomic_compare_exchange_n(&sites[s].maxn, &m, (uint64_t)n, 1, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
        }
        if (slot_put(p, s, w)) {
            __atomic_fetch_add(&sites[s].live, (int64_t)w, __ATOMIC_RELAXED);
            __atomic_fetch_add(&live_samples, 1, __ATOMIC_RELAXED);
        } else {
            __atomic_fetch_add(&dropped_slots, 1, __ATOMIC_RELAXED);
        }
    }
    __atomic_store_n(&g_sampling, 0, __ATOMIC_RELEASE);
}

static void note_free(void *p) {
    if (!p || !__atomic_load_n(&live_samples, __ATOMIC_RELAXED)) return;
    const uint32_t h = slot_of(p);
    for (uint32_t i = 0; i < PROBE; i++) {
        const uint32_t at = (h + i) & (SLOTS - 1);
        uintptr_t v = __atomic_load_n(&slot_ptr[at], __ATOMIC_ACQUIRE);
        if (v == 0) return;
        if (v != (uintptr_t)p) continue;
        if (__atomic_compare_exchange_n(&slot_ptr[at], &v, SLOT_TOMB, 0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
            __atomic_fetch_sub(&sites[slot_site[at]].live, (int64_t)slot_w[at], __ATOMIC_RELAXED);
            __atomic_fetch_sub(&live_samples, 1, __ATOMIC_RELAXED);
        }
        return;
    }
}

/* ---- the wraps every malloc, free, operator new and delete of the title reaches (PS5_Vulkan tools/radv-link.sh) ---- */
void *__wrap_malloc(size_t n) {
    void *p = ps5w_malloc(n);
    note_alloc(p, n);
    return p;
}
void *__wrap_calloc(size_t count, size_t n) {
    void *p = ps5w_calloc(count, n);
    note_alloc(p, count * n);
    return p;
}
void *__wrap_realloc(void *old, size_t n) {
    void *p = ps5w_realloc(old, n);
    if (p || !n) note_free(old);   /* (refused: the old block is still the caller's) */
    note_alloc(p, n);
    return p;
}
void __wrap_free(void *p) {
    note_free(p);
    ps5w_free(p);
}
int __wrap_posix_memalign(void **out, size_t alignment, size_t n) {
    const int r = ps5w_posix_memalign(out, alignment, n);
    if (!r) note_alloc(*out, n);
    return r;
}
void *__wrap_aligned_alloc(size_t alignment, size_t n) {
    void *p = ps5w_aligned_alloc(alignment, n);
    note_alloc(p, n);
    return p;
}
void *__wrap_memalign(size_t alignment, size_t n) {
    void *p = ps5w_memalign(alignment, n);
    note_alloc(p, n);
    return p;
}
size_t __wrap_malloc_usable_size(void *p) { return ps5w_malloc_usable_size(p); }
void *__wrap_reallocf(void *old, size_t n) {
    void *p = ps5w_reallocf(old, n);
    note_free(old);   /* (refused, reallocf frees it all the same) */
    note_alloc(p, n);
    return p;
}
void *__wrap_reallocarray(void *old, size_t count, size_t n) {
    void *p = ps5w_reallocarray(old, count, n);
    if (p) note_free(old), note_alloc(p, count * n);
    return p;
}
ssize_t __wrap_getline(char **line, size_t *cap, FILE *f) {
    char *old = line ? *line : NULL;
    const ssize_t r = ps5w_getline(line, cap, f);
    if (line && *line != old) note_free(old);
    return r;
}
ssize_t __wrap_getdelim(char **line, size_t *cap, int delim, FILE *f) {
    char *old = line ? *line : NULL;
    const ssize_t r = ps5w_getdelim(line, cap, delim, f);
    if (line && *line != old) note_free(old);
    return r;
}

/* ---- the report ---- */
static void *heap_report(void *arg) {
    const unsigned sec = (unsigned)(uintptr_t)arg;
    const uintptr_t base = hos_ps5_exe_base();
    for (;;) {
        sceKernelUsleep(sec * 1000000u);
        struct ps5_heap_stats st;
        memset(&st, 0, sizeof st);
        ps5_heap_stats(&st);
        int64_t live[SITES], all = 0;
        for (unsigned i = 0; i < SITES; i++) {
            live[i] = sites[i].used ? __atomic_load_n(&sites[i].live, __ATOMIC_RELAXED) : 0;
            if (live[i] > 0) all += live[i];
        }
        hos_ps5_klog_now("[ps5] heap by caller: ~%lld MiB live in the samples (the heap's blocks in use %llu MiB, mapped %llu), "
                         "%u callers, %llu samples (%llu dropped); one every %llu KB",
                         (long long)(all >> 20), (unsigned long long)(st.in_use_bytes >> 20),
                         (unsigned long long)(st.mapped_bytes >> 20), sites_n,
                         (unsigned long long)__atomic_load_n(&samples_all, __ATOMIC_RELAXED),
                         (unsigned long long)(__atomic_load_n(&dropped_slots, __ATOMIC_RELAXED) +
                                              __atomic_load_n(&dropped_sites, __ATOMIC_RELAXED)),
                         (unsigned long long)(interval >> 10));
        int chosen[TOP], n = 0;
        for (; n < TOP; n++) {
            int best = -1;
            for (unsigned i = 0; i < SITES; i++) {
                if (live[i] <= 0) continue;
                int taken = 0;
                for (int k = 0; k < n; k++) taken |= chosen[k] == (int)i;
                if (!taken && (best < 0 || live[i] > live[best])) best = (int)i;
            }
            if (best < 0) break;
            chosen[n] = best;
        }
        for (int k = 0; k < n; k++) {
            const struct site *s = &sites[chosen[k]];
            char pcs[200];
            int m = 0;
            for (int i = 0; i < s->depth && m < (int)sizeof pcs - 20; i++)
                m += snprintf(pcs + m, sizeof pcs - (size_t)m, " %llx", (unsigned long long)(s->pc[i] - base));
            const int64_t grew = live[chosen[k]] - last_live[chosen[k]];
            hos_ps5_klog_now("[ps5] heap site %d: %lld MiB live (%+lld), %llu MiB made, %u samples:%s", k + 1,
                             (long long)(live[chosen[k]] >> 20), (long long)(grew / (1 << 20)),
                             (unsigned long long)(__atomic_load_n(&s->made, __ATOMIC_RELAXED) >> 20),
                             __atomic_load_n(&s->samples, __ATOMIC_RELAXED), pcs);
        }
        /* (HF1) what was made and freed again since the last report, by size class and by caller: the churn that fragments */
        {
            static const char *const names[CLASSES] = {"<1K", "<16K", "<256K", "<4M", ">=4M"};
            char line[400];
            int m = 0;
            for (int c = 0; c < CLASSES; c++) {
                const uint64_t cn = __atomic_exchange_n(&class_n[c], 0, __ATOMIC_RELAXED);
                const uint64_t cb = __atomic_exchange_n(&class_bytes[c], 0, __ATOMIC_RELAXED);
                m += snprintf(line + m, sizeof line - (size_t)m, "%s %s %llu (%llu MiB)", c ? ";" : "", names[c],
                              (unsigned long long)cn, (unsigned long long)(cb >> 20));
            }
            hos_ps5_klog_now("[ps5] heap churn: allocations since the last report by size:%s; mapped %llu MiB, in use %llu MiB, "
                             "free inside %llu MiB, %u segments, %u arenas", line, (unsigned long long)(st.mapped_bytes >> 20),
                             (unsigned long long)(st.in_use_bytes >> 20),
                             (unsigned long long)((st.mapped_bytes - st.in_use_bytes) >> 20), st.segments, st.arenas);
            int64_t freed[SITES];
            for (unsigned i = 0; i < SITES; i++) {
                if (!sites[i].used) { freed[i] = 0; continue; }
                const uint64_t made = __atomic_load_n(&sites[i].made, __ATOMIC_RELAXED);
                freed[i] = (int64_t)(made - last_made[i]) - (live[i] - last_live[i]);
                last_made[i] = made;
            }
            for (int k = 0; k < 10; k++) {
                int best = -1;
                for (unsigned i = 0; i < SITES; i++)
                    if (freed[i] > 0 && (best < 0 || freed[i] > freed[best])) best = (int)i;
                if (best < 0 || freed[best] < (4 << 20)) break;
                const struct site *s = &sites[best];
                char pcs[200];
                int q = 0;
                for (int i = 0; i < s->depth && q < (int)sizeof pcs - 20; i++)
                    q += snprintf(pcs + q, sizeof pcs - (size_t)q, " %llx", (unsigned long long)(s->pc[i] - base));
                hos_ps5_klog_now("[ps5] heap churn site %d: ~%lld MiB made and freed again, largest block %llu KB, %lld MiB live:%s", k + 1,
                                 (long long)(freed[best] >> 20), (unsigned long long)(__atomic_load_n(&s->maxn, __ATOMIC_RELAXED) >> 10),
                                 (long long)(live[best] >> 20), pcs);
                freed[best] = 0;
            }
        }
        for (unsigned i = 0; i < SITES; i++) last_live[i] = live[i];
        if (hos_ps5_heap_extra) hos_ps5_heap_extra();
    }
    return NULL;
}

void hos_ps5_heap_prof_start(void) {
    const char *e = getenv("HOS_PS5_HEAP_PROF");
    if (e && *e == '0') return;
    const char *kb = getenv("HOS_PS5_HEAP_PROF_KB"), *sec = getenv("HOS_PS5_HEAP_PROF_SEC");
    if (kb && atoi(kb) >= 16) interval = (uint64_t)atoi(kb) << 10, g_left = (int64_t)interval;
    const unsigned every = sec && atoi(sec) >= 5 ? (unsigned)atoi(sec) : 60;
    pthread_t t;
    if (pthread_create(&t, NULL, heap_report, (void *)(uintptr_t)every) != 0) return;
    pthread_detach(t);
    __atomic_store_n(&prof_on, 1, __ATOMIC_RELEASE);
    hos_ps5_klog_now("[ps5] heap by caller: one allocation in every %llu KB sampled, the callers holding most reported every %u s "
                     "(HOS_PS5_HEAP_PROF=0: off; ps5/tools/heap_sites.py names them)", (unsigned long long)(interval >> 10), every);
}
