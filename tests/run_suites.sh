#!/bin/bash
# Run the emu88 validation suites and hold them to what they scored last time.
#
# This exists because both suites could be green-by-inspection and nobody would
# know: tests/build.sh built neither test386 nor a comparison, and the harness
# reports POST 0xFF and stops.  A run with four wrong IDIV lines therefore read
# as a pass for as long as both suites had existed (fixed in 7352fc5).  The
# check that matters is the last one here - test386's arithmetic output against
# the reference the upstream diagnostic ships.
#
# Usage:  bash tests/run_suites.sh          (needs tests/data; see fetch_tests.sh)
set -u
cd "$(dirname "$0")/.."

BUILD=tests/build
DATA=tests/data
fail=0
note() { printf '\n== %s\n' "$1"; }
ok()   { printf '   ok   %s\n' "$1"; }
bad()  { printf '   FAIL %s\n' "$1"; fail=$((fail + 1)); }

# SingleStepTests scores 297 failures out of 1,758,699 and always has: 287 IMUL
# undefined-flag cases, 6 IN-from-port cases whose expected value is whatever
# device sat on the capture bench, and 4 self-modifying REP cases needing a
# cycle-accurate prefetch queue.  All are officially-undefined or
# environment-specific; tests/README.md section 1 has the detail.  So the gate
# is "no worse than", not "zero failures" - and a BETTER score fails too, loudly,
# because it means this number is stale and should be raised.
SST_BASELINE=1758402

[ -x "$BUILD/sst386" ] || { echo "no $BUILD/sst386 - run tests/build.sh first"; exit 2; }
[ -d "$DATA/80386" ]   || { echo "no $DATA/80386 - run tests/fetch_tests.sh first"; exit 2; }

note "unit harnesses"
for t in opl_unit sb_unit uart_unit vesa_test hardware_test; do
  if "$BUILD/$t" >/dev/null 2>&1; then ok "$t"; else bad "$t (exit $?)"; fi
done

note "x87 FPU and DPMI host"
# These two differ from the harnesses above: each holds a count of KNOWN,
# REAL defects to a baseline, the same way SingleStepTests is held to
# SST_BASELINE.  Their bug() assertions state the architecturally correct
# behaviour and are red on purpose.  FIXING one FAILS the harness, on purpose -
# a silent improvement means the baseline is stale and must be lowered by hand.
# tests/README.md sections 4 and 5 list what is still red and why.
for t in fpu_test dpmi_test; do
  if [ ! -x "$BUILD/$t" ]; then bad "$t not built - run tests/build.sh"; continue; fi
  out=$("$BUILD/$t" 2>&1); rc=$?
  if [ "$rc" -eq 0 ]; then
    ok "$t - $(printf '%s' "$out" | grep -E '^ALL ' | tail -1)"
  else
    bad "$t (exit $rc)"
    printf '%s\n' "$out" | grep -E 'FAIL|FIXED' | head -12 | sed 's/^/        /'
  fi
done

note "SingleStepTests/80386 (per-instruction, real mode)"
sst_out=$("$BUILD/sst386" --summary --revoke "$DATA/80386/revocation_list.txt" \
                          "$DATA/80386/v1_ex_real_mode" 2>&1 | tail -1)
echo "   $sst_out"
passed=$(printf '%s' "$sst_out" | sed -n 's/.*TOTAL \([0-9]*\)\/.*/\1/p')
if [ -z "$passed" ]; then
  bad "could not parse a total out of: $sst_out"
elif [ "$passed" -lt "$SST_BASELINE" ]; then
  bad "$passed passed, below the $SST_BASELINE baseline - a real regression"
elif [ "$passed" -gt "$SST_BASELINE" ]; then
  bad "$passed passed, ABOVE the $SST_BASELINE baseline - good news; raise SST_BASELINE here"
else
  ok "$passed passed, matching the baseline"
fi

note "test386.asm (full-system: real -> protected -> paging -> V86)"
if [ ! -f "$DATA/test386/test386.bin" ]; then
  bad "no $DATA/test386/test386.bin - run tests/fetch_tests.sh"
else
  t386_out=$(mktemp); "$BUILD/test386" "$DATA/test386/test386.bin" >"$t386_out" 2>/dev/null
  if grep -q '^\[POST\] FF' "$t386_out"; then ok "reached POST 0xFF"; else bad "never reached POST 0xFF"; fi
  # The comparison the harness does not do.  POST 0xFF only says the ROM ran to
  # the end; it says nothing about whether the arithmetic was right.
  ref="$DATA/test386/test386-EE-reference.txt"
  if [ ! -f "$ref" ]; then
    bad "no reference at $ref"
  else
    got=$(mktemp)
    grep -vE '^\[POST\]|^====|^----|^final CS:EIP|^$' "$t386_out" > "$got"
    if diff -q "$got" "$ref" >/dev/null; then
      ok "arithmetic output matches the reference exactly ($(wc -l < "$ref") lines)"
    else
      bad "arithmetic output differs from the reference:"
      diff "$got" "$ref" | head -20 | sed 's/^/        /'
    fi
    rm -f "$got"
  fi
  rm -f "$t386_out"
fi

printf '\n'
if [ "$fail" -eq 0 ]; then echo "all suites passed"; else echo "$fail check(s) failed"; fi
exit $((fail > 0))
