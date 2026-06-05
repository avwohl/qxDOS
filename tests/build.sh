#!/bin/bash
# Build the standalone emu88 CPU-core test harness (SingleStepTests/80386).
set -e
cd "$(dirname "$0")/.."

OUT=tests/build
mkdir -p "$OUT"

CXX=${CXX:-clang++}
CXXFLAGS="-std=c++20 -O2 -g -Wall -Wno-unused-parameter -I emu88 -I tests/vendor -DMOO_USE_ZLIB"

CORE="emu88/emu88.cc emu88/emu88_pmode.cc emu88/emu88_fpu.cc emu88/emu88_mem.cc"
DOS="emu88/dos_machine.cc emu88/dos_bios.cc emu88/dos_dpmi.cc emu88/ne2000.cc"

$CXX $CXXFLAGS tests/sst386.cc $CORE -lz -o "$OUT/sst386"
echo "built $OUT/sst386"

# VESA/SVGA end-to-end test: drives the full emu88 + DOS machine through the
# real INT 10h dispatch (4F00-4F09), checking VbeInfoBlock/ModeInfoBlock layout,
# the LFB + bank-switched VRAM routing, and the compositor pan clamp.
$CXX $CXXFLAGS tests/vesa_test.cc $CORE $DOS -o "$OUT/vesa_test"
echo "built $OUT/vesa_test"
