#!/bin/bash
# Fetch the CPU test suites used by the emu88 test harnesses.
# Data lands in tests/data/ (gitignored — large).
set -e
cd "$(dirname "$0")/data"

# SingleStepTests 80386 — exhaustive per-instruction real-mode tests (~576MB).
if [ ! -d 80386 ]; then
  git clone --depth 1 https://github.com/SingleStepTests/80386 80386
fi

# test386.asm — PCjs/barotto full-system 386 diagnostic (real/PM/paging/V86).
if [ ! -d test386 ]; then
  git clone --depth 1 https://github.com/barotto/test386.asm test386
fi
# Build the ROM with a custom ASCII output port (0xE9) so results can be captured.
( cd test386
  sed -i.bak 's/^OUT_PORT equ 0$/OUT_PORT equ 0xE9/' src/configuration.asm || true
  make )

echo "test data ready in tests/data/"
