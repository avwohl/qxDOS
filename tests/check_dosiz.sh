#!/bin/bash
# Build the downstream consumer of emu88 and run its fixtures against THIS tree.
#
# WHY THIS EXISTS
#
# dosiz (github.com/avwohl/dosiz) compiles six emu88 files straight out of this
# working tree.  Not a submodule, not a vendored copy, not a pinned SHA in this
# repo - a relative path, `${CMAKE_SOURCE_DIR}/../../qxDOS/emu88`, in
# dosiz/src/CMakeLists.txt.  dosiz/CLAUDE.md states which way the obligation
# runs: "emu88 belongs to qxDOS.  Do not fix emu88 bugs from this repo", and
# points at qxDOS/tests/ as the gate.  So a commit here lands in dosiz's next
# `make` with nothing in between.
#
# The coverage runs the wrong way round, which is the real argument for this
# script.  This repo owns the suites and they are real-mode per-instruction plus
# one full-system ROM.  dosiz owns roughly thirty PROTECTED-MODE DPMI fixtures,
# committed as .COM/.EXE binaries and driven from its ci.yml - so the only
# automated check on emu88's protected-mode behaviour lives in the repo that is
# forbidden to fix emu88.  This runs those fixtures from here, where the change
# is being made.
#
# 7352fc5's commit message asserted dosiz "builds clean and behaves identically
# before and after".  That was checked by hand and nothing in either repo could
# reproduce it.  This is that check, written down.
#
# WHAT IT IS NOT: it is not in CI here, because it needs a dosiz checkout this
# repo does not carry and must not depend on.  It is the thing to run by hand
# before committing a change to emu88/, alongside tests/run_suites.sh.
#
# Usage:
#     bash tests/check_dosiz.sh                    # expects ../dosiz
#     DOSIZ_DIR=/path/to/dosiz bash tests/check_dosiz.sh
#
# Exit 0 if dosiz configures, builds with no new warning out of emu88, and every
# fixture its ci.yml asserts still prints its tag.  Exit 2 if dosiz is not here
# at all, which is not a failure - it is "not checked".

set -u
cd "$(dirname "$0")/.."
QXDOS=$PWD

DOSIZ_DIR=${DOSIZ_DIR:-$QXDOS/../dosiz}
BUILD=${DOSIZ_BUILD:-$(mktemp -d)}

if [ ! -f "$DOSIZ_DIR/src/CMakeLists.txt" ]; then
  echo "no dosiz checkout at $DOSIZ_DIR - skipping the downstream gate."
  echo "clone github.com/avwohl/dosiz beside this repo, or set DOSIZ_DIR."
  exit 2
fi

echo "== dosiz at $DOSIZ_DIR, emu88 from $QXDOS/emu88"
( cd "$DOSIZ_DIR" && git log --oneline -1 ) || true

log=$(mktemp)
if ! cmake -S "$DOSIZ_DIR/src" -B "$BUILD" -DEMU88_DIR="$QXDOS/emu88" >"$log" 2>&1; then
  echo "FAIL dosiz did not configure:"; tail -20 "$log"; exit 1
fi
if ! cmake --build "$BUILD" -j"$(nproc 2>/dev/null || echo 4)" >>"$log" 2>&1; then
  echo "FAIL dosiz did not build:"; grep -E 'error:' "$log" | head -20; exit 1
fi

# dosiz builds with -Wall -Wextra.  Warnings out of dosiz's own sources are
# dosiz's business; warnings out of emu88/ are ours, and are the point of
# compiling it here at all - this tree's own tests/build.sh is the only other
# thing that passes -Wextra over emu88.
emu_warn=$(grep 'warning:' "$log" | grep -c "$QXDOS/emu88" || true)
if [ "$emu_warn" -ne 0 ]; then
  echo "FAIL $emu_warn warning(s) from emu88/ in dosiz's build:"
  grep 'warning:' "$log" | grep "$QXDOS/emu88" | head -20
  exit 1
fi
echo "   ok  configured, built, 0 warnings from emu88/"

# The fixture list is read out of dosiz's own ci.yml rather than duplicated here,
# so it cannot drift: every `build/dosiz tests/X` paired with the `grep -q 'tag'`
# that follows it.  If they restructure that workflow this stops finding pairs,
# which is why the count is asserted below.
fixtures=$(python3 - "$DOSIZ_DIR/.github/workflows/ci.yml" <<'PY'
import re, sys
cur = None
for line in open(sys.argv[1]):
    m = re.search(r'build/dosiz (tests/[A-Za-z0-9_.]+)', line)
    if m:
        cur = m.group(1)
    m2 = re.search(r"grep -q '([^']+)'", line)
    if m2 and cur:
        print(m2.group(1) + '\t' + cur)
        cur = None
PY
)
count=$(printf '%s\n' "$fixtures" | grep -c . || true)
if [ "$count" -lt 20 ]; then
  echo "FAIL only $count fixtures found in dosiz's ci.yml - the parse has drifted"
  exit 1
fi

pass=0; fail=0
while IFS=$'\t' read -r tag prog; do
  [ -z "${prog:-}" ] && continue
  if [ "$prog" = "tests/ECHOIN.COM" ]; then
    out=$(cd "$DOSIZ_DIR" && printf 'hi-stdin\n' | "$BUILD/dosiz" "$prog" 2>&1)
  else
    out=$(cd "$DOSIZ_DIR" && "$BUILD/dosiz" "$prog" 2>&1)
  fi
  if printf '%s' "$out" | grep -q -- "$tag"; then
    pass=$((pass + 1))
  else
    fail=$((fail + 1))
    echo "   FAIL $prog did not print '$tag'"
    printf '%s\n' "$out" | tail -5 | sed 's/^/        /'
  fi
done <<< "$fixtures"

echo "   ok  $pass/$((pass + fail)) dosiz fixtures passed"
printf '\n'
if [ "$fail" -eq 0 ]; then
  echo "dosiz builds clean against this emu88 and behaves as its CI expects"
  exit 0
fi
echo "$fail dosiz fixture(s) failed - this emu88 change moves downstream behaviour"
exit 1
