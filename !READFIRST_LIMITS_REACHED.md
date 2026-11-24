## LIMITS REACHED

This port runs close to several of the base PS5's limits. They are listed here so it's clear why some settings are what
they are, and where future work has the least room.

| Limit | Where it is now | What it means |
|---|---|---|
| **Executable's zero-filled data (.bss)** | **171.5 MB of a 176 MB ceiling** | See below. The tightest limit of all |
| **Memory for the title** | 12,016 MiB, shared by the game, the driver and every texture | 1 GB is always kept free; in the market the lowest free memory is ~650 MB |
| **One replay thread** | ~8.4 µs a draw; a heavy frame is ~19 ms of replay plus ~11 ms of game work | The frame rate's limit at 2,500+ draws, not the GPU |
| **CPU time** | 13 logical CPUs (6.5 cores) of Zen 2, 4 MB of L3 per 4-core cluster | Large tables and record walks miss the cache far more than on a desktop CPU |
| **No mesh shaders** | Task/mesh programs run as compute | A different path, same geometry; costs CPU and GPU time a native path wouldn't |
| **Texture size** | 1024 texels at most, held in code | 2048 crashed the whole console |
| **Resolution** | Handheld profile (3D at 1536×864) shown at 1080p | Docked 1080p costs 130–450 MB more; 1440p another ~500 MB. Both ran short in the market |
| **Shadows** | The console's own shadow resolution | Higher costs memory the title doesn't have |

### The .bss ceiling

The console maps an executable's zero-filled data (`.bss`: static tables, buffers and counters that start at zero) out
of the title's *flexible memory* when it loads the title. That pool is small and is shared with the homebrew payloads
already running (kstuff, ftpsrv, ShadowMountPlus, elfldr, klogsrv). With those loaded, about **188 MB** is left for a title.

- **Over it, the title doesn't start:** the system answers "Not enough flexible memory to load executable"
  (EXECFAIL 12). On the firmware used for this port, that failure **crashed SceSysCore: the whole console**, not just the title.
- **Where it stands:** the current build's `.bss` is **171.5 MB**. The link refuses anything over **176 MB**
  (`PS5_BSS_LIMIT_MB`), which leaves room for the payloads and is checked at every build, so a too-big title is never
  made.
- **How it got there:** after one build crossed the line, the host's large tables were moved from static storage to
  the heap, which brought `.bss` back under the limit. That leaves about 4.5 MB of margin.
- **What it means for changes:** a new static array, a bigger fixed table or another large `static` buffer can push
  the title over. Anything bigger than a few hundred KB belongs on the heap (allocated at start or on first use), not in
  `.bss`. If the link stops with `.bss ... is over PS5_BSS_LIMIT_MB`, that is the reason. Raising the limit is not the
  fix: it trades a build error for a console that won't start the title, or crashes.
- Running fewer payloads gives a little more room, but the port doesn't rely on that.
THIS WAS TESTED WITH SHADOWMOUNT, KSTUFF, FTPSRV, PLDMGR, ELFLDR & KLOGSRV running. HEAVY PAYLOADS LIKE ETAHEN OR OTHER DERIVATIVES UNTESTED WITH THIS TITLE RUNNING.
