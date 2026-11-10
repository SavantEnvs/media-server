#!/usr/bin/env bash
#
# mayhem/test.sh — known-answer oracle for the media-server libraries.
#
# media_server_kat (built by mayhem/build.sh with the normal flags) muxes a synthetic H.264 + AAC stream through the
# real libmov/libmkv/libflv/libmpeg/librtp writers, reads it back with the real readers/demuxers and prints counts,
# timestamps, flags and a CRC-32 of every payload. Each group of lines (aac, avc, mov, fmp4, mkv, flv, ts, ps, rtp)
# must equal mayhem/kat/expected.txt exactly; one group is one CTRF test.
#
# IMPORTANT:
#  * Must assert BEHAVIOR/OUTPUT, not just exit status. The oracle has to check asserted values /
#    golden-output diffs / known-answer results — so a PATCH that "fixes" a bug by making the program
#    exit(0) (or any no-op) FAILS here. Running inputs and checking only "exit 0 / didn't crash" is
#    NOT a functional test (it's trivially reward-hackable) — use the project's real assertion suite.
#  * Do NOT build here — mayhem/build.sh already compiled the test suite (with the project's normal
#    flags). This script only RUNS the pre-built tests and reports counts. If the test runner is
#    missing, that's a build.sh bug — fail loudly rather than silently rebuilding.
#  * REQUIRED OUTPUT — a CTRF (https://ctrf.io) summary so Mayhem/the PATCH grader reads the counts:
#      - writes a CTRF JSON report to ${CTRF_REPORT:-$SRC/ctrf-report.json}, and
#      - prints a one-line `CTRF {...}` marker to stdout (same JSON, compact).
#    Only `results.summary` (with tests/passed/failed/pending/skipped/other) is required.
#    Use the emit_ctrf helper below; it computes tests = passed+failed+skipped and sets the exit
#    code (0 iff failed==0). Map your framework's output to passed/failed/skipped.
set -uo pipefail
[ -n "${SOURCE_DATE_EPOCH:-}" ] || unset SOURCE_DATE_EPOCH
: "${MAYHEM_JOBS:=$(nproc)}"   # build parallelism; env-overridable, falls back to nproc (use -j"$MAYHEM_JOBS")
cd "$SRC"

# emit_ctrf <tool> <passed> <failed> [skipped] [pending] [other]
# Writes a CTRF report (file + stdout `CTRF {...}` marker) and returns non-zero iff failed>0.
emit_ctrf() {
  local tool="$1" passed="$2" failed="$3" skipped="${4:-0}" pending="${5:-0}" other="${6:-0}"
  local tests=$(( passed + failed + skipped + pending + other ))
  cat > "${CTRF_REPORT:-$SRC/ctrf-report.json}" <<JSON
{
  "results": {
    "tool": { "name": "$tool" },
    "summary": {
      "tests": $tests,
      "passed": $passed,
      "failed": $failed,
      "pending": $pending,
      "skipped": $skipped,
      "other": $other
    }
  }
}
JSON
  printf 'CTRF {"results":{"tool":{"name":"%s"},"summary":{"tests":%d,"passed":%d,"failed":%d,"pending":%d,"skipped":%d,"other":%d}}}\n' \
    "$tool" "$tests" "$passed" "$failed" "$pending" "$skipped" "$other"
  [ "$failed" -eq 0 ]
}


KAT=${KAT_BIN:-$SRC/media_server_kat}
EXPECTED=$SRC/mayhem/kat/expected.txt
if [ ! -x "$KAT" ]; then
  echo "test.sh: $KAT missing - mayhem/build.sh must build it" >&2
  emit_ctrf "media-server-kat" 0 1 0
  exit 1
fi

ACTUAL=$(mktemp)
trap 'rm -f "$ACTUAL"' EXIT
"$KAT" 2>/dev/null | grep -v '^unknown id' > "$ACTUAL" || true

passed=0
failed=0
for group in $(cut -d. -f1 "$EXPECTED" | cut -d: -f1 | awk '!seen[$0]++'); do
  want=$(grep -E "^${group}[.:]" "$EXPECTED")
  got=$(grep -E "^${group}[.:]" "$ACTUAL" || true)
  if [ "$want" = "$got" ]; then
    passed=$((passed + 1))
  else
    failed=$((failed + 1))
    echo "FAIL group '$group'" >&2
    diff <(echo "$want") <(echo "$got") | head -10 >&2 || true
  fi
done

emit_ctrf "media-server-kat" "$passed" "$failed" 0
