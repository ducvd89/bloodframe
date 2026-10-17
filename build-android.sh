#!/usr/bin/env bash
# Builds Bloodframe for Android arm64 with the NDK: libbloodframe.so (the single shared library with the
# C API of android/bb_lsfg.h) and the licence files of what is linked into it.
#
#   ./build-android.sh [--ndk PATH] [--api N] [--out DIR] [--build-dir DIR] [--jobs N]
#
#   --ndk        the NDK (default: $ANDROID_NDK_HOME, $ANDROID_NDK_ROOT, or the newest under
#                $ANDROID_HOME/ndk or $ANDROID_SDK_ROOT/ndk)
#   --api        Android API level, minSdk of the library (default 30)
#   --out        where the results go (default ./out): DIR/lib/libbloodframe.so, DIR/licenses/
#   --build-dir  the CMake build tree (default DIR/bloodframe-build)
#   --jobs       parallel jobs (default: the number of CPUs)
#
# Needs git (submodules: dxbc, pe-parse, volk), cmake >= 3.18 and ninja. Run it under `nice` if the
# machine should stay responsive. The shaders are not part of the build: they come from the user's own
# Lossless.dll at run time.
set -euo pipefail
cd -- "$(dirname -- "$0")"
src=$PWD
ndk=
api=30
out=$src/out
build=
jobs=$(nproc)
while (($#)); do
    case $1 in
        --ndk) ndk=$2; shift 2 ;;
        --api) api=$2; shift 2 ;;
        --out) out=$(realpath -m -- "$2"); shift 2 ;;
        --build-dir) build=$(realpath -m -- "$2"); shift 2 ;;
        --jobs|-j) jobs=$2; shift 2 ;;
        -h|--help) sed -n '2,16p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "Unknown argument: $1 (see --help)" >&2; exit 2 ;;
    esac
done
build=${build:-$out/bloodframe-build}

if [[ -z $ndk ]]; then
    ndk=${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-}}
    if [[ -z $ndk ]]; then
        for sdk in "${ANDROID_HOME:-}" "${ANDROID_SDK_ROOT:-}"; do
            [[ -n $sdk && -d $sdk/ndk ]] && ndk=$(ls -d "$sdk"/ndk/*/ 2>/dev/null | sort -V | tail -1) && ndk=${ndk%/} && break
        done
    fi
fi
[[ -n $ndk && -f $ndk/build/cmake/android.toolchain.cmake ]] ||
    { echo "No NDK found (${ndk:-none}): pass --ndk PATH or set ANDROID_NDK_HOME" >&2; exit 1; }
llvm=$ndk/toolchains/llvm/prebuilt/linux-x86_64
[[ -d $llvm ]] || { echo "The NDK has no linux-x86_64 toolchain: $llvm" >&2; exit 1; }

# The fork's submodules; toml11 (the layer's config parser) is not used.
git submodule update -q --init -- thirdparty/dxbc thirdparty/pe-parse thirdparty/volk

mkdir -p "$out/lib" "$out/licenses"
if [[ ! -f $build/build.ninja ]]; then
    rm -rf "$build"
    cmake -S "$src" -B "$build" -G Ninja -DBLOODFRAME=ON \
        -DCMAKE_TOOLCHAIN_FILE="$ndk/build/cmake/android.toolchain.cmake" -DANDROID_ABI=arm64-v8a \
        -DANDROID_PLATFORM=android-$api -DANDROID_STL=c++_shared -DCMAKE_BUILD_TYPE=Release \
        > "$build.log" 2>&1 || { tail -30 "$build.log" >&2; rm -rf "$build"; exit 1; }
fi
cmake --build "$build" -j "$jobs" --target bloodframe >> "$build.log" 2>&1 ||
    { grep -E "error|FAILED" "$build.log" | tail -40 >&2; exit 1; }
built=$(find "$build" -name libbloodframe.so -print -quit)
[[ -n $built ]] || { echo "libbloodframe.so was not built, see $build.log" >&2; exit 1; }
"$llvm/bin/llvm-strip" --strip-debug -o "$out/lib/libbloodframe.so" "$built"

# The licences of what is linked in (the fork's toml11 is not).
cp LICENSE.md "$out/licenses/bloodframe-lsfg-vk.LICENSE.md"
cp thirdparty/dxbc/LICENSE.md "$out/licenses/bloodframe-dxbc.LICENSE"
cp thirdparty/pe-parse/LICENSE "$out/licenses/bloodframe-pe-parse.LICENSE"
cp thirdparty/volk/LICENSE.md "$out/licenses/bloodframe-volk.LICENSE.md"
rev=$(git rev-parse --short=12 HEAD 2>/dev/null || echo unknown)
printf 'Bloodframe %s: lsfg-vk 1.0 by PancakeTAS (MIT) via GameNative'"'"'s lsfg-vk-android by Ragnarok93 (MIT)\n' "$rev" \
    > "$out/licenses/bloodframe-source.txt"

exported=$("$llvm/bin/llvm-nm" -D --defined-only "$out/lib/libbloodframe.so" | awk '{print $3}' | grep -v '^_' | sort | tr '\n' ' ')
echo "Bloodframe $rev: $out/lib/libbloodframe.so (exports: $exported)"
