# Shader cache and first-start rework (1.2.0)

What changed in the shader preparation and the renderer since 1.1.0, why, and what was measured on a base PS5 (Slim).
Every number below comes from the title's own log lines on the console. "Before" runs are the 1.1.0 code, "after" runs
are this release, on the same save and the same route (Hyrule Field, Kokiri Forest, the field again, the daytime Market).

## 1. The first-sight glow (pop-in of bright, unlit objects)

**What players saw:** the first time something was on screen it showed up bright white, or as a "silhouette of light",
and filled in piece by piece: Link's house, Ganondorf and his horse, Kaepora Gaebora, the Market, the wolfos in the Sacred
Meadow, white ground and cyan holes in Hyrule Field's cliffs on a fresh cache. Never a second time.

**Why:** the materials' pipelines weren't built yet, so their draws were left out (up to 990 draws a frame "left out (not
ready)", 200-300 pipeline jobs queued), while the glow, rim and additive passes, whose pipelines were ready, still drew.
The PS5 build only knew the pipelines it had already drawn itself: it never shipped Longshot's pipeline list.

**Fix:** the builder now merges Longshot's pipeline list (`assets/pipeline_states/`: Longshot's pipeline tour of every map,
actor and cutscene, plus 126 lists contributed by its players) into `longshot_pipeline_states_shipped.bin` in the title
folder: 76,290 pipelines of 27,268 programs, 42 MB. These are hashes, formats and draw state only, with no game code: the
console turns them into shaders from the player's own `longshot_shaders.bin`. The first start builds them all before the
title screen.

- A fresh install builds the whole list.
- An install with a cache notices a new or changed list (`cache/longshot_shipped_prepared.bin`), builds only what's missing,
  then closes once.

**Result:** "draws left out" 0 in play, no glow anywhere on the test route.

## 2. A faster first start

**1.1.0 measured (no list, so less work):** ~2 h 50 min. The first step translated ~4 programs a second with the CPU at 14%
of one core. **1.1.0 code with the list (its steps added up):** ~4 h 10 min. **1.2.0 with the list: ~1 h 25 min.**

| Step | 1.1.0 | 1.2.0 |
|---|---|---|
| Shader programs (38,809) | ~2.5 h, ~4 a second | ~41 min, ~70 a second at the start, CPU ~880% |
| The pipeline list | ~7 min with no progress shown | ~18 min, with its own count on screen |
| Pipelines (38,038 graphics, 9,701 compute) | 12-19 a second on ~4 cores (~76 min with the list) | ~57 a second on ~11 cores, ~24 min |
| Saving the pipeline cache | 230 s (84 MB) | under 1 s |

So it's ~3x faster than 1.1.0 would be with the list, and ~2x faster than 1.1.0 was without it, while building about 38,000
more pipelines.

**What made the difference:**
- **One write call instead of hundreds (PW1, NB1).** Every write call into the title folder costs ~60 ms whatever its size,
  and the console's C library sends an `fwrite` out in 4 KB pieces, one call each. Each program record went out as several
  slow calls under one lock, with every worker waiting. Now a preparation's records, the pipeline notes and the cache save
  are held in memory and written with one call. Everything that reads them back flushes first.
- **12 workers instead of 8 (PT1).** The rule gave half of 16 CPUs (8 of the 13 a game can use). It's 12 now on the
  console. `RX_VK_PREPARE_THREADS=<n>` overrides it.
- **The driver's own disk cache is off while preparing (DC1).** Its many small file calls held the pipeline step to ~4 cores.
  Play starts keep it on. `RX_VK_PREPARE_DRIVER_CACHE=1` keeps it on while preparing too.
- **The pipeline list's step has its own count (LP1)** on the start-up screen and in the log, so it no longer looks stuck.

**Credit:** the thread count follows ProjectReLink (`prepare_threads`, every CPU but one).

## 3. Less memory in play: the pipeline pack

With the list, the single pipeline cache grew to 454 MB, read whole at every start and kept by the driver. That memory was
missing from the textures: 13 memory emergencies and textures made small in the Market.

**Fix (PK1, on by default):** each program's graphics pipelines go into a piece of their own in
`cache/longshot_vk_pipelines.pack` (+ `.idx`). Play reads only the piece of a program it creates (one read, ~0.5 ms). The
pipeline cache kept in memory now holds the compute pipelines alone. `RX_VK_PIPELINE_PACK=0` goes back to the one cache.

| Same route | 1.1.0 code + list | 1.2.0 (first session) | 1.2.0 (final run) |
|---|---|---|---|
| Pipeline cache held by the driver | 454 MB | 107 MB | 107 MB |
| Free memory, median | 1,083 MB | 1,483 MB | 1,556 MB |
| Free memory, lowest | 558 MB | 584 MB | 609 MB |
| Memory emergencies | 13 | 4 | 0 |
| Stalls over 300 ms | 2 | 0 | 0 |

The pack is ~660 MB on disk.

## 4. Frame rate

The game is held to 30 fps, so the gains show as headroom and steadier frames, not a higher number.
- The 30 holds in the daytime Market while turning the camera, with the full list loaded (2,000-2,499 draws: 29.8 fps
  average, the same as before while carrying ~38,000 more pipelines).
- **The reflection cube on every 3rd turn (CM1, on by default).** OoT repaints one face of a reflection cube every frame by
  drawing the scene around the camera again: 400-500 draws a frame in the Market (~17-20% of the frame), 60-160 in the
  field. Now each face is repainted every 3rd time its turn comes. In the Market that's ~150 draws a frame instead of ~440.
  Reflections still look the same (playtested at 2 and 3). `RX_VK_CUBEMAP_EVERY=1` draws every turn, as before.
- **No light flash on area changes (CM1b).** After a load or a black transition (a frame under 250 draws), every cube face is
  drawn on every turn for 90 frames, so the new area never reflects the old one's lighting for a moment.

## 5. Fixes found along the way

- **"Compile shaders: All" did nothing on an existing cache.** It now predicts on an already prepared folder too (PA1). It
  isn't needed anymore: the shipped list covers more.
- **The title closed itself at every start after a fresh build.** The start's cold check asked the driver's disk cache,
  which is off on purpose while preparing. The check is skipped with the pack, and the title only closes after a start that
  really built something.

## 6. Tried and left off

- **Letting go of unused kept-ready pipelines on a timer (KR1, `RX_VK_KEPT_READY_S`, default 0 = off).** With the list, a
  program's create keeps every noted state ready, ~10,500 pipelines against ~3,800 before the list, most of them never drawn
  by a given player. Letting go of the ones no draw took in 120 s freed memory (median free 1,522 MB), but the ones needed
  again were rebuilt on the frame thread: 12.7 ms a frame on average, up to 124 ms (0.3 ms normally). That meant drops to a
  few fps and slower transitions. Off until it's done as below.
- **A lower texture guard.** In the late Market free memory dips to ~610 MB, ~100 MB above the 512 MB emergency line, and
  that's with textures already made small. A lower `HOS_PS5_TEX_RESERVE_MB` would push those dips under the line, so the
  guard stays at 1024.

## 7. Next: rebuilding freed pipelines on a background thread (planned)

The pieces already exist. Pipelines nobody prepared are already built by worker threads at their first draw, and the draw
waits a frame or two. The change would send freed pipelines down that path instead of rebuilding them on the frame thread.
On its own that's a small change.

The catch is what you see while it waits. That draw is skipped until its pipeline is ready, so an object can blink out for a
frame or two when you come back to it. That's pop-in, the thing this release removes. So it only makes sense combined with a
memory-pressure trigger: free pipelines only when free memory gets near the guard, like the Market dips to ~600 MB. Then the
rare blink replaces a texture downscale, and normal play never sees it.

Doing it properly is a few hours of work plus test runs, so it's left for a later update.

Other open items:
- The 180-degree turn hitch in the field and Kokiri: textures thrown out and loaded again (spread the reload over frames).
- The pipeline list's read-back step (~15 min of small reads) and the step-1 slowdown over its length.
- A ~138 s load at the end of the first start (the mesh fallback programs kept ready).
- The FSR upscaler's own compute pipelines are compiled the first time they run (a few one-time stalls in the very first
  session after deleting `radv-shader-cache/`).

## 8. Known issue: "Can't continue using game or app" when the TV turns off

Seen with an AV receiver between the PS5 and the TV, and the console set to 120 Hz and HDR. When the TV turns off, the
receiver stops offering that video mode, and the PS5 suspends the title (`NOT_SUPPORTED_BY_DISPLAY` in the kernel log). A
jailbroken title can't come back from that suspend. It isn't rest mode, and pressing the PS button is unaffected. Leave the
TV on during the first start, or set 120 Hz Output and HDR to Off in Settings > Screen and Video > Video Output.

## 9. Switches

| Switch | Default | What |
|---|---|---|
| `RX_VK_PIPELINE_PACK` | on | per-program pipeline pieces; `0`: one pipeline cache |
| `RX_VK_PREPARE_THREADS` | 12 on the PS5 | the preparation's workers |
| `RX_VK_CUBEMAP_EVERY` | 3 | reflection cube faces on every Nth turn; `1`: every turn |
| `RX_VK_KEPT_READY_S` | 0 (off) | let go of kept-ready pipelines no draw took in N seconds (see §6) |
| `RX_VK_PREPARE_DRIVER_CACHE` | off | `1`: keep the driver's disk cache on while preparing |
| `RX_VK_DISK_STDIO` | off | `1`: the old write path |

## Credits

- **Longshot** and its pipeline tour, and everyone who contributed a pipeline list (`assets/pipeline_states/contrib/*.md`).
  The merge tool and the state-list code are Longshot's.
- **ProjectReLink** for the preparation thread count.
