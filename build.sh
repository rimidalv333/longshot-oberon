#!/usr/bin/env bash
# Longshot Oberon: builds the PS5 title folder PPSA93339 from your own game file, on Linux or Ubuntu under WSL.
#
#   ./build.sh <your oot.nup> [--jobs N] [--out DIR] [--skip-hash-check]
#
# Everything the build needs comes in deps/ (the release zip): clang/lld 18, Python with its packages, the PS5 payload
# SDK, RADV (PS5_Mesa) and PS5_Vulkan's link tools, FFmpeg, zstd, the Vulkan headers and PS5_MihawkGen. Nothing is
# downloaded and nothing needs installing. The only input is your game file: The Legend of Zelda: Ocarina of Time
# (Switch 2) 1.1.1 as a .nup. Its title and update, the hashes the .nup holds for each file the build reads, and the
# game's program (EXEFS_SHA256 below) are checked before anything is built.
#
# A step that is done is not done again: after an interrupted run, run it again. Nothing made from your game leaves
# work/ and dist/.
#
# --jobs N           compile N files at once (default: one per 6 GB of memory, at most your cores)
# --out DIR          also copy the finished PPSA93339 folder into DIR
# --skip-hash-check  build from a .nup that fails those checks (unsupported)
set -euo pipefail

TITLE_ID=PPSA93339
VERSION=1.1.0
# (the game's program as the build lifts it, decompressed: the same from any good dump of 1.1.1)
EXEFS_SHA256="2591fb99008d0814f8056c3f36b36790048ed857ff0ae2bb3a4c1e37b960a4de  main
94c154d83cbaca00d092958cc2b3a0494d7308fe49de8cb33ece7e9e67303ea7  main.npdm
219f25ef4a64a704e86dacfeb25d9e8977045f2145e629268b94baf34d5e0bbe  rtld
3ae3cd4cf340d72bdaba9e6f66016a74c1eae13737a5f7299375ed6654b9b1c5  sdk
0d32c4bdbcfcfa1c4110c37eff18eaf70061a55090b7fedc741a2e37224065ba  subsdk0"

die() { printf '\nbuild: %s\n' "$*" >&2; exit 1; }
step() { printf '\n==> %s\n' "$*"; }
note() { printf '    %s\n' "$*"; }

game= out= jobs= skip_hash=
while (( $# )); do
    case $1 in
        --jobs) jobs=${2:?--jobs takes a number}; shift ;;
        --out) out=${2:?--out takes a folder}; shift ;;
        --skip-hash-check) skip_hash=1 ;;
        -h | --help) sed -n '2,18p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        -*) die "unknown option $1 (--help lists them)" ;;
        *) game=$1 ;;
    esac
    shift
done
[[ -n $game ]] || die "usage: ./build.sh <your oot.nup> [--jobs N] [--out DIR]"
[[ -f $game ]] || die "no such file: $game"
game=$(realpath -- "$game")

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
deps="$root/deps"
cd "$root"
logs="$root/work/logs"
mkdir -p "$logs"

run() {   # log-name command...: quiet while it works, the end of its log when it fails
    local log="$logs/$1.log" start=$SECONDS
    shift
    "$@" > "$log" 2>&1 || { tail -n 40 "$log" >&2; die "that step failed; the whole log: $log"; }
    note "done in $(( SECONDS - start )) s"
}

step "this PC"
[[ $(uname -s) == Linux && $(uname -m) == x86_64 ]] || die "this builds on x86-64 Linux (or Ubuntu under WSL) only"
[[ -d $deps/llvm && -d $deps/python && -d $deps/ps5-payload-sdk && -d $deps/PS5_Vulkan ]] ||
    die "deps/ is missing or incomplete: build from the release zip (longshot-oberon-*.zip), which carries it"
for command in bash tar sha256sum awk sed sort comm find cp; do
    command -v "$command" > /dev/null || die "$command is needed (it is part of every Linux system's base tools)"
done
case $root in
    /mnt/[a-z]/*) note "warning: $root is on a Windows drive: the build is several times slower there. Unzip into your"
                  note "Linux home folder (cd ~) for a faster build." ;;
esac
# (a zip unpacked by another tool may lose the executable bits: given back here)
find "$deps/llvm/bin" "$deps/python/bin" "$deps/ps5-payload-sdk/bin" "$deps/PS5_Vulkan/build/host" "$deps/PS5_Vulkan/tooling" \
    "$deps/PS5_Vulkan/tools" "$root/ps5/tools" -type f -exec chmod +x {} + 2> /dev/null || true
export PATH="$deps/llvm/bin:$deps/python/bin:$PATH"
export PS5_CLANG="$deps/llvm/bin/clang" LLVM_CONFIG="$deps/llvm/bin/llvm-config"
export PS5_PAYLOAD_SDK="$deps/ps5-payload-sdk" PS5_VULKAN="$deps/PS5_Vulkan" PS5_MIHAWKGEN="$deps/PS5_MihawkGen"
export PS5_DEPS="$deps" LONGSHOT_VERSION="oberon-$VERSION"
unset PYTHONPATH PYTHONHOME
python="$deps/python/bin/python3"
"$python" -c 'import capstone, unicorn, numpy, zstandard, lz4' 2> /dev/null || die "the bundled Python in deps/python does not run"
"$PS5_CLANG" --version > /dev/null 2>&1 || die "the bundled clang in deps/llvm does not run"
ulimit -s unlimited 2> /dev/null || ulimit -s 1048576 2> /dev/null || note "warning: the stack size could not be raised (ulimit -s)"
mem_gb=$(awk '/MemTotal/ { printf "%d", $2 / 1048576 }' /proc/meminfo)
if [[ -z $jobs ]]; then
    jobs=$(( mem_gb / 6 ))
    (( jobs <= $(nproc) )) || jobs=$(nproc)
    (( jobs >= 1 )) || jobs=1
fi
note "ok: $(nproc) cores, $mem_gb GB of memory, $jobs files at once (--jobs)"
(( mem_gb >= 8 )) || note "warning: under 8 GB of memory; under WSL, raise it in %UserProfile%\\.wslconfig (memory=12GB)"

step "your game file"
check="$root/work/.nup-checked"
mark="$(stat -c '%s %Y' -- "$game") $game"
if [[ -n $skip_hash ]]; then
    note "not checked (--skip-hash-check)"
elif [[ $(cat "$check" 2> /dev/null) == "$mark" ]]; then
    note "checked already"
else
    if ! "$python" -I -X utf8 ps5/tools/check_nup.py "$game" | sed 's/^/    /'; then
        die "$(basename -- "$game") is not a good .nup of Ocarina of Time 1.1.1 (see above)"
    fi
    printf '%s\n' "$mark" > "$check"
fi

exefs_ok() {   # the extracted program is the one this build is for
    [[ -n $skip_hash ]] || (cd "$root/work/game/exefs" 2> /dev/null && sha256sum --quiet --check <<< "$EXEFS_SHA256" > /dev/null 2>&1)
}

step "the game's program: read out of the .nup and translated to C (5 to 15 minutes)"
lift_mark="$mark $(sha256sum nx2/lift.py nx2/oot_patches.py nx2/split_hot.py ps5/profiles/hot_funcs_longshot_v3.txt | sha256sum | cut -c1-16)"
if [[ -f work/lift/sa/gen/a64_table.c && -f work/lift/sa/image/image.bin && $(cat work/lift/.done 2> /dev/null) == "$lift_mark" ]]; then
    note "done already, from this file"
else
    rm -f work/lift/.done
    for attempt in 1 2 3; do
        rm -rf -- "$root/work/game"
        run game-extract "$python" -I -u -X utf8 nx2/nup.py "$game" --only exefs --extract "$root/work/game"
        exefs_ok && break
        note "the extracted program does not match its hash (attempt $attempt): extracting it again"
        (( attempt < 3 )) || die "the game's program never extracted right: not Ocarina of Time 1.1.1, or this PC's memory is unstable"
    done
    mkdir -p "$root/work/lift/image"
    OOT_WORK="$root/work/lift" A64_HOT_FUNCS="$root/ps5/profiles/hot_funcs_longshot_v3.txt" A64_HOT_CHUNK=1 \
        run game-lift "$python" -I -u -X utf8 nx2/lift.py --standalone --write --exefs "$root/work/game/exefs" --work "$root/work/lift"
    run game-split-hot "$python" -I nx2/split_hot.py work/lift/sa/gen
    run game-patches "$python" -s -E nx2/oot_patches.py work/lift/sa/gen
    printf '%s\n' "$lift_mark" > work/lift/.done
fi

step "the game's shader list and button prompts, from its files (5 to 10 minutes)"
romfs="$root/work/romfs"
assets="$root/work/assets"
if [[ -f $assets/longshot_shaders.bin && $(cat "$assets/.done" 2> /dev/null) == "$mark" ]]; then
    note "done already, from this file"
else
    rm -rf -- "$assets" "$romfs"
    mkdir -p "$assets"
    run game-romfs "$python" -I -u -X utf8 nx2/nup.py "$game" --only romfs --extract "$root/work/romfs-extract" \
        --romfs-parts "Shader/,Layout2Shader/,Lib/,Movie/Ndve/,Effect/,Pack/ZsDic.pack.zs,System/Resource/,UI/Layout2Archive/,Font2/"
    found=$(find "$root/work/romfs-extract" -maxdepth 3 -type d -name romfs | head -1)
    [[ -n $found ]] || die "the .nup's romfs was not extracted; the log: $logs/game-romfs.log"
    heads=$(find "$root/work/romfs-extract" -maxdepth 3 -name romfs_zstd_heads.txt | head -1)
    if [[ -n $heads ]] && (( $(wc -l < "$heads") > 1 )); then
        die "game files failed their hashes as they were extracted ($heads lists them): run again; if it repeats, this PC's memory is unstable"
    fi
    mv -- "$found" "$romfs"
    rm -rf -- "$root/work/romfs-extract"
    run shader-list "$python" -I -X utf8 nx2/shader_census.py "$romfs" --out "$assets/longshot_shaders.bin"
    # (without the prompts the game is built all the same and shows the Switch's icons)
    if "$python" -I -X utf8 nx2/prompt_assets.py --romfs "$romfs" --out "$assets/prompts" > "$logs/prompts.log" 2>&1; then
        note "button prompts made"
    else
        rm -rf -- "$assets/prompts"
        note "warning: the button prompts were not made (the log: $logs/prompts.log)"
    fi
    rm -rf -- "$romfs"
    printf '%s\n' "$mark" > "$assets/.done"
fi

step "the title: the game, the runtime and the renderer compiled and linked (30 to 90 minutes the first time)"
if ! PS5_VK_PGO="$root/ps5/profiles/vk.profdata" PS5_VK_LTO=thin "$python" -I ps5/build.py --jobs "$jobs" 2>&1 | tee "$logs/title.log"; then
    die "the title was not built; the whole log: $logs/title.log (if the compiler was killed for want of memory, use --jobs 2)"
fi
title="$root/dist/$TITLE_ID"
[[ -f $title/eboot.bin && -f $title/image.bin ]] || die "no title at $title; the log: $logs/title.log"

step "the title folder"
cp -- "$assets/longshot_shaders.bin" "$title/"
rm -rf -- "$title/prompts" "$title/mods"
[[ -d $assets/prompts ]] && cp -r -- "$assets/prompts" "$title/prompts" || note "no button prompts (see $logs/prompts.log): the game shows the Switch's"
cp -r -- "$root/title/mods" "$title/mods"
[[ -f $title/longshot_options.ini ]] || cp -- "$root/title/longshot_options.ini" "$title/"
[[ -f $title/longshot_env.txt ]] || cp -- "$root/title/longshot_env.txt" "$title/"
if [[ -n $out ]]; then
    mkdir -p -- "$out"
    rm -rf -- "${out:?}/$TITLE_ID"
    cp -r -- "$title" "$out/$TITLE_ID"
    title="$out/$TITLE_ID"
fi

cat << EOF

Built: $title ($(du -sh -- "$title" | cut -f1))

On the console, with its homebrew environment running and an FTP server on:
  1. Copy the whole $TITLE_ID folder to /data/homebrew/$TITLE_ID.
  2. Copy your game file to /data/lichii/roms/oot/oot.nup (make the folders; the name must be oot.nup).
  3. Start Longshot Oberon from the home screen (ShadowMountPlus puts it there) or your homebrew launcher.
The first start translates every shader into the title's cache/ folder (several hours, once); the title closes when
that is done: start it again to play. longshot_env.txt and longshot_options.ini in the folder are the settings.
eboot.bin, image.bin, longshot_shaders.bin and prompts/ are made from your game: keep them to yourself.
EOF
