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
# -Wextra, not just -Wall.  ad01cd0 cleared emu88's warnings and 2026-08-27
# cleared the last ten, but that was a MEASUREMENT and nothing held it: the
# command that proved it clean was not the command this script ran, so the
# warnings could come back without failing anything.  The harness side cost six
# warnings to add it - three -Wsign-compare out of the vendored mooreader.h
# (suppressed at the include site so the vendored copy stays byte-identical to
# upstream), a -Wcomment each from sb_unit.cc and uart_unit.cc where a trailing
# backslash spliced the next line into a // comment, and a half-written liveness
# counter in test386_run.cc.
CXXFLAGS="-std=c++20 -O2 -g -Wall -Wextra -I emu88 -I tests/vendor -DMOO_USE_ZLIB"

# ASAN=1 rebuilds everything under AddressSanitizer + UBSan into tests/build-asan.
# tests/README.md used to say the VESA pan clamp was "checked under
# AddressSanitizer"; `fsanitize` appeared nowhere in the repo, so whatever was
# done was a one-off that nothing committed could reproduce and no regression
# would have caught.  This makes it a command:
#     ASAN=1 bash tests/build.sh && tests/build-asan/vesa_test
# It is not in CI: the sanitized sst386 run costs minutes, not seconds.  It is
# the thing to run by hand when touching memory routing or a frame buffer.
if [ "${ASAN:-0}" != "0" ]; then
  OUT=tests/build-asan
  mkdir -p "$OUT"
  CXXFLAGS="-std=c++20 -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined \
            -Wall -Wextra -I emu88 -I tests/vendor -DMOO_USE_ZLIB"
fi

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

# The NE2000.  emu88/ne2000.cc was 425 lines that every harness linking $DOS
# compiled and none of them executed - `grep -lni ne2000 tests/*.cc` returned
# nothing at all.  Same shape as the three unit tests above: one .cc, no corpus,
# the class driven through its I/O ports the way a packet driver sees it.
$CXX $CXXFLAGS tests/ne2000_test.cc emu88/ne2000.cc       -o "$OUT/ne2000_test" && echo "built $OUT/ne2000_test"

# The x87 FPU. emu88_fpu.cc had no coverage of any kind until 2026-08-27 - it
# is CPU core, so every harness above already compiled it and none executed a
# single x87 opcode. This one runs real opcode bytes through the decoder.
$CXX $CXXFLAGS tests/fpu_test.cc $CORE -o "$OUT/fpu_test"
echo "built $OUT/fpu_test"

# The DPMI host. dos_dpmi.cc is linked into every harness that
# pulls in $DOS, and was executed by none of them. This drives a client in
# through the real INT 2Fh/1687h detection and the mode switch, then exercises
# the INT 31h services.
$CXX $CXXFLAGS tests/dpmi_test.cc $CORE $DOS -o "$OUT/dpmi_test"
echo "built $OUT/dpmi_test"

# The PC BIOS.  dos_bios.cc is 2200 lines that every harness linking $DOS
# compiled and almost none of it ran - only whatever the INT 10h VESA path in
# vesa_test.cc and the hand-assembled INT 21h handler in dpmi_test.cc happened
# to touch.  This assembles `CD <vec> / F4` into guest memory and lets the
# decoder dispatch it, the way vesa_test and dpmi_test do, so the real INT
# entry path is exercised rather than the handlers being called behind it.
$CXX $CXXFLAGS tests/bios_test.cc $CORE $DOS -o "$OUT/bios_test"
echo "built $OUT/bios_test"

# test386.asm's full-system harness - real mode -> protected -> paging -> V86.
# It used to be a command in tests/README.md and nothing built it, which is why
# it was also the suite nobody ran: the automated half of the validation was the
# real-mode half, and the IDIV bug fixed in 7352fc5 lived outside it.
$CXX $CXXFLAGS tests/test386_run.cc $CORE -o "$OUT/test386"
echo "built $OUT/test386"
