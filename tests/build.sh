#!/bin/bash
# Build the standalone emu88 CPU-core test harness (SingleStepTests/80386).
set -e
cd "$(dirname "$0")/.."

OUT=tests/build
mkdir -p "$OUT"

# clang++ where it exists, g++ otherwise.  Hardcoding clang++ meant this script
# simply did not run on a machine without it, which is how the sibling cpmemu
# had its release build broken on two architectures for two days - a Clang-only
# flag nobody could have hit locally.
if [ -z "${CXX:-}" ]; then
  if command -v clang++ >/dev/null 2>&1; then CXX=clang++; else CXX=g++; fi
fi
CXXFLAGS="-std=c++20 -O2 -g -Wall -I emu88 -I tests/vendor -DMOO_USE_ZLIB"

CORE="emu88/emu88.cc emu88/emu88_pmode.cc emu88/emu88_fpu.cc emu88/emu88_mem.cc"
DOS="emu88/dos_machine.cc emu88/dos_bios.cc emu88/dos_dpmi.cc emu88/ne2000.cc \
     emu88/opl.cc emu88/sound_blaster.cc emu88/uart16550.cc"

$CXX $CXXFLAGS tests/sst386.cc $CORE -lz -o "$OUT/sst386"
echo "built $OUT/sst386"

# VESA/SVGA end-to-end test: drives the full emu88 + DOS machine through the
# real INT 10h dispatch (4F00-4F09), checking VbeInfoBlock/ModeInfoBlock layout,
# the LFB + bank-switched VRAM routing, and the compositor pan clamp.
$CXX $CXXFLAGS tests/vesa_test.cc $CORE $DOS -o "$OUT/vesa_test"
echo "built $OUT/vesa_test"

# Optional-hardware test: joystick, PC speaker (audio_render), LPT, Hercules,
# and the integrated AdLib/OPL, Sound Blaster, and 16550 UART.
$CXX $CXXFLAGS tests/hardware_test.cc $CORE $DOS -o "$OUT/hardware_test"
echo "built $OUT/hardware_test"

# Per-module unit tests for the sound/serial devices.
$CXX $CXXFLAGS tests/opl_unit.cc  emu88/opl.cc            -o "$OUT/opl_unit"  && echo "built $OUT/opl_unit"
$CXX $CXXFLAGS tests/sb_unit.cc   emu88/sound_blaster.cc  -o "$OUT/sb_unit"   && echo "built $OUT/sb_unit"
$CXX $CXXFLAGS tests/uart_unit.cc emu88/uart16550.cc      -o "$OUT/uart_unit" && echo "built $OUT/uart_unit"

# test386.asm's full-system harness - real mode -> protected -> paging -> V86.
# It used to be a command in tests/README.md and nothing built it, which is why
# it was also the suite nobody ran: the automated half of the validation was the
# real-mode half, and the IDIV bug fixed in 7352fc5 lived outside it.
$CXX $CXXFLAGS tests/test386_run.cc $CORE -o "$OUT/test386"
echo "built $OUT/test386"
