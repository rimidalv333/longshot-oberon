"""PS5: link the title and lay out its folder: ps5/tools/link.sh in Python, for the Windows builder (build.cmd).

    python -I ps5/tools/link.py BUILD_DIR IMAGE_BIN

ps5/build.py runs it in link.sh's place on Windows (os.name == "nt"); Linux and WSL keep link.sh. Every step is link.sh's, in its
order and with its arguments in the same order (with PS5_Vulkan's tools/radv-link.sh recipe and the payload SDK's prospero-lld
and prospero-clang18 wrappers folded in), so the same objects link into the same bytes: the lists link.sh sorts are sorted the way
`sort` does under the C locale (code points), as WSL's does. What it needs, from build.cmd's environment: PS5_PAYLOAD_SDK,
PS5_VULKAN, PS5_FFMPEG, PS5_DEPS, PS5_CLANG (clang.exe; ld.lld.exe, llvm-ar.exe, llvm-objcopy.exe and llvm-nm.exe beside it) and
PS5_NATIVE_TOOL (ps5-native-tool.exe; default PS5_Vulkan's build/host/ps5-native-tool).
"""
import glob
import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def die(msg, code=1):
    print(msg, file=sys.stderr, flush=True)
    sys.exit(code)


def run(cmd, cwd=None, capture=False):
    r = subprocess.run(cmd, cwd=cwd, capture_output=capture, text=capture)
    if r.returncode:
        if capture:
            sys.stderr.write((r.stdout or "") + (r.stderr or ""))
        die("link.py: failed (%d): %s" % (r.returncode, " ".join(cmd[:6]) + (" ..." if len(cmd) > 6 else "")), 2)
    return r.stdout if capture else None


def main():
    if len(sys.argv) != 3:
        die("usage: link.py BUILD_DIR IMAGE_BIN")
    work, image = sys.argv[1], sys.argv[2]
    env = os.environ
    sdk, vulkan, ffmpeg = env["PS5_PAYLOAD_SDK"], env["PS5_VULKAN"], env["PS5_FFMPEG"]
    deps = env.get("PS5_DEPS") or os.path.join(ROOT, ".local", "ps5-deps")
    archive = env.get("RADV_ARCHIVE") or os.path.join(deps, "radv", "lib", "libvulkan_radeon.ps5.a")
    if not os.path.isfile(archive):
        archive = os.path.join(vulkan, ".deps", "native", "radv-release", "lib", "libvulkan_radeon.ps5.a")
    clang = env.get("PS5_CLANG") or shutil.which("clang")
    bindir = os.path.dirname(clang)
    exe = ".exe" if os.name == "nt" else ""
    lld, ar, objcopy, nm = (os.path.join(bindir, t + exe) for t in ("ld.lld", "llvm-ar", "llvm-objcopy", "llvm-nm"))
    tool = env.get("PS5_NATIVE_TOOL") or os.path.join(vulkan, "build", "host", "ps5-native-tool")
    native = os.path.join(vulkan, "tooling", "native")
    param = os.path.join(ROOT, "ps5", "sce_sys", "param.json")
    for f in (archive, tool, param, lld, os.path.join(vulkan, "runtime", "libc.prx"), image,
              os.path.join(ffmpeg, "lib", "libavcodec.a"), os.path.join(ffmpeg, "lib", "libdav1d.a")):
        if not os.path.exists(f):
            die("missing: %s" % f, 2)
    for d in ("obj", "stubs", "heapwrap"):
        os.makedirs(os.path.join(work, "link", d), exist_ok=True)
    obj, stubs = os.path.join(work, "link", "obj"), os.path.join(work, "link", "stubs")

    # (the payload SDK's prospero-lld: its -pie and linker script unless asked otherwise, and its fixed options around the call's)
    def prospero_lld(args):
        pie = not any(a in ("--shared", "-r", "--static") for a in args)
        script = not any(a in ("--script", "-T") for a in args)
        return [lld, "-m", "elf_x86_64"] + (["-pie"] if pie else []) + \
            (["-T" + os.path.join(sdk, "ldscripts", "elf_x86_64.x")] if script else []) + \
            ["--eh-frame-hdr", "-z", "max-page-size=0x4000", "-mllvm", "-emulated-tls"] + args + ["--hash-style=gnu"]

    # (PS5_Vulkan's tooling/prospero-clang18)
    def cc(args):
        return [clang, "-target", "x86_64-sie-ps5", "-fvisibility-nodllstorageclass=default", "-isysroot", sdk,
                "-isystem", os.path.join(sdk, "target", "include", "c++", "v1"), "-isystem", os.path.join(sdk, "target", "include"),
                "-fno-stack-protector", "-fno-plt", "-femulated-tls", "-fdenormal-fp-math=ieee"] + args

    run(cc(["-std=c++20", "-O2", "-fno-exceptions", "-fno-rtti", "-c", os.path.join(native, "app_crt.cpp"), "-o",
            os.path.join(obj, "app_crt.o")]))
    for library, source in (("libSceAgc", "vendor/ps5/sdk/stubs/agc_canary_link_stub.c"),
                            ("libSceAgcDriver", "vendor/ps5/sdk/stubs/agc_driver_canary_link_stub.c")):
        o = os.path.join(obj, library + "_stub.o")
        run(cc(["-std=c11", "-O2", "-fPIC", "-c", os.path.join(vulkan, *source.split("/")), "-o", o]))
        run(prospero_lld(["--shared", "-soname", library + ".prx", "-o", os.path.join(stubs, library + ".so"), o]))
    heapwrap = os.path.join(work, "link", "heapwrap")
    run([ar, "x", os.path.join(sdk, "target", "lib", "libps5platform.a"), "heap_wrap.o"], cwd=heapwrap)
    redefs = []
    for name in ("malloc", "calloc", "realloc", "free", "posix_memalign", "aligned_alloc", "memalign", "malloc_usable_size",
                 "reallocf", "reallocarray", "getline", "getdelim"):
        redefs += ["--redefine-sym", "__wrap_%s=ps5w_%s" % (name, name)]
    run([objcopy] + redefs + [os.path.join(heapwrap, "heap_wrap.o"), os.path.join(obj, "heap_wrap_ps5w.o")])

    # ---- tools/radv-link.sh's radv_link_recipe ----
    builtins = os.path.join(run([clang, "--print-resource-dir"], capture=True).strip(), "lib", "linux",
                            "libclang_rt.builtins-x86_64.a")
    platform = os.path.join(sdk, "target", "lib", "libps5platform.a")
    linker_script = ["-T", os.path.join(vulkan, "tooling", "psbc", "ps5-pie-unwind.ld"), "-L", native]
    lib = os.path.join(sdk, "target", "lib")
    inputs = ["-L", lib, "--whole-archive", archive, "--no-whole-archive", "--start-group", os.path.join(lib, "libc++.a"),
              os.path.join(lib, "libc++abi.a"), os.path.join(lib, "libunwind.a"), builtins, platform, "--end-group"]
    flags = ["--wrap=pthread_create", "--wrap=pthread_join", "--wrap=pthread_detach", "--wrap=fclose", "--wrap=fflush"]
    for name in ("malloc", "calloc", "realloc", "free", "posix_memalign", "aligned_alloc", "memalign", "malloc_usable_size",
                 "reallocf", "reallocarray", "getline", "getdelim"):
        flags.append("--wrap=" + name)
    for name in ("qsort_r mkstemps openlog popen pclose open_memstream __xuname __assert __memset_chk regcomp regexec regfree "
                 "regerror localtime_r newlocale freelocale strtod_l strtof_l dladdr utimensat localeconv_l strtoll_l strtoull_l "
                 "strtold_l snprintf_l sscanf_l asprintf_l strcoll_l strxfrm_l strftime_l wcscoll_l wcsxfrm_l btowc_l wctob_l "
                 "iswctype_l mbrlen_l mbrtowc_l mbsrtowcs_l mbsnrtowcs_l wcrtomb_l wcsnrtombs_l mbtowc_l ___mb_cur_max_l "
                 "___runetype_l ___tolower_l ___toupper_l __runes_for_locale catopen catgets catclose backtrace "
                 "backtrace_symbols_fd __cxa_thread_atexit_impl").split():
        flags.append("--defsym=%s=ps5_%s" % (name, name))
    r = subprocess.run([nm, "--defined-only", platform], capture_output=True, text=True)
    platform_symbols = r.stdout if r.returncode == 0 else ""
    lines = set(platform_symbols.splitlines())
    def defines(sym):
        return any(l.endswith(" T " + sym) for l in lines)
    if defines("ps5_localeconv"):
        flags.append("--defsym=localeconv=ps5_localeconv")
    for name in ("arc4random arc4random_buf arc4random_uniform gmtime_r statvfs fstatvfs futimens clock_nanosleep getaddrinfo "
                 "freeaddrinfo if_nameindex if_freenameindex opendir fdopendir readdir rewinddir dirfd closedir nl_langinfo "
                 "nl_langinfo_l getpwuid_r posix_fallocate access openat unlinkat fchmodat fstatat mkdirat renameat "
                 "memfd_create").split():
        flags.append("--defsym=%s=ps5_%s" % (name, name))
    for name in ("strcasestr", "mkstemp", "readlink", "link", "symlink"):
        if defines("ps5_" + name):
            flags.append("--defsym=%s=ps5_%s" % (name, name))
    local_map = os.path.join(vulkan, "build", "radv-platform-local.map")
    os.makedirs(os.path.dirname(local_map), exist_ok=True)
    with open(local_map + ".tmp", "w", newline="\n") as f:
        f.write("{\n    local:\n")
        for flag in flags:
            if flag.startswith("--defsym="):
                f.write("        %s;\n" % flag[len("--defsym="):].split("=", 1)[0])
        f.write("};\n")
    os.replace(local_map + ".tmp", local_map)
    flags += ["--version-script", local_map]
    for f in (archive, platform, os.path.join(lib, "libc++.a"), os.path.join(lib, "libc++abi.a"), os.path.join(lib, "libunwind.a"),
              builtins):
        if not os.path.isfile(f):
            die("missing " + f, 2)
    # ---- back in link.sh ----
    if env.get("PS5_NATIVE_TLS", "1") not in ("0", "off", "no", "false", ""):
        linker_script = ["-T", os.path.join(ROOT, "ps5", "tools", "ps5-pie-tls.ld")]
    with open(os.path.join(work, "link", "local.map"), "w", newline="\n") as f:
        f.write("{\n    local:\n        fnmatch;\n        isatty;\n        pthread_setname_np;\n        timegm;\n};\n")
    extra = env.get("PS5_LINK_EXTRA", "").split()
    sos = sorted(glob.glob(os.path.join(glob.escape(lib), "*.so")), key=lambda p: os.path.basename(p).encode())
    elf = os.path.join(work, "link", "llvm-pie.elf")

    def link(more):
        args = linker_script + ["--eh-frame-hdr"] + flags + more + \
            ["--version-script", os.path.join(native, "app-symbols.map"), "--version-script", os.path.join(work, "link", "local.map"),
             "--exclude-libs=ALL", "--gc-sections", "-e", "_start", "-Map=" + os.path.join(work, "link", "hookshot.map"), "-o", elf,
             os.path.join(obj, "app_crt.o"), os.path.join(obj, "heap_wrap_ps5w.o"), "@" + os.path.join(work, "objects.txt"),
             os.path.join(ffmpeg, "lib", "libavcodec.a"), os.path.join(ffmpeg, "lib", "libswscale.a"),
             os.path.join(ffmpeg, "lib", "libavutil.a"), os.path.join(ffmpeg, "lib", "libdav1d.a"),
             os.path.join(deps, "zstd", "lib", "libzstd.a"),
             os.path.join(stubs, "libSceAgc.so"), os.path.join(stubs, "libSceAgcDriver.so")] + inputs + ["--as-needed"] + sos
        # (a response file: the command is past what Windows takes on one line)
        rsp = os.path.join(work, "link", "link.rsp")
        cmd = prospero_lld(args)
        with open(rsp, "w", newline="\n") as f:
            for a in cmd[1:]:
                f.write('"%s"\n' % a.replace("\\", "\\\\").replace('"', '\\"'))
        # (ld.lld reads a response file the Windows way on Windows: told the posix way, as objects.txt is written)
        run([cmd[0]] + (["--rsp-quoting=posix"] if os.name == "nt" else []) + ["@" + rsp])

    start = time.time()
    link(extra)
    exported = set()
    for library in sos + [os.path.join(stubs, "libSceAgc.so"), os.path.join(stubs, "libSceAgcDriver.so")]:
        if os.path.basename(library) in ("libkernel_sys.so", "libScePosixForWebKit.so"):
            continue
        r = subprocess.run([nm, "-D", "--defined-only", library], capture_output=True, text=True)
        for line in r.stdout.splitlines():
            if line.split():
                exported.add(line.split()[-1])
    with open(os.path.join(work, "link", "exported.txt"), "w", newline="\n") as f:
        f.write("".join(s + "\n" for s in sorted(exported, key=str.encode)))
    undefined = run([nm, "-D", "--undefined-only", elf], capture=True)
    weak = set()
    for line in undefined.splitlines():
        p = line.split()
        if len(p) >= 2 and p[0] in ("w", "v"):
            weak.add(p[1].split("@")[0])
    weak_rsp = os.path.join(work, "link", "weak.rsp")
    with open(weak_rsp, "w", newline="\n") as f:
        f.write("".join("--defsym=%s=0\n" % s for s in sorted(weak - exported, key=str.encode)))
    if os.path.getsize(weak_rsp):
        link(extra + ["@" + weak_rsp])
    print("linked: %d MB (%d s)" % (os.path.getsize(elf) >> 20, time.time() - start), flush=True)
    undefined = run([nm, "-D", "--undefined-only", elf], capture=True)
    null = sorted({l.split()[1].split("@")[0] for l in undefined.splitlines() if len(l.split()) >= 2 and l.split()[0] == "U"}
                  - exported, key=str.encode)
    if null:
        die("link.py: imports that no module a title loads exports (null at run time): " + " ".join(null))
    with open(elf, "rb") as f:
        h = f.read(64)
        phoff, = struct.unpack_from("<Q", h, 32)
        phentsize, phnum = struct.unpack_from("<HH", h, 54)
        zero = 0
        for i in range(phnum):
            f.seek(phoff + i * phentsize)
            ptype, pflags, off, va, pa, filesz, memsz, align = struct.unpack("<IIQQQQQQ", f.read(56))
            if ptype == 1 and pflags & 2:
                zero += memsz - filesz
    limit = int(env.get("PS5_BSS_LIMIT_MB", "176")) << 20
    print("link.py: zero-filled data (.bss) %.1f MB of %d MB the console's flexible memory allows" % (zero / 2**20, limit >> 20),
          flush=True)
    if zero > limit:
        die("link.py: .bss %.1f MB is over PS5_BSS_LIMIT_MB=%d" % (zero / 2**20, limit >> 20))
    new = os.path.join(work, "eboot.elf.new")
    run([tool, "link", "--in", elf, "--out", new, "--stub-dir", lib, "--stub", os.path.join(stubs, "libSceAgc.so"),
         "--stub", os.path.join(stubs, "libSceAgcDriver.so"), "--module-sdk", "0x02000009", "--companion-sdk", "0x08050001",
         "--file-name", "eboot.elf"])
    title_id = json.load(open(param))["titleId"]
    app = os.path.join(env.get("PS5_DIST") or os.path.join(ROOT, "dist"), title_id)   # (PS5_DIST: a test link elsewhere)
    os.makedirs(os.path.join(app, "sce_sys"), exist_ok=True)
    os.makedirs(os.path.join(app, "sce_module"), exist_ok=True)
    run([tool, "self", "--sign", "--in", new, "--out", os.path.join(app, "eboot.bin"), "--magic", "0x1D3D154F"])
    run([tool, "self", "--inspect", "--file", os.path.join(app, "eboot.bin")], capture=True)
    shutil.copyfile(param, os.path.join(app, "sce_sys", "param.json"))
    for asset in ("icon0.png", "pic0.dds", "pic1.dds"):
        src = os.path.join(ROOT, "ps5", "sce_sys", asset)
        if os.path.isfile(src):
            shutil.copyfile(src, os.path.join(app, "sce_sys", asset))
    prx = os.path.join(vulkan, "runtime", "libc.prx")
    want = open(prx + ".sha256").read().split()[0]
    if hashlib.sha256(open(prx, "rb").read()).hexdigest() != want:
        die("link.py: runtime/libc.prx does not match libc.prx.sha256")
    shutil.copyfile(prx, os.path.join(app, "sce_module", "libc.prx"))
    shutil.copyfile(image, os.path.join(app, "image.bin"))
    for p in (new, os.path.join(app, "eboot.bin")):
        print("wrote %d bytes: %s\nsha256 %s" % (os.path.getsize(p), p, hashlib.sha256(open(p, "rb").read()).hexdigest()), flush=True)
    print("title: %s; the romfs is read from the game's .nup on the console (host/ps5/nup_romfs.c)" % app, flush=True)


if __name__ == "__main__":
    main()
