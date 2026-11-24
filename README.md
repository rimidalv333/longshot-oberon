<p align="center">
  <img src="assets/banner.jpg" alt="Longshot Oberon: the Longshot on the PS5's Oberon die" width="100%">
  <br>
  <sub><sup>Die shot of AMD Oberon (PS5 SoC) by Fritzchens Fritz, via <a href="https://www.tomshardware.com/news/amd-oberon-playstation-5-soc-die-says-cheese-to-the-camera">Tom's Hardware</a></sup></sub>
</p>

<h1 align="center">Longshot Oberon</h1>

<p align="center">
  <b>The Legend of Zelda: Ocarina of Time (Switch 2) recompiled, running natively on the base PlayStation 5</b><br>
  Title <code>PPSA93339</code> · locked 30 fps · built from your own game file in one command
</p>

<table align="center">
  <tr>
    <td><a href="#introduction">Introduction</a></td>
    <td><a href="#upstream-longshot-for-pc">Upstream: Longshot for PC</a></td>
    <td><a href="#status-and-performance">Status and performance</a></td>
  </tr>
  <tr>
    <td><a href="#install-and-play">Install and play</a></td>
    <td><a href="#addons-mods-and-button-prompts">Addons: mods and button prompts</a></td>
    <td><a href="#settings">Settings</a></td>
  </tr>
  <tr>
    <td><a href="#custom-implementations">Custom implementations</a></td>
    <td><a href="#manual-fixes-and-tests">Manual fixes and tests</a></td>
    <td><a href="#build-from-source">Build from source</a></td>
  </tr>
  <tr>
    <td><a href="#credits">Credits</a></td>
    <td><a href="#license">License</a></td>
    <td></td>
  </tr>
</table>

---

## Introduction

Longshot Oberon is a port of [Longshot](#upstream-longshot-for-pc), the static recompilation of *The Legend of Zelda:
Ocarina of Time* for Nintendo Switch 2, to the **base PS5**. *Oberon* is the codename of the base PS5's system-on-chip,
and this port is tuned for it specifically: its 12 GB of memory for a title, its 6.5 cores' worth of CPU time, and its
RDNA2-class GPU driven through RADV, Mesa's Vulkan driver, built for the console.

The game's ARM64 code is translated to C ahead of time and compiled for the console's Zen 2 CPU. The game's NVN graphics
calls are recorded and replayed through Vulkan, and its shaders are translated to SPIR-V. Nothing is emulated at run time.

**The builder.** This repository holds no game files. Its release, `release.zip`, carries the source and every tool the
build needs, prebuilt: the compiler, Python, the PS5 payload SDK, the graphics driver, FFmpeg and the rest. You give it
your own game file and it builds the title folder:

```
./build.sh /path/to/oot.nup
```

Out comes `dist/PPSA93339`, ready to copy to the console: the executable, the game's shader list, the PS5 button
prompts, the data mods, and the settings this port ships with. Nothing is downloaded and nothing is installed on your PC.
See [Install and play](#install-and-play).

**What it is built on.** The port started from the very first, primitive PS5 Hookshot release
([mihawk-99/PS5_Hookshot](https://github.com/mihawk-99/PS5_Hookshot) `ps5-v0.1.0-beta.2`). That release booted the game on a
PS5; on a base console it hung the GPU on the first 3D frame and ran out of memory at the first area change. From there
it was merged with Longshot v3.0.0-rc.4 and has followed Longshot's releases and nightlies since, porting their fixes
by hand, with the base-console work described below on top.

**What it is not.** It is not a fork of, and does not build on,
[heydemoura/PS5_Longshot](https://github.com/heydemoura/PS5_Longshot). That project was initially aimed at the PS5 Pro (inherited from Mihawk's) and is a
separate line of work. Two fixes are shared because both projects took them from Longshot (the voices' romfs paths,
Longshot #1346, and the stepped character-shadow storage, Longshot #1256).

---

## Upstream: Longshot for PC

**[Longshot](https://git.magicbean.dev/MagicBeanDev/Longshot)** by MagicBeanDev is the PC version and this port's
upstream: the renderer, the runtime, the lifter and most of the fixes come from it. If you want to play on a PC, use
Longshot. Longshot itself grew out of NotRexed's [Hookshot](https://git.notrexed.dev/Hookshot/Hookshot).

What was taken from Longshot, release by release and PR by PR, is in the [Credits](#credits).

---

## Status and performance

> [!NOTE]
> **Locked 30 fps, stable about 99% of the time in Kokiri Forest, Hyrule Field and the Castle Town market.**
> **Castle Town works and doesn't crash.** A full session has been played from a new game to Castle Town with memory
> holding all the way: no leaks, no low-memory emergencies.

The numbers behind it, from the console's own logs (base PS5, handheld profile, the current build, frame rate left
uncapped so the headroom shows; with the 30 lock on, any window at 30 fps or above is a steady 30):

| Draws a frame | Average fps (lowest) | 5-second windows under 27 fps |
|---|---|---|
| 1,000–1,499 | 48.1 (35.5) | 0 |
| 1,500–1,999 | 42.8 (32.8) | 0 |
| 2,000–2,499 | 39.3 (30.9) | 0 |
| 2,500–2,999 | 33.9 (28.8) | 0 |
| 3,000+ | 30.6 (26.1) | 1 of 4 |

- 97.9% of all gameplay windows in that run were at 29.5 fps or above, with a per-draw diagnostic still switched on that
  has since been removed from the shipped settings.
- **Memory:** lowest free memory 646 MiB, median 1,370 MiB; no emergencies. The texture reserve keeps 1 GB free for
  the game.
- **CPU:** the limit is the renderer's replay of the game's draws, on one thread. It is now **8.4 µs a draw**, down
  from 12–14 µs at the first boot. The GPU has headroom (about 10 ms a frame on average).
- **Picture:** the game's handheld profile (3D at 1536×864) with AMD FSR 3.1 as anti-aliasing, sharpened before the HUD
  and shown at 1920×1080. Docked 1080p works too, but costs 130–450 MB more memory and doesn't raise the frame rate; it
  ran short of memory in the market.

**Known issues**
- The building wall that lit up as you came close is fixed by capping the local-light list (`RX_VK_LIGHT_CAP=8`). With
  the cap, a screen cell takes at most 8 local lights (torches, lamps, Navi's glow); the sun is unaffected.
- One game-code crash was seen once, in an experiment that ran the game at 9 fps. It has not been seen in normal play.
- The first start translates every shader (several hours, once). See [First start](#first-start).

---

## Install and play

### What you need

- **The game:** *The Legend of Zelda: Ocarina of Time* for Nintendo Switch 2, version 1.1.1 (title `0400486024DEC000`,
  v131072), dumped by yourself as a `.nup` file. The builder checks it against this dump:
  - size: 25,020,741,061 bytes
  - SHA-256: `5a23bbbb382193020eb7235ea21c368bfd8473e7795d24e17a487f43380ce74c`

  Other versions or dumps are not supported (`--skip-hash-check` builds anyway, at your own risk).
- **The console:** a **base PS5** (disc or digital) on a jailbroken firmware with a homebrew environment (kstuff,
  elfldr, ShadowMountPlus) and an FTP server (ftpsrv). The Pro has more memory and native mesh shaders; this port does
  not target it.
- **The PC:** 64-bit Linux, or Windows 10/11 with WSL2 (Ubuntu). At least 8 GB of memory for the build (the builder runs
  one compile per 6 GB), about 10 GB of free disk space, and 30–120 minutes depending on your cores.

### Build the title

1. Download `release.zip` from the [Releases](../../releases) page.
2. Unzip it **inside Linux or WSL** (your Linux home folder, not a Windows drive: it builds several times faster there).
   On WSL, open *Ubuntu* and run:
   ```bash
   cd ~
   cp /mnt/c/Users/<you>/Downloads/release.zip .
   sudo apt install -y unzip    # if unzip is missing
   unzip release.zip
   cd longshot-oberon
   ```
3. Build, giving it your game file (a Windows path is under `/mnt/<drive>/`):
   ```bash
   bash build.sh /mnt/d/Games/oot.nup
   ```
   Options: `--jobs N` (files compiled at once; fewer if the build is killed for memory), `--out DIR` (also copy the
   finished folder there). An interrupted build continues where it stopped when you run it again.

The result is `dist/PPSA93339`. Everything in it that was made from your game (`eboot.bin`, `image.bin`,
`longshot_shaders.bin`, `prompts/`) is yours alone: don't share it.

### Put it on the console

1. Copy the whole `PPSA93339` folder to `/data/homebrew/PPSA93339`.
2. Copy your game file to `/data/lichii/roms/oot/oot.nup` (make the folders; the name must be `oot.nup`).
   `/data/prosperoeden/roms/oot/oot.nup` works too.
3. Start **Longshot Oberon** from the home screen (ShadowMountPlus puts it there) or from your homebrew launcher.

### First start

The first start translates every shader of the game into `/data/homebrew/PPSA93339/cache/` (about 6.8 GB, several
hours). The title closes itself when it's done, because the translation's memory isn't given back. Start it again to
play. Later starts load from the cache. The cache is rebuilt only when a build changes the shader translator, and each
build prints its stamp (`2ad58d0865030a27` for this release).

Coming from the old `PPSA99024` Hookshot folder? Move its `cache/` into `PPSA93339/` and you keep the prepared shaders.
Then rename `hookshot_env.txt` to `longshot_env.txt`: this build reads only the new name.

---

## Addons: mods and button prompts

The title reads *romfs* mods from its `mods/` folder (`mods/<name>/romfs/...`, as emulators lay them out). A folder whose
name starts with `_` is skipped, so renaming a folder turns a mod on or off. The builder puts these in `mods/`:

| Mod | On by default | What it does | Measured |
|---|---|---|---|
| `PerfShadow500` | yes | Sun-shadow distance of the first cascade 3000 → 500 (`gsys/ModelSceneConfigParam`) | 2,500–2,999 draws: 30.7 → 34.3 fps; lighter scenes unchanged |
| `_PerfShadow200` | no | The same at 200 | Further, more visible cut |
| `_PerfShadowLite` | no | 200, and two shadow cascades instead of three | Untested |
| `Saturation115` | yes | Turns on the game's own (shipped but disabled) colour-correction step, at saturation 1.15 with a neutral tint (`agl/ColorCorrectionParam`) | Visual only |
| `_Saturation130` | no | The same at 1.30 | Visual only |

Use one shadow mod and one saturation mod at a time: each pair replaces the same file.

**PS5 button prompts.** The builder runs Longshot's prompt generator on your game files. It redraws the game's button
icons (dialogue, hints, menus, the ocarina notes, the pause-menu controller) for other controllers from Kenney's CC0
Input Prompts, and writes them to `prompts/`. `longshot_options.ini` ships with `button_prompts=4`: PS5 icons. They
follow your button bindings.

---

## Settings

Two text files beside `eboot.bin` hold the settings. Edit them on the console over FTP.

- **`longshot_options.ini`**: the in-game menu's options (resolution, upscaler, FOV, controls, button prompts…). The
  shipped one is the tested setup: FSR 3.1 (`dlss=3`) with the Lanczos output filter, handheld profile, field of view 100,
  PS5 prompts, anisotropic filtering 4×, console-level shadows (`memory_budget=1`).
- **`longshot_env.txt`**: the engine switches, `NAME=value` per line, `#` for comments. The shipped file is the tested
  baseline. The most useful lines:

| Line | Meaning |
|---|---|
| `HOS_PERF_30=1` | Locked, evenly paced 30: each frame shown for exactly two vblanks |
| `HOS_KLOG=0` | Quiet log, worth 1–2%. Remove it when you want a log to read |
| `HOS_HANDHELD=1` | The game's handheld profile (3D at 1536×864). `0` with `HOS_OUTPUT_RES=1920x1080`: docked 1080p (more memory) |
| `RX_VK_RECORDER=thread` | Vulkan commands recorded on a thread of their own (Longshot #1866): 9.4 → 8.4 µs a draw |
| `RX_VK_READBACK_LATE=newest` | The game's reads of GPU results don't wait for the newest frame: 4.2 → 0.8 ms of waiting a frame |
| `RX_VK_OCC_QUERIES=0` | No occlusion queries: heavy scenes 22.9 → 28–29 fps, about 200 MB more textures |
| `RX_VK_UPLOAD_BUDGET_MS=20` | Spreads texture uploads over frames: frames with uploads over 40 ms 16 → 3 |
| `RX_VK_LIGHT_CAP=8` | The local-light cap (see [Known issues](#status-and-performance)) |
| `RX_VK_UP_SHARPEN=15` | Sharpening of the 3D before the HUD, 0–100 |
| `RX_VK_FRAMEGEN=1` | Frame generation (PS5_MihawkGen) to 60 shown, off by default |

For a test run, `longshot_test_env.txt` in the title folder replaces `longshot_env.txt` entirely and accepts any variable.
Delete it before playing.

---

## Custom implementations

Built for this port, for the base PS5. Each has a switch to turn it off. They are listed by area.

### Memory: 12 GB for the game, the driver and every texture

- **Memory guard** (`host/ps5/win32_ps5.c`). At start a 128 MB *parachute* of memory is taken and held. Under 1,024 MB
  free, new textures are made smaller. Under 512 MB the renderer enters *emergency* mode and drops textures unused for a
  few frames. If memory still runs out, the parachute is handed back at once. Under 192 MB for 2 seconds after that, or
  under 64 MB at any point, the title saves its caches and closes itself. This is the reason a memory problem ends the
  game and never takes the whole console down. (`HOS_PS5_MEM_GUARD`, `_PARACHUTE_MB`, `_MEM_LOW_MB`, `_MEM_FLOOR_MB`)
- **Swap file, the cold store** (`HOS_PS5_SWAP`). Game memory not touched for 90 seconds is written to
  `cache/longshot_swap.bin` and its memory given back, while free memory is under 2.5 GB. To find cold memory it watches
  units by making them inaccessible and catching the first touch, which brings a unit back in about 0.5 ms. In handheld
  sessions it gave back 312–952 MB. In one market session it kept the console above the 512 MB emergency line it would
  otherwise have crossed. It releases its lock while it writes, so the game never waits on it (`HOS_PS5_SWAP_YIELD`).
- **Zero reclaim.** Every 20 seconds, game memory that held only zeros on two looks goes back to the pool. Memory
  handed to the GPU or to file reads is never touched. In early runs it gave back about 400 MB. (`HOS_PS5_ZERO_RECLAIM`)
- **Cold-memory meter.** It measured how much game memory stays untouched for 1, 5 and 10 minutes, which is what showed
  that a swap file would pay off. (`HOS_PS5_COLD`)
- **Texture reserve and budget.** 1 GB is always kept free for the game, the renderer's texture budget is 1,280 MB, and
  textures are capped at 1024 texels. The 2048 cap is held in code, because it once crashed the whole console.
  (`HOS_PS5_TEX_RESERVE_MB`, `RX_VK_VRAM_MB`, `RX_VK_TEX_MAX_DIM`)
- **Dedicated images only at about 1 MiB and up, and only while memory is short.** An "every image dedicated" experiment
  at 2 MiB and up once crashed the console into safe mode, so this rule is fixed.
- **Textures don't age in pause and menu frames.** Opening the inventory no longer evicts the world's textures, which
  removed the unpause hitch. (`RX_VK_EVICT_PAUSE_AGE`)
- **Packed shader code freed** once a program is loaded: about 100–120 MB of heap. (`RX_VK_DROP_PACKED`)
- **Depth images never filled from game memory**, which never holds their contents. One 2400×2400 shadow map upload went
  from 36 ms to nothing. (`RX_VK_DEPTH_UPLOAD`)
- **Close after shader preparation.** The translation's heap is not given back, so a start that translated shaders
  closes cleanly instead of playing on with gigabytes missing. (`RX_VK_PS5_PREPARE_STAY=1` keeps playing)
- **Zero-filled data (.bss) kept under 176 MB**, with the check at the link. Above about 188 MB the console refuses
  to start the title, and on one firmware the refusal crashed the console's system process. Large tables live on the
  heap instead. (`PS5_BSS_LIMIT_MB`)

### GPU: RADV on Oberon

- **Mesh shaders as compute** (`RX_VK_FORCE_MESH_FALLBACK=1`, `RX_VK_FB_MULTI=1`). PS5_Mesa describes every PS5 GPU as
  GFX10.3, but the base console has GFX10.1-style primitive shaders and no mesh shaders, so the native task/mesh path
  hangs the GPU on the first 3D frame. Task and mesh programs run as compute shaders instead. A GPU-side gate draws their
  indirect draws record by record, which brings back the GPU-culled foliage.
- **The driver's NGG geometry path off** (`RADV_DEBUG=nongg`, set on every PS5; `HOS_PS5_NGG=1` keeps it). It is the
  same mismatch: with NGG on, long passes of lit draws stalled the GPU (the Kakariko gate, the night market's pine tree)
  until the console killed the title. Off, the hang was gone and the frame rate was unchanged.
- **GPU-hang watchdog.** After 3 seconds without a finished submission the title ends itself, with a GPU state dump in
  the log. (`RX_VK_GPU_HANG_SEC=3`)

### CPU: the replay and the game's threads

- **Native thread-local storage** for the runtime and the renderer. The SDK's compiler wrapper emulates TLS, which made
  every access a library call. With native TLS each access is one load. It needed its own linker layout, because the
  console's loader rejects 64-byte TLS alignment. (`PS5_NATIVE_TLS`)
- **Renderer profile-guided optimisation for the console** (`PS5_VK_PGO`). A counting build writes its profile on the
  console (`host/pgo_rt.c`), and the release build is optimised with it: replay 11.2–11.6 → 10.7 µs a draw. The profile
  ships in `ps5/profiles/vk.profdata`.
- **ThinLTO for the renderer** (`PS5_VK_LTO=thin`): 10.7 → 10.1 µs a draw, and 26.7 → 28.7 fps at 2,500–2,999 draws.
- **The 680 hottest game functions at -O2** (Longshot's hot list), split into eight pieces so they compile in parallel.
- **Locked 30, paced like a console** (`HOS_PERF_30`). The game steps exactly 1/30 s each frame, and the PS5's
  video output shows each frame for two vblanks through PS5_Mesa's flip-rate call. Movies keep their own rate.
- **Quiet log** (`HOS_KLOG=0`): only errors and crash lines go out.
- **Game-file dump for data research** (`HOS_FS_DUMP`). It made the shadow and saturation mods possible.

### Picture and sound

- **The sky fix.** New half-screen depth images start at "far" (1.0) instead of 0.0, so the Hyrule Field clouds no longer
  drop out for single frames.
- **Depth-comparison dummy.** Empty shadow-map slots read as lit, not black: correct shading where the game binds none.
- **5.1 mixed down to stereo** for the console's output.
- **Black props fixed.** The shader translator's guest page table now starts at the PS5 heap's address (16 GB), so the
  black boxes and props and the missing grass are gone.

### The builder

- `build.sh` and the bundled `deps/`. A self-contained clang/lld 18, Python 3.12 with the build's packages, and every
  PS5 library prebuilt, so a build needs nothing from the PC but bash and coreutils.
- It checks the game file's size and SHA-256 and lifts the game with Longshot's hot list. It makes the shader list and
  the PS5 button prompts from the game's own files, builds with the console profile and ThinLTO, and lays out the title
  with the shipped settings and mods.
- Checked against the development build: from the same `.nup`, the builder produces identical game code, an identical
  shader list and identical prompts.

---

## Manual fixes and tests

Everything here was found and confirmed by playing on a base PS5 with a kernel log (klog) running: more than 150 logged runs.
Each change was tested on its own, with a backup and an off switch, and the logs are the evidence.

| Problem | What was seen | Cause | Fix |
|---|---|---|---|
| GPU hang on the first 3D frame | Black screen, console GPU reset | Native mesh shaders on a GPU without them (see above) | Mesh programs as compute (`RX_VK_FORCE_MESH_FALLBACK`) |
| Out of memory at the first area change | Opening the door to Kokiri Forest: free memory hit 5 MB, GPU protection fault, the system UI restarted | 7–8 GB of game memory plus the driver's buffers plus textures over 12 GB | Memory guard, zero reclaim, texture reserve, compaction with 16 MB image blocks |
| Whole-console crash | The console restarted | `RX_VK_TEX_MAX_DIM=2048` | 1024-texel cap held in code |
| Console into safe mode | Needed a system reinstall | Every image of 2 MiB and up given dedicated memory | ~1 MiB rule, only while memory is short |
| Black props, black boxes, missing grass | All over the world | Every pointer the shaders loaded from a game pool read 0: the translator's page table covered 64 GB and 508 GB, the PS5 heap is at 16 GB | Page table window at the heap's address (found by translating the programs offline and reading their SPIR-V) |
| Voices missing | Narration and voice lines silent | Romfs paths with `//` | Longshot #1346 |
| Title refused to start, console system process crashed | "Not enough flexible memory to load executable" | .bss over the console's flexible memory | Tables to the heap, 176 MB check at the link |
| Clouds flashing in Hyrule Field | Clouds gone for single frames | Recreated half-size depth images started at 0.0, so the clouds failed their depth test | The sky fix (see above). It took 30+ test runs: every other rule tried either whitened the world or broke after the Temple of Time |
| White world frames | Single white frames, worst in the market | An experimental second read of the game's render conditions | That experiment switched off for good |
| Temple of Time light rays flashing | The rays flickered | An exposure-meter reading that came back zero reused a stale result | That hold switched off |
| GPU hang on lit draws | Freeze, then the title killed: the Kakariko gate route and the night market's pine tree | RADV's NGG geometry path on GC 10.1.3 | NGG off. Ruled out on the way, one run each: resolution, the breakable crates, cold store, VRAM compaction, texture eviction, the light-list loop, memory, occlusion queries, draw state, vertex data, any single program, shader loops, primitive binning |
| A wall lighting up as you approach | One building lit far too bright | The lit shaders' local-light list held up to ~24,000 lights a cell | `RX_VK_LIGHT_CAP` |
| Camera-turn hitches | 30–100 ms frames when turning | Hundreds of textures uploaded in one frame | Layer-batched uploads, ASTC read in place (2.6 GB not copied a session), the upload budget, the ASTC decoder with LDS scratch |
| Unpause hitch | A stall after the inventory | Textures evicted as "unused" during the pause | Pause frames don't age textures |
| Docked 1080p in the market | Two memory emergencies | 130–450 MB more for the renderer at 1080p | Ships in the handheld profile at 1080p output; 1440p tested too (another ~500 MB) |
| Slow frames from the present | ~3 ms waiting on the present's copy | A submission of its own | The present's copy merged into the frame's last submission |
| Neighbouring-area preload loop | A reload loop at the Gerudo Valley gate | The preloaded neighbour's choice flickering | Longshot #1814 / #1870 and ReLink's "hold only a flip back" rule |

Also measured and dropped, so nobody has to try them again: keeping textures longer (no gain), tiered texture shrinking
(memory emergencies), a CPU core mask (9% slower), a descriptor fast path (not worth it).

---

## Build from source

The [release zip](#build-the-title) is this repository plus `deps/`. To build from a clone instead:

```bash
git clone https://github.com/rimidalv333/longshot-oberon.git
cd longshot-oberon
# take deps/ from the release zip of the same version:
unzip -q ../release.zip 'longshot-oberon/deps/*' -d /tmp/lo && mv /tmp/lo/longshot-oberon/deps .
bash build.sh /path/to/oot.nup
```

What `build.sh` runs, step by step, if you want to do it by hand (from the repository root, with
`deps/llvm/bin` and `deps/python/bin` first in `PATH`):

```bash
ulimit -s unlimited
export PS5_CLANG=$PWD/deps/llvm/bin/clang LLVM_CONFIG=$PWD/deps/llvm/bin/llvm-config \
       PS5_PAYLOAD_SDK=$PWD/deps/ps5-payload-sdk PS5_VULKAN=$PWD/deps/PS5_Vulkan \
       PS5_MIHAWKGEN=$PWD/deps/PS5_MihawkGen PS5_DEPS=$PWD/deps
# 1. the game's program out of the .nup, lifted to C with Longshot's hot list
python3 -I nx2/nup.py oot.nup --only exefs --extract work/game
OOT_WORK=$PWD/work/lift A64_HOT_FUNCS=$PWD/ps5/profiles/hot_funcs_longshot_v3.txt A64_HOT_CHUNK=1 \
  python3 -I -X utf8 nx2/lift.py --standalone --write --exefs work/game/exefs --work work/lift
python3 -I nx2/split_hot.py work/lift/sa/gen
# 2. the shader list and the button prompts, from the game's files
python3 -I nx2/nup.py oot.nup --only romfs --extract work/rx \
  --romfs-parts "Shader/,Layout2Shader/,Lib/,Movie/Ndve/,Effect/,Pack/ZsDic.pack.zs,System/Resource/,UI/Layout2Archive/,Font2/"
python3 -I nx2/shader_census.py work/rx/romfs --out longshot_shaders.bin
python3 -I nx2/prompt_assets.py --romfs work/rx/romfs --out prompts
# 3. compile and link, with the console profile and ThinLTO
PS5_VK_PGO=$PWD/ps5/profiles/vk.profdata PS5_VK_LTO=thin python3 -I ps5/build.py --jobs 4
```

Then copy `longshot_shaders.bin`, `prompts/` and the contents of `title/` into `dist/PPSA93339/`.

**Where things are**

| Path | What |
|---|---|
| `host/` | The runtime that stands in for the Switch's OS and SDK; `host/ps5/` is the PS5 layer (memory guard, swap file, romfs from the `.nup`) |
| `gpu/vk/` | The Vulkan renderer and Eden's shader recompiler |
| `nx2/`, `third_party/a64-kit/` | The lifter, the patches applied to the lifted game, the `.nup` reader, the shader list and prompt tools |
| `ps5/` | The PS5 build (`build.py`), link (`tools/link.sh`), title metadata and icon (`sce_sys/`), the renderer profile and hot list (`profiles/`) |
| `title/` | The settings and mods that ship in the title folder |

**Rules for changes**
- Don't change the *stamped* files (`gpu/vk/vk_shader.cpp`, `vk_bank.cpp`, `vk_state.h`, `gpu/vk/eden/`, `sirit/`,
  `compat/`) unless you mean to. Any change to them makes every console translate all its shaders again. The build
  prints the stamp.
- Nothing that raises memory use goes in without a guard that backs off under the texture reserve.

---

## Credits

**This port** by [vladimir (rimidalv333)](https://github.com/rimidalv333), developed with the help of Claude
(Anthropic).

**Upstream and the projects ported from**

| Project | By | What was taken and how it was used |
|---|---|---|
| [Longshot](https://git.magicbean.dev/MagicBeanDev/Longshot) | MagicBeanDev and contributors | The base: renderer, runtime and lifter, merged at v3.0.0-rc.4 (`53f96d78`). Ported by hand since, through rc.6–rc.8, v3.0.0, v3.1 and the nightlies up to nightly-797: #341, #391, #446, #472, #637, #903, #910, #923, #924, #936, #943, #952, #977, #1031, #1091, #1150, #1158, #1162, #1167, #1188, #1209, #1220, #1222, #1235, #1241–#1244, #1248, #1250, #1251, #1255, #1256, #1346, #1402, #1418, #1422, #1462, #1478, #1484, #1553, #1555, #1564, #1594, #1603, #1617, #1632, #1635, #1662, #1663, #1683, #1763, #1781, #1814, #1828, #1844, #1866, #1870, #2015, #2194, #2258, #2343, #2366. Also its hot-function list, its button-prompt generator, and its Steam Deck findings (late readbacks, #238) |
| [Hookshot](https://git.notrexed.dev/Hookshot/Hookshot) | NotRexed and contributors | Longshot's own upstream; the a64-kit lifter and runtime (TOTKPort). The OptimizeChangeBank heap retry (`55d68e8cd6`, `65c1998222`) |
| [PS5_Hookshot](https://github.com/mihawk-99/PS5_Hookshot) | Mihawk-99 | The starting point: the first PS5 Hookshot release (`ps5-v0.1.0-beta.2`) and its PS5 build and link |
| [PS5_Vulkan](https://github.com/mihawk-99/PS5_Vulkan) | Mihawk-99, BlackBearReloaded | The Vulkan bring-up on the PS5: the RADV link recipe, the clang wrapper, the title converter, `libc.prx` |
| [PS5_Mesa](https://github.com/mihawk-99/PS5_Mesa) | Mihawk-99, on Mesa | RADV, Mesa's Vulkan driver, built for the console (`0f86253`) |
| [PS5_MihawkGen](https://github.com/mihawk-99/PS5_MihawkGen) | Mihawk-99 | Frame generation, compiled in with its trained network |
| [PS5 payload SDK](https://github.com/ps5-payload-dev/sdk) | John Törnblom; fork by Aleksei Kulaev | The platform layer the title is compiled and linked with |
| [ProjectReLink](https://github.com/pixl27/ProjectReLink) | pixl27 | Water-volume draws (`3af2a28`), pipelines by program (`e894874`), sync table (`b63c02a`), memory freed off the render thread (`4decf67`), retired pools (`d2c3497`), shared fallback buffers (`da8b11b`), native floating point in the lifter (`0a05c24`), upscaler sharpening before the HUD (0.7.9.3), the neighbouring-area rule (0.7.9.4), the parallel-snapshot idea (#102) |
| [HeyListen](https://github.com/Sacrappy/HeyListen) | Sacrappy | Texel copies without the virtual core (0.4.1), the exposure-meter fix (`33873659`, from Hookshot `53eb0441` by pixl27), kept descriptor pools (`6ed31109`) |
| [Rekarina](https://git.rekarina.dev/rekarina/rekarina) | Rekarina | Ideas only: reading the light-injection uniforms in place, early light results |
| [totk-pc-recomp](https://github.com/valentderah/totk-pc-recomp) | valentderah | Through Hookshot: the virtual cores' design and the host zstd decoder |
| [heydemoura/PS5_Longshot](https://github.com/heydemoura/PS5_Longshot) | heydemoura | Read for comparison; its PRs #11 and #23 are the same Longshot fixes taken here from Longshot |

**Libraries and tools**

| What | License | Used for |
|---|---|---|
| Eden / suyu / yuzu shader recompiler, sirit | GPL-2.0+/GPL-3.0+ | Translating the game's shaders to SPIR-V |
| suyu's AArch64 → C translator | GPL | The lifter's instruction translation |
| AMD FidelityFX SDK (FSR 3.1), FSR API headers | MIT | The upscaler and anti-aliasing |
| Dear ImGui | MIT | The in-game menu, FPS counter, start screen |
| Libertinus fonts | OFL 1.1 | The overlays' fallback font |
| Kenney Input Prompts | CC0 | The PS5, Xbox, Steam Deck and keyboard button prompts |
| libopus 1.5.2 | BSD-3-Clause | The game's Opus audio |
| zstd 1.5.7, zbic (Atmosphere-NX's ZBIC zstd) | BSD / GPLv2 | Game data and module decompression |
| FFmpeg, dav1d | LGPL-2.1+ / BSD-2-Clause | The game's AV1 movies |
| Vulkan-Headers, SPIRV-Headers, VulkanMemoryAllocator | Apache-2.0 / MIT | Building the renderer |
| Intel XeSS headers, libretro CRT shaders | MIT / per file | Carried from Longshot (not used on PS5) |
| LLVM/Clang 18.1.8 | Apache-2.0 WITH LLVM-exception | The bundled compiler and linker |
| CPython (python-build-standalone), capstone, unicorn, numpy, lz4, zstandard, fontTools, Pillow, astc-encoder-py, texture2ddecoder | Various (in `deps/python`) | The build's tools |

Every vendored component's provenance and license text is beside it: `third_party/*/PROVENANCE.md`,
`gpu/PROVENANCE.md`, `host/PROVENANCE.md`, and `deps/SOURCES.txt` in the release.

**Banner:** Oberon die photography by Fritzchens Fritz
([Tom's Hardware](https://www.tomshardware.com/news/amd-oberon-playstation-5-soc-die-says-cheese-to-the-camera)), with
Longshot's icon.

*The Legend of Zelda: Ocarina of Time* is © Nintendo. This project contains no game code or data and is not affiliated
with Nintendo or Sony. Build only from a game you own.

---

## License

Longshot Oberon is free software, licensed under the **GNU General Public License v3.0**, as Longshot and Hookshot are.
See [`LICENSE`](LICENSE). You may copy, change and share it under the GPL's terms. Anything you distribute that is built
from it must carry its source under the same license. Third-party components keep their own licenses, listed in
[Credits](#credits) and beside each component.
