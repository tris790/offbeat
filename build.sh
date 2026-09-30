#!/usr/bin/env bash
#
# Offbeat build.
#
#   ./build.sh            debug build (tcc, -g)            -> build/offbeat
#   ./build.sh release    optimized build (gcc -O2)        -> build/offbeat
#   ./build.sh asan       debug + address/UB sanitizers (gcc)
#   ./build.sh test       unit tests (tcc)
#   ./build.sh testasan   unit tests under address/UB sanitizers (gcc)
#
# App code (everything under src/ except third_party/) is compiled in a single
# cc invocation, so new .c files are picked up automatically. Heavy third-party
# single-header libraries (decoders, image, font rasterizer) are compiled ONCE
# with gcc -O2 into cached objects -- they are both slow to compile and hot at
# runtime, so the debug build stays <1s while decoding runs at full speed.
# Every src/third_party/*.c is such a unit; drop in a new one and it is built.
#
set -euo pipefail

cd "$(dirname "$0")"

ROOT="$PWD"
SRC="$ROOT/src"
BUILD="$ROOT/build"
GEN="$BUILD/gen"
OBJ="$BUILD/obj"
OUT="$BUILD/offbeat"

MODE="${1:-debug}"

mkdir -p "$BUILD" "$GEN" "$OBJ"

start=$(date +%s%N)

# ---- cached optimized objects -----------------------------------------------
# Objects are rebuilt when their source (or any third_party header) is newer.
# gcc's .note.GNU-stack/.note.gnu.property sections trip tcc's linker, so they
# are stripped; the objects then link with either tcc or gcc.
OPT_CC="${OPT_CC:-gcc}"
OPT_FLAGS="-O2 -g -fPIC -fno-stack-protector -fcf-protection=none -w"
newest_tp_header="$(ls -t "$SRC"/third_party/*.h | head -1)"
CACHED_OBJS=()
PIDS=()

build_obj() { # src obj extra_flags...
    local src="$1" obj="$2"; shift 2
    if [ ! -f "$obj" ] || [ "$src" -nt "$obj" ] || [ "$newest_tp_header" -nt "$obj" ] \
       || [ "$ROOT/build.sh" -nt "$obj" ]; then
        (
            $OPT_CC $OPT_FLAGS "$@" -c "$src" -o "$obj.tmp" &&
            objcopy --remove-section .note.GNU-stack --remove-section .note.gnu.property \
                    "$obj.tmp" "$obj" && rm -f "$obj.tmp"
        ) &
        PIDS+=($!)
    fi
    CACHED_OBJS+=("$obj")
}

# Wayland protocol glue: generated from each vendored xml, then compiled.
for xml in "$SRC"/third_party/*.xml; do
    base="$(basename "$xml" .xml)"
    hdr="$GEN/$base-client-protocol.h"
    csrc="$GEN/$base-protocol.c"
    if [ "$xml" -nt "$hdr" ] || [ ! -f "$hdr" ]; then
        wayland-scanner client-header "$xml" "$hdr"
        wayland-scanner private-code  "$xml" "$csrc"
    fi
    build_obj "$csrc" "$OBJ/proto_$base.o"
done

for tp in "$SRC"/third_party/*.c; do
    [ -e "$tp" ] || continue
    build_obj "$tp" "$OBJ/tp_$(basename "$tp" .c).o" -I"$SRC"
done

for pid in "${PIDS[@]}"; do wait "$pid"; done

# ---- app compiler -----------------------------------------------------------
WARN="-Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-unused-function"
STD="-std=c2x -D_GNU_SOURCE"

case "$MODE" in
    debug)   CC="${CC:-tcc}"; OPT="-g" ;;
    release) CC="${CC:-gcc}"; OPT="-O2 -DNDEBUG" ;;
    asan)    CC="${CC:-gcc}"; OPT="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer" ;;
    test)    CC="${CC:-tcc}"; OPT="-g" ;;
    testasan) CC="${CC:-gcc}"; OPT="-O1 -g -w -fsanitize=address,undefined -fno-omit-frame-pointer" ;;
    *) echo "unknown mode '$MODE' (use: debug | release | asan | test | testasan)" >&2; exit 2 ;;
esac

if [[ "$MODE" == test || "$MODE" == testasan ]]; then
    # Each tests/*.c is a standalone program that may #include app sources.
    for t in "$ROOT"/tests/*.c; do
        exe="$BUILD/test_$(basename "$t" .c)"
        # shellcheck disable=SC2086
        "$CC" $STD $OPT -I"$SRC" "$t" "$OBJ"/tp_*.o -o "$exe" -lm -lpthread $(pkg-config --libs alsa)
        "$exe"
    done
    exit 0
fi

DEPS="wayland-client wayland-cursor wayland-egl egl gl xkbcommon alsa"
PKG_CFLAGS="$(pkg-config --cflags $DEPS)"
PKG_LIBS="$(pkg-config --libs $DEPS)"

mapfile -t SOURCES < <(find "$SRC" -name '*.c' -not -path '*/third_party/*' | sort)

# shellcheck disable=SC2086
"$CC" $STD $WARN $OPT \
    -I"$GEN" -I"$SRC" \
    $PKG_CFLAGS \
    "${SOURCES[@]}" "${CACHED_OBJS[@]}" \
    -o "$OUT" \
    $PKG_LIBS -lm -lpthread

end=$(date +%s%N)
ms=$(( (end - start) / 1000000 ))
printf 'built %s (%s) in %d.%03ds\n' "$OUT" "$MODE" "$((ms / 1000))" "$((ms % 1000))"
