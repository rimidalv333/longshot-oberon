"""Longshot Oberon: build.sh's steps in Python, for the Windows builder (build.cmd): the PS5 title folder PPSA93339 from your own
game file, with the Windows tools build.cmd bundles (LLVM 18.1.8 for Windows, Python 3.12, a64emit.dll, zbic.dll and
ps5-native-tool.exe), no WSL.

    python -I -u ps5/win_build.py <your oot.nup> --tools <the tools folder> [--jobs N] [--out DIR] [--skip-hash-check]

Every step is build.sh's, with its checks, its marks (a step that is done is not done again) and its logs in work/logs; the
compile and the link are ps5/build.py's (which runs ps5/tools/link.py in link.sh's place on Windows). From the same release zip
and the same .nup it makes the same eboot.bin as build.sh on Linux or WSL: compare the sha256 both print at the end.
"""
import argparse
import ctypes
import glob
import hashlib
import os
import shutil
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TITLE_ID = "PPSA93339"
VERSION = "2.0.0"
EXEFS_SHA256 = {
    "main": "2591fb99008d0814f8056c3f36b36790048ed857ff0ae2bb3a4c1e37b960a4de",
    "main.npdm": "94c154d83cbaca00d092958cc2b3a0494d7308fe49de8cb33ece7e9e67303ea7",
    "rtld": "219f25ef4a64a704e86dacfeb25d9e8977045f2145e629268b94baf34d5e0bbe",
    "sdk": "3ae3cd4cf340d72bdaba9e6f66016a74c1eae13737a5f7299375ed6654b9b1c5",
    "subsdk0": "0d32c4bdbcfcfa1c4110c37eff18eaf70061a55090b7fedc741a2e37224065ba",
}
ROMFS_PARTS = "Shader/,Layout2Shader/,Lib/,Movie/Ndve/,Effect/,Pack/ZsDic.pack.zs,System/Resource/,UI/Layout2Archive/,Font2/"
T0 = time.time()


def die(msg):
    print("\nbuild: %s" % msg, file=sys.stderr, flush=True)
    sys.exit(1)


def step(msg):
    print("\n==> %s" % msg, flush=True)


def note(msg):
    print("    %s" % msg, flush=True)


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def run(log_name, cmd, env=None):
    """quiet while it works (a dot a minute), the end of its log when it fails"""
    log = os.path.join(ROOT, "work", "logs", log_name + ".log")
    start = time.time()
    with open(log, "w", newline="\n") as f:
        p = subprocess.Popen(cmd, stdout=f, stderr=subprocess.STDOUT, cwd=ROOT, env=env)
        while p.poll() is None:
            time.sleep(1)
            if int(time.time() - start) % 60 == 0 and time.time() - start > 1:
                note("... still working (%d min)" % ((time.time() - start) // 60))
                time.sleep(1)
    if p.returncode:
        with open(log, encoding="utf-8", errors="replace") as f:
            print("".join(f.readlines()[-40:]), file=sys.stderr)
        die("that step failed; the whole log: " + log)
    note("done in %d s" % (time.time() - start))


def rmtree(path):
    if os.path.isdir(path):
        shutil.rmtree(path, ignore_errors=False, onerror=lambda f, p, e: (os.chmod(p, 0o700), f(p)))
    elif os.path.exists(path):
        os.remove(path)


def mem_gb():
    if os.name == "nt":
        class MS(ctypes.Structure):
            _fields_ = [("dwLength", ctypes.c_ulong), ("dwMemoryLoad", ctypes.c_ulong), ("ullTotalPhys", ctypes.c_ulonglong),
                        ("ullAvailPhys", ctypes.c_ulonglong), ("ullTotalPageFile", ctypes.c_ulonglong),
                        ("ullAvailPageFile", ctypes.c_ulonglong), ("ullTotalVirtual", ctypes.c_ulonglong),
                        ("ullAvailVirtual", ctypes.c_ulonglong), ("sullAvailExtendedVirtual", ctypes.c_ulonglong)]
        m = MS()
        m.dwLength = ctypes.sizeof(MS)
        ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(m))
        return m.ullTotalPhys // (1 << 30)
    return os.sysconf("SC_PAGE_SIZE") * os.sysconf("SC_PHYS_PAGES") // (1 << 30)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("game")
    ap.add_argument("--tools", required=True)
    ap.add_argument("--jobs", type=int)
    ap.add_argument("--out")
    ap.add_argument("--skip-hash-check", action="store_true")
    ap.add_argument("--log", help="also write everything printed to this file (build.cmd: build.log)")
    a = ap.parse_args()
    if a.log:
        class Tee:
            def __init__(self, a_, b_):
                self.a, self.b = a_, b_
            def write(self, t):
                self.a.write(t)
                self.a.flush()
                self.b.write(t)
                self.b.flush()
            def flush(self):
                self.a.flush()
                self.b.flush()
        logf = open(a.log, "a", encoding="utf-8", newline="\n")
        logf.write("\n---- %s ----\n" % time.strftime("%Y-%m-%d %H:%M:%S"))
        sys.stdout, sys.stderr = Tee(sys.stdout, logf), Tee(sys.stderr, logf)
    game = os.path.abspath(a.game)
    if not os.path.isfile(game):
        die("no such file: " + game)
    tools = os.path.abspath(a.tools)
    deps = os.path.join(ROOT, "deps")
    os.chdir(ROOT)
    os.makedirs(os.path.join(ROOT, "work", "logs"), exist_ok=True)

    step("this PC")
    exe = ".exe" if os.name == "nt" else ""
    llvm = os.path.join(tools, "llvm", "bin")
    python = sys.executable
    for need in (os.path.join(llvm, "clang" + exe), os.path.join(llvm, "ld.lld" + exe), os.path.join(tools, "bin", "a64emit.dll"),
                 os.path.join(tools, "bin", "zbic.dll"), os.path.join(tools, "bin", "ps5-native-tool" + exe),
                 os.path.join(deps, "ps5-payload-sdk"), os.path.join(deps, "PS5_Vulkan")):
        if not os.path.exists(need):
            die("missing %s: the builder's tools folder or the release's deps/ is incomplete" % need)
    try:
        import capstone, unicorn, numpy, zstandard, lz4  # noqa: F401
    except ImportError as e:
        die("the bundled Python is missing a package: %s" % e)
    if subprocess.run([os.path.join(llvm, "clang" + exe), "--version"], capture_output=True).returncode:
        die("the bundled clang does not run")
    # (the prebuilt helpers in the places the lift and the .nup reader look: a64emit's with build_emit.py's stamp, so it is used
    # as it is and not built again)
    lift_dir = os.path.join(ROOT, "third_party", "a64-kit", "lift")
    shutil.copyfile(os.path.join(tools, "bin", "a64emit.dll"), os.path.join(lift_dir, "a64emit.dll"))
    key = hashlib.sha1(b"".join(open(os.path.join(lift_dir, f), "rb").read()
                                for f in ("a64emit.cpp", os.path.join("suyu", "arm64_to_c.h")))).hexdigest()
    with open(os.path.join(lift_dir, "a64emit.dll.sha1"), "w") as f:
        f.write(key)
    shutil.copyfile(os.path.join(tools, "bin", "zbic.dll"), os.path.join(ROOT, "third_party", "zbic", "zbic.dll"))
    env = dict(os.environ)
    env["PATH"] = llvm + os.pathsep + os.path.dirname(python) + os.pathsep + env.get("PATH", "")
    env.update(PS5_CLANG=os.path.join(llvm, "clang" + exe), LLVM_CONFIG=os.path.join(llvm, "llvm-config" + exe),
               PS5_PAYLOAD_SDK=os.path.join(deps, "ps5-payload-sdk"), PS5_VULKAN=os.path.join(deps, "PS5_Vulkan"),
               PS5_MIHAWKGEN=os.path.join(deps, "PS5_MihawkGen"), PS5_DEPS=deps, LONGSHOT_VERSION="oberon-" + VERSION,
               PS5_NATIVE_TOOL=os.path.join(tools, "bin", "ps5-native-tool" + exe), PYTHONUTF8="1")
    for k in ("PYTHONPATH", "PYTHONHOME"):
        env.pop(k, None)
    os.environ.update(env)
    gb = mem_gb()
    jobs = a.jobs or max(1, min(os.cpu_count() or 1, gb // 4))
    note("ok: %d cores, %d GB of memory, %d files at once (--jobs)" % (os.cpu_count() or 1, gb, jobs))
    for k in ("RX_FP_NATIVE", "PS5_PGO", "PS5_VK_PGO", "PS5_GAME_OPT", "PS5_HOT_OPT", "RX_MARK_BITMAP", "PS5_NATIVE_TLS"):
        if os.environ.get(k):
            note("%s=%s" % (k, os.environ[k]))

    step("your game file")
    check = os.path.join(ROOT, "work", ".nup-checked")
    st = os.stat(game)
    mark = "%d %d %s" % (st.st_size, int(st.st_mtime), game)
    if a.skip_hash_check:
        note("not checked (--skip-hash-check)")
    elif os.path.exists(check) and open(check).read().strip() == mark:
        note("checked already")
    else:
        note("(if this check fails on a dump you know is good, run the same command again with --skip-hash-check at the end)")
        r = subprocess.run([python, "-I", "-X", "utf8", os.path.join(ROOT, "ps5", "tools", "check_nup.py"), game], env=env)
        if r.returncode:
            die("%s is not a good .nup of Ocarina of Time 1.1.1 (see above). If you are sure it is your own good dump, run "
                "build.cmd again with --skip-hash-check (unsupported)" % os.path.basename(game))
        with open(check, "w") as f:
            f.write(mark + "\n")

    def exefs_ok():
        if a.skip_hash_check:
            return True
        d = os.path.join(ROOT, "work", "game", "exefs")
        return all(os.path.isfile(os.path.join(d, n)) and sha256(os.path.join(d, n)) == h for n, h in EXEFS_SHA256.items())

    step("the game's program: read out of the .nup and translated to C (5 to 15 minutes)")
    lift_files = ("nx2/lift.py", "nx2/oot_patches.py", "nx2/split_hot.py", "ps5/profiles/hot_funcs_longshot_v3.txt")
    lift_mark = mark + " " + hashlib.sha256("".join(sha256(os.path.join(ROOT, *f.split("/"))) + "  " + f + "\n"
                                                    for f in lift_files).encode()).hexdigest()[:16]
    done_file = os.path.join(ROOT, "work", "lift", ".done")
    if os.path.isfile(os.path.join(ROOT, "work", "lift", "sa", "gen", "a64_table.c")) and \
            os.path.isfile(os.path.join(ROOT, "work", "lift", "sa", "image", "image.bin")) and \
            os.path.exists(done_file) and open(done_file).read().strip() == lift_mark:
        note("done already, from this file")
    else:
        if os.path.exists(done_file):
            os.remove(done_file)
        for attempt in (1, 2, 3):
            rmtree(os.path.join(ROOT, "work", "game"))
            run("game-extract", [python, "-I", "-u", "-X", "utf8", "nx2/nup.py", game, "--only", "exefs", "--extract",
                                 os.path.join(ROOT, "work", "game")], env)
            if exefs_ok():
                break
            note("the extracted program does not match its hash (attempt %d): extracting it again" % attempt)
            if attempt == 3:
                die("the game's program never extracted right: not Ocarina of Time 1.1.1, or this PC's memory is unstable")
        os.makedirs(os.path.join(ROOT, "work", "lift", "image"), exist_ok=True)
        lenv = dict(env, OOT_WORK=os.path.join(ROOT, "work", "lift"),
                    A64_HOT_FUNCS=os.path.join(ROOT, "ps5", "profiles", "hot_funcs_longshot_v3.txt"), A64_HOT_CHUNK="1")
        run("game-lift", [python, "-I", "-u", "-X", "utf8", "nx2/lift.py", "--standalone", "--write", "--exefs",
                          os.path.join(ROOT, "work", "game", "exefs"), "--work", os.path.join(ROOT, "work", "lift")], lenv)
        run("game-split-hot", [python, "-I", "nx2/split_hot.py", "work/lift/sa/gen"], env)
        run("game-patches", [python, "-s", "-E", "nx2/oot_patches.py", "work/lift/sa/gen"], env)
        with open(done_file, "w") as f:
            f.write(lift_mark + "\n")

    step("the game's shader list and button prompts, from its files (5 to 10 minutes)")
    romfs = os.path.join(ROOT, "work", "romfs")
    assets = os.path.join(ROOT, "work", "assets")
    adone = os.path.join(assets, ".done")
    if os.path.isfile(os.path.join(assets, "longshot_shaders.bin")) and os.path.exists(adone) and open(adone).read().strip() == mark:
        note("done already, from this file")
    else:
        rmtree(assets)
        rmtree(romfs)
        os.makedirs(assets)
        extract = os.path.join(ROOT, "work", "romfs-extract")
        run("game-romfs", [python, "-I", "-u", "-X", "utf8", "nx2/nup.py", game, "--only", "romfs", "--extract", extract,
                           "--romfs-parts", ROMFS_PARTS], env)
        found = heads = None
        for base, dirs, files in os.walk(extract):
            depth = os.path.relpath(base, extract).count(os.sep) + 1
            if depth > 3:
                dirs[:] = []
                continue
            if found is None and "romfs" in dirs and depth <= 2:
                found = os.path.join(base, "romfs")
            if heads is None and "romfs_zstd_heads.txt" in files:
                heads = os.path.join(base, "romfs_zstd_heads.txt")
        if not found:
            die("the .nup's romfs was not extracted; the log: work/logs/game-romfs.log")
        if heads and len(open(heads).readlines()) > 1:
            die("game files failed their hashes as they were extracted (%s lists them): run again" % heads)
        shutil.move(found, romfs)
        rmtree(extract)
        run("shader-list", [python, "-I", "-X", "utf8", "nx2/shader_census.py", romfs, "--out",
                            os.path.join(assets, "longshot_shaders.bin")], env)
        with open(os.path.join(ROOT, "work", "logs", "prompts.log"), "w", newline="\n") as f:
            r = subprocess.run([python, "-I", "-X", "utf8", "nx2/prompt_assets.py", "--romfs", romfs, "--out",
                                os.path.join(assets, "prompts")], stdout=f, stderr=subprocess.STDOUT, env=env)
        if r.returncode == 0:
            note("button prompts made")
        else:
            rmtree(os.path.join(assets, "prompts"))
            note("warning: the button prompts were not made (the log: work/logs/prompts.log)")
        rmtree(romfs)
        with open(adone, "w") as f:
            f.write(mark + "\n")

    step("the title: the game, the runtime and the renderer compiled and linked (30 to 90 minutes the first time)")
    tenv = dict(env, PS5_VK_PGO=os.environ.get("PS5_VK_PGO") or os.path.join(ROOT, "ps5", "profiles", "vk.profdata"),
                PS5_VK_LTO="thin")
    for attempt in (1, 2):
        log = os.path.join(ROOT, "work", "logs", "title.log")
        with open(log, "w", newline="\n") as f:
            p = subprocess.Popen([python, "-I", "-u", "ps5/build.py", "--jobs", str(jobs)], stdout=subprocess.PIPE,
                                 stderr=subprocess.STDOUT, env=tenv, cwd=ROOT, text=True, encoding="utf-8", errors="replace")
            for line in p.stdout:   # (tee)
                sys.stdout.write(line)
                sys.stdout.flush()
                f.write(line)
            p.wait()
        if p.returncode == 0:
            break
        if attempt == 2:
            die("the title was not built twice; the whole log: %s (if the compiler was killed for want of memory, use "
                "--jobs 2 or --jobs 1)" % log)
        note("the title's build failed (the end of its log is above): trying once more, with what compiled kept")
    title = os.path.join(ROOT, "dist", TITLE_ID)
    if not (os.path.isfile(os.path.join(title, "eboot.bin")) and os.path.isfile(os.path.join(title, "image.bin"))):
        die("no title at %s; the log: work/logs/title.log" % title)

    step("the title folder")
    shutil.copyfile(os.path.join(assets, "longshot_shaders.bin"), os.path.join(title, "longshot_shaders.bin"))
    shipped = os.path.join(title, "longshot_pipeline_states_shipped.bin")
    if os.path.exists(shipped):
        os.remove(shipped)
    if os.path.isdir(os.path.join(ROOT, "assets", "pipeline_states")):
        with open(os.path.join(ROOT, "work", "logs", "pipeline_states.log"), "w", newline="\n") as f:
            r = subprocess.run([python, "-I", os.path.join(ROOT, "nx2", "pipeline_states.py"), "merge", "--dir",
                                os.path.join(ROOT, "assets", "pipeline_states"), "--out", shipped], stdout=f,
                               stderr=subprocess.STDOUT, env=env)
        if r.returncode == 0:
            last = open(os.path.join(ROOT, "work", "logs", "pipeline_states.log")).read().strip().splitlines()
            note("shipped pipeline list: " + (last[-1] if last else ""))
        else:
            if os.path.exists(shipped):
                os.remove(shipped)
            note("no shipped pipeline list (see work/logs/pipeline_states.log): pipelines are built when first drawn")
    rmtree(os.path.join(title, "prompts"))
    rmtree(os.path.join(title, "mods"))
    if os.path.isdir(os.path.join(assets, "prompts")):
        shutil.copytree(os.path.join(assets, "prompts"), os.path.join(title, "prompts"))
    else:
        note("no button prompts (see work/logs/prompts.log): the game shows the Switch's")
    shutil.copytree(os.path.join(ROOT, "title", "mods"), os.path.join(title, "mods"))
    for f in ("longshot_options.ini", "longshot_env.txt"):
        if not os.path.exists(os.path.join(title, f)):
            shutil.copyfile(os.path.join(ROOT, "title", f), os.path.join(title, f))
    if a.out:
        out = os.path.join(os.path.abspath(a.out), TITLE_ID)
        rmtree(out)
        shutil.copytree(title, out)
        title = out
    eboot = os.path.join(title, "eboot.bin")
    print("\nBuilt: %s in %d min\neboot.bin sha256 %s\n" % (title, (time.time() - T0) // 60, sha256(eboot)), flush=True)
    print("On the console, with its homebrew environment running and an FTP server on:\n"
          "  1. Copy the whole %s folder to /data/homebrew/%s.\n"
          "  2. Copy your game file, named oot.nup, to one of these (the first one there is used):\n"
          "       /data/homebrew/%s/oot.nup       in the title folder itself: everything in one folder\n"
          "       /data/lichii/roms/oot/oot.nup          shared with PS5_Lichii (make the folders)\n"
          "       /data/prosperoeden/roms/oot/oot.nup    PS5_Lichii's older folder\n"
          "     or any path, set as HOS_PS5_NUP=<path> in longshot_env.txt.\n"
          "  3. Start Longshot Oberon from the home screen (ShadowMountPlus puts it there) or your homebrew launcher.\n"
          "eboot.bin, image.bin, longshot_shaders.bin and prompts/ are made from your game: keep them to yourself." % (TITLE_ID, TITLE_ID, TITLE_ID),
          flush=True)


if __name__ == "__main__":
    main()
