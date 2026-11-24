### The renderer profile (PGO) and ThinLTO

The frame rate is limited by one CPU thread replaying the game's draws through the renderer, so the renderer is built
with **profile-guided optimisation** (PGO) from a profile recorded **on the console**, and with **ThinLTO**.
`build.sh` does both by default, using the profile in `ps5/profiles/vk.profdata`.

| Build | Replay cost per draw |
|---|---|
| No profile | 11.2–11.6 µs |
| With the console profile | 10.7 µs (about −6%) |
| Profile + ThinLTO | 10.1 µs (2,500–2,999 draws: 26.7 → 28.7 fps) |

**How it's used.** `ps5/build.py` reads two switches (`build.sh` sets both):
- `PS5_VK_PGO=<file.profdata>`: the renderer's own files (not Eden's recompiler, sirit, Dear ImGui or MihawkGen) are
  compiled with `-fprofile-instr-use`, so clang knows which branches and calls are hot. An out-of-date profile is fine:
  functions that changed since it was recorded are simply optimised without it.
- `PS5_VK_LTO=thin`: the same files are compiled to LLVM bitcode and optimised together at the link. Small calls across
  files on the replay path can then be inlined. The linker's LLVM must not be older than the compiler; the bundled
  clang and lld are the same version (18.1.8).

**Recording a new profile** (after renderer changes, to get the full gain back):

1. Build a counting title:
   ```bash
   export PATH=$PWD/deps/llvm/bin:$PWD/deps/python/bin:$PATH PS5_CLANG=$PWD/deps/llvm/bin/clang \
          LLVM_CONFIG=$PWD/deps/llvm/bin/llvm-config PS5_PAYLOAD_SDK=$PWD/deps/ps5-payload-sdk \
          PS5_VULKAN=$PWD/deps/PS5_Vulkan PS5_MIHAWKGEN=$PWD/deps/PS5_MihawkGen PS5_DEPS=$PWD/deps
   ulimit -s unlimited
   PS5_VK_PGO=gen python3 -I ps5/build.py --jobs 2
   ```
   (Run `build.sh` once first, so the lifted game is in `work/`.) This title runs slower: it counts every branch.
2. Copy it to the console and tell it where to write the profile. `longshot_env.txt` doesn't accept paths, so use a
   `longshot_test_env.txt` in the title folder: copy your full `longshot_env.txt` into it (it replaces the normal
   file entirely) and add `HOS_PGO=/app0/vk.profraw`.
3. Start the game and load your save. Create an empty file `/app0/vk.profraw.reset` (this drops the counts from
   loading), then play 10–15 minutes of normal play: Kokiri Forest, Hyrule Field, the market, a few area changes.
4. Create an empty file `/app0/vk.profraw.dump`. The title writes `/app0/vk.profraw`.
5. Copy `vk.profraw` to the PC and convert it:
   ```bash
   deps/llvm/bin/llvm-profdata merge -o ps5/profiles/vk.profdata vk.profraw
   ```
6. Delete `longshot_test_env.txt` from the console and run `bash build.sh` again. The renderer recompiles with the
   new profile, and the build log says `ps5: the renderer built with the profile ...`.

The game's own code isn't profiled this way. Its 680 hottest functions are compiled at `-O2` from Longshot's hot list
(`ps5/profiles/hot_funcs_longshot_v3.txt`).
