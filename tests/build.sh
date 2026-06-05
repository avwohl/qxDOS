#!/bin/bash
# Build the standalone emu88 CPU-core test harness (SingleStepTests/80386).
set -e
cd "$(dirname "$0")/.."

OUT=tests/build
mkdir -p "$OUT"

CXX=${CXX:-clang++}
CXXFLAGS="-std=c++20 -O2 -g -Wall -Wno-unused-parameter -I emu88 -I tests/vendor -DMOO_USE_ZLIB"

CORE="emu88/emu88.cc emu88/emu88_pmode.cc emu88/emu88_fpu.cc emu88/emu88_mem.cc"

$CXX $CXXFLAGS tests/sst386.cc $CORE -lz -o "$OUT/sst386"
echo "built $OUT/sst386"
