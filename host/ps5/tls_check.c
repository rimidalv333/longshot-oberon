/* (base PS5) Native thread-local storage, checked once at the start.

   The console's compiler wrapper (PS5_Vulkan tooling/prospero-clang18) builds with -femulated-tls: every thread_local
   access is a call to __emutls_get_address (pthread_getspecific, then an index into the thread's array). The replay's
   hot path reaches dozens of them a draw (vk_draw.cpp's cur, dw(), pass(), pipe_fast, the sampler table, the descriptor
   fingerprints; the host's guest-thread pointer and nn_os_sync's object cache), on the one thread that sets the frame
   rate. ps5/build.py with PS5_NATIVE_TLS (on by default; PS5_NATIVE_TLS=0: emulated as before) builds the host and the
   renderer with -fno-emulated-tls -ftls-model=initial-exec instead: an access is one %fs-relative load.

   That needs the system loader to give the executable's PT_TLS block to every thread, as it does for a title and as it
   does for sce_module/libc.prx's own TLS. If it did not, two threads would see the same storage (or the initial image
   would be missing), and the renderer would fail in ways far from the cause. This check makes that a clear, immediate
   title-only stop instead: a second thread must see its own, freshly initialized copy. */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

void hos_log(const char *fmt, ...);

static _Thread_local uint64_t tls_probe = 0x5EEDF00DCAFEULL;   /* (initialized: .tdata, the image each thread copies) */
static _Thread_local uint64_t tls_zero;                        /* (zeroed: .tbss) */

static void *tls_other(void *out) {
    uint64_t *r = (uint64_t *)out;
    r[0] = tls_probe, r[1] = tls_zero, r[2] = (uint64_t)(uintptr_t)&tls_probe;
    tls_probe = 7, tls_zero = 9;   /* (must not reach the first thread's) */
    return NULL;
}

void hos_ps5_tls_check(void) {
    tls_probe += 1, tls_zero = 3;   /* (this thread's own values, before the other starts) */
    uint64_t seen[3] = {0, 0, 0};
    pthread_t t;
    if (pthread_create(&t, NULL, tls_other, seen) != 0) {
        hos_log("[ps5] tls: the check's thread could not start; not checked");
        return;
    }
    pthread_join(t, NULL);
    const int ok = seen[0] == 0x5EEDF00DCAFEULL && seen[1] == 0 && seen[2] != (uint64_t)(uintptr_t)&tls_probe &&
                   tls_probe == 0x5EEDF00DCAFEULL + 1 && tls_zero == 3;
#if defined(__clang__) && !defined(PS5_EMULATED_TLS)
    const char *how = "native (PS5_NATIVE_TLS)";
#else
    const char *how = "emulated (-femulated-tls)";
#endif
    if (ok) {
        hos_log("[ps5] tls: %s, each thread its own copy: checked", how);
        return;
    }
    hos_log("[ps5] tls: %s BROKEN: the second thread saw %#llx / %#llx at %#llx (this thread %#llx / %#llx at %p). The loader "
            "did not give the executable's TLS block to each thread: rebuild with PS5_NATIVE_TLS=0. The title stops now",
            how, (unsigned long long)seen[0], (unsigned long long)seen[1], (unsigned long long)seen[2],
            (unsigned long long)tls_probe, (unsigned long long)tls_zero, (void *)&tls_probe);
    fflush(NULL);
    _Exit(4);
}
