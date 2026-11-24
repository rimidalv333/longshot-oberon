/* (2026-11-21, base PS5) A light's record taken one frame late instead of the light dropped for the frame.
   nx2/oot_patches.py hooks the place in f_80365f00 (main+0x345f00, Rekarina 1.2.8's f_7000345f00).

   The light update takes a light's record (its +0x1540 the frame stamp a shader writes on the GPU) only when the stamp says
   "the frame just before" ([obj+0x18] - 1); otherwise it leaves the light out for that frame. On the console the GPU has
   always finished the frame before by then. Here the game's next frame can run while the GPU still works on the last one:
   the record still holds the stamp of the frame before that, and the light went out for a frame (Rekarina: distant candles
   15-28 % of frames; here the user's clouds and the Temple of Time's light shafts gone for a frame, a few times a second).
   The record is in guest memory the GPU writes in place and the game reads it with plain loads: no invalidate, so the
   readback wait never runs for it. Rekarina wrote those ranges back early; the guard on the reads cost it up to 9 %.

   Here a record whose stamp is at most HOS_LIGHT_LAG frames older than the one wanted (1 by default) is taken as it is: the
   light of the frame before, not none. HOS_LIGHT_LAG=0: the game's own test. */
#include "hos.h"

#include <stdint.h>
#include <stdlib.h>

static int lag = -1;
static volatile long long reads, late_taken, late_left;

int hos_oot_light_late(uint32_t stamp, uint32_t want) {
    if (lag < 0) {
        const char *e = getenv("HOS_LIGHT_LAG");
        int v = e && *e ? atoi(e) : 1;
        lag = v < 0 ? 0 : v > 4 ? 4 : v;
        hos_log("light: a light's record up to %d frame(s) late is taken as it is, not the light left out for the frame "
                "(HOS_LIGHT_LAG; 0: the game's own test)", lag);
    }
    const uint32_t behind = want - stamp;   /* (the frames the record is older than the one wanted; wraps like the game's) */
    const int ok = lag > 0 && behind >= 1 && behind <= (uint32_t)lag;
    const long long n = __atomic_add_fetch(&reads, 1, __ATOMIC_RELAXED);
    if (ok) __atomic_add_fetch(&late_taken, 1, __ATOMIC_RELAXED);
    else __atomic_add_fetch(&late_left, 1, __ATOMIC_RELAXED);
    if (n % 2000 == 0)
        hos_log("light: %lld records not of the frame before; %lld of them up to %d frame(s) late taken, %lld left out "
                "(older or newer)", n, late_taken, lag, late_left);
    return ok;
}
