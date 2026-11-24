#!/usr/bin/env bash
# PS5: link the title and lay out its folder (run by ps5/build.py once the objects are compiled).
#
#   link.sh BUILD_DIR IMAGE_BIN
#
# The link is PS5_VulkanTemplate's (ps5/tools/link-title.sh): PS5_Vulkan's RADV release archive and link recipe
# (tools/radv-link.sh: the platform layer, the heap and thread wraps, the libc names bound to ps5_*), its CRT, its native
# tool (ELF to the console's fake SELF) and its libc.prx. The title folder, dist/<titleId>/, gets the signed
# eboot.bin, sce_sys/ and sce_module/; image.bin (made from your game by the lift) goes beside it, in dist/ only.
set -euo pipefail
work=$1 image=$2
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
sdk=${PS5_PAYLOAD_SDK:?} vulkan=${PS5_VULKAN:?} ffmpeg=${PS5_FFMPEG:?}
# (the driver: RADV_ARCHIVE, else a build kept in .local/ps5-deps/radv (its REVISION beside it), else PS5_Vulkan's own)
deps=${PS5_DEPS:-$root/.local/ps5-deps}
archive=${RADV_ARCHIVE:-$deps/radv/lib/libvulkan_radeon.ps5.a}
[[ -f $archive ]] || archive=$vulkan/.deps/native/radv-release/lib/libvulkan_radeon.ps5.a
export PS5_CLANG=${PS5_CLANG:-$(command -v clang)}
tool="$vulkan/build/host/ps5-native-tool"
native="$vulkan/tooling/native"
param="$root/ps5/sce_sys/param.json"
for file in "$archive" "$tool" "$param" "$sdk/bin/prospero-lld" "$vulkan/tools/radv-link.sh" "$vulkan/runtime/libc.prx" \
        "$image" "$ffmpeg/lib/libavcodec.a" "$ffmpeg/lib/libdav1d.a"; do
    [[ -e $file ]] || { echo "missing: $file" >&2; exit 2; }
done
mkdir -p "$work/link/obj" "$work/link/stubs"
cc() { PS5_PAYLOAD_SDK="$sdk" sh "$vulkan/tooling/prospero-clang18" "$@"; }
# The CRT; the C++ runtime is libc++abi's (Eden's recompiler throws and catches)
cc -std=c++20 -O2 -fno-exceptions -fno-rtti -c "$native/app_crt.cpp" -o "$work/link/obj/app_crt.o"
stub() {   # RADV calls AGC, which the SDK has no stubs for: these name its imports
    local library=$1 source=$2
    cc -std=c11 -O2 -fPIC -c "$vulkan/$source" -o "$work/link/obj/${library}_stub.o"
    "$sdk/bin/prospero-lld" --shared -soname "${library}.prx" -o "$work/link/stubs/${library}.so" "$work/link/obj/${library}_stub.o"
}
stub libSceAgc vendor/ps5/sdk/stubs/agc_canary_link_stub.c
stub libSceAgcDriver vendor/ps5/sdk/stubs/agc_driver_canary_link_stub.c
# (base PS5) the heap by caller (host/ps5/heap_prof.c): the platform heap's wraps (heap_wrap.o in libps5platform.a) renamed
# ps5w_*, and host/ps5/heap_prof.c's __wrap_* in front of them. With all twelve defined by the objects, the archive's own
# heap_wrap.o is never pulled in
mkdir -p "$work/link/heapwrap"
(cd "$work/link/heapwrap" && "$sdk/bin/llvm-ar" x "$sdk/target/lib/libps5platform.a" heap_wrap.o)
heap_redefs=()
for name in malloc calloc realloc free posix_memalign aligned_alloc memalign malloc_usable_size reallocf reallocarray \
        getline getdelim; do
    heap_redefs+=(--redefine-sym "__wrap_$name=ps5w_$name")
done
"$sdk/bin/llvm-objcopy" "${heap_redefs[@]}" "$work/link/heapwrap/heap_wrap.o" "$work/link/obj/heap_wrap_ps5w.o"
# shellcheck source=/dev/null
source "$vulkan/tools/radv-link.sh"
radv_link_recipe "$vulkan" "$sdk" "$archive" || exit 2
# (base PS5) native thread-local storage (ps5/build.py PS5_NATIVE_TLS, on unless 0): the layout with a PT_TLS header and
# .tdata inside the data segment's stored bytes (ps5/tools/ps5-pie-tls.ld says why); PS5_Vulkan's own layout otherwise
case ${PS5_NATIVE_TLS:-1} in
    0 | off | no | false | "") ;;
    *) radv_linker_script=(-T "$root/ps5/tools/ps5-pie-tls.ld") ;;
esac
# The libc calls host/ps5/win32_ps5.c defines because no module a title loads exports them (fnmatch, isatty,
# pthread_setname_np, timegm): kept inside the executable, which may export nothing
printf '{\n    local:\n        fnmatch;\n        isatty;\n        pthread_setname_np;\n        timegm;\n};\n' > "$work/link/local.map"
start=$SECONDS
link() {
"$sdk/bin/prospero-lld" "${radv_linker_script[@]}" --eh-frame-hdr "${radv_link_flags[@]}" "$@" \
    --version-script "$native/app-symbols.map" --version-script "$work/link/local.map" --exclude-libs=ALL --gc-sections \
    -e _start -Map="$work/link/hookshot.map" -o "$work/link/llvm-pie.elf" \
    "$work/link/obj/app_crt.o" "$work/link/obj/heap_wrap_ps5w.o" @"$work/objects.txt" \
    "$ffmpeg/lib/libavcodec.a" "$ffmpeg/lib/libswscale.a" "$ffmpeg/lib/libavutil.a" "$ffmpeg/lib/libdav1d.a" \
    "$deps/zstd/lib/libzstd.a" \
    "$work/link/stubs/libSceAgc.so" "$work/link/stubs/libSceAgcDriver.so" \
    "${radv_link_inputs[@]}" \
    --as-needed "$sdk"/target/lib/*.so
}
# shellcheck disable=SC2086  # (PS5_LINK_EXTRA: options, split on spaces: ps5/build.py PS5_ORDER)
link ${PS5_LINK_EXTRA:-}
# What the modules a title loads export: the SDK's stubs but libkernel_sys's and libScePosixForWebKit's, and AGC's.
for library in "$sdk"/target/lib/*.so "$work/link/stubs/libSceAgc.so" "$work/link/stubs/libSceAgcDriver.so"; do
    case ${library##*/} in libkernel_sys.so | libScePosixForWebKit.so) continue ;; esac
    "$sdk/bin/llvm-nm" -D --defined-only "$library" 2>/dev/null | awk '{ print $NF }'
done | sort -u > "$work/link/exported.txt"
# A weak reference nothing defines reads as null: the entry points RADV leaves to Mesa's common code (some 5,700), and
# the initialiser of a C++ thread_local that needs none (_ZTH...). lld 18 keeps them all in the dynamic table and later
# ones a few, and the native tool takes every undefined symbol there for an import. They are bound to 0 here, which is
# what they read as anyway.
"$sdk/bin/llvm-nm" -D --undefined-only "$work/link/llvm-pie.elf" |
    awk '$1 == "w" || $1 == "v" { sub(/@.*/, "", $2); print $2 }' | sort -u |
    comm -23 - "$work/link/exported.txt" | sed 's/.*/--defsym=&=0/' > "$work/link/weak.rsp"
# shellcheck disable=SC2086  # (the second link takes PS5_LINK_EXTRA too: PS5_VK_PGO=gen's --no-dependent-libraries)
[[ ! -s $work/link/weak.rsp ]] || link ${PS5_LINK_EXTRA:-} @"$work/link/weak.rsp"
echo "linked: $(( $(stat -c %s "$work/link/llvm-pie.elf") >> 20 )) MB ($(( SECONDS - start )) s)"
# A title loads neither libkernel_sys's exports nor libScePosixForWebKit's: an import only their stubs define links,
# and is null at run time (its first call jumps to address 0). Refuse it here, not on the console.
null_imports=$(comm -23 \
    <("$sdk/bin/llvm-nm" -D --undefined-only "$work/link/llvm-pie.elf" | awk '$1 == "U" { sub(/@.*/, "", $2); print $2 }' | sort -u) \
    "$work/link/exported.txt")
if [[ -n $null_imports ]]; then
    echo "link.sh: imports that no module a title loads exports (null at run time): ${null_imports//$'\n'/ }" >&2
    exit 1
fi
# (base PS5, 11-20: klog_51) The console maps an executable's zero-filled data (.bss) from the title's flexible memory at
# load, ~188 MB with the usual payloads (kstuff, ftpsrv, shadowmountplus, elfldr, klogsrv). Over it the launch fails
# ("Not enough flexible memory to load executable", EXECFAIL 12) and on this firmware SceSysCore crashed with it: the whole
# console. Refuse such a title here. PS5_BSS_LIMIT_MB (default 176) is the line; large tables go to the heap instead.
python3 -I - "$work/link/llvm-pie.elf" "${PS5_BSS_LIMIT_MB:-176}" <<'PYEOF' || exit 1
import struct, sys
f = open(sys.argv[1], "rb"); h = f.read(64)
phoff, = struct.unpack_from("<Q", h, 32); phentsize, phnum = struct.unpack_from("<HH", h, 54)
zero = 0
for i in range(phnum):
    f.seek(phoff + i * phentsize)
    ptype, flags, off, va, pa, filesz, memsz, align = struct.unpack("<IIQQQQQQ", f.read(56))
    if ptype == 1 and flags & 2:
        zero += memsz - filesz
limit = int(sys.argv[2]) << 20
print("link.sh: zero-filled data (.bss) %.1f MB of %d MB the console's flexible memory allows" % (zero / 2**20, limit >> 20))
if zero > limit:
    sys.exit("link.sh: .bss %.1f MB is over PS5_BSS_LIMIT_MB=%d: the console would refuse to load the title (and may crash "
             "with it). Move a large static table to the heap" % (zero / 2**20, limit >> 20))
PYEOF
"$tool" link --in "$work/link/llvm-pie.elf" --out "$work/eboot.elf.new" \
    --stub-dir "$sdk/target/lib" --stub "$work/link/stubs/libSceAgc.so" \
    --stub "$work/link/stubs/libSceAgcDriver.so" --module-sdk 0x02000009 \
    --companion-sdk 0x08050001 --file-name eboot.elf
title_id=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["titleId"])' "$param")
app="$root/dist/$title_id"
mkdir -p "$app/sce_sys" "$app/sce_module"
"$tool" self --sign --in "$work/eboot.elf.new" --out "$app/eboot.bin" --magic 0x1D3D154F
"$tool" self --inspect --file "$app/eboot.bin" > /dev/null
cp "$param" "$app/sce_sys/param.json"
for asset in icon0.png pic0.dds pic1.dds; do
    [[ -f $root/ps5/sce_sys/$asset ]] && cp "$root/ps5/sce_sys/$asset" "$app/sce_sys/$asset"
done
(cd "$vulkan/runtime" && sha256sum --check --strict --quiet libc.prx.sha256)
cp "$vulkan/runtime/libc.prx" "$app/sce_module/libc.prx"
# image.bin is made from your game: it stays in dist/ (ignored by git) and goes to the console with the title folder
cp "$image" "$app/image.bin"
echo "title: $app ($(du -sh "$app" | cut -f1)); the romfs is read from the game's .nup on the console (host/ps5/nup_romfs.c)"
