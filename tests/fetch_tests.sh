#!/bin/bash
# Fetch the CPU test suites used by the emu88 test harnesses.
# Data lands in tests/data/ (gitignored — large).
set -e
# tests/data is gitignored and no tracked file creates it, so on a fresh
# checkout this directory does not exist and `cd` would fail under `set -e`
# before either clone ran.  .github/workflows/tests.yml had been working around
# that with its own `mkdir -p tests/data` immediately before calling this
# script; the workaround belongs here.
mkdir -p "$(dirname "$0")/data"
cd "$(dirname "$0")/data"

# Both upstreams are shallow clones whose objects ARE the payload: the suites
# read files and never git, so the .git the clone leaves behind is pure
# duplication -- 567MB of it for SingleStepTests alone, against a 579MB working
# tree.  Dropping it takes tests/data from 1.2G to 585M with nothing the suites
# read removed.  .github/workflows/tests.yml did this by hand after calling this
# script; doing it here means a local checkout matches CI.

# SingleStepTests 80386 — exhaustive per-instruction real-mode tests (579MB).
if [ ! -d 80386 ]; then
  git clone --depth 1 https://github.com/SingleStepTests/80386 80386
  rm -rf 80386/.git
fi

# test386.asm — PCjs/barotto full-system 386 diagnostic (real/PM/paging/V86).
if [ ! -d test386 ]; then
  git clone --depth 1 https://github.com/barotto/test386.asm test386
fi
# Build the ROM with a custom ASCII output port (0xE9) so results can be captured.
( cd test386
  sed -i.bak 's/^OUT_PORT equ 0$/OUT_PORT equ 0xE9/' src/configuration.asm || true
  make )
# Dropped after the ROM is built, not before: `make` needs the working tree, not
# the history.
rm -rf test386/.git

echo "test data ready in tests/data/ ($(du -sh . | cut -f1))"
