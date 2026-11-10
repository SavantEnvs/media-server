#!/usr/bin/env bash
#
# mayhem/build.sh — builds the fuzz harnesses (sanitized + SanCov-instrumented libs), their standalone
# reproducers, and the known-answer probe that mayhem/test.sh runs (normal flags, no sanitizers).
#
# The upstream Makefiles hard-code gcc (gcc.mk), so the five self-contained libraries (libflv, libmov,
# libmkv, libmpeg, librtp) are compiled directly: every source file in each library's own source dir(s), the
# same set and the same defines as the upstream Makefiles.
set -euo pipefail

[ -n "${SOURCE_DATE_EPOCH:-}" ] || unset SOURCE_DATE_EPOCH

: "${SANITIZER_FLAGS=-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer}"
: "${DEBUG_FLAGS:=-g -gdwarf-3}"
: "${CC:=clang}" ; : "${CXX:=clang++}" ; : "${LIB_FUZZING_ENGINE:=-fsanitize=fuzzer}"
: "${MAYHEM_JOBS:=$(nproc)}"
: "${COVERAGE_FLAGS=}"
: "${SRC:=/mayhem}"
: "${STANDALONE_FUZZ_MAIN:=}"
export SANITIZER_FLAGS DEBUG_FLAGS CC CXX LIB_FUZZING_ENGINE MAYHEM_JOBS COVERAGE_FLAGS

cd "$SRC"
OUT="$SRC"
B="$SRC/mayhem/build"
H="$SRC/mayhem/harnesses"

LIBS="libflv libmov libmkv libmpeg librtp"
SRC_DIRS="libflv/source libmov/source libmkv/src libmpeg/source librtp/source librtp/payload librtp/rtpext"
INC=""
for l in $LIBS; do INC="$INC -I$SRC/$l/include"; done
# Every build uses upstream's RELEASE=1 configuration (NDEBUG): the asserts in the parsers guard untrusted-input
# conditions and fire within seconds on any mutated input, which would end each fuzzing session at the first one;
# the memory errors behind them are what the sanitizers are here to find. NDEBUG also keeps libmov's box-tree printf
# out of the fuzz loop and the known-answer output (libmov only compiles with DEBUG or NDEBUG set).
BASEDEFS="-DOS_LINUX -DMPEG_H26X_VERIFY -DMPEG_ZERO_PAYLOAD_LENGTH -DMPEG_DAHUA_AAC_FROM_G711 -DMPEG_KEDA_H265_FROM_H264"

rm -rf "$B"
mkdir -p "$B/fuzz" "$B/plain" "$B/normal"

# Every library source, compiled three ways:
#   fuzz   - sanitizers + SanCov edges (-fsanitize=fuzzer-no-link, unconditional or the libs report edges=0)
#   plain  - sanitizers only (for the standalone reproducers, which have no libFuzzer runtime)
#   normal - the project's normal flags, no sanitizers (the known-answer probe)
FILES=$(for d in $SRC_DIRS; do ls "$SRC"/$d/*.c; done)
compile_all() {
  local mode="$1" flags="$2" defs="$3" dir="$B/$1"
  printf '%s\n' $FILES | xargs -P "$MAYHEM_JOBS" -I{} sh -c '
    f="$1"; mode="$2"; dir="$3"; shift 3
    o="$dir/$(echo "${f#'"$SRC"'/}" | tr / _).o"
    extra=""
    case "$f" in */mov-reader.c|*/mkv-reader.c) extra="-fno-sanitize=null,alignment,object-size" ;; esac   # upstream container_of via a NULL-based offsetof
    '"$CC"' "$@" $extra -c "$f" -o "$o"' _ {} "$mode" "$dir" $flags $defs $INC -w
}
FUZZDEFS="$BASEDEFS -DNDEBUG"
# Benign C idioms that flood on every input are relaxed (ASan and the rest of UBSan stay halting): shifting a byte into
# the sign bit (`data[0] << 24`), memcpy(NULL, ..., 0), NULL + 0 on an empty payload, unaligned loads/stores of packed bytes.
RELAX="-fno-sanitize=shift-base,nonnull-attribute,pointer-overflow,alignment"
compile_all fuzz   "$SANITIZER_FLAGS -fsanitize=fuzzer-no-link $DEBUG_FLAGS -O1 $RELAX" "$FUZZDEFS"
compile_all plain  "$SANITIZER_FLAGS $DEBUG_FLAGS -O1 $RELAX" "$FUZZDEFS"
compile_all normal "-O2 $COVERAGE_FLAGS" "$BASEDEFS -DNDEBUG"
for m in fuzz plain normal; do
  rm -f "$B/lib$m.a"
  ar rcs "$B/lib$m.a" "$B/$m"/*.o
done

# LeakSanitizer off at build time (docs/rules/c.md); links into every fuzz + standalone binary.
$CXX $SANITIZER_FLAGS $DEBUG_FLAGS -c "$SRC/mayhem/lsan_off.cc" -o "$B/lsan_off.o"

if [ -n "$STANDALONE_FUZZ_MAIN" ]; then
  $CC $SANITIZER_FLAGS $DEBUG_FLAGS -c "$STANDALONE_FUZZ_MAIN" -o "$B/standalone_main.o"
fi

TARGETS="fuzz_flv_parser fuzz_flv_demuxer fuzz_mov_reader fuzz_mkv_reader fuzz_ts_demuxer fuzz_ps_demuxer fuzz_rtp_payload fuzz_rtp_demuxer fuzz_codec_config"
for t in $TARGETS; do
  $CC $SANITIZER_FLAGS -fsanitize=fuzzer-no-link $DEBUG_FLAGS -O1 $RELAX $FUZZDEFS $INC -I"$H" $LIB_FUZZING_ENGINE \
      "$H/$t.c" "$B/lsan_off.o" "$B/libfuzz.a" -lm -o "$OUT/$t"
  if [ -n "$STANDALONE_FUZZ_MAIN" ]; then
    $CC $SANITIZER_FLAGS $DEBUG_FLAGS -O1 $RELAX $FUZZDEFS $INC -I"$H" \
        "$H/$t.c" "$B/standalone_main.o" "$B/lsan_off.o" "$B/libplain.a" -lm -o "$OUT/$t-standalone"
  fi
done

# Known-answer probe for mayhem/test.sh: normal flags, dynamically linked against libc.
$CC -O2 $COVERAGE_FLAGS $BASEDEFS -DNDEBUG $INC "$SRC/mayhem/kat/kat.c" "$B/libnormal.a" -lm -o "$OUT/media_server_kat"
