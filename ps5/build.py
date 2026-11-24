"""PS5: the game, the runtime and the Vulkan back end compiled for the console and linked into one title, dist/<titleId> (ps5/sce_sys/param.json: PPSA93339).

    python3 -I ps5/build.py [--jobs N] [--compile-only]

What it builds from (docs/PS5.md has the whole route):
  - the lifted game: work/lift/sa/gen (nx2/lift.py --standalone --write, from your own dump; never committed);
  - host/ as the Linux build compiles it (host/posix's Win32 calls), with host/ps5 in place of SDL, and the PS5's
    memory, threads and modules (host/ps5/win32_ps5.c);
  - gpu/vk with Eden's shader recompiler, linked in rather than loaded (no dynamic loading on the console);
  - Mesa's RADV from PS5_Vulkan (its release archive and link recipe), the payload SDK fork's platform layer.

Paths: OOT_WORK (default work/lift), PS5_PAYLOAD_SDK (default .local/ps5-deps/ps5-payload-sdk, else ../PS5_VulkanTemplate's), PS5_VULKAN
(default ../PS5_Vulkan). Objects are kept by a key of their flags, source and headers: a rebuild compiles what changed.
"""
import argparse
import concurrent.futures
import hashlib
import json
import re
import os
import shutil
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "nx2"))
os.environ.setdefault("OOT_WORK", os.path.join(ROOT, "work", "lift"))
import kit       # noqa: E402  (nx2: the lift's paths)
import rtgen     # noqa: E402  (the 64-bit guest window's runtime header)
import logo_rgba  # noqa: E402
import ui_fonts   # noqa: E402  (the overlays' bundled fonts, compiled in)
import version   # noqa: E402

# (the SDK ps5/build-title.sh installs at its pinned revision; before it did, PS5_VulkanTemplate's)
SDK = os.environ.get("PS5_PAYLOAD_SDK") or next(
    (d for d in (os.path.join(os.environ.get("PS5_DEPS") or os.path.join(ROOT, ".local", "ps5-deps"), "ps5-payload-sdk"),) if os.path.exists(os.path.join(d, "bin", "prospero-lld"))),
    os.path.join(ROOT, "..", "PS5_VulkanTemplate", ".deps", "native", "ps5-payload-sdk"))
VULKAN = os.environ.get("PS5_VULKAN", os.path.join(ROOT, "..", "PS5_Vulkan"))
DEPS = os.environ.get("PS5_DEPS") or os.path.join(ROOT, ".local", "ps5-deps")   # (build.sh: the bundled deps/)
CPM = os.path.join(DEPS, "cpm")
FFMPEG = os.path.join(DEPS, "ffmpeg")
GEN = os.path.join(kit.WORK, "sa", "gen")
OUT = os.path.join(ROOT, "work", "ps5")
HOST, POSIX, PS5 = (os.path.join(ROOT, "host", d) for d in ("", "posix", "ps5"))
VK = os.path.join(ROOT, "gpu", "vk")
OPUS = os.path.join(ROOT, "third_party", "opus")
# PS5_MihawkGen (github.com/mihawk-99/PS5_MihawkGen, GPL-3.0-or-later), checked out beside this repository: its generator
# (framegen/src) is compiled in and its trained network goes beside the game, as PS5_RetroArch's build has them. Absent:
# the renderer's own frame interpolation.
MIHAWKGEN = os.environ.get("PS5_MIHAWKGEN", os.path.join(ROOT, "..", "PS5_MihawkGen"))
MIHAWKGEN_SOURCES = [os.path.join(MIHAWKGEN, "framegen", "src", n + ".cpp")
                     for n in ("fg_common", "fg_vulkan", "fg_neural", "fg_splat")]
MIHAWKGEN_NETWORK = os.path.join(MIHAWKGEN, "framegen", "models", "network.fgw")
HAS_MIHAWKGEN = all(os.path.exists(f) for f in MIHAWKGEN_SOURCES + [MIHAWKGEN_NETWORK])

# The guest window at 64 GiB (host/ps5/win32_ps5.h); Zen 2; the SDK wrapper adds the console's target and its
# required flags (PS5_Vulkan tooling/prospero-clang18). Frame pointers: console backtraces walk them.
COMMON = ["-march=znver2", "-ffp-contract=off", "-DA64_HOST_BASE=0x1000000000ULL", "-fno-omit-frame-pointer",
          "-ffunction-sections", "-fdata-sections", "-g0", "-Wno-everything"]

# (base PS5) Native thread-local storage for the host and the renderer: the wrapper's -femulated-tls makes every thread_local
# access a call to __emutls_get_address (pthread_getspecific and an index), and the replay's hot path, on the one thread that
# sets the frame rate, reaches dozens a draw. With PS5_NATIVE_TLS (on; 0: emulated, as before) an access is one %fs-relative
# load: local-exec (offsets fixed at the link, no GOT entry or dynamic TLS relocation for the converter; ps5/tools/ps5-pie-tls.ld). Not the lifted game (no thread_local in it, and
# its objects' keys stay). host/ps5/tls_check.c checks at the start that each thread has its own copy, or stops the title
NATIVE_TLS = os.environ.get("PS5_NATIVE_TLS", "1") not in ("0", "off", "no", "false", "")
TLS_FL = ["-fno-emulated-tls", "-ftls-model=local-exec"] if NATIVE_TLS else ["-DPS5_EMULATED_TLS"]


def pkg(name, version_):
    return os.path.join(CPM, name, version_)


def vk_lto_mode():
    """(2026-11-23) PS5_VK_LTO: "thin" for ThinLTO of the renderer's own files; anything else, or unset: off"""
    v = os.environ.get("PS5_VK_LTO", "").strip().lower()
    if v in ("", "0", "off", "no", "false"):
        return ""
    if v != "thin":
        sys.exit("ps5/build.py: PS5_VK_LTO=%s: only thin is known (or unset: off)" % v)
    return v


def llvm_major(cmd):
    """The major version a clang or llvm-config prints, or 0"""
    try:
        out = subprocess.run(cmd, capture_output=True, text=True, timeout=60).stdout
    except (OSError, subprocess.SubprocessError):
        return 0
    m = re.search(r"(\d+)\.\d+", out)
    return int(m.group(1)) if m else 0


def cc_wrapper(cxx=False):
    return ["sh", os.path.join(VULKAN, "tooling", "prospero-clang18")] + (["-x", "c++"] if cxx else [])


def tree_digest(*dirs, ext=(".h", ".hpp", ".inc")):
    h = hashlib.sha1()
    for d in dirs:
        for base, _, files in sorted(os.walk(d)):
            for f in sorted(files):
                if f.endswith(ext):
                    p = os.path.join(base, f)
                    h.update(p.encode())
                    with open(p, "rb") as fh:
                        h.update(fh.read())
    return h.hexdigest()


def jobs_for(rt64, gen_dir):
    """(source, object, flags, header digest) of everything the title links"""
    out = []
    game_digest = tree_digest(GEN, rt64, os.path.join(kit.KIT, "rt"), os.path.join(ROOT, "nx2", "rt"))
    # (base PS5) the PC build compiles its hot code at -O2 with a play profile and orders it at the link (nx2/build.py); the
    # console's had -O1 throughout. PS5_GAME_OPT=O2: the whole game at -O2 (a longer first build). PS5_PGO=<.profdata>: built
    # with that profile; it must be readable by the console's clang (18): `llvm-profdata merge --text` with the newer tool, then
    # `llvm-profdata-18 merge` of that text, makes one. PS5_ORDER=<hot_order.txt>: the functions placed in that order (link.sh)
    game_opt = "-" + os.environ.get("PS5_GAME_OPT", "O1").lstrip("-")
    # (longshot #1617) the lifted game's stores mark their page only where the page is a GPU page (host/mark_bitmap.c,
    # nx2/rtgen.py A64_MARK_BITMAP); the host's own marks stay. On by default; RX_MARK_BITMAP=0 builds the lifted game marking
    # every page it stores to, as before. (The game objects are compiled again either way: their flags change)
    mark_bitmap = [] if os.environ.get("RX_MARK_BITMAP", "").strip().lower() in ("0", "off", "false", "no") else ["-DA64_MARK_BITMAP"]
    pgo = os.environ.get("PS5_PGO")
    pgo_fl = []
    if pgo:
        with open(pgo, "rb") as fh:
            pgo_fl = ["-fprofile-instr-use=" + os.path.abspath(pgo), "-Wno-profile-instr-unprofiled", "-Wno-profile-instr-out-of-date",
                      "-DHOOKSHOT_PROFILE_SHA1=" + hashlib.sha1(fh.read()).hexdigest()[:16]]
    game_fl = COMMON + ["-I" + rt64, "-I" + GEN, "-I" + os.path.join(kit.KIT, "rt"), "-I" + os.path.join(ROOT, "nx2", "rt"),
                        "-fno-strict-aliasing", "-mcx16", game_opt] + pgo_fl + ["-mllvm", "-rotation-max-header-size=0", "-mllvm",
                        "-capture-tracking-max-uses-to-explore=5000", "-DA64_POLLING", "-DA64_WRITE_TRACK"] + mark_bitmap
    chunks = sorted(f for f in os.listdir(GEN) if f.startswith("code_") and f.endswith(".c"))
    game = [os.path.join(GEN, c) for c in chunks] + \
        [os.path.join(GEN, f) for f in ("a64_table.c", "a64_hooks.c", "a64_suyu.c", "a64_stubs.c", "hos_image.c", "nvn2_exports.c")] + \
        [os.path.join(rt64, "a64_rt.c")]
    # (longshot #1635, c21a191f) the lift's hot functions (code_hot.c, or nx2/split_hot.py's code_hot_*.c pieces of it: a lift with
    # A64_HOT_FUNCS=ps5/profiles/hot_funcs_longshot_v3.txt A64_HOT_CHUNK=1) at PS5_HOT_OPT (default O2), the rest at PS5_GAME_OPT:
    # on PC +24 % on Death Mountain Trail and +10 % in Kakariko for 680 functions at -O2. The list is Longshot v3.0.0's
    # assets/profiles/hot_funcs.txt (680 functions, recorded with Longshot on the same game build). Without a hot lift there is no
    # code_hot*.c and nothing changes
    hot_opt = "-" + os.environ.get("PS5_HOT_OPT", "O2").lstrip("-")
    hot_fl = [hot_opt if f == game_opt else f for f in game_fl]
    out += [(s, os.path.join(OUT, "obj", "game_" + os.path.basename(s)[:-2] + ".o"),
             hot_fl if os.path.basename(s).startswith("code_hot") else game_fl, game_digest) for s in game]

    # the runtime: host/ as Linux builds it, without SDL's loader (host/ps5/hos_sdl_ps5.c) and the Linux-only files
    host_digest = tree_digest(HOST, rt64, GEN) + tree_digest(os.path.join(VK), ext=("vk_api.h", "dlss_math.h"))
    host_fl = COMMON + ["-I" + POSIX, "-D_GNU_SOURCE", "-I" + HOST, "-I" + rt64, "-I" + GEN, "-O2", "-mcx16", "-DA64_POLLING",
                        "-DA64_WRITE_TRACK", "-I" + os.path.join(OPUS, "include"), "-I" + os.path.join(FFMPEG, "include"),
                        "-I" + os.path.join(DEPS, "zstd", "include"),
                        "-I" + os.path.join(pkg("vulkanheaders", "v1.4.345"), "include")] + TLS_FL
    # (PS5_VK_PGO=gen: host/pgo_rt.c writes the renderer's counters, HOS_PGO=<file> in the env file, <file>.reset and <file>.dump)
    vk_pgo = os.environ.get("PS5_VK_PGO", "")
    skip = {"display.c", "mf_av1.c", "rx_nowgpu.c", "hos_sdl.c", "mingw_msvc_shims.c", "msvc_oldnames.c",
            # (Longshot rc.4's crash window, a helper process the game starts, and its restart, an execve of itself: neither
            # is possible on the console, host/ps5/rc4_stubs.c answers for them)
            "hos_crash_dialog.c", "hos_relaunch.c"} | (set() if vk_pgo == "gen" else {"pgo_rt.c"})
    host = [os.path.join(HOST, f) for f in sorted(os.listdir(HOST)) if f.endswith(".c") and f not in skip] + \
        [os.path.join(POSIX, f) for f in ("win32_posix.c", "display_sdl.c", "linux_stubs.c")] + \
        [os.path.join(PS5, f) for f in sorted(os.listdir(PS5)) if f.endswith(".c")]
    stamp = version.define(ROOT)
    for s in host:
        rel = os.path.relpath(s, HOST).replace(os.sep, "_")[:-2]
        fl = host_fl + ([stamp] if os.path.basename(s) == "hos_main.c" else [])
        out.append((s, os.path.join(OUT, "obj", "host_" + rel + ".o"), fl, host_digest))

    # libopus (nn::codec's Opus decoder)
    ofl = COMMON + ["-O2", "-DOPUS_BUILD", "-DUSE_ALLOCA", "-DHAVE_LRINTF", "-DHAVE_LRINT"] + \
        ["-I" + os.path.join(OPUS, d) for d in ("include", "celt", "silk", "silk/float", "src")]
    for rel in open(os.path.join(OPUS, "sources.txt")).read().split():
        out.append((os.path.join(OPUS, rel), os.path.join(OUT, "obj", "opus_" + rel.replace("/", "_")[:-2] + ".o"), ofl,
                    "opus-1.5.2"))

    # the Vulkan back end, Eden's recompiler and sirit, as nx2/build.py's vk_sources (C++20 with exceptions: Eden throws)
    import build as upstream   # (nx2/build.py: the source list, kept in one place)
    spirv = pkg("spirv-headers", "vulkan-sdk-1.4.341.0")
    vfl = COMMON + ["-I" + POSIX, "-D_GNU_SOURCE", "-include", os.path.join(PS5, "win32_ps5.h"),
                    "-include", "algorithm", "-include", "cstdio", "-include", "cstdlib", "-include", "cstring",
                    "-include", "functional", "-include", "string", "-include", "vector",
                    "-std=c++20", "-O2", "-DNDEBUG", "-fexceptions", "-fcxx-exceptions", "-DVK_NO_PROTOTYPES",
                    "-DMKWVK_BUILDING", "-DHOOKSHOT_RELEASE", "-DLONGSHOT_RELEASE", "-DMKWVK_INCLUDES=2",
                    "-I" + os.path.join(VK, "compat"), "-I" + os.path.join(ROOT, "gpu", "compat"), "-I" + os.path.join(VK, "eden"),
                    "-I" + os.path.join(ROOT, "gpu", "third_party", "suyu"),
                    "-I" + os.path.join(VK, "sirit", "include"), "-I" + os.path.join(VK, "sirit", "src"),
                    "-I" + os.path.join(spirv, "include"), "-I" + os.path.join(spirv, "include", "spirv", "unified1"),
                    "-I" + os.path.join(pkg("vulkanheaders", "v1.4.345"), "include"), "-I" + VK, "-I" + gen_dir,
                    "-I" + os.path.join(pkg("vulkanmemoryallocator", "v3.3.0"), "include"),
                    "-I" + os.path.join(DEPS, "zstd", "include")] + TLS_FL   # (Longshot's program cache: vk_progcache.cpp)
    # The translator's stamp: Longshot's rule (nx2/translator_files.py, rc.4: vk_api.h no longer in it), unless ps5/shader_stamp_pin.txt
    # pins the one the console's caches were prepared with, for this very translator. A prepared console holds ~6.7 GB of
    # programs a new stamp throws away (a night of preparing again); rc.4's changes to the stamped files (the driver-fault
    # handling in vk_shader.cpp, PsychoV35's tone mapping pass, which runs only with that tone mapper chosen) and its new rule do
    # not change one translated program on the console. The pin holds only while the stamped files are those it names: any
    # change to them since is a new translator, and the stamp is Longshot's again (and the console prepares once)
    import translator_files
    shader_hex = translator_files.stamp(VK)
    pin = os.path.join(ROOT, "ps5", "shader_stamp_pin.txt")
    if os.path.exists(pin):
        kv = dict(l.split(None, 1) for l in open(pin).read().splitlines() if l.strip() and not l.startswith("#"))
        if kv.get("translator", "").strip() == shader_hex:
            print("ps5: shader stamp %s, pinned (ps5/shader_stamp_pin.txt: the console's prepared shaders stay)" % kv["stamp"].strip(),
                  flush=True)
            shader_hex = kv["stamp"].strip()
        else:
            print("ps5: the shader translator changed since ps5/shader_stamp_pin.txt: stamp %s, the console prepares its shaders "
                  "again once" % shader_hex, flush=True)
    shader_stamp = "-DLONGSHOT_SHADER_STAMP=0x%sull" % shader_hex   # (Longshot's name for it)
    lib_digest = tree_digest(os.path.join(VK, "eden"), os.path.join(VK, "sirit"), os.path.join(VK, "compat"))
    vk_digest = tree_digest(VK, os.path.join(ROOT, "gpu", "compat"), os.path.join(ROOT, "gpu", "render"), POSIX, PS5,
                            os.path.join(ROOT, "gpu", "third_party", "suyu", "video_core", "textures"))
    # Dear ImGui (the menu and the overlays: vk_imgui.cpp, vk_menu.cpp), as nx2/build.py compiles it; the game's font files
    # are zstd frames, read with the console's zstd (ps5/tools/build-zstd.sh), not third_party/zstd's second copy
    imgui = os.path.join(ROOT, "third_party", "imgui")
    imgui_fl = ["-I" + imgui, "-I" + os.path.join(DEPS, "zstd", "include"), "-DIMGUI_IMPL_VULKAN_NO_PROTOTYPES",
                "-DIMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS", "-DIMGUI_DISABLE_WIN32_FUNCTIONS", "-DIMGUI_DISABLE_DEMO_WINDOWS",
                "-DIMGUI_DISABLE_DEBUG_TOOLS"]
    imgui_digest = tree_digest(imgui, os.path.join(DEPS, "zstd", "include"))
    for f in ("imgui.cpp", "imgui_draw.cpp", "imgui_tables.cpp", "imgui_widgets.cpp", "backends/imgui_impl_vulkan.cpp"):
        out.append((os.path.join(imgui, *f.split("/")), os.path.join(OUT, "obj", "vk_imgui_" + os.path.basename(f)[:-4] + ".o"),
                    vfl + imgui_fl, imgui_digest))
    out.append((os.path.join(gen_dir, "ui_fonts.cpp"), os.path.join(OUT, "obj", "vk_ui_fonts.o"), COMMON + ["-O0", "-w"], ""))
    # (base PS5) PS5_VK_PGO=gen: the renderer's own files (not Eden's recompiler, sirit or Dear ImGui) count their branches, and
    # host/pgo_rt.c writes them in the format clang 18 reads (HOS_PGO=<file> in the env file; <file>.dump put beside it writes
    # it). PS5_VK_PGO=<.profdata> (llvm-profdata-18 merge of that file): built with that profile. klog_15, Castle Town: the
    # replay on the game's thread is 31 ms of a 35 ms frame; the GPU 8. Neither set: the build as it was, the same object keys
    vk_pgo_fl = []
    if vk_pgo == "gen":
        vk_pgo_fl = ["-fprofile-instr-generate", "-mllvm", "-disable-vp=true"]
        print("ps5: the renderer counts its branches for a profile (PS5_VK_PGO=gen; HOS_PGO=<file> in the env file)", flush=True)
    elif vk_pgo:
        with open(vk_pgo, "rb") as fh:
            vk_pgo_fl = ["-fprofile-instr-use=" + os.path.abspath(vk_pgo), "-Wno-profile-instr-unprofiled",
                         "-Wno-profile-instr-out-of-date", "-Wno-backend-plugin",
                         "-DPS5_VK_PROFILE_SHA1=" + hashlib.sha1(fh.read()).hexdigest()[:16]]
        print("ps5: the renderer built with the profile %s" % vk_pgo, flush=True)
    # (2026-11-23, PS5_VK_LTO=thin) ThinLTO for the renderer's own files, the same set as the profile's (not Eden's recompiler,
    # sirit, Dear ImGui or MihawkGen): their objects are LLVM bitcode, optimised together at the link, so the replay's small calls
    # across files (vk_bank.cpp's slot versions, vk_shader.cpp's constant words: stamped, their source is not touched) can be
    # inlined. With PS5_VK_PGO=<.profdata> the link knows which of those calls are hot. Not set: the build as it was, the same
    # object keys. The link's side (the jobs, the cache, the native TLS kept): main(), below
    vk_lto_fl = []
    if vk_lto_mode() == "thin":
        vk_lto_fl = ["-flto=thin"]
        print("ps5: the renderer's own files built for ThinLTO (PS5_VK_LTO=thin)", flush=True)
    for s in upstream.vk_sources():
        if os.path.basename(s) == "vk_ngx_msvc_rt.cpp":
            continue
        rel = os.path.relpath(s, os.path.join(ROOT, "gpu")).replace(os.sep, "_")[:-4]
        lib = os.path.relpath(s, VK).split(os.sep)[0] in ("eden", "sirit")
        fl = vfl + ([] if lib else vk_pgo_fl + vk_lto_fl) + ([shader_stamp] if os.path.basename(s) == "vk_shader.cpp" else []) + \
            ([stamp] if os.path.basename(s) == "vk_menu.cpp" else []) + \
            (imgui_fl if os.path.basename(s) in ("vk_imgui.cpp", "vk_menu.cpp") else []) + \
            (["-DHOOKSHOT_MIHAWKGEN", "-I" + os.path.join(MIHAWKGEN, "framegen", "include")]
             if HAS_MIHAWKGEN and os.path.basename(s) == "vk_framegen.cpp" else [])
        out.append((s, os.path.join(OUT, "obj", "vk_" + rel + ".o"), fl,
                    lib_digest if lib else vk_digest + (imgui_digest if os.path.basename(s) in ("vk_imgui.cpp", "vk_menu.cpp") else "")))
    if HAS_MIHAWKGEN:   # (its Vulkan calls through the back end's loaded pointers: vk_loader.h, as every unit of gpu/vk)
        mfl = vfl + ["-include", os.path.join(VK, "vk_loader.h"), "-I" + os.path.join(MIHAWKGEN, "framegen", "include"),
                     "-I" + os.path.join(MIHAWKGEN, "framegen", "src")]
        mdigest = tree_digest(os.path.join(MIHAWKGEN, "framegen")) + vk_digest
        for s in MIHAWKGEN_SOURCES:
            out.append((s, os.path.join(OUT, "obj", "mihawkgen_" + os.path.basename(s)[:-4] + ".o"), mfl, mdigest))
    return out


def key_of(src, flags, digest):
    h = hashlib.sha1(" ".join(flags).encode() + digest.encode())
    with open(src, "rb") as f:
        h.update(f.read())
    return h.hexdigest()


def compile_one(job):
    src, obj, flags, digest = job
    key = key_of(src, flags, digest)
    stamp = obj + ".key"
    if os.path.exists(obj) and os.path.exists(stamp) and open(stamp).read() == key:
        return src, None, 0.0
    cxx = src.endswith(".cpp")
    t = time.time()
    r = subprocess.run(cc_wrapper(cxx) + flags + ["-c", src, "-o", obj], capture_output=True, text=True,
                       env=dict(os.environ, PS5_PAYLOAD_SDK=SDK, PS5_CLANG=os.environ.get("PS5_CLANG") or shutil.which("clang")))
    if r.returncode:
        return src, (r.stdout + r.stderr)[-4000:], time.time() - t
    if not os.path.exists(obj):   # (a compiler killed for want of memory can leave no object and no error)
        return src, (r.stdout + r.stderr)[-4000:] + "\nno object was written (the compiler was likely killed for want of " \
            "memory: build again with fewer --jobs)", time.time() - t
    with open(stamp, "w") as f:
        f.write(key)
    return src, None, time.time() - t


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 4) - 2))
    p.add_argument("--compile-only", action="store_true")
    a = p.parse_args()
    for need, what in ((os.path.join(GEN, "a64_table.c"), "the lifted game (nx2/lift.py --standalone --write)"),
                       (os.path.join(SDK, "bin", "prospero-lld"), "the PS5 payload SDK (PS5_PAYLOAD_SDK)"),
                       (os.path.join(VULKAN, "tools", "radv-link.sh"), "PS5_Vulkan (PS5_VULKAN)"),
                       (os.path.join(FFMPEG, "include", "libavcodec", "avcodec.h"), "FFmpeg (ps5/tools/build-ffmpeg.sh)"),
                       (os.path.join(DEPS, "zstd", "lib", "libzstd.a"), "zstd (ps5/tools/build-zstd.sh)"),
                       (pkg("vulkanheaders", "v1.4.345"), "the Vulkan headers (ps5/build-title.sh fetches them)")):
        if not os.path.exists(need):
            sys.exit("ps5/build.py: missing %s: %s" % (what, need))
    os.makedirs(os.path.join(OUT, "obj"), exist_ok=True)
    rt64 = rtgen.write(os.path.join(OUT, "rt64"))
    gen_dir = os.path.join(OUT, "gen")
    logo_rgba.write(os.path.join(gen_dir, "longshot_logo.h"))
    ui_fonts.write(os.path.join(gen_dir, "ui_fonts.cpp"))
    jobs = jobs_for(rt64, gen_dir)
    # the longest first (the lifted chunks), so no long compile is left alone at the end
    jobs.sort(key=lambda j: -os.path.getsize(j[0]))
    print("ps5: %d files, %d jobs; thread-local storage %s" % (len(jobs), a.jobs,
          "native (PS5_NATIVE_TLS)" if NATIVE_TLS else "emulated (PS5_NATIVE_TLS=0)"), flush=True)
    failed, built, t0 = [], 0, time.time()
    with concurrent.futures.ThreadPoolExecutor(a.jobs) as pool:
        for src, err, secs in pool.map(compile_one, jobs):
            if err:
                failed.append(src)
                print("FAILED %s\n%s" % (os.path.relpath(src, ROOT), err), flush=True)
            elif secs:
                built += 1
                if secs > 30:
                    print("  %s: %.0f s" % (os.path.relpath(src, ROOT), secs), flush=True)
    print("ps5: %d compiled, %d failed (%.0f s)" % (built, len(failed), time.time() - t0), flush=True)
    if failed:
        return 1
    with open(os.path.join(OUT, "objects.txt"), "w") as f:
        f.write("\n".join(j[1] for j in jobs) + "\n")
    if a.compile_only:
        return 0
    env = dict(os.environ, PS5_PAYLOAD_SDK=SDK, PS5_VULKAN=VULKAN, PS5_FFMPEG=FFMPEG)
    order = os.environ.get("PS5_ORDER")
    if order:   # (base PS5: the hot functions together, as the PC build links them; their sections are per function already)
        env["PS5_LINK_EXTRA"] = "--symbol-ordering-file=%s --no-warn-symbol-ordering" % os.path.abspath(order)
        print("ps5: functions ordered by %s" % order, flush=True)
    if os.environ.get("PS5_VK_PGO") == "gen":
        # (the console's target has clang name its profile runtime in each counting object, libclang_rt.profile-x86_64_nosubmission.a,
        # which the payload SDK has not: host/pgo_rt.c stands in for it, so those names are not followed)
        env["PS5_LINK_EXTRA"] = (env.get("PS5_LINK_EXTRA", "") + " --no-dependent-libraries").strip()
    if vk_lto_mode() == "thin":
        # (2026-11-23, PS5_VK_LTO=thin) The link compiles the bitcode, so it must read it: the linker (prospero-lld runs the
        # ld.lld of the newest llvm-config it finds) may not be older than the clang that made it. Checked here, not left to a
        # "bitcode version" error at the end of a long link
        cc = os.environ.get("PS5_CLANG") or shutil.which("clang") or "clang"
        cc_v = llvm_major([cc, "--version"])
        ld_v = llvm_major([os.path.join(SDK, "bin", "prospero-llvm-config"), "--version"])
        print("ps5: ThinLTO: clang %d, the linker's LLVM %d" % (cc_v, ld_v), flush=True)
        if not cc_v or not ld_v or ld_v < cc_v:
            print("ps5/build.py: PS5_VK_LTO=thin: the linker's LLVM (%d) must be the clang's (%d) or newer; build without "
                  "PS5_VK_LTO, or install the matching lld" % (ld_v, cc_v), file=sys.stderr)
            return 1
        # (the jobs: as the compile's; the cache: the second link (the weak names bound to 0, link.sh) and later builds take
        # the modules unchanged from it. -mllvm -emulated-tls=0: prospero-lld passes -mllvm -emulated-tls to the link, which
        # the link's own code generation follows: the renderer's thread_locals would become __emutls_v.* while the rest of the
        # title (and ps5-pie-tls.ld, host/ps5/tls_check.c) has them native. The later option wins. With PS5_NATIVE_TLS=0 the
        # wrapper's emulated TLS is everyone's, so it is left)
        extra = "--thinlto-jobs=%d --thinlto-cache-dir=%s" % (max(1, a.jobs), os.path.join(OUT, "lto-cache"))
        if NATIVE_TLS:
            extra += " -mllvm -emulated-tls=0"
        env["PS5_LINK_EXTRA"] = (env.get("PS5_LINK_EXTRA", "") + " " + extra).strip()
        print("ps5: ThinLTO at the link (%s)" % extra, flush=True)
    linked = subprocess.run(["bash", os.path.join(ROOT, "ps5", "tools", "link.sh"), OUT,
                             os.path.join(kit.WORK, "sa", "image", "image.bin")], env=env).returncode
    if not linked and HAS_MIHAWKGEN:   # (the network beside the game: /app0/framegen/network.fgw, vk_framegen.cpp)
        title_id = json.load(open(os.path.join(ROOT, "ps5", "sce_sys", "param.json")))["titleId"]
        os.makedirs(os.path.join(ROOT, "dist", title_id, "framegen"), exist_ok=True)
        shutil.copy(MIHAWKGEN_NETWORK, os.path.join(ROOT, "dist", title_id, "framegen", "network.fgw"))
        rev = subprocess.run(["git", "-C", MIHAWKGEN, "rev-parse", "--short", "HEAD"], capture_output=True, text=True).stdout.strip()
        print("ps5: frame generation by PS5_MihawkGen (%s)" % (rev or "no revision"), flush=True)
    return linked


if __name__ == "__main__":
    sys.exit(main())
