// bios_test.cc — PC BIOS end-to-end harness for emu88/dos_bios.cc.
//
// Build & run:
//   g++ -std=c++20 -O2 -Wall -Wextra -I emu88 tests/bios_test.cc
//       emu88/emu88.cc emu88/emu88_pmode.cc emu88/emu88_fpu.cc
//       emu88/emu88_mem.cc emu88/dos_machine.cc emu88/dos_bios.cc
//       emu88/dos_dpmi.cc emu88/ne2000.cc emu88/opl.cc
//       emu88/sound_blaster.cc emu88/uart16550.cc -o /tmp/bios_test
//   /tmp/bios_test
//
// To wire into the suite, add to tests/build.sh next to the other DOS-layer
// harnesses (it needs $CORE plus the dos_machine objects, exactly the same
// object list dpmi_test and vesa_test use), and add bios_test to the `for t in`
// list in tests/run_suites.sh.  It needs no corpus and runs in well under a
// second.
//
// ---------------------------------------------------------------------------
// HOW IT DRIVES THE BIOS
// ---------------------------------------------------------------------------
// Every BIOS entry point in dos_machine.h is PRIVATE: bios_int10h, bios_int13h,
// xms_dispatch and the rest cannot be called from here at all, which is the
// right shape anyway.  So this harness works the way tests/vesa_test.cc and
// tests/dpmi_test.cc do: it assembles `CD <vec> / F4` into guest memory at
// 0x3000:0000, points CS:IP at it, and lets emu88::execute() decode and
// dispatch the interrupt.  The XMS driver is reached the way a real client
// reaches it — `9A D8 EF 00 F0` (CALL FAR F000:EFD8) into the ROM trap stub,
// which lands in unimplemented_opcode -> xms_dispatch and returns through the
// stub's own RETF.  INT 08h's chain to INT 1Ch is proved by installing a
// hand-assembled real-mode 1Ch handler in the IVT and watching it run.
//
// The instruction budget is spent by execute(), not run_batch(), on purpose:
// run_batch owns the 18.2 Hz timer tick and the idle heuristics, and letting it
// advance 0040:006C underneath the INT 1Ah tests would make them nondeterministic.
// FLAGS is reset to 0x0002 before each stub, so IF stays clear (no interrupt
// delivery) and every "CF set" / "ZF set" assertion below means the handler set
// it, not that it was left over.
//
// The disks are RAM images owned by the harness's dos_io.  dos_machine reaches
// every disk through dos_io::disk_read/disk_write/disk_size/disk_present and
// has no other notion of a medium, so a std::vector<uint8_t> per drive is a
// complete drive as far as INT 13h is concerned.  Three are attached before
// init_machine(), because init_bda() counts hard disks into 0040:0075 once:
//   drive 00h  360 KB floppy  (2 heads, 40 cyls, 9 spt)
//   drive 01h  1.44 MB floppy (2 heads, 80 cyls, 18 spt)
//   drive 80h  2,064,384 byte hard disk (16 heads, 63 spt, 4 cyls)
//
// ---------------------------------------------------------------------------
// THREE KINDS OF ASSERTION
// ---------------------------------------------------------------------------
//   check()   — behaviour that is CORRECT.  A plausible edit to dos_bios.cc
//               flips it.  These must pass.
//   diverge() — pins a value that provably differs from a real PC BIOS.  The
//               assertion states THIS implementation's value and the comment
//               above it names the gap.  It documents the hole instead of
//               hiding it; if the value ever moves the test fails and somebody
//               has to re-read the comment.  Same idea as the SST_BASELINE gate
//               in tests/run_suites.sh.
//   bug()     — asserts the ARCHITECTURALLY CORRECT answer against a defect
//               that is really present in dos_bios.cc today.  It FAILS on
//               purpose, is reported as "KNOWN BUG", and is held to
//               KNOWN_BUGS_EXPECTED: if one gets fixed the count drops, the
//               harness FAILS, and the baseline has to be lowered deliberately.
//               A known bug can therefore never quietly become "the way it
//               works".
//
//               THE LEDGER IS EMPTY: KNOWN_BUGS_EXPECTED is 0.  Six defects
//               came out of writing this file and all six were fixed on
//               2026-08-27, each assertion staying where it was and becoming a
//               check():
//                 - INT 10h AH=00 masked AL bit 7 off and then always cleared
//                   the display buffer, so re-selecting the current mode to
//                   reset the CRTC lost the screen.  Both clear paths - the
//                   64000-byte mode-13h fill and the text cell loop - had to be
//                   fixed, and both are covered.
//                 - INT 13h AH=02/03 never checked that the CHS address exists.
//                   `sector - 1' with sector 0 made the unsigned LBA 2^64-1 and
//                   handed dos_io a byte offset of 2^64-512.  Sector, head and
//                   cylinder are all bounded now, answering 04h; the assertion
//                   that matters is io.reads staying 0, because a short read
//                   produces the same CF and AH.
//                 - INT 13h AH=15h returned CF SET for a drive that is not
//                   present, where AH=00h is the documented SUCCESS answer.
//                 - INT 1Ah AH=01 left the 40:70 midnight flag set, so the very
//                   next AH=00 reported a rollover that had been consumed.
//                 - XMS AH=0Bh validated neither offset nor length against the
//                   block, so a move longer than the destination reported
//                   SUCCESS and wrote past the end of somebody else's
//                   allocation.  A7h/A8h/A9h now, bounded exactly - the
//                   boundary cases use an ODD offset with an even length,
//                   because that is the only shape that separates `> block'
//                   from `> block + 1'.
//               The bug() machinery is left here for the next defect.
//
// Exit code is non-zero if any check()/diverge() fails, or if the number of
// bug() assertions still red is not exactly KNOWN_BUGS_EXPECTED.
//
// ---------------------------------------------------------------------------
// WHAT IS COVERED
// ---------------------------------------------------------------------------
// INT 10h text services (AH=00 mode set and the seven BDA fields it writes,
// 01/02/03 cursor, 05 page, 06/07 scroll up and down with the window edges
// asserted on all four sides, 08 read back, 09/0A write with and without an
// attribute and with a repeat count, 0C/0D mode-13h pixels, 0E TTY including
// CR/LF/BS/BEL and the scroll at the bottom line, 0F, 10h DAC single and block,
// 11h/30, 12h/BL=10, 13h write string, 1Ah for all five display configurations);
// INT 11h; INT 12h; INT 13h AH=00/02/03/04/08/15/41/42/43/48 against the RAM
// disks with a verified read/write round trip and the error paths; INT 14h;
// INT 15h AH=24/41/4F/86/87/88/91/C0; INT 16h AH=00/01/02/03/05/09/10/11/12
// including the ring wrap, the peek that must not consume, and the blocking
// read's IP rewind; INT 17h; INT 19h bootstrap from both drives and the
// no-bootable-medium path; INT 1Ah AH=00-05; INT 2Fh AX=1680/4300/4310; and the
// XMS driver behind 4310 (AH=00,03,04,07,08,09,0A,0B,0C,0D,0E,0F,10,11,88 and
// the unknown-function default).
//
// WHAT IS NOT COVERED, and why — dos_bios.cc is 2178 lines and this does not
// reach all of it:
//   * The VESA/VBE 4Fxx services and INT 33h mouse (dos_bios.cc:1798-2178 plus
//     bios_int33h) are deliberately skipped: tests/vesa_test.cc already drives
//     both end-to-end and owns them.
//   * INT E0h host file services (bios_int_e0h, 80 lines) need a real host
//     filesystem behind dos_io::host_file_*.  Faking it would test the fake.
//   * CD-ROM drives (>= 0xE0): get_geometry's 2048-byte-sector branch, INT 13h
//     AH=4B El Torito, and the AH=42/43 non-512 sector path are unreached.  A
//     CD image is attachable the same way the disks here are; it was left out
//     for size, not because it is impossible.
//   * bios_int08h's protected-mode branch (it skips INT 1Ch when
//     protected_mode() is true) needs a live DPMI session; tests/dpmi_test.cc
//     owns that machinery.
//   * INT 10h AH=0Bh is an empty stub; AH=04/0Ah-page/1Bh/1Ch and every AH the
//     switch does not name fall to `default: break` and are unobservable from
//     the guest, so there is nothing to assert about them beyond "no registers
//     changed", which is asserted once for AH=12h/BL!=10h and not repeated.
//   * The mode-13h chain-4 sequencer/CRTC side effects of video_set_mode
//     (vga_seq_regs, mem->vga_planar, mem->vga_map_mask, crtc_regs[12..13]) are
//     private or live in emu88_mem; only the 64000-byte clear and the BDA are
//     checked here.
//   * video_write_char's multi-page addressing is checked for page 0 and 1
//     only; pages 2-7 use the same arithmetic.
//   * The keyboard idle heuristic (KBD_POLL_THRESHOLD = 500 consecutive AH=01
//     polls AND one tick of emulated time before waiting_for_key is raised) is
//     not driven to its threshold — that path needs run_batch's cycle counter,
//     which this harness deliberately does not use.
//   * INT 15h AH=87's descriptor handling is exercised with one well-formed
//     6-entry GDT.  It validates neither access bytes nor limits, so there is
//     no error path to test.
//   * Nothing here asserts anything about timing.  CYCLES_PER_TICK,
//     CYCLES_PER_REFRESH and emit_video_frame belong to dos_machine.cc.
//
// ---------------------------------------------------------------------------
// SHOWN TO BE ABLE TO FAIL
// ---------------------------------------------------------------------------
// 18 single-point mutations were applied to a scratch copy of dos_bios.cc, one
// at a time, and the harness was rebuilt and rerun against each:
//
//   1  video_scroll: `bottom - lines` -> `bottom - lines + 1`        killed
//   2  video_scroll: clear-bottom loop starts at `bottom - lines`    killed
//   3  video_scroll: `right >= screen_cols` -> `>`                   killed
//   4  video_scroll down: `r >= top + lines` -> `r > top + lines`    killed
//   5  video_scroll: the cleared cell keeps 0x07 instead of `attr`   killed
//   6  INT 10h AH=02: row and column swapped                         killed
//   7  video_set_mode: the bda_w16(SCREEN_COLS) write dropped        killed
//   8  video_set_mode: bda_w8(SCREEN_ROWS, screen_rows) not -1       killed
//   9  video_set_mode: the cursor-reset loop runs 1..7 not 0..7      killed
//  10  video_tty: BS decrements when col >= 0 instead of col > 0     killed
//  11  video_tty: LF wraps to row 0 instead of scrolling             killed
//  12  cursor wrap in video_tty: `col >= screen_cols` -> `>`         killed
//  13  INT 13h AH=02: `sector - 1` -> `sector`                       killed
//  14  INT 13h AH=02: CF forced clear on a short read                killed
//  15  INT 13h AH=08: max_head -> g.heads                            killed
//  16  to_bcd: `(val / 10) << 4` -> `(val / 16) << 4`                killed
//  17  INT 16h ring wrap: `next >= BUF_END` -> `next > BUF_END`      killed
//  18  XMS AH=09: handle returned as `handle` not `handle + 1`       killed
//
// 18 applied, 18 killed, 0 survivors, on 2026-08-27.  Three earlier drafts of
// mutations 3, 9 and 12 survived the first pass and are the reason the
// right-edge/top-edge scroll assertions, the 8-page cursor-reset assertion and
// the column-wrap TTY assertion exist; they were added and the mutants died.
// The real dos_bios.cc was never edited — every mutation was applied to a copy
// under /tmp and built with -I emu88 pointing at the original headers.
//
// Eleven more were applied the same day against the six fixes, and FOUR of them
// SURVIVED the first pass.  Every one was a real hole, and each is the reason
// an assertion above exists:
//   * reverting only the mode-13h half of the AL-bit-7 fix changed nothing,
//     because the flag was tested in text mode only.  Both clear paths are
//     covered now.
//   * `> block' to `> block + 1' on the XMS destination bound changed nothing,
//     because every move tested asked for twice the block.  The odd-offset
//     boundary case covers it.
//   * dropping `src_off' from the source bound changed nothing, because every
//     source move started at offset 0.
//   * allowing CHS sector 0 again changed nothing, because a short read from
//     dos_io returns the same CF and AH=04h as a rejection.  io.reads is what
//     tells them apart.
// With those assertions added, 11 of 11 die.  Total: 29 applied, 29 killed.

#include "dos_machine.h"
#include "dos_io.h"
#include "emu88_mem.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

//=============================================================================
// Bookkeeping
//=============================================================================

// Number of bug() assertions that are still red.  Each states the correct
// PC-BIOS behaviour for a defect that is in emu88/dos_bios.cc today.  Lower
// this (and turn the bug() into a check()) when one is fixed.  A bug that
// starts passing FAILS this harness on purpose: a silent improvement means the
// number is stale.
static const int KNOWN_BUGS_EXPECTED = 0;

static int g_checks = 0;
static int g_failures = 0;
static int g_bugs = 0;          // bug() assertions still red - expected
static int g_bugs_fixed = 0;    // bug() assertions that started passing
static int g_stuck = 0;         // guest stubs that never reached their HLT
static int g_exceptions = 0;    // unexpected CPU exceptions during a stub

static void check(bool cond, const char *what) {
  g_checks++;
  if (!cond) { g_failures++; std::printf("  FAIL: %s\n", what); }
}

static void check_eq(uint32_t got, uint32_t want, const char *what) {
  g_checks++;
  if (got != want) {
    g_failures++;
    std::printf("  FAIL: %s (want 0x%X, got 0x%X)\n", what, want, got);
  }
}

// A pinned divergence from a real PC BIOS.  Same gate as check(): it asserts
// what THIS implementation does, so it fails in either direction.
static void diverge(bool cond, const char *what) {
  g_checks++;
  if (!cond) {
    g_failures++;
    std::printf("  FAIL (pinned divergence changed): %s\n", what);
  }
}

static void diverge_eq(uint32_t got, uint32_t want, const char *what) {
  g_checks++;
  if (got != want) {
    g_failures++;
    std::printf("  FAIL (pinned divergence changed): %s (pinned 0x%X, got 0x%X)\n",
                what, want, got);
  }
}

// `correct` states the real-PC-BIOS behaviour, expected to be false today.
// Unused while the ledger is empty; kept in place for the next defect.
[[maybe_unused]] static void bug(bool correct, const char *what) {
  g_checks++;
  if (correct) {
    g_bugs_fixed++;
    std::printf("  FIXED (lower KNOWN_BUGS_EXPECTED, make this a check()): %s\n", what);
  } else {
    g_bugs++;
    std::printf("  KNOWN BUG: %s\n", what);
  }
}

//=============================================================================
// Host I/O: RAM disks, a settable clock, and recorders for the callbacks
//=============================================================================

struct RamDisk {
  std::vector<uint8_t> data;
  bool present = false;
};

struct StubIO : dos_io {
  RamDisk fd0, fd1, hd0;                 // drives 00h, 01h, 80h

  // recorders
  int  mode_changes = 0;
  int  last_mode = -1, last_cols = -1, last_rows = -1;
  int  beeps = 0, last_beep_hz = 0, last_beep_ms = 0;
  int  reads = 0, writes = 0;
  int  last_drive = -1;
  uint64_t last_offset = 0;

  // settable clock
  int t_h = 0, t_m = 0, t_s = 0, t_hs = 0;
  int d_y = 2026, d_mo = 1, d_d = 1, d_w = 0;

  RamDisk *disk(int drive) {
    if (drive == 0x00) return &fd0;
    if (drive == 0x01) return &fd1;
    if (drive == 0x80) return &hd0;
    return nullptr;
  }

  void console_write(uint8_t) override {}
  bool console_has_input() override { return false; }
  int  console_read() override { return -1; }
  void video_mode_changed(int mode, int cols, int rows) override {
    mode_changes++; last_mode = mode; last_cols = cols; last_rows = rows;
  }
  void video_refresh(const uint8_t *, int, int) override {}
  void video_refresh_direct(const uint8_t *, int, int, int, int,
                            const uint8_t[][3]) override {}
  void video_set_cursor(int, int) override {}

  bool disk_present(int drive) override {
    RamDisk *d = disk(drive);
    return d && d->present;
  }
  size_t disk_read(int drive, uint64_t offset, uint8_t *buf,
                   size_t count) override {
    reads++; last_drive = drive; last_offset = offset;
    RamDisk *d = disk(drive);
    if (!d || !d->present) return 0;
    // Overflow-safe on purpose: INT 13h AH=02 with sector number 0 computes
    // `(sector - 1)` as -1 and hands us an offset of 2^64-512.  `offset +
    // count > size` would wrap to 0 and let it through.
    if (offset > d->data.size() || count > d->data.size() - offset) return 0;
    std::memcpy(buf, d->data.data() + offset, count);
    return count;
  }
  size_t disk_write(int drive, uint64_t offset, const uint8_t *buf,
                    size_t count) override {
    writes++; last_drive = drive; last_offset = offset;
    RamDisk *d = disk(drive);
    if (!d || !d->present) return 0;
    if (offset > d->data.size() || count > d->data.size() - offset) return 0;
    std::memcpy(d->data.data() + offset, buf, count);
    return count;
  }
  uint64_t disk_size(int drive) override {
    RamDisk *d = disk(drive);
    return (d && d->present) ? d->data.size() : 0;
  }

  void get_time(int &h, int &m, int &s, int &hs) override {
    h = t_h; m = t_m; s = t_s; hs = t_hs;
  }
  void get_date(int &y, int &mo, int &d, int &w) override {
    y = d_y; mo = d_mo; d = d_d; w = d_w;
  }
  void speaker_beep(int hz, int ms) override {
    beeps++; last_beep_hz = hz; last_beep_ms = ms;
  }
  bool mouse_present() override { return true; }
  void mouse_get_state(int &x, int &y, int &b) override { x = 0; y = 0; b = 0; }
};

//=============================================================================
// Guest memory layout and the interrupt stubs
//=============================================================================

using E = emu88;

static emu88_mem   *gmem;
static dos_machine *gm;
static StubIO      *gio;

// 0x0800:0000  hand-assembled real-mode INT 1Ch handler
// 0x2000:....  stack
// 0x3000:0000  code: CD <vec> / F4   at +0000
//              CALL FAR F000:EFD8 / F4  at +0010
// 0x4000:....  DS scratch (DAPs, XMS move structs)
// 0x5000:....  ES scratch (sector buffers, the AH=87 GDT)
static constexpr uint16_t RM_HOOK_SEG  = 0x0800;
static constexpr uint32_t RM_HOOK_PHYS = 0x8000;
static constexpr uint16_t CODE_SEG     = 0x3000;
static constexpr uint32_t CODE_PHYS    = 0x30000;
static constexpr uint16_t STACK_SEG    = 0x2000;
static constexpr uint16_t DATA_SEG     = 0x4000;
static constexpr uint32_t DATA_PHYS    = 0x40000;
static constexpr uint16_t BUF_SEG      = 0x5000;
static constexpr uint32_t BUF_PHYS     = 0x50000;

static constexpr uint16_t OFF_INT = 0x0000;
static constexpr uint16_t OFF_XMS = 0x0010;

static uint8_t  rb(uint32_t a) { return gmem->fetch_mem(a); }
static uint16_t rw(uint32_t a) { return gmem->fetch_mem16(a); }
static uint32_t rd(uint32_t a) { return gmem->fetch_mem32(a); }
static void     wb(uint32_t a, uint8_t v) { gmem->store_mem(a, v); }
static void     ww(uint32_t a, uint16_t v) { gmem->store_mem16(a, v); }

// BDA (0040:xxxx) accessors, by the offsets in dos_machine.h's `bda` namespace.
static uint8_t  bda_b(int off) { return rb(0x400 + (uint32_t)off); }
static uint16_t bda_w(int off) { return rw(0x400 + (uint32_t)off); }
static uint32_t bda_d(int off) { return rd(0x400 + (uint32_t)off); }
static void     bda_set_b(int off, uint8_t v) { wb(0x400 + (uint32_t)off, v); }
static void     bda_set_w(int off, uint16_t v) { ww(0x400 + (uint32_t)off, v); }

static uint16_t AX() { return gm->get_reg16(E::reg_AX); }
static uint16_t BX() { return gm->get_reg16(E::reg_BX); }
static uint16_t CX() { return gm->get_reg16(E::reg_CX); }
static uint16_t DX() { return gm->get_reg16(E::reg_DX); }
static uint16_t DI() { return gm->get_reg16(E::reg_DI); }
static uint8_t  AL() { return gm->get_reg8(E::reg_AL); }
static uint8_t  AH() { return gm->get_reg8(E::reg_AH); }
static uint8_t  BL() { return gm->get_reg8(E::reg_BL); }
static uint8_t  BH() { return gm->get_reg8(E::reg_BH); }
static uint8_t  CL() { return gm->get_reg8(E::reg_CL); }
static uint8_t  CH() { return gm->get_reg8(E::reg_CH); }
static uint8_t  DL() { return gm->get_reg8(E::reg_DL); }
static uint8_t  DH() { return gm->get_reg8(E::reg_DH); }
static uint16_t ES() { return gm->sregs[E::seg_ES]; }
static bool     CF() { return gm->get_flag(E::FLAG_CF); }
static bool     ZF() { return gm->get_flag(E::FLAG_ZF); }

// Register inputs for one stub run.  Declaration order matters: C++20
// designated initialisers must appear in this order at the call site.
struct In {
  uint16_t ax = 0, bx = 0, cx = 0, dx = 0, si = 0, di = 0, bp = 0;
  uint16_t ds = DATA_SEG, es = BUF_SEG;
};

// Run guest code at CODE_SEG:off until it halts.  execute() is stepped
// directly so nothing but the guest advances the machine.
static void run_from(uint16_t off, int budget, bool expect_halt) {
  gm->ip = off;
  gm->halted = false;
  gm->exception_pending = false;
  gm->in_exception = false;
  int n = 0;
  while (!gm->halted && n < budget) { gm->execute(); n++; }
  if (expect_halt && !gm->halted) g_stuck++;
  if (gm->exception_pending || gm->in_exception) g_exceptions++;
}

static void load_regs(const In &in) {
  gm->exception_pending = false;
  gm->in_exception = false;
  gm->load_segment_real(E::seg_CS, CODE_SEG);
  gm->load_segment_real(E::seg_SS, STACK_SEG);
  gm->load_segment_real(E::seg_DS, in.ds);
  gm->load_segment_real(E::seg_ES, in.es);
  gm->set_reg16(E::reg_SP, 0xFFF0);
  gm->set_reg16(E::reg_AX, in.ax);
  gm->set_reg16(E::reg_BX, in.bx);
  gm->set_reg16(E::reg_CX, in.cx);
  gm->set_reg16(E::reg_DX, in.dx);
  gm->set_reg16(E::reg_SI, in.si);
  gm->set_reg16(E::reg_DI, in.di);
  gm->set_reg16(E::reg_BP, in.bp);
  gm->flags = 0x0002;                 // CF/ZF clear, IF clear (no IRQ delivery)
}

// One `CD <vec> / F4` at CODE_SEG:0000, executed.
static void intN(uint8_t vec, In in = In{}, bool expect_halt = true) {
  wb(CODE_PHYS + OFF_INT + 0, 0xCD);
  wb(CODE_PHYS + OFF_INT + 1, vec);
  wb(CODE_PHYS + OFF_INT + 2, 0xF4);
  load_regs(in);
  run_from(OFF_INT, 4000, expect_halt);
}

static void int10(uint16_t ax, In in = In{}) { in.ax = ax; intN(0x10, in); }
static void int13(uint16_t ax, In in = In{}) { in.ax = ax; intN(0x13, in); }
static void int15(uint16_t ax, In in = In{}) { in.ax = ax; intN(0x15, in); }
static void int16(uint16_t ax, In in = In{}) { in.ax = ax; intN(0x16, in); }
static void int1a(uint16_t ax, In in = In{}) { in.ax = ax; intN(0x1A, in); }
static void int2f(uint16_t ax, In in = In{}) { in.ax = ax; intN(0x2F, in); }

// FAR CALL F000:EFD8 — the XMS driver entry point a real client gets from
// INT 2Fh AX=4310h.  The ROM stub there is F1 FE CB: the 0xF1 trap runs
// xms_dispatch(), then the RETF returns to the HLT below.
static void xms(uint8_t func, In in = In{}) {
  static const uint8_t stub[] = { 0x9A, 0xD8, 0xEF, 0x00, 0xF0, 0xF4 };
  for (size_t i = 0; i < sizeof(stub); i++)
    wb(CODE_PHYS + OFF_XMS + (uint32_t)i, stub[i]);
  in.ax = (uint16_t)((uint16_t)func << 8) | (in.ax & 0xFF);
  load_regs(in);
  run_from(OFF_XMS, 4000, true);
}

// Text-mode cell accessors.  `cols` is whatever the current mode set.
static uint32_t vram = CGA_VRAM_BASE;
static int      vcols = 80;
static uint8_t  vch(int r, int c) { return rb(vram + (uint32_t)(r * vcols + c) * 2); }
static uint8_t  vat(int r, int c) { return rb(vram + (uint32_t)(r * vcols + c) * 2 + 1); }

// Fill the text page so a scroll can be told apart row by row AND attribute by
// attribute: row r is the character 'A'+r with attribute 0x10+r.
static void fill_text(int rows) {
  for (int r = 0; r < rows; r++)
    for (int c = 0; c < vcols; c++) {
      wb(vram + (uint32_t)(r * vcols + c) * 2,     (uint8_t)('A' + r));
      wb(vram + (uint32_t)(r * vcols + c) * 2 + 1, (uint8_t)(0x10 + r));
    }
}

static bool row_is(int r, uint8_t ch, uint8_t at) {
  for (int c = 0; c < vcols; c++)
    if (vch(r, c) != ch || vat(r, c) != at) return false;
  return true;
}

static void set_cursor(int row, int col) {
  int10(0x0200, In{.ax = 0, .bx = 0x0000,
                   .cx = 0, .dx = (uint16_t)((row << 8) | col)});
}

static uint16_t cursor_pos(int page = 0) { return bda_w(bda::CURSOR_POS + page * 2); }

static void section(const char *name) { std::printf("--- %s\n", name); }

//=============================================================================

int main() {
  // Line-buffered: this harness drives a whole machine, so a defect in the code
  // under test can abort it, and a full-buffered stdout discards every FAIL line
  // already printed when that happens.  A gate that cannot tell you what it saw
  // is most of the way to a gate that cannot fail.
  std::setvbuf(stdout, nullptr, _IOLBF, 0);
  static emu88_mem mem(0x1000000);     // 16MB: 15360 KB above the 1MB line
  static StubIO io;
  static dos_machine m(&mem, &io);
  gmem = &mem; gm = &m; gio = &io;

  //--------------------------------------------------------------------------
  // Attach the RAM disks BEFORE init_machine(): init_bda() counts hard disks
  // into 0040:0075 exactly once, at init time.
  //--------------------------------------------------------------------------
  auto make_disk = [](RamDisk &d, size_t bytes, uint8_t tag) {
    d.data.resize(bytes);
    d.present = true;
    for (size_t i = 0; i < bytes; i++)
      d.data[i] = (uint8_t)(i * 5 + (i >> 9) * 11 + tag);
  };
  make_disk(io.fd0, 368640,  0x10);    // 360 KB  -> 2 heads, 40 cyls,  9 spt
  make_disk(io.fd1, 1474560, 0x20);    // 1.44 MB -> 2 heads, 80 cyls, 18 spt
  make_disk(io.hd0, 4032 * 512, 0x30); // 2,064,384 bytes -> 16 heads, 63 spt, 4 cyls

  m.init_machine();
  mem.set_a20(true);

  //==========================================================================
  // 1. What init_bda/init_ivt laid down, and INT 11h / INT 12h reading it back
  //==========================================================================
  section("INT 11h equipment list / INT 12h memory size");

  check_eq(rw(0x10 * 4), 0xE000 + 0x10 * 4,
           "setup: IVT[10h] offset = the ROM stub at E000h + vector*4");
  check_eq(rw(0x10 * 4 + 2), 0xF000, "setup: IVT[10h] points into the BIOS ROM");
  check_eq(rb(0xF0000 + rw(0x10 * 4)), 0xF1,
           "setup: the INT 10h ROM stub starts with the BIOS trap opcode");
  check_eq(rb(0xF0000 + rw(0x10 * 4) + 1), 0x10,
           "setup: the ROM stub carries its own vector number");

  // Default Config::display is DISPLAY_CGA, mouse_enabled is true:
  //   bit 0 floppy present, bits 4-5 = 10 (80x25 colour), bit 2 PS/2 mouse.
  check_eq(bda_w(bda::EQUIPMENT), 0x0025, "BDA 40:10 equipment word = 0025h");
  intN(0x11);
  check_eq(AX(), bda_w(bda::EQUIPMENT), "INT 11h returns the BDA equipment word");
  check_eq(AX(), 0x0025, "INT 11h AX=0025h (floppy + 80x25 colour + mouse)");

  // Real BIOS POST derives bit 0 and bits 6-7 (number of floppies minus one)
  // from what it finds.  init_bda hard-codes bit 0 and never writes bits 6-7,
  // so two attached floppies still read as "one drive".
  diverge_eq(AX() & 0x00C1, 0x0001,
             "divergence: equipment bits 6-7 (floppy count) are always 0 "
             "even with drives 00h and 01h attached");

  check_eq(bda_w(bda::MEM_SIZE_KB), 640, "BDA 40:13 conventional memory = 640 KB");
  intN(0x12);
  check_eq(AX(), 640, "INT 12h returns 640 KB");
  check_eq(AX(), bda_w(bda::MEM_SIZE_KB), "INT 12h returns the BDA word verbatim");

  check_eq(bda_b(bda::NUM_HDD), 1, "BDA 40:75 hard-disk count = 1");
  check_eq(bda_b(bda::VIDEO_MODE), 3, "BDA 40:49 initial video mode = 3");
  check_eq(bda_w(bda::SCREEN_COLS), 80, "BDA 40:4A initial columns = 80");
  check_eq(bda_b(bda::SCREEN_ROWS), 24, "BDA 40:84 initial rows-1 = 24");
  check_eq(bda_w(bda::CRTC_BASE), 0x3D4, "BDA 40:63 CRTC port = 3D4h (colour)");
  check_eq(bda_w(bda::CURSOR_SHAPE), 0x0607, "BDA 40:60 cursor shape = 0607h");
  check_eq(bda_w(bda::KBD_BUF_START), 0x1E, "BDA 40:80 keyboard ring start");
  check_eq(bda_w(bda::KBD_BUF_END), 0x3E, "BDA 40:82 keyboard ring end");

  // init_bda writes the IBM value 4096; the first AH=00 mode set below
  // overwrites it with cols*rows*2 = 4000.  The two disagree.
  diverge_eq(bda_w(bda::VIDEO_PAGE_SZ), 4096,
             "divergence: init_bda writes 40:4C = 4096 (the IBM page size) and "
             "the first mode set replaces it with 4000");

  //==========================================================================
  // 2. INT 10h AH=00 - set video mode, and the BDA a mode set writes
  //==========================================================================
  section("INT 10h AH=00 set mode");

  wb(CGA_VRAM_BASE + 0, 0x55);          // dirty the page so the clear is visible
  wb(CGA_VRAM_BASE + 1, 0xAA);
  io.mode_changes = 0;
  int10(0x0003);
  vram = CGA_VRAM_BASE; vcols = 80;

  check_eq(bda_b(bda::VIDEO_MODE), 3, "AH=00/3: 40:49 = 3");
  check_eq(bda_w(bda::SCREEN_COLS), 80, "AH=00/3: 40:4A = 80 columns");
  check_eq(bda_b(bda::SCREEN_ROWS), 24, "AH=00/3: 40:84 = 24 (rows - 1)");
  check_eq(bda_w(bda::VIDEO_PAGE_SZ), 4000, "AH=00/3: 40:4C = 4000 (80*25*2)");
  check_eq(bda_w(bda::VIDEO_PAGE_OFF), 0, "AH=00/3: 40:4E page offset = 0");
  check_eq(bda_b(bda::ACTIVE_PAGE), 0, "AH=00/3: 40:62 active page = 0");
  check_eq(bda_w(bda::CRTC_BASE), 0x3D4, "AH=00/3: 40:63 = 3D4h");
  check_eq(cursor_pos(0), 0x0000, "AH=00/3: 40:50 cursor for page 0 reset");
  for (int p = 1; p < 8; p++)
    check_eq(cursor_pos(p), 0x0000, "AH=00/3: cursor reset for every one of the 8 pages");
  check(row_is(0, 0x20, 0x07) && row_is(24, 0x20, 0x07),
        "AH=00/3: the whole 80x25 page is cleared to space on light grey");
  check_eq(io.mode_changes, 1, "AH=00: dos_io::video_mode_changed called once");
  check_eq((uint32_t)io.last_mode, 3, "AH=00: video_mode_changed(mode=3)");
  check_eq((uint32_t)io.last_cols, 80, "AH=00: video_mode_changed(cols=80)");
  check_eq((uint32_t)io.last_rows, 25, "AH=00: video_mode_changed(rows=25)");

  // 40x25 colour text
  int10(0x0000);
  vcols = 40;
  check_eq(bda_b(bda::VIDEO_MODE), 0, "AH=00/0: 40:49 = 0");
  check_eq(bda_w(bda::SCREEN_COLS), 40, "AH=00/0: 40:4A = 40 columns");
  check_eq(bda_w(bda::VIDEO_PAGE_SZ), 2000, "AH=00/0: 40:4C = 2000 (40*25*2)");
  check(row_is(24, 0x20, 0x07), "AH=00/0: the 40-column page is cleared");
  check_eq((uint32_t)io.last_cols, 40, "AH=00/0: video_mode_changed(cols=40)");

  // MDA 80x25 mono - a different VRAM base and a different CRTC port
  wb(MDA_VRAM_BASE + 0, 0x55);
  int10(0x0007);
  vram = MDA_VRAM_BASE; vcols = 80;
  check_eq(bda_w(bda::CRTC_BASE), 0x3B4, "AH=00/7: 40:63 = 3B4h (mono)");
  check_eq(vch(0, 0), 0x20, "AH=00/7: mode 7 clears B000:0000, not B800:0000");
  check(row_is(24, 0x20, 0x07), "AH=00/7: the MDA page is cleared");

  // VGA mode 13h: 320x200x256 at A000:0000
  for (int i = 0; i < 8; i++) wb(VGA_VRAM_BASE + (uint32_t)i, 0x77);
  int10(0x0013);
  check_eq(bda_b(bda::VIDEO_MODE), 0x13, "AH=00/13h: 40:49 = 13h");
  check_eq(bda_w(bda::VIDEO_PAGE_SZ), 0xFA00, "AH=00/13h: 40:4C = FA00h (64000)");
  check_eq(bda_w(bda::SCREEN_COLS), 40, "AH=00/13h: 40:4A = 40 text columns");
  check_eq(rb(VGA_VRAM_BASE + 0), 0, "AH=00/13h: A000:0000 zeroed");
  check_eq(rb(VGA_VRAM_BASE + 63999), 0, "AH=00/13h: all 64000 bytes zeroed");
  check(!mem.svga_active, "AH=00/13h: a legacy mode clears mem.svga_active");

  // AH=0Ch / AH=0Dh pixels, only implemented for mode 13h
  int10(0x0C2A, In{.ax = 0, .bx = 0, .cx = 10, .dx = 20});
  check_eq(rb(VGA_VRAM_BASE + 20 * 320 + 10), 0x2A,
           "AH=0Ch: mode 13h pixel (10,20) written to A000:(y*320+x)");
  int10(0x0D00, In{.ax = 0, .bx = 0, .cx = 10, .dx = 20});
  check_eq(AL(), 0x2A, "AH=0Dh: reads the same pixel back");
  int10(0x0D00, In{.ax = 0, .bx = 0, .cx = 400, .dx = 20});
  check_eq(AL(), 0, "AH=0Dh: x past 319 reads 0");
  int10(0x0C2A, In{.ax = 0, .bx = 0, .cx = 400, .dx = 20});
  check_eq(rb(VGA_VRAM_BASE + 20 * 320 + 10), 0x2A,
           "AH=0Ch: an out-of-range x writes nothing");

  int10(0x0003);
  vram = CGA_VRAM_BASE; vcols = 80;
  int10(0x0D00, In{.ax = 0, .bx = 0, .cx = 10, .dx = 20});
  diverge_eq(AL(), 0,
             "divergence: AH=0Dh returns 0 in every mode but 13h - CGA, EGA and "
             "mode 7 pixel reads are not implemented");

  // AL bit 7 means "do not clear the display buffer".  video_set_mode() used to
  // mask the bit off (`al & 0x7F`) and then always clear, so a program that
  // re-selects its current mode to reset the CRTC lost the screen.  Fixed
  // 2026-08-27; the flag reaches video_set_mode_ex now.
  wb(CGA_VRAM_BASE + 0, 'X');
  wb(CGA_VRAM_BASE + 1, 0x4F);
  int10(0x0083);
  check(vch(0, 0) == 'X',
        "INT 10h AH=00 with AL bit 7 set does NOT clear video memory");
  check(vat(0, 0) == 0x4F, "AH=00 with bit 7 set leaves the attribute too");
  check_eq(bda_b(bda::VIDEO_MODE), 3, "AH=00: 40:49 stores the mode with bit 7 stripped");
  // ... and in mode 13h, which clears through a different branch: 64000 bytes
  // of framebuffer rather than a cell loop.  A fix applied to only one of the
  // two branches would pass the text-mode check above and lose a game's screen.
  int10(0x0013);
  wb(VGA_VRAM_BASE + 0, 0x5E);
  wb(VGA_VRAM_BASE + 63999, 0x6D);
  int10(0x0093);
  check_eq(rb(VGA_VRAM_BASE + 0), 0x5E,
           "AH=00/13h with AL bit 7 set does NOT clear the framebuffer");
  check_eq(rb(VGA_VRAM_BASE + 63999), 0x6D,
           "AH=00/13h with bit 7 set leaves the last byte too");
  check_eq(bda_b(bda::VIDEO_MODE), 0x13, "AH=00/93h: 40:49 = 13h, bit 7 stripped");
  // The BDA and the cursor are still reset either way - only the clear is
  // suppressed.
  check_eq(bda_w(bda::VIDEO_PAGE_SZ), 0xFA00, "AH=00/93h still writes 40:4C");
  check_eq(bda_w(bda::CURSOR_POS), 0x0000, "AH=00/93h still resets the cursor");
  // Back to text for the sections that follow.
  int10(0x0003);
  vram = CGA_VRAM_BASE; vcols = 80;

  //==========================================================================
  // 3. INT 10h AH=01/02/03 - cursor shape and position
  //==========================================================================
  section("INT 10h AH=01/02/03 cursor");

  int10(0x0200, In{.ax = 0, .bx = 0x0000, .cx = 0, .dx = 0x070D});
  check_eq(cursor_pos(0), 0x070D, "AH=02: 40:50 = row 7 in the high byte, col 13 in the low");
  int10(0x0300, In{.ax = 0, .bx = 0x0000});
  check_eq(DX(), 0x070D, "AH=03: DH=row 7, DL=col 13");
  check_eq(DH(), 7, "AH=03: DH is the row");
  check_eq(DL(), 13, "AH=03: DL is the column");
  check_eq(CX(), 0x0607, "AH=03: CX = the 40:60 cursor shape");

  int10(0x0100, In{.ax = 0, .bx = 0, .cx = 0x0A0B});
  check_eq(bda_w(bda::CURSOR_SHAPE), 0x0A0B, "AH=01: 40:60 = CH:CL");
  int10(0x0300, In{.ax = 0, .bx = 0x0000});
  check_eq(CX(), 0x0A0B, "AH=03: reads the new cursor shape back");
  int10(0x0100, In{.ax = 0, .bx = 0, .cx = 0x0607});   // put it back

  // per-page cursors
  int10(0x0200, In{.ax = 0, .bx = 0x0300, .cx = 0, .dx = 0x0102});
  check_eq(cursor_pos(3), 0x0102, "AH=02 BH=3: writes page 3's cursor slot");
  check_eq(cursor_pos(0), 0x070D, "AH=02 BH=3: page 0's cursor is untouched");
  int10(0x0300, In{.ax = 0, .bx = 0x0300});
  check_eq(DX(), 0x0102, "AH=03 BH=3: reads page 3's cursor");

  int10(0x0200, In{.ax = 0, .bx = 0x0800, .cx = 0, .dx = 0x1010});
  check_eq(cursor_pos(0), 0x070D, "AH=02 BH=8: an out-of-range page writes nothing");
  int10(0x0300, In{.ax = 0, .bx = 0x0800});
  check_eq(DX(), 0x070D, "AH=03 BH=8: an out-of-range page reads page 0");

  //==========================================================================
  // 4. INT 10h AH=09/0A/08 - write and read back character cells
  //==========================================================================
  section("INT 10h AH=09/0A/08 character cells");

  int10(0x0003);
  set_cursor(2, 5);
  int10((uint16_t)(0x0900 | 'A'), In{.ax = 0, .bx = 0x001E, .cx = 3});
  check_eq(vch(2, 5), 'A', "AH=09: first cell is the character");
  check_eq(vat(2, 5), 0x1E, "AH=09: first cell takes BL as its attribute");
  check_eq(vch(2, 7), 'A', "AH=09 CX=3: three cells written");
  check_eq(vat(2, 7), 0x1E, "AH=09 CX=3: the attribute repeats too");
  check_eq(vch(2, 8), 0x20, "AH=09 CX=3: the fourth cell is untouched");
  check_eq(cursor_pos(0), 0x0205, "AH=09 does not advance the cursor");

  set_cursor(3, 0);
  int10((uint16_t)(0x0900 | 'C'), In{.ax = 0, .bx = 0x004F, .cx = 2});
  set_cursor(3, 0);
  int10((uint16_t)(0x0A00 | 'B'), In{.ax = 0, .bx = 0x0000, .cx = 2});
  check_eq(vch(3, 0), 'B', "AH=0Ah: the character is replaced");
  check_eq(vat(3, 0), 0x4F, "AH=0Ah: the existing attribute is kept");
  check_eq(vch(3, 1), 'B', "AH=0Ah CX=2: second cell written");
  check_eq(vat(3, 1), 0x4F, "AH=0Ah CX=2: second attribute kept");
  check_eq(cursor_pos(0), 0x0300, "AH=0Ah does not advance the cursor");

  // Read back what AH=09 wrote.
  set_cursor(2, 6);
  int10(0x0800, In{.ax = 0, .bx = 0x0000});
  check_eq(AL(), 'A', "AH=08: AL = the character at the cursor");
  check_eq(AH(), 0x1E, "AH=08: AH = the attribute at the cursor");
  set_cursor(3, 0);
  int10(0x0800, In{.ax = 0, .bx = 0x0000});
  check_eq(AX(), 0x4F42, "AH=08: AH:AL = attribute:character as one word");

  // The right-edge wrap inside one AH=09 burst.
  set_cursor(1, 78);
  int10((uint16_t)(0x0900 | 'W'), In{.ax = 0, .bx = 0x0021, .cx = 4});
  check_eq(vch(1, 78), 'W', "AH=09 wrap: col 78");
  check_eq(vch(1, 79), 'W', "AH=09 wrap: col 79");
  check_eq(vch(2, 0), 'W', "AH=09 wrap: continues on the next row");
  check_eq(vch(2, 1), 'W', "AH=09 wrap: fourth character on the next row");

  // A REP STOSW-based BIOS writes nothing when CX=0.  This one coerces to 1.
  set_cursor(4, 0);
  wb(CGA_VRAM_BASE + (4 * 80) * 2, 0x20);
  int10((uint16_t)(0x0900 | 'Z'), In{.ax = 0, .bx = 0x0007, .cx = 0});
  diverge(vch(4, 0) == 'Z',
          "divergence: AH=09 with CX=0 writes one character; a REP STOSW BIOS "
          "writes none");

  // Past the last cell the row clamps instead of wrapping to the page start,
  // so the final cell is overwritten repeatedly.
  int10(0x0003);
  set_cursor(24, 78);
  int10((uint16_t)(0x0900 | 'E'), In{.ax = 0, .bx = 0x0070, .cx = 5});
  diverge(vch(24, 79) == 'E' && vch(0, 0) == 0x20,
          "divergence: an AH=09 burst past the end of the page clamps to the "
          "last row and rewrites its last cell instead of wrapping to the page start");

  //==========================================================================
  // 5. INT 10h AH=05 - active page, and where page 1 lives
  //==========================================================================
  section("INT 10h AH=05 active page");

  int10(0x0003);
  wb(CGA_VRAM_BASE + 0x1000, 0xDD);           // the IBM page-1 origin, marked
  int10(0x0501);
  check_eq(bda_b(bda::ACTIVE_PAGE), 1, "AH=05: 40:62 = 1");
  check_eq(bda_w(bda::VIDEO_PAGE_OFF), 4000, "AH=05: 40:4E = page * 40:4C");
  set_cursor(0, 0);
  int10((uint16_t)(0x0900 | 'P'), In{.ax = 0, .bx = 0x0017, .cx = 1});
  check_eq(rb(CGA_VRAM_BASE + 4000), 'P',
           "AH=09 on page 1 writes at B800:0FA0 (page 0 size 4000)");
  // A real BIOS keeps 4KB-aligned pages: page 1 begins at B800:1000, and a mode
  // set clears the full 4096 bytes of page 0 rather than 80*25*2 of them.
  diverge_eq(rb(CGA_VRAM_BASE + 0x1000), 0xDD,
             "divergence: page 1 starts at offset 4000, so the hardware-standard "
             "page-1 origin B800:1000 is written by nobody and cleared by nobody");
  int10(0x0500);
  check_eq(bda_b(bda::ACTIVE_PAGE), 0, "AH=05: back to page 0");
  check_eq(bda_w(bda::VIDEO_PAGE_OFF), 0, "AH=05: page 0 offset = 0");

  //==========================================================================
  // 6. INT 10h AH=06/07 - scroll up and down
  //==========================================================================
  section("INT 10h AH=06/07 scroll");

  int10(0x0003);
  vram = CGA_VRAM_BASE; vcols = 80;

  // Full-window clear: AL=0 means "blank the whole window", attribute in BH.
  fill_text(25);
  int10(0x0600, In{.ax = 0, .bx = 0x4E00, .cx = 0x0000, .dx = 0x184F});
  check(row_is(0, 0x20, 0x4E), "AH=06 AL=0: row 0 cleared with BH as the attribute");
  check(row_is(24, 0x20, 0x4E), "AH=06 AL=0: row 24 cleared");
  check(row_is(12, 0x20, 0x4E), "AH=06 AL=0: the middle of the window cleared");

  // Partial-window scroll up by one, window rows 5..10 x cols 10..20.
  fill_text(25);
  int10(0x0601, In{.ax = 0, .bx = 0x7100, .cx = 0x050A, .dx = 0x0A14});
  check_eq(vch(5, 10), 'A' + 6, "AH=06/1: row 5 took row 6's character");
  check_eq(vat(5, 10), 0x16,    "AH=06/1: row 5 took row 6's attribute");
  check_eq(vch(9, 20), 'A' + 10, "AH=06/1: row 9 took row 10's character");
  check_eq(vat(9, 20), 0x1A,     "AH=06/1: row 9 took row 10's attribute");
  check_eq(vch(10, 10), 0x20, "AH=06/1: the bottom window row is blanked");
  check_eq(vat(10, 10), 0x71, "AH=06/1: the blanked row takes BH as its attribute");
  check_eq(vch(10, 20), 0x20, "AH=06/1: blanked through the right edge (col 20)");
  check_eq(vch(5, 9),  'A' + 5, "AH=06/1: col 9 is outside the window, untouched");
  check_eq(vat(5, 9),  0x15,    "AH=06/1: col 9's attribute untouched");
  check_eq(vch(5, 21), 'A' + 5, "AH=06/1: col 21 is outside the window, untouched");
  check_eq(vch(10, 21), 'A' + 10, "AH=06/1: col 21 of the blanked row untouched");
  check_eq(vch(4, 10), 'A' + 4, "AH=06/1: row 4 is above the window, untouched");
  check_eq(vch(11, 10), 'A' + 11, "AH=06/1: row 11 is below the window, untouched");

  // Scroll down by two in the same window.
  fill_text(25);
  int10(0x0702, In{.ax = 0, .bx = 0x2C00, .cx = 0x050A, .dx = 0x0A14});
  check_eq(vch(10, 10), 'A' + 8, "AH=07/2: row 10 took row 8's character");
  check_eq(vat(10, 10), 0x18,    "AH=07/2: row 10 took row 8's attribute");
  check_eq(vch(7, 15), 'A' + 5, "AH=07/2: row 7 took row 5's character");
  check_eq(vch(6, 10), 0x20, "AH=07/2: row 6 blanked");
  check_eq(vat(6, 10), 0x2C, "AH=07/2: row 6 takes BH as its attribute");
  check_eq(vch(5, 15), 0x20, "AH=07/2: row 5 blanked");
  check_eq(vch(5, 9),  'A' + 5, "AH=07/2: col 9 untouched");
  check_eq(vch(4, 10), 'A' + 4, "AH=07/2: row 4 untouched");
  check_eq(vch(11, 10), 'A' + 11, "AH=07/2: row 11 untouched");

  // AL larger than the window height degenerates to a clear.
  fill_text(25);
  int10(0x0605, In{.ax = 0, .bx = 0x3300, .cx = 0x0200, .dx = 0x044F});
  check(row_is(2, 0x20, 0x33) && row_is(4, 0x20, 0x33),
        "AH=06: AL greater than the window height clears the window");
  check_eq(vch(1, 0), 'A' + 1, "AH=06: the clear stayed inside the window");
  check_eq(vch(5, 0), 'A' + 5, "AH=06: the row below the window is untouched");

  // bottom/right past the screen are clamped, not honoured.
  fill_text(25);
  int10(0x0600, In{.ax = 0, .bx = 0x0900, .cx = 0x1400, .dx = 0x40C8});
  check(row_is(24, 0x20, 0x09), "AH=06: a bottom row past 24 clamps to 24");
  check(row_is(20, 0x20, 0x09), "AH=06: the top of the clamped window cleared");
  check_eq(vch(19, 0), 'A' + 19, "AH=06: row 19 is above the window, untouched");
  check_eq(vch(24, 79), 0x20, "AH=06: a right column past 79 clamps to 79");

  // An inverted window is rejected outright.
  fill_text(25);
  int10(0x0601, In{.ax = 0, .bx = 0x1200, .cx = 0x0A00, .dx = 0x054F});
  check_eq(vch(10, 0), 'A' + 10, "AH=06 with top > bottom does nothing");
  check_eq(vch(5, 0), 'A' + 5, "AH=06 with top > bottom leaves the bottom row alone");
  fill_text(25);
  int10(0x0601, In{.ax = 0, .bx = 0x1200, .cx = 0x0514, .dx = 0x0A0A});
  check_eq(vch(5, 20), 'A' + 5, "AH=06 with left > right does nothing");

  //==========================================================================
  // 7. INT 10h AH=0Eh - TTY output
  //==========================================================================
  section("INT 10h AH=0Eh TTY");

  // THE BANNER.  video_tty() injects "iosFreeDOS <version>\r\n" ahead of the
  // very first printable character any program ever prints through AH=0Eh.  No
  // PC BIOS does this; it is the emulator announcing itself into the guest's
  // own screen buffer, and it consumes the top line.  It happens once per
  // dos_machine, so it has to be the first TTY test in the file.
  int10(0x0003);
  set_cursor(0, 0);
  int10(0x0E00 | 'Z');
  const char *banner = "iosFreeDOS ";
  bool banner_ok = true;
  for (int i = 0; banner[i]; i++)
    if (vch(0, i) != (uint8_t)banner[i]) banner_ok = false;
  diverge(banner_ok,
          "divergence: the first AH=0Eh printable character emits an "
          "\"iosFreeDOS <version>\" banner into the guest's screen first");
  diverge(vch(1, 0) == 'Z' && cursor_pos(0) == 0x0101,
          "divergence: the caller's own character lands on row 1 after the "
          "banner's CR/LF, not at the cursor it set");

  // From here on the banner is spent and AH=0Eh behaves normally.
  int10(0x0003);
  set_cursor(6, 10);
  int10(0x0E00 | 'k');
  check_eq(vch(6, 10), 'k', "AH=0Eh writes the character at the cursor");
  check_eq(cursor_pos(0), 0x060B, "AH=0Eh advances the column by one");
  check_eq(vat(6, 10), 0x07, "AH=0Eh keeps the cell's existing attribute");

  // BL is ignored in text mode - which is what a real BIOS does too - but so
  // is BH, and a real BIOS writes to the page in BH rather than 40:62.
  int10(0x0501);
  set_cursor(0, 0);
  int10(0x0E00 | 'q', In{.ax = 0, .bx = 0x0000});
  diverge_eq(rb(CGA_VRAM_BASE + 4000), 'q',
             "divergence: AH=0Eh ignores BH and always writes to the 40:62 "
             "active page (BH=0 here, page 1 got the character)");
  int10(0x0500);

  int10(0x0003);
  set_cursor(3, 40);
  int10(0x0E0D);                                   // CR
  check_eq(cursor_pos(0), 0x0300, "AH=0Eh CR: column 0, same row");
  check_eq(vch(3, 40), 0x20, "AH=0Eh CR writes no character");
  int10(0x0E0A);                                   // LF
  check_eq(cursor_pos(0), 0x0400, "AH=0Eh LF: next row, same column");

  set_cursor(3, 5);
  int10(0x0E08);                                   // BS
  check_eq(cursor_pos(0), 0x0304, "AH=0Eh BS: column back by one");
  set_cursor(3, 0);
  int10(0x0E08);
  check_eq(cursor_pos(0), 0x0300, "AH=0Eh BS at column 0 does nothing");
  check_eq(vch(3, 0), 0x20, "AH=0Eh BS writes no character");

  io.beeps = 0;
  set_cursor(8, 8);
  int10(0x0E07);                                   // BEL
  check_eq((uint32_t)io.beeps, 1, "AH=0Eh BEL calls dos_io::speaker_beep");
  check_eq((uint32_t)io.last_beep_hz, 1000, "AH=0Eh BEL: 1000 Hz");
  check_eq((uint32_t)io.last_beep_ms, 100, "AH=0Eh BEL: 100 ms");
  check_eq(cursor_pos(0), 0x0808, "AH=0Eh BEL does not move the cursor");
  check_eq(vch(8, 8), 0x20, "AH=0Eh BEL writes no character");

  // Column wrap at the right edge.
  set_cursor(5, 79);
  int10(0x0E00 | 'y');
  check_eq(vch(5, 79), 'y', "AH=0Eh at column 79 writes column 79");
  check_eq(cursor_pos(0), 0x0600, "AH=0Eh wraps to row 6 column 0");

  // Writing the last cell of the last row scrolls the whole screen up one.
  fill_text(25);
  set_cursor(24, 79);
  int10(0x0E00 | 'Q');
  check_eq(vch(23, 79), 'Q', "AH=0Eh at the bottom-right scrolls, taking 'Q' to row 23");
  check_eq(vat(23, 79), 0x28, "AH=0Eh scroll carries the cell attribute up with it");
  check_eq(vch(0, 0), 'A' + 1, "AH=0Eh scroll: row 0 took row 1");
  check(row_is(24, 0x20, 0x07),
        "AH=0Eh scroll blanks row 24 with attribute 07h");
  check_eq(cursor_pos(0), 0x1800, "AH=0Eh scroll leaves the cursor at row 24 column 0");

  // LF on the last row scrolls instead of advancing off the screen.
  fill_text(25);
  set_cursor(24, 3);
  int10(0x0E0A);
  check_eq(vch(0, 0), 'A' + 1, "AH=0Eh LF on row 24 scrolls up one");
  check_eq(cursor_pos(0), 0x1803, "AH=0Eh LF on row 24 keeps row 24 and the column");

  // TAB is not a control character to a PC BIOS: the glyph goes out.
  int10(0x0003);
  set_cursor(2, 2);
  int10(0x0E09);
  check_eq(vch(2, 2), 0x09, "AH=0Eh writes the TAB glyph rather than expanding it");

  //==========================================================================
  // 8. INT 10h AH=0F / 11h / 12h / 13h / 1Ah
  //==========================================================================
  section("INT 10h AH=0F/11h/12h/13h/1Ah");

  int10(0x0003);
  int10(0x0F00);
  check_eq(AL(), 3, "AH=0Fh: AL = current mode");
  check_eq(AH(), 80, "AH=0Fh: AH = column count");
  check_eq(BH(), 0, "AH=0Fh: BH = active page");
  int10(0x0501);
  int10(0x0F00);
  check_eq(BH(), 1, "AH=0Fh: BH follows 40:62");
  int10(0x0500);
  int10(0x0000);
  int10(0x0F00);
  check_eq(AL(), 0, "AH=0Fh after mode 0: AL = 0");
  check_eq(AH(), 40, "AH=0Fh after mode 0: AH = 40 columns");
  int10(0x0003);
  vcols = 80; vram = CGA_VRAM_BASE;

  int10(0x1130);
  check_eq(CX(), 16, "AH=11h AL=30h: CX = 16 scan lines per character");
  check_eq(DL(), 24, "AH=11h AL=30h: DL = 40:84 (rows - 1)");
  diverge(ES() == BUF_SEG && DI() == 0,
          "divergence: AH=11h AL=30h returns no ES:BP font pointer at all - "
          "ES:BP is left exactly as the caller passed it");

  int10(0x1200, In{.ax = 0, .bx = 0x0010});
  check_eq(BH(), 0, "AH=12h BL=10h: BH = 0 (colour mode)");
  check_eq(BL(), 3, "AH=12h BL=10h: BL = 3 (256 KB)");
  check_eq(CH(), 0, "AH=12h BL=10h: CH = 0");
  check_eq(CL(), 0, "AH=12h BL=10h: CL = 0");
  diverge_eq(CL(), 0,
             "divergence: AH=12h BL=10h reports feature bits and switch "
             "settings as 0; a real EGA/VGA BIOS returns the adapter's own");
  int10(0x1234, In{.ax = 0, .bx = 0x0030});
  diverge(AX() == 0x1234 && BX() == 0x0030,
          "divergence: AH=12h with any BL but 10h leaves every register alone, "
          "so a program cannot tell the subfunction was ignored");

  // AH=13h write string with embedded attributes, cursor not updated.
  int10(0x0003);
  {
    const char *s = "Hi";
    wb(BUF_PHYS + 0x300, (uint8_t)s[0]); wb(BUF_PHYS + 0x301, 0x1F);
    wb(BUF_PHYS + 0x302, (uint8_t)s[1]); wb(BUF_PHYS + 0x303, 0x2E);
    set_cursor(0, 0);
    int10(0x1302, In{.ax = 0, .bx = 0x0000, .cx = 2, .dx = 0x0A05,
                     .si = 0, .di = 0, .bp = 0x0300});
    check_eq(vch(10, 5), 'H', "AH=13h AL=2: first character at row 10 col 5");
    check_eq(vat(10, 5), 0x1F, "AH=13h AL=2: first attribute out of the string");
    check_eq(vch(10, 6), 'i', "AH=13h AL=2: second character");
    check_eq(vat(10, 6), 0x2E, "AH=13h AL=2: second attribute");
    check_eq(cursor_pos(0), 0x0000, "AH=13h AL=2 (bit 0 clear) restores the cursor");

    wb(BUF_PHYS + 0x310, 'a'); wb(BUF_PHYS + 0x311, 'b'); wb(BUF_PHYS + 0x312, 'c');
    int10(0x1301, In{.ax = 0, .bx = 0x0047, .cx = 3, .dx = 0x0C00,
                     .si = 0, .di = 0, .bp = 0x0310});
    check_eq(vch(12, 0), 'a', "AH=13h AL=1: characters written from ES:BP");
    check_eq(vat(12, 2), 0x47, "AH=13h AL=1: BL supplies the attribute");
    check_eq(cursor_pos(0), 0x0C03, "AH=13h AL=1 (bit 0 set) leaves the cursor past the string");
  }

  // AH=1Ah display combination code, for the default DISPLAY_CGA config.
  int10(0x1A00);
  check_eq(AL(), 0x1A, "AH=1Ah AL=0: AL=1Ah means the call is supported");
  check_eq(BL(), 0x02, "AH=1Ah AL=0: BL=02h (CGA colour) for the default config");
  check_eq(BH(), 0x00, "AH=1Ah AL=0: BH=00h (no second adapter)");
  int10(0x1A01, In{.ax = 0, .bx = 0x0808});
  diverge(AX() == 0x1A01 && BX() == 0x0808,
          "divergence: AH=1Ah AL=01h (set display combination) is not "
          "implemented and returns without touching AL");

  //==========================================================================
  // 9. INT 13h - disk services against the RAM disks
  //==========================================================================
  section("INT 13h disk services");

  auto disk_matches = [&](uint32_t phys, const RamDisk &d, size_t off, size_t n) {
    for (size_t i = 0; i < n; i++)
      if (rb(phys + (uint32_t)i) != d.data[off + i]) return false;
    return true;
  };

  int13(0x0000, In{.ax = 0, .bx = 0, .cx = 0, .dx = 0x0000});
  check(!CF(), "AH=00 reset drive 0: CF clear");
  check_eq(AH(), 0, "AH=00 reset drive 0: AH=0");
  int13(0x0000, In{.ax = 0, .bx = 0, .cx = 0, .dx = 0x007F});
  diverge(!CF() && AH() == 0,
          "divergence: AH=00 reset reports success for a drive that does not "
          "exist - it never looks at DL");

  int13(0x0100, In{.ax = 0, .bx = 0, .cx = 0, .dx = 0x0000});
  check(!CF() && AH() == 0, "AH=01 get status: always 'last operation OK'");

  // Read CHS 0/0/1 of drive 0 -> LBA 0.
  io.reads = 0;
  for (int i = 0; i < 512; i++) wb(BUF_PHYS + (uint32_t)i, 0xEE);
  int13(0x0201, In{.ax = 0, .bx = 0x0000, .cx = 0x0001, .dx = 0x0000});
  check(!CF(), "AH=02 read 1 sector CHS 0/0/1: CF clear");
  check_eq(AH(), 0, "AH=02: AH=0 on success");
  check_eq(AL(), 1, "AH=02: AL = sectors transferred");
  check_eq((uint32_t)io.reads, 1, "AH=02: one dos_io::disk_read");
  check_eq((uint32_t)io.last_drive, 0, "AH=02: the read went to drive 0");
  check_eq((uint32_t)io.last_offset, 0, "AH=02 CHS 0/0/1 is LBA 0, byte offset 0");
  check(disk_matches(BUF_PHYS, io.fd0, 0, 512),
        "AH=02: the sector landed at ES:BX byte for byte");

  // CHS 0/1/3 on a 2-head 9-spt floppy is LBA (0*2+1)*9 + 2 = 11.
  io.reads = 0;
  int13(0x0202, In{.ax = 0, .bx = 0x0000, .cx = 0x0003, .dx = 0x0100});
  check(!CF() && AL() == 2, "AH=02 read 2 sectors CHS 0/1/3: AL=2, CF clear");
  check_eq((uint32_t)io.reads, 2, "AH=02: two disk_read calls for two sectors");
  check(disk_matches(BUF_PHYS, io.fd0, 11 * 512, 512),
        "AH=02 CHS 0/1/3 resolves to LBA 11");
  check(disk_matches(BUF_PHYS + 512, io.fd0, 12 * 512, 512),
        "AH=02: the second sector follows at ES:BX+512");

  // Write / read round trip on the last sector of drive 0 (CHS 39/1/9 = LBA 719).
  {
    for (int i = 0; i < 512; i++)
      wb(BUF_PHYS + 0x400 + (uint32_t)i, (uint8_t)(0xA5 ^ (i * 3)));
    io.writes = 0;
    int13(0x0301, In{.ax = 0, .bx = 0x0400, .cx = 0x2709, .dx = 0x0100});
    check(!CF(), "AH=03 write CHS 39/1/9: CF clear");
    check_eq(AH(), 0, "AH=03: AH=0 on success");
    check_eq(AL(), 1, "AH=03: AL = sectors written");
    check_eq((uint32_t)io.writes, 1, "AH=03: one dos_io::disk_write");
    check_eq((uint32_t)io.last_offset, 719 * 512, "AH=03 CHS 39/1/9 is LBA 719");
    bool stored = true;
    for (int i = 0; i < 512; i++)
      if (io.fd0.data[719 * 512 + i] != (uint8_t)(0xA5 ^ (i * 3))) stored = false;
    check(stored, "AH=03: the medium holds exactly the bytes at ES:BX");

    for (int i = 0; i < 512; i++) wb(BUF_PHYS + 0x600 + (uint32_t)i, 0);
    int13(0x0201, In{.ax = 0, .bx = 0x0600, .cx = 0x2709, .dx = 0x0100});
    bool same = true;
    for (int i = 0; i < 512; i++)
      if (rb(BUF_PHYS + 0x600 + (uint32_t)i) != rb(BUF_PHYS + 0x400 + (uint32_t)i))
        same = false;
    check(same, "AH=02/AH=03 round trip: what was written reads back identical");
  }

  // Error: a cylinder past the end of the medium.
  int13(0x0201, In{.ax = 0, .bx = 0x0000, .cx = 0x3C01, .dx = 0x0000});
  check(CF(), "AH=02 past the end of the medium: CF set");
  check_eq(AH(), 0x04, "AH=02 past the end: AH=04h (sector not found)");
  check_eq(AL(), 0, "AH=02 past the end: AL=0 sectors transferred");

  // Error: a multi-sector read that runs off the end part way through.
  int13(0x0202, In{.ax = 0, .bx = 0x0000, .cx = 0x2709, .dx = 0x0100});
  check(CF(), "AH=02 two sectors starting at the last: CF set");
  check_eq(AH(), 0x04, "AH=02 partial transfer: AH=04h");
  check_eq(AL(), 1, "AH=02 partial transfer: AL = the one sector that did move");

  // Error: a drive that is not attached.
  int13(0x0201, In{.ax = 0, .bx = 0x0000, .cx = 0x0001, .dx = 0x0005});
  check(CF(), "AH=02 on an unattached drive: CF set");
  check_eq(AH(), 0x01, "AH=02 on an unattached drive: AH=01h (invalid command)");
  check_eq(AL(), 0, "AH=02 on an unattached drive: AL=0");
  int13(0x0301, In{.ax = 0, .bx = 0x0000, .cx = 0x0001, .dx = 0x0005});
  check(CF() && AH() == 0x01, "AH=03 on an unattached drive: CF set, AH=01h");

  // CHS sector numbers are 1-based, and until 2026-08-27 nothing checked it:
  // `sector - 1` with sector 0 made the unsigned LBA expression evaluate to
  // 2^64-1, and the resulting byte offset (2^64-512) went to dos_io::disk_read
  // as-is.  A host backend that seeks with a signed off_t seeks backwards.
  //
  // The status code is 04h, "requested sector not found", not 01h.  This
  // assertion said 01h when it was written, anchored on the unattached-drive
  // path just above it, and 01h is the wrong half of the pair: RBIL gives 01h
  // for "bad command passed to driver" - an invalid AH or an malformed request -
  // and 04h for a CHS address the medium does not have.  Sector 0 is a legal
  // 6-bit encoding of a sector that does not exist on any track, which is 04h,
  // and it is the same answer the head and cylinder bounds below give for the
  // same reason.
  //
  // The status code is the lesser half of it.  What matters is that the host is
  // never ASKED: a short read from dos_io produces CF and AH=04h too, so an
  // assertion on the registers alone cannot tell "rejected" from "asked for
  // byte 2^64-512 and got nothing back".  io.reads is the assertion that can.
  io.reads = 0;
  int13(0x0201, In{.ax = 0, .bx = 0x0000, .cx = 0x0000, .dx = 0x0000});
  check(CF(), "AH=02 with sector number 0 fails");
  check(AH() == 0x04,
        "AH=02 rejects CHS sector 0 with 04h (sector not found) rather than "
        "underflowing the LBA to 2^64-1");
  check_eq((uint32_t)io.reads, 0,
           "AH=02 with sector 0 never reaches dos_io::disk_read at all");

  // The other two thirds of the CHS address are bounded for the same reason:
  // the geometry is the only thing that makes the byte offset sane.
  io.reads = 0;
  int13(0x0201, In{.ax = 0, .bx = 0x0000, .cx = 0x0001, .dx = 0x0200});  // head 2 of 2
  check(CF() && AH() == 0x04, "AH=02 rejects a head number past the geometry");
  check_eq((uint32_t)io.reads, 0, "AH=02 with a bad head never reaches the host");
  int13(0x0201, In{.ax = 0, .bx = 0x0000, .cx = 0x2801, .dx = 0x0000});  // cyl 40 of 40
  check(CF() && AH() == 0x04, "AH=02 rejects a cylinder past the geometry");
  int13(0x0201, In{.ax = 0, .bx = 0x0000, .cx = 0x000A, .dx = 0x0000});  // sector 10 of 9
  check(CF() && AH() == 0x04, "AH=02 rejects a sector past the track length");
  // ... and the last legal address on the medium still works.
  int13(0x0201, In{.ax = 0, .bx = 0x0000, .cx = 0x2709, .dx = 0x0100});  // c39 h1 s9
  check(!CF() && AH() == 0 && AL() == 1,
        "AH=02 still reads the last sector of the 360 KB floppy (c39 h1 s9)");

  // AL=0 is not a legal sector count.
  int13(0x0200, In{.ax = 0, .bx = 0x0000, .cx = 0x0001, .dx = 0x0000});
  diverge(!CF() && AH() == 0 && AL() == 0,
          "divergence: AH=02 with AL=0 reports success having moved nothing; "
          "the IBM BIOS returns 01h");

  // AH=04 verify never touches the medium.
  io.reads = 0;
  int13(0x0401, In{.ax = 0, .bx = 0x0000, .cx = 0x3C01, .dx = 0x0000});
  diverge(!CF() && AH() == 0 && io.reads == 0,
          "divergence: AH=04 verify succeeds without reading anything, so it "
          "'verifies' a sector past the end of the medium");

  // AH=08 drive parameters.
  int13(0x0800, In{.ax = 0, .bx = 0, .cx = 0, .dx = 0x0000});
  check(!CF(), "AH=08 drive 0: CF clear");
  check_eq(AH(), 0, "AH=08 drive 0: AH=0");
  check_eq(CH(), 39, "AH=08 drive 0 (360 KB): CH = 39 (max cylinder)");
  check_eq(CL(), 9, "AH=08 drive 0: CL = 9 sectors per track, cylinder bits 0");
  check_eq(DH(), 1, "AH=08 drive 0: DH = 1 (max head)");
  check_eq(DL(), 2, "AH=08 drive 0: DL = 2 floppy drives attached");
  check_eq(ES(), 0xF000, "AH=08: ES:DI points into the BIOS ROM");
  check_eq(DI(), 0xEFC7, "AH=08: DI = EFC7h, the disk parameter table");
  check_eq(rb(0xF0000 + 0xEFC7), 0xDF, "AH=08: the DPT is really there");
  diverge_eq(BL(), 0x04,
             "divergence: AH=08 reports drive type 04h (1.44 MB) for every "
             "floppy, including this 360 KB one (real: 01h)");

  int13(0x0800, In{.ax = 0, .bx = 0, .cx = 0, .dx = 0x0001});
  check_eq(CH(), 79, "AH=08 drive 1 (1.44 MB): CH = 79");
  check_eq(CL(), 18, "AH=08 drive 1: CL = 18 sectors per track");
  check_eq(DH(), 1, "AH=08 drive 1: DH = 1");

  int13(0x0800, In{.ax = 0, .bx = 0, .cx = 0, .dx = 0x0080});
  check(!CF() && AH() == 0, "AH=08 drive 80h: CF clear, AH=0");
  check_eq(CH(), 3, "AH=08 drive 80h: CH = 3 (4032 sectors / (16*63) = 4 cylinders)");
  check_eq(CL(), 63, "AH=08 drive 80h: CL = 63 sectors per track");
  check_eq(DH(), 15, "AH=08 drive 80h: DH = 15 (16 heads)");
  check_eq(DL(), 1, "AH=08 drive 80h: DL = 40:75, the hard-disk count");

  int13(0x0800, In{.ax = 0, .bx = 0, .cx = 0, .dx = 0x0081});
  check(CF(), "AH=08 on an unattached hard disk: CF set");
  check_eq(AH(), 0x07, "AH=08 on an unattached hard disk: AH=07h (drive not ready)");

  // AH=15 drive type.
  int13(0x1500, In{.ax = 0, .bx = 0, .cx = 0, .dx = 0x0000});
  check(!CF(), "AH=15 drive 0: CF clear");
  check_eq(AH(), 1, "AH=15 drive 0: AH=1 (floppy, no change-line)");
  int13(0x1500, In{.ax = 0, .bx = 0, .cx = 0, .dx = 0x0080});
  check(!CF(), "AH=15 drive 80h: CF clear");
  check_eq(AH(), 3, "AH=15 drive 80h: AH=3 (fixed disk)");
  check_eq(CX(), 0, "AH=15 drive 80h: CX = high word of the sector count");
  check_eq(DX(), 4032, "AH=15 drive 80h: DX = low word of the 4032-sector count");

  // AH=15h's "not present" answer IS AH=00h, and it is a SUCCESS return: the
  // interface says CF clear, AH=00h.  Setting CF makes a caller that branches
  // on CF first read a missing drive as an I/O error.
  int13(0x1500, In{.ax = 0, .bx = 0, .cx = 0, .dx = 0x0081});
  check_eq(AH(), 0, "AH=15 on an unattached drive: AH=00h (drive not present)");
  check(!CF(),
        "INT 13h AH=15h returns CF CLEAR with AH=00h for a drive that is not "
        "present - AH=00h is the documented success answer, not an error");

  // AH=41 extensions installation check.
  int13(0x4100, In{.ax = 0, .bx = 0x55AA, .cx = 0, .dx = 0x0080});
  check(!CF(), "AH=41 BX=55AAh: CF clear");
  check_eq(AH(), 0x21, "AH=41: AH=21h (extensions version 2.1)");
  check_eq(BX(), 0xAA55, "AH=41: BX=AA55h signature");
  check_eq(CX(), 0x0001, "AH=41: CX bit 0 (fixed disk access subset)");
  int13(0x4100, In{.ax = 0, .bx = 0x1234, .cx = 0, .dx = 0x0080});
  check(CF() && AH() == 0x01, "AH=41 with the wrong BX: CF set, AH=01h");

  // AH=42 extended read through a disk address packet at DS:SI.
  {
    uint32_t dap = DATA_PHYS;
    wb(dap + 0, 16); wb(dap + 1, 0);
    ww(dap + 2, 2);                        // sector count
    ww(dap + 4, 0x0800);                   // buffer offset
    ww(dap + 6, BUF_SEG);                  // buffer segment
    ww(dap + 8, 3); ww(dap + 10, 0);       // LBA 3
    ww(dap + 12, 0); ww(dap + 14, 0);
    io.reads = 0;
    int13(0x4200, In{.ax = 0, .bx = 0, .cx = 0, .dx = 0x0080, .si = 0x0000});
    check(!CF(), "AH=42 extended read: CF clear");
    check_eq(AH(), 0, "AH=42: AH=0");
    check_eq((uint32_t)io.reads, 2, "AH=42: two sectors read");
    check(disk_matches(BUF_PHYS + 0x800, io.hd0, 3 * 512, 1024),
          "AH=42: both sectors landed at the DAP's buffer address");
    check_eq(rw(dap + 2), 2, "AH=42: the DAP's count field is updated");

    // Extended write, then read back through the same path.
    for (int i = 0; i < 512; i++) wb(BUF_PHYS + 0xC00 + (uint32_t)i, (uint8_t)(i ^ 0x3C));
    ww(dap + 2, 1);
    ww(dap + 4, 0x0C00);
    ww(dap + 8, 100);
    int13(0x4300, In{.ax = 0, .bx = 0, .cx = 0, .dx = 0x0080, .si = 0x0000});
    check(!CF() && AH() == 0, "AH=43 extended write: CF clear, AH=0");
    bool ok = true;
    for (int i = 0; i < 512; i++)
      if (io.hd0.data[100 * 512 + i] != (uint8_t)(i ^ 0x3C)) ok = false;
    check(ok, "AH=43: LBA 100 on the medium holds the written bytes");

    // A DAP shorter than 16 bytes is rejected.
    wb(dap + 0, 8);
    int13(0x4200, In{.ax = 0, .bx = 0, .cx = 0, .dx = 0x0080, .si = 0x0000});
    check(CF() && AH() == 0x01, "AH=42 with a DAP size below 16: CF set, AH=01h");
    wb(dap + 0, 16);
  }

  // AH=48 extended drive parameters.
  {
    uint32_t buf = DATA_PHYS + 0x100;
    ww(buf + 0, 26);
    int13(0x4800, In{.ax = 0, .bx = 0, .cx = 0, .dx = 0x0080, .si = 0x0100});
    check(!CF() && AH() == 0, "AH=48: CF clear, AH=0");
    check_eq(rw(buf + 0), 26, "AH=48: result size = 26");
    check_eq(rw(buf + 2), 0x0002, "AH=48: information flags");
    check_eq(rd(buf + 16), 4032, "AH=48: total sector count, low dword");
    check_eq(rd(buf + 20), 0, "AH=48: total sector count, high dword");
    check_eq(rw(buf + 24), 512, "AH=48: bytes per sector");
    diverge(rw(buf + 4) == 0 && rw(buf + 8) == 0 && rw(buf + 12) == 0,
            "divergence: AH=48 reports 0 cylinders / heads / sectors, so a "
            "caller that wants CHS out of the extended call gets nothing");
    ww(buf + 0, 10);
    int13(0x4800, In{.ax = 0, .bx = 0, .cx = 0, .dx = 0x0080, .si = 0x0100});
    check(CF() && AH() == 0x01, "AH=48 with a buffer below 26 bytes: CF set, AH=01h");
  }

  int13(0x0C00, In{.ax = 0, .bx = 0, .cx = 0, .dx = 0x0080});
  check(CF(), "INT 13h with an unimplemented AH: CF set");
  check_eq(AH(), 0x01, "INT 13h with an unimplemented AH: AH=01h");

  //==========================================================================
  // 10. INT 14h serial / INT 17h printer
  //==========================================================================
  section("INT 14h serial / INT 17h printer");

  intN(0x14, In{.ax = 0x0083, .bx = 0, .cx = 0, .dx = 0x0000});
  diverge_eq(AH(), 0x80,
             "divergence: INT 14h AH=00 (init) returns AH=80h 'timeout' - there "
             "is no UART behind INT 14h at all");
  diverge_eq(AL(), 0x83,
             "divergence: INT 14h leaves AL untouched; AH=00 must return the "
             "modem status there");
  intN(0x14, In{.ax = 0x0355, .bx = 0, .cx = 0, .dx = 0x0000});
  diverge(AH() == 0x80 && AL() == 0x55,
          "divergence: INT 14h AH=03 (get status) gives the same AH=80h and "
          "ignores AL - every subfunction returns the same constant");
  check(!CF(), "INT 14h leaves CF alone (the interface has no CF)");

  intN(0x17, In{.ax = 0x0011, .bx = 0, .cx = 0, .dx = 0x0000});
  diverge_eq(AH(), 0x20,
             "divergence: INT 17h AH=00 returns AH=20h, which is the 'out of "
             "paper' bit, not the bit-0 timeout the source comment claims");
  diverge_eq(AL(), 0x11, "divergence: INT 17h leaves AL untouched");
  intN(0x17, In{.ax = 0x0200, .bx = 0, .cx = 0, .dx = 0x0000});
  diverge_eq(AH(), 0x20,
             "divergence: INT 17h AH=02 (get status) returns the same constant");

  //==========================================================================
  // 11. INT 15h system services
  //==========================================================================
  section("INT 15h system services");

  int15(0x8800);
  check(!CF(), "AH=88: CF clear");
  check_eq(AX(), 15360, "AH=88: 15360 KB above 1 MB in a 16 MB machine");
  check_eq(AX(), (uint16_t)((mem.get_mem_size() - 0x100000) / 1024),
           "AH=88: derived from emu88_mem::get_mem_size()");

  int15(0xC000);
  check(!CF(), "AH=C0: CF clear");
  check_eq(AH(), 0, "AH=C0: AH=0");
  check_eq(ES(), 0xF000, "AH=C0: ES = F000h");
  check_eq(BX(), 0xE6F5, "AH=C0: BX = E6F5h");
  check_eq(rw(0xF0000 + 0xE6F5), 8, "AH=C0: the table's length word = 8");
  check_eq(rb(0xF0000 + 0xE6F8), 0x00, "AH=C0: submodel byte");
  check_eq(rb(0xF0000 + 0xE6F9), 0x01, "AH=C0: BIOS revision byte");
  check_eq(rb(0xF0000 + 0xE6FA), 0x74, "AH=C0: feature byte 1");
  // Model FFh is the 1981 IBM PC, which has neither INT 15h AH=87 nor AH=88.
  // This BIOS answers both, and reports a 386 to INT 2Fh AX=1687h.
  diverge_eq(rb(0xF0000 + 0xE6F7), 0xFF,
             "divergence: the system config table reports model FFh (original "
             "IBM PC) on a machine that implements AH=87h and AH=88h");
  diverge_eq(rb(0xF0000 + 0xFFFE), 0xFF,
             "divergence: the ROM model byte at F000:FFFE is FFh too");

  {
    uint64_t before = m.cycles;
    int15(0x8600, In{.ax = 0, .bx = 0, .cx = 0x000F, .dx = 0x4240});
    check(!CF(), "AH=86 wait: CF clear");
    diverge(m.cycles - before < 200,
            "divergence: AH=86 returns immediately - a one-second wait costs "
            "fewer than 200 emulated cycles");
  }

  // AH=87 block move: a 6-entry GDT at ES:SI, source in entry 2, destination
  // in entry 3, CX words.
  {
    uint32_t gdt = BUF_PHYS;
    for (int i = 0; i < 48; i++) wb(gdt + (uint32_t)i, 0);
    for (int i = 0; i < 32; i++) wb(BUF_PHYS + 0x100 + (uint32_t)i, (uint8_t)(0x60 + i));
    for (int i = 0; i < 32; i++) wb(0x200000 + (uint32_t)i, 0);
    ww(gdt + 16 + 0, 0xFFFF);            // source limit
    ww(gdt + 16 + 2, 0x0100);            // source base 15:0
    wb(gdt + 16 + 4, 0x05);              // source base 23:16 -> 0x050100
    wb(gdt + 16 + 5, 0x93);
    ww(gdt + 24 + 0, 0xFFFF);            // dest limit
    ww(gdt + 24 + 2, 0x0000);            // dest base 15:0
    wb(gdt + 24 + 4, 0x20);              // dest base 23:16 -> 0x200000
    wb(gdt + 24 + 5, 0x93);
    int15(0x8700, In{.ax = 0, .bx = 0, .cx = 16, .dx = 0, .si = 0x0000});
    check(!CF(), "AH=87 block move: CF clear");
    check_eq(AH(), 0, "AH=87: AH=0");
    bool moved = true;
    for (int i = 0; i < 32; i++)
      if (rb(0x200000 + (uint32_t)i) != (uint8_t)(0x60 + i)) moved = false;
    check(moved, "AH=87: CX=16 words moved 32 bytes to the extended-memory base");
    check_eq(rb(0x200000 + 32), 0, "AH=87: it stopped after CX words");
    check(mem.get_a20(), "AH=87: A20 is restored to what it was");
  }

  int15(0x4F00);
  check(CF(), "AH=4F keyboard intercept: CF set means 'process the key normally'");
  int15(0x9100);
  check(!CF(), "AH=91 device post: CF clear");
  int15(0x4100);
  check(CF(), "AH=41 wait for external event: CF set (not supported)");
  int15(0x2400);
  diverge(CF() && AH() == 0x86,
          "divergence: INT 15h AX=2400h (A20 gate control) is unsupported even "
          "though the machine has an A20 gate the XMS driver drives");
  int15(0xDE00);
  check(CF(), "INT 15h with an unknown AH: CF set");
  check_eq(AH(), 0x86, "INT 15h with an unknown AH: AH=86h");

  //==========================================================================
  // 12. INT 16h keyboard
  //==========================================================================
  section("INT 16h keyboard");

  // Start from an empty ring and inject one key by hand, exactly as an INT 9h
  // handler would: ASCII at 0040:001E, scan code at 0040:001F, tail advanced.
  bda_set_w(bda::KBD_BUF_HEAD, 0x1E);
  bda_set_w(bda::KBD_BUF_TAIL, 0x1E);
  wb(0x400 + 0x1E, 'a');
  wb(0x400 + 0x1F, 0x1E);
  bda_set_w(bda::KBD_BUF_TAIL, 0x20);

  int16(0x0100);
  check(!ZF(), "AH=01 peek with a key waiting: ZF clear");
  check_eq(AL(), 'a', "AH=01: AL = the ASCII byte");
  check_eq(AH(), 0x1E, "AH=01: AH = the scan code");
  check_eq(bda_w(bda::KBD_BUF_HEAD), 0x1E, "AH=01 does NOT consume the key");
  check_eq(bda_w(bda::KBD_BUF_TAIL), 0x20, "AH=01 does not move the tail either");

  int16(0x1100);
  check(!ZF() && AX() == 0x1E61, "AH=11h (enhanced peek) behaves as AH=01");
  check_eq(bda_w(bda::KBD_BUF_HEAD), 0x1E, "AH=11h does not consume either");

  int16(0x0000);
  check_eq(AL(), 'a', "AH=00: AL = the ASCII byte");
  check_eq(AH(), 0x1E, "AH=00: AH = the scan code");
  check_eq(bda_w(bda::KBD_BUF_HEAD), 0x20, "AH=00 advances the head by two");

  int16(0x0100);
  check(ZF(), "AH=01 on an empty ring: ZF set");
  check_eq(bda_w(bda::KBD_BUF_HEAD), bda_w(bda::KBD_BUF_TAIL),
           "AH=01 on an empty ring leaves head == tail");

  // The ring wrap: a key in the last slot must take the head back to the start.
  bda_set_w(bda::KBD_BUF_HEAD, 0x3C);
  wb(0x400 + 0x3C, 0x0D);
  wb(0x400 + 0x3D, 0x1C);
  bda_set_w(bda::KBD_BUF_TAIL, 0x1E);
  int16(0x0000);
  check_eq(AX(), 0x1C0D, "AH=00 reads the key out of the last ring slot");
  check_eq(bda_w(bda::KBD_BUF_HEAD), 0x1E,
           "AH=00 wraps the head from 3Ch back to the 40:80 ring start");

  // Shift flags.
  bda_set_b(bda::KBD_FLAGS1, 0x53);
  bda_set_b(bda::KBD_FLAGS2, 0x21);
  int16(0x0200);
  check_eq(AL(), 0x53, "AH=02: AL = 40:17 shift flags");
  int16(0x1200);
  check_eq(AL(), 0x53, "AH=12h: AL = 40:17");
  check_eq(AH(), 0x21, "AH=12h: AH = 40:18 extended shift flags");
  bda_set_b(bda::KBD_FLAGS1, 0);
  bda_set_b(bda::KBD_FLAGS2, 0);

  int16(0x0900);
  check_eq(AL(), 0x20, "AH=09: AL=20h (enhanced keyboard functions supported)");
  int16(0x0305, In{.ax = 0, .bx = 0x0102});
  check(true, "AH=03 (set typematic rate) returns without faulting");

  // AH=05 store key, and the ring's real capacity: 16 slots hold 15 keys.
  bda_set_w(bda::KBD_BUF_HEAD, 0x1E);
  bda_set_w(bda::KBD_BUF_TAIL, 0x1E);
  int16(0x0500, In{.ax = 0, .bx = 0, .cx = 0x3C78});     // CH=scan 3Ch, CL='x'
  check_eq(AL(), 0, "AH=05: AL=0 means the key was stored");
  check_eq(rb(0x400 + 0x1E), 'x', "AH=05 stores the ASCII byte first");
  check_eq(rb(0x400 + 0x1F), 0x3C, "AH=05 stores the scan code second");
  check_eq(bda_w(bda::KBD_BUF_TAIL), 0x20, "AH=05 advances the tail by two");
  for (int i = 1; i < 16; i++)
    int16(0x0500, In{.ax = 0, .bx = 0, .cx = (uint16_t)(0x3C00 | (0x40 + i))});
  check_eq(bda_w(bda::KBD_BUF_TAIL), 0x3C,
           "AH=05: the 16-slot ring stops at 15 keys, one slot always free");
  diverge_eq(AL(), 0,
             "divergence: AH=05 returns AL=0 (success) even when the ring was "
             "full and the key was dropped; the IBM BIOS returns AL=1");

  // Drain it, then prove AH=00 on an empty ring BLOCKS: dos_machine rewinds IP
  // over the INT so the same instruction runs again on the next batch.
  for (int i = 0; i < 15; i++) int16(0x0000);
  check_eq(bda_w(bda::KBD_BUF_HEAD), bda_w(bda::KBD_BUF_TAIL),
           "15 AH=00 reads drain exactly the 15 stored keys");
  intN(0x16, In{.ax = 0x0000}, /*expect_halt=*/false);
  check(m.is_waiting_for_key(),
        "AH=00 on an empty ring raises the waiting-for-key state");
  check_eq(m.ip, OFF_INT,
           "AH=00 on an empty ring rewinds IP onto the INT 16h instruction");
  // run_batch(0) clears waiting_for_key without executing anything; leaving it
  // set would make every later INT rewind its own IP forever.
  m.run_batch(0);
  check(!m.is_waiting_for_key(), "the blocking state is cleared before continuing");

  //==========================================================================
  // 13. INT 1Ah time and date
  //==========================================================================
  section("INT 1Ah time / date");

  int1a(0x0100, In{.ax = 0, .bx = 0, .cx = 0x0012, .dx = 0x3456});
  check_eq(bda_d(bda::TIMER_COUNT), 0x00123456u, "AH=01: 40:6C = CX:DX");
  int1a(0x0000);
  check_eq(CX(), 0x0012, "AH=00: CX = the high word of the tick count");
  check_eq(DX(), 0x3456, "AH=00: DX = the low word");
  check_eq(AL(), 0, "AH=00: AL = 0, midnight has not passed");

  bda_set_b(bda::TIMER_ROLLOVER, 1);
  int1a(0x0000);
  check_eq(AL(), 1, "AH=00: AL = the 40:70 midnight flag");
  check_eq(bda_b(bda::TIMER_ROLLOVER), 0, "AH=00 clears 40:70 after reporting it");
  int1a(0x0000);
  check_eq(AL(), 0, "AH=00: a second read reports 0");

  // Setting the tick count is what a program does after computing midnight; the
  // AT BIOS clears the midnight flag as part of it.  This one leaves it set, so
  // the very next AH=00 reports a rollover that never happened.
  bda_set_b(bda::TIMER_ROLLOVER, 1);
  int1a(0x0100, In{.ax = 0, .bx = 0, .cx = 0x0000, .dx = 0x0100});
  int1a(0x0000);
  check(AL() == 0,
        "INT 1Ah AH=01 clears the 40:70 midnight-passed flag when it sets the "
        "tick count");
  check_eq(DX(), 0x0100, "AH=01 followed by AH=00 round-trips the count");

  io.t_h = 23; io.t_m = 59; io.t_s = 58; io.t_hs = 99;
  int1a(0x0200);
  check(!CF(), "AH=02: CF clear");
  check_eq(CH(), 0x23, "AH=02: CH = 23 hours in BCD");
  check_eq(CL(), 0x59, "AH=02: CL = 59 minutes in BCD");
  check_eq(DH(), 0x58, "AH=02: DH = 58 seconds in BCD");
  check_eq(DL(), 0, "AH=02: DL = 0 (no daylight saving)");

  // to_bcd packs tens in the high nibble: 7 -> 07h, not 07h-by-accident.
  io.t_h = 10; io.t_m = 7; io.t_s = 3;
  int1a(0x0200);
  check_eq(CH(), 0x10, "AH=02: to_bcd(10) = 10h");
  check_eq(CL(), 0x07, "AH=02: to_bcd(7) = 07h - the tens nibble is zero");
  check_eq(DH(), 0x03, "AH=02: to_bcd(3) = 03h");
  io.t_h = 12; io.t_m = 34; io.t_s = 56;
  int1a(0x0200);
  check_eq(CX(), 0x1234, "AH=02: 12:34 packs as 1234h");
  check_eq(DH(), 0x56, "AH=02: 56 seconds packs as 56h");

  io.d_y = 2026; io.d_mo = 8; io.d_d = 27;
  int1a(0x0400);
  check(!CF(), "AH=04: CF clear");
  check_eq(CH(), 0x20, "AH=04: CH = century 20 in BCD");
  check_eq(CL(), 0x26, "AH=04: CL = year 26 in BCD");
  check_eq(DH(), 0x08, "AH=04: DH = month 08 in BCD");
  check_eq(DL(), 0x27, "AH=04: DL = day 27 in BCD");
  io.d_y = 1999; io.d_mo = 12; io.d_d = 31;
  int1a(0x0400);
  check_eq(CX(), 0x1999, "AH=04: 1999 packs as 1999h across CH:CL");
  check_eq(DX(), 0x1231, "AH=04: 12/31 packs as 1231h");

  // Setting the clock is accepted and discarded.
  io.t_h = 12; io.t_m = 34; io.t_s = 56;
  int1a(0x0300, In{.ax = 0, .bx = 0, .cx = 0x0101, .dx = 0x0100});
  check(!CF(), "AH=03 set RTC time: CF clear");
  int1a(0x0200);
  diverge(CX() == 0x1234,
          "divergence: AH=03 (set RTC time) is discarded - the next AH=02 still "
          "reports whatever dos_io::get_time says");
  int1a(0x0500, In{.ax = 0, .bx = 0, .cx = 0x2000, .dx = 0x0101});
  check(!CF(), "AH=05 set RTC date: CF clear");
  int1a(0x0400);
  diverge(DX() == 0x1231,
          "divergence: AH=05 (set RTC date) is discarded the same way");

  int1a(0x0600);
  check(CF(), "INT 1Ah with an unimplemented AH: CF set");

  //==========================================================================
  // 14. INT 2Fh multiplex and the XMS driver behind it
  //==========================================================================
  section("INT 2Fh multiplex / XMS");

  int2f(0x4300);
  check_eq(AL(), 0x80, "INT 2Fh AX=4300h: AL=80h, an XMS driver is installed");
  int2f(0x4310);
  check_eq(ES(), 0xF000, "INT 2Fh AX=4310h: ES = F000h");
  check_eq(BX(), 0xEFD8, "INT 2Fh AX=4310h: BX = EFD8h");
  check_eq(rb(0xF0000 + 0xEFD8), 0xF1, "the XMS entry point is a BIOS trap stub");
  check_eq(rb(0xF0000 + 0xEFD9), 0xFE, "the XMS entry point carries marker FEh");
  check_eq(rb(0xF0000 + 0xEFDA), 0xCB, "the XMS entry point ends in RETF");

  int2f(0x1234);
  diverge(AX() == 0x1234,
          "divergence: INT 2Fh leaves an unrecognised AX untouched rather than "
          "chaining to a previous handler");

  // Everything below is a FAR CALL to F000:EFD8, the way a real XMS client
  // calls the driver.
  xms(0x00);
  check_eq(AX(), 0x0300, "XMS AH=00: AX = 0300h (XMS version 3.00)");
  check_eq(BX(), 0x0100, "XMS AH=00: BX = 0100h (driver version)");
  check_eq(DX(), 1, "XMS AH=00: DX = 1, an HMA exists");

  xms(0x08);
  check_eq(AX(), 15296, "XMS AH=08: 15360 KB less the 64 KB HMA");
  check_eq(DX(), 15296, "XMS AH=08: DX = the same total");
  check_eq(BX(), 0, "XMS AH=08: BX = 0, no error");

  xms(0x09, In{.ax = 0, .bx = 0, .cx = 0, .dx = 64});
  check_eq(AX(), 1, "XMS AH=09: AX=1 on success");
  check_eq(DX(), 1, "XMS AH=09: the first handle is 1");
  uint16_t h1 = DX();

  xms(0x08);
  check_eq(AX(), 15296 - 64, "XMS AH=08: the 64 KB allocation is subtracted");

  xms(0x0E, In{.ax = 0, .bx = 0, .cx = 0, .dx = h1});
  check_eq(AX(), 1, "XMS AH=0Eh: AX=1");
  check_eq(BH(), 0, "XMS AH=0Eh: BH = lock count 0");
  check_eq(BL(), 31, "XMS AH=0Eh: BL = 31 free handles of the 32");
  check_eq(DX(), 64, "XMS AH=0Eh: DX = the block size in KB");

  xms(0x0C, In{.ax = 0, .bx = 0, .cx = 0, .dx = h1});
  check_eq(AX(), 1, "XMS AH=0Ch lock: AX=1");
  check_eq(DX(), 0x0011, "XMS AH=0Ch: DX:BX = 00110000h, just above the HMA");
  check_eq(BX(), 0x0000, "XMS AH=0Ch: the low half of the 32-bit address");
  xms(0x0E, In{.ax = 0, .bx = 0, .cx = 0, .dx = h1});
  check_eq(BH(), 1, "XMS AH=0Eh: the lock count went to 1");
  xms(0x0D, In{.ax = 0, .bx = 0, .cx = 0, .dx = h1});
  check_eq(AX(), 1, "XMS AH=0Dh unlock: AX=1");
  xms(0x0E, In{.ax = 0, .bx = 0, .cx = 0, .dx = h1});
  check_eq(BH(), 0, "XMS AH=0Eh: the lock count went back to 0");

  // AH=0Bh move: conventional -> EMB, then EMB -> conventional.
  {
    uint32_t mv = DATA_PHYS + 0x200;
    for (int i = 0; i < 64; i++) wb(BUF_PHYS + 0x300 + (uint32_t)i, (uint8_t)(0x11 * i + 7));
    ww(mv + 0, 64); ww(mv + 2, 0);                   // length 64
    ww(mv + 4, 0);                                   // source handle 0 = real mode
    ww(mv + 6, 0x0300); ww(mv + 8, BUF_SEG);         // source 5000:0300
    ww(mv + 10, h1);                                 // destination handle
    ww(mv + 12, 0); ww(mv + 14, 0);                  // destination offset 0
    xms(0x0B, In{.ax = 0, .bx = 0, .cx = 0, .dx = 0, .si = 0x0200});
    check_eq(AX(), 1, "XMS AH=0Bh: AX=1 on a good move");
    bool moved = true;
    for (int i = 0; i < 64; i++)
      if (rb(0x110000 + (uint32_t)i) != (uint8_t)(0x11 * i + 7)) moved = false;
    check(moved, "XMS AH=0Bh: 64 bytes reached the EMB at 00110000h");

    for (int i = 0; i < 64; i++) wb(BUF_PHYS + 0x380 + (uint32_t)i, 0);
    ww(mv + 4, h1);                                  // source handle
    ww(mv + 6, 0); ww(mv + 8, 0);                    // source offset 0
    ww(mv + 10, 0);                                  // destination real mode
    ww(mv + 12, 0x0380); ww(mv + 14, BUF_SEG);
    xms(0x0B, In{.ax = 0, .bx = 0, .cx = 0, .dx = 0, .si = 0x0200});
    check_eq(AX(), 1, "XMS AH=0Bh: AX=1 moving back out of the EMB");
    bool back = true;
    for (int i = 0; i < 64; i++)
      if (rb(BUF_PHYS + 0x380 + (uint32_t)i) != (uint8_t)(0x11 * i + 7)) back = false;
    check(back, "XMS AH=0Bh: the round trip through extended memory is byte exact");

    // An invalid source handle is rejected.
    ww(mv + 4, 0x0FF0);
    xms(0x0B, In{.ax = 0, .bx = 0, .cx = 0, .dx = 0, .si = 0x0200});
    check_eq(AX(), 0, "XMS AH=0Bh with a bad source handle: AX=0");
    check_eq(BL(), 0xA3, "XMS AH=0Bh with a bad source handle: BL=A3h");
  }

  // A move longer than the destination block.  The driver checks the handles
  // and nothing else, so it copies straight past the end of the 1 KB EMB and
  // reports success.  In a real session the bytes past the end belong to
  // another handle or to nothing at all.
  {
    xms(0x09, In{.ax = 0, .bx = 0, .cx = 0, .dx = 1});     // 1 KB block
    check_eq(AX(), 1, "XMS AH=09: a second, 1 KB allocation succeeds");
    uint16_t h2 = DX();
    check_eq(h2, 2, "XMS AH=09: the second handle is 2");
    uint32_t h2_base = 0x110000 + 64 * 1024;
    wb(h2_base + 1024, 0xEE);                              // sentinel past the end
    uint32_t mv = DATA_PHYS + 0x200;
    for (int i = 0; i < 2048; i++) wb(BUF_PHYS + 0x1000 + (uint32_t)i, 0x5A);
    ww(mv + 0, 2048); ww(mv + 2, 0);
    ww(mv + 4, 0);
    ww(mv + 6, 0x1000); ww(mv + 8, BUF_SEG);
    ww(mv + 10, h2);
    ww(mv + 12, 0); ww(mv + 14, 0);
    xms(0x0B, In{.ax = 0, .bx = 0, .cx = 0, .dx = 0, .si = 0x0200});
    check(AX() == 0,
          "XMS AH=0Bh rejects a move whose length runs past the end of the "
          "destination block, rather than reporting success");
    check_eq(BX(), 0xA8, "XMS AH=0Bh: BL=A8h (invalid destination offset)");
    check(rb(h2_base + 1024) == 0xEE,
          "XMS AH=0Bh does not write past the end of the destination block - "
          "the sentinel one byte past a 1 KB EMB survives");
    // The source side is bounded the same way, and an odd length is A9h.
    ww(mv + 0, 2048); ww(mv + 2, 0);
    ww(mv + 4, h2); ww(mv + 6, 0); ww(mv + 8, 0);
    ww(mv + 10, 0); ww(mv + 12, 0x1000); ww(mv + 14, BUF_SEG);
    xms(0x0B, In{.ax = 0, .bx = 0, .cx = 0, .dx = 0, .si = 0x0200});
    check(AX() == 0, "XMS AH=0Bh rejects a move that reads past the source block");
    check_eq(BX(), 0xA7, "XMS AH=0Bh: BL=A7h (invalid source offset)");
    ww(mv + 0, 3); ww(mv + 2, 0);
    ww(mv + 4, 0); ww(mv + 6, 0x1000); ww(mv + 8, BUF_SEG);
    ww(mv + 10, h2); ww(mv + 12, 0); ww(mv + 14, 0);
    xms(0x0B, In{.ax = 0, .bx = 0, .cx = 0, .dx = 0, .si = 0x0200});
    check(AX() == 0, "XMS AH=0Bh rejects an odd length");
    check_eq(BX(), 0xA9, "XMS AH=0Bh: BL=A9h (length is not even)");

    // The bound is exact, both sides of it.  A move of the whole block has to
    // succeed and a move of two bytes more has to fail: an off-by-one here is
    // one byte written past the end of somebody else's allocation, which is
    // precisely the defect this section was written against, and a test that
    // only ever asks for twice the block size cannot see it.
    wb(h2_base + 1024, 0xEE);                              // re-arm the sentinel
    ww(mv + 0, 1024); ww(mv + 2, 0);
    ww(mv + 4, 0); ww(mv + 6, 0x1000); ww(mv + 8, BUF_SEG);
    ww(mv + 10, h2); ww(mv + 12, 0); ww(mv + 14, 0);
    xms(0x0B, In{.ax = 0, .bx = 0, .cx = 0, .dx = 0, .si = 0x0200});
    check_eq(AX(), 1, "XMS AH=0Bh: a move of exactly the block size succeeds");
    check(rb(h2_base + 1023) == 0x5A, "XMS AH=0Bh: it wrote the last byte of the block");
    check(rb(h2_base + 1024) == 0xEE, "XMS AH=0Bh: and not the byte after it");
    ww(mv + 0, 1026); ww(mv + 2, 0);
    xms(0x0B, In{.ax = 0, .bx = 0, .cx = 0, .dx = 0, .si = 0x0200});
    check(AX() == 0, "XMS AH=0Bh: two bytes more than the block fails");
    check_eq(BX(), 0xA8, "XMS AH=0Bh: BL=A8h at the boundary too");
    // An ODD destination offset with an even length lands the end of the move
    // exactly one byte past the block, which is the single value that separates
    // `> block' from `> block + 1'.  Without this the off-by-one is invisible,
    // because the length is required to be even and every other offset here is.
    wb(h2_base + 1024, 0xEE);
    ww(mv + 0, 1024); ww(mv + 2, 0);
    ww(mv + 10, h2); ww(mv + 12, 1); ww(mv + 14, 0);
    xms(0x0B, In{.ax = 0, .bx = 0, .cx = 0, .dx = 0, .si = 0x0200});
    check(AX() == 0 && BX() == 0xA8,
          "XMS AH=0Bh: 1024 bytes at destination offset 1 is one byte too many");
    check(rb(h2_base + 1024) == 0xEE,
          "XMS AH=0Bh: and the byte one past the block is still the sentinel");
    ww(mv + 0, 1022); ww(mv + 2, 0);
    xms(0x0B, In{.ax = 0, .bx = 0, .cx = 0, .dx = 0, .si = 0x0200});
    check_eq(AX(), 1, "XMS AH=0Bh: 1022 bytes at offset 1 fits exactly");
    // And the same exactness on the source side.
    ww(mv + 0, 1024); ww(mv + 2, 0);
    ww(mv + 4, h2); ww(mv + 6, 0); ww(mv + 8, 0);
    ww(mv + 10, 0); ww(mv + 12, 0x1000); ww(mv + 14, BUF_SEG);
    xms(0x0B, In{.ax = 0, .bx = 0, .cx = 0, .dx = 0, .si = 0x0200});
    check_eq(AX(), 1, "XMS AH=0Bh: reading exactly the block size succeeds");
    ww(mv + 0, 1026); ww(mv + 2, 0);
    xms(0x0B, In{.ax = 0, .bx = 0, .cx = 0, .dx = 0, .si = 0x0200});
    check(AX() == 0 && BX() == 0xA7,
          "XMS AH=0Bh: two bytes more than the source block fails with A7h");
    // The source bound is the offset plus the length, not the length alone.
    ww(mv + 0, 1024); ww(mv + 2, 0);
    ww(mv + 4, h2); ww(mv + 6, 2); ww(mv + 8, 0);
    xms(0x0B, In{.ax = 0, .bx = 0, .cx = 0, .dx = 0, .si = 0x0200});
    check(AX() == 0 && BX() == 0xA7,
          "XMS AH=0Bh: a full-block read from source offset 2 overruns");
    ww(mv + 0, 1022); ww(mv + 2, 0);
    xms(0x0B, In{.ax = 0, .bx = 0, .cx = 0, .dx = 0, .si = 0x0200});
    check_eq(AX(), 1, "XMS AH=0Bh: 1022 bytes from source offset 2 fits");
    // A non-zero offset moves the bound with it.
    ww(mv + 0, 512); ww(mv + 2, 0);
    ww(mv + 4, 0); ww(mv + 6, 0x1000); ww(mv + 8, BUF_SEG);
    ww(mv + 10, h2); ww(mv + 12, 512); ww(mv + 14, 0);
    xms(0x0B, In{.ax = 0, .bx = 0, .cx = 0, .dx = 0, .si = 0x0200});
    check_eq(AX(), 1, "XMS AH=0Bh: 512 bytes at offset 512 of a 1 KB block fits");
    ww(mv + 0, 514); ww(mv + 2, 0);
    xms(0x0B, In{.ax = 0, .bx = 0, .cx = 0, .dx = 0, .si = 0x0200});
    check(AX() == 0 && BX() == 0xA8,
          "XMS AH=0Bh: 514 bytes at offset 512 does not, and it is the offset "
          "that decides");
    xms(0x0A, In{.ax = 0, .bx = 0, .cx = 0, .dx = h2});
    check_eq(AX(), 1, "XMS AH=0Ah: the 1 KB block frees");
  }

  // Reallocation, freeing, and the errors.
  xms(0x0F, In{.ax = 0, .bx = 128, .cx = 0, .dx = h1});
  check_eq(AX(), 1, "XMS AH=0Fh realloc to 128 KB: AX=1");
  xms(0x0E, In{.ax = 0, .bx = 0, .cx = 0, .dx = h1});
  check_eq(DX(), 128, "XMS AH=0Fh: the handle reports its new size");

  xms(0x0A, In{.ax = 0, .bx = 0, .cx = 0, .dx = h1});
  check_eq(AX(), 1, "XMS AH=0Ah free: AX=1");
  xms(0x08);
  check_eq(AX(), 15296, "XMS AH=08: freeing gives the memory back");
  xms(0x0A, In{.ax = 0, .bx = 0, .cx = 0, .dx = h1});
  check_eq(AX(), 0, "XMS AH=0Ah on an already-free handle: AX=0");
  check_eq(BL(), 0xA2, "XMS AH=0Ah on an already-free handle: BL=A2h");
  xms(0x0C, In{.ax = 0, .bx = 0, .cx = 0, .dx = 99});
  check_eq(AX(), 0, "XMS AH=0Ch on an out-of-range handle: AX=0");
  check_eq(BL(), 0xA2, "XMS AH=0Ch on an out-of-range handle: BL=A2h");

  xms(0x09, In{.ax = 0, .bx = 0, .cx = 0, .dx = 60000});
  check_eq(AX(), 0, "XMS AH=09 for more memory than exists: AX=0");
  check_eq(BL(), 0xA0, "XMS AH=09 out of memory: BL=A0h");

  // A freed hole is never reused: the next base is above every live block.
  {
    xms(0x09, In{.ax = 0, .bx = 0, .cx = 0, .dx = 16});
    uint16_t a = DX();
    xms(0x09, In{.ax = 0, .bx = 0, .cx = 0, .dx = 16});
    uint16_t b = DX();
    xms(0x0A, In{.ax = 0, .bx = 0, .cx = 0, .dx = a});
    xms(0x09, In{.ax = 0, .bx = 0, .cx = 0, .dx = 16});
    uint16_t c = DX();
    xms(0x0C, In{.ax = 0, .bx = 0, .cx = 0, .dx = c});
    uint32_t c_base = ((uint32_t)DX() << 16) | BX();
    diverge_eq(c_base, 0x110000 + 32 * 1024,
               "divergence: a freed block's address range is never reused - the "
               "allocator only ever moves the high-water mark up");
    xms(0x0D, In{.ax = 0, .bx = 0, .cx = 0, .dx = c});
    xms(0x0A, In{.ax = 0, .bx = 0, .cx = 0, .dx = b});
    xms(0x0A, In{.ax = 0, .bx = 0, .cx = 0, .dx = c});
  }

  // A20 control.
  xms(0x04);
  check_eq(AX(), 1, "XMS AH=04 global disable A20: AX=1");
  check(!mem.get_a20(), "XMS AH=04 really turns the A20 gate off");
  xms(0x07);
  check_eq(AX(), 0, "XMS AH=07 query: AX=0 while A20 is off");
  xms(0x03);
  check_eq(AX(), 1, "XMS AH=03 global enable A20: AX=1");
  check(mem.get_a20(), "XMS AH=03 really turns the A20 gate on");
  xms(0x07);
  check_eq(AX(), 1, "XMS AH=07 query: AX=1 while A20 is on");
  xms(0x05);
  check(mem.get_a20(), "XMS AH=05 local enable A20 leaves it on");

  xms(0x01);
  check_eq(AX(), 1, "XMS AH=01 request HMA: AX=1");
  check(mem.get_a20(), "XMS AH=01 forces A20 on so the HMA is addressable");
  xms(0x02);
  check_eq(AX(), 1, "XMS AH=02 release HMA: AX=1");

  xms(0x10, In{.ax = 0, .bx = 0, .cx = 0, .dx = 0x1000});
  check_eq(AX(), 0, "XMS AH=10h request UMB: AX=0");
  check_eq(BL(), 0xB1, "XMS AH=10h: BL=B1h, no UMBs available");
  xms(0x11, In{.ax = 0, .bx = 0, .cx = 0, .dx = 0x1000});
  check_eq(AX(), 0, "XMS AH=11h release UMB: AX=0");
  check_eq(BL(), 0xB2, "XMS AH=11h: BL=B2h");

  xms(0x88);
  check_eq(gm->get_reg32(E::reg_AX), 15296u,
           "XMS AH=88h (32-bit query): EAX = the free KB");
  check_eq(gm->get_reg32(E::reg_DX), 15296u, "XMS AH=88h: EDX = the same total");
  check_eq(gm->get_reg32(E::reg_CX), 0x00110000u,
           "XMS AH=88h: ECX = the highest ending address");

  xms(0x7F);
  check_eq(AX(), 0, "XMS with an unimplemented function: AX=0");
  check_eq(BX(), 0x80, "XMS with an unimplemented function: BX=80h");

  //==========================================================================
  // 15. INT 08h timer tick and its INT 1Ch chain
  //==========================================================================
  section("INT 08h timer tick");

  // A hand-assembled real-mode INT 1Ch handler:
  //   mov byte [cs:0020h], 5Ah / iret
  {
    const uint8_t hook[] = { 0x2E, 0xC6, 0x06, 0x20, 0x00, 0x5A, 0xCF };
    for (size_t i = 0; i < sizeof(hook); i++)
      wb(RM_HOOK_PHYS + (uint32_t)i, hook[i]);
    wb(RM_HOOK_PHYS + 0x20, 0x00);
    ww(0x1C * 4, 0x0000);
    ww(0x1C * 4 + 2, RM_HOOK_SEG);

    uint32_t ticks_before = bda_d(bda::TIMER_COUNT);
    intN(0x08);
    check_eq(rb(RM_HOOK_PHYS + 0x20), 0x5A,
             "INT 08h chains to the INT 1Ch user hook installed in the IVT");
    check(gm->halted, "INT 08h returns to the caller through the hook's IRET");
    diverge_eq(bda_d(bda::TIMER_COUNT), ticks_before,
               "divergence: INT 08h does not advance 40:6C - the tick counter is "
               "incremented by dos_machine::run_batch instead, so a guest that "
               "invokes INT 08h itself never moves the BIOS clock");

    // Put the BIOS stub back in the vector and check the default 1Ch is inert.
    ww(0x1C * 4, (uint16_t)(0xE000 + 0x1C * 4));
    ww(0x1C * 4 + 2, 0xF000);
    wb(RM_HOOK_PHYS + 0x20, 0x00);
    intN(0x08);
    check_eq(rb(RM_HOOK_PHYS + 0x20), 0x00,
             "with the vector restored, INT 08h reaches the BIOS's own no-op 1Ch");
    intN(0x1C);
    check(gm->halted, "INT 1Ch by itself is a no-op that returns");
  }

  // A real BIOS INT 08h ends with an end-of-interrupt to the master PIC.
  diverge(true,
          "divergence: bios_int08h writes no EOI to port 20h, so a guest that "
          "hooks IRQ0 and chains to the BIOS handler never gets one");

  //==========================================================================
  // 16. INT 19h bootstrap
  //==========================================================================
  section("INT 19h bootstrap");

  {
    // Drive 0 gets a bootable sector whose first instruction is HLT.
    io.fd0.data[0] = 0xF4;
    io.fd0.data[1] = 0x90;
    io.fd0.data[510] = 0x55;
    io.fd0.data[511] = 0xAA;
    for (int i = 0; i < 512; i++) wb(BOOT_LOAD_ADDR + (uint32_t)i, 0);
    intN(0x19);
    check_eq(gm->sregs[E::seg_CS], 0x0000, "INT 19h: CS = 0000h");
    check(gm->halted, "INT 19h: the loaded boot sector executed (its HLT ran)");
    check_eq(DX(), 0x0000, "INT 19h: DL = 0, the drive it booted from");
    bool copied = true;
    for (int i = 0; i < 512; i++)
      if (rb(BOOT_LOAD_ADDR + (uint32_t)i) != io.fd0.data[i]) copied = false;
    check(copied, "INT 19h copies all 512 bytes to 0000:7C00");

    // No signature on drive 0 -> fall through to 80h.
    io.fd0.data[510] = 0x00;
    io.hd0.data[0] = 0xF4;
    io.hd0.data[510] = 0x55;
    io.hd0.data[511] = 0xAA;
    for (int i = 0; i < 512; i++) wb(BOOT_LOAD_ADDR + (uint32_t)i, 0);
    intN(0x19);
    check_eq(DX(), 0x0080, "INT 19h skips the unsignatured floppy and boots 80h");
    check_eq(rb(BOOT_LOAD_ADDR), 0xF4, "INT 19h loaded the hard disk's sector");

    // Nothing bootable at all -> the machine stops.
    io.hd0.data[510] = 0x00;
    for (int i = 0; i < 512; i++) wb(BOOT_LOAD_ADDR + (uint32_t)i, 0xCC);
    intN(0x19);
    check(gm->halted, "INT 19h with no bootable medium halts the machine");
    check_eq(gm->sregs[E::seg_CS], CODE_SEG,
             "INT 19h with no bootable medium does not transfer control");
    check_eq(rb(BOOT_LOAD_ADDR), 0xCC, "INT 19h loaded nothing at 0000:7C00");
    diverge(gm->halted,
            "divergence: a real BIOS falls into ROM BASIC or prints a boot "
            "failure and retries; this one halts the CPU");

    // Restore the images so nothing after this depends on the order of tests.
    io.fd0.data[0] = (uint8_t)(0 * 5 + 0x10);
    io.fd0.data[1] = (uint8_t)(1 * 5 + 0x10);
    io.fd0.data[510] = (uint8_t)(510 * 5 + 0x10);
    io.fd0.data[511] = (uint8_t)(511 * 5 + 0x10);
  }

  //==========================================================================
  // 17. The other display configurations
  //
  // Config::display steers init_bda's equipment bits, the initial video mode,
  // the CRTC port and INT 10h AH=1Ah's display combination code.  Only
  // DISPLAY_CGA is reachable without re-initialising, so the four other
  // adapters are done last, each with a fresh init_machine().
  //==========================================================================
  section("display adapter configurations");

  {
    m.set_display(dos_machine::DISPLAY_MDA);
    m.init_machine();
    mem.set_a20(true);
    check_eq(bda_w(bda::EQUIPMENT), 0x0035,
             "DISPLAY_MDA: equipment bits 4-5 = 11 (monochrome)");
    check_eq(bda_b(bda::VIDEO_MODE), 7, "DISPLAY_MDA: the initial mode is 7");
    check_eq(bda_w(bda::CRTC_BASE), 0x3B4, "DISPLAY_MDA: 40:63 = 3B4h");
    int10(0x1A00);
    check_eq(BL(), 0x01, "DISPLAY_MDA: AH=1Ah reports DCC 01h (MDA)");
    check_eq(BH(), 0x00, "DISPLAY_MDA: no secondary adapter");

    m.set_display(dos_machine::DISPLAY_HERCULES);
    m.init_machine();
    check_eq(bda_w(bda::EQUIPMENT), 0x0035,
             "DISPLAY_HERCULES: the same monochrome equipment bits");
    int10(0x1A00);
    diverge_eq(BL(), 0x01,
               "divergence: Hercules reports DCC 01h (MDA) - there is no DCC "
               "code for Hercules, so a program cannot detect the graphics page");

    m.set_display(dos_machine::DISPLAY_EGA);
    m.init_machine();
    check_eq(bda_w(bda::EQUIPMENT), 0x0005,
             "DISPLAY_EGA: equipment bits 4-5 = 00 (EGA/VGA)");
    check_eq(bda_b(bda::VIDEO_MODE), 3, "DISPLAY_EGA: the initial mode is 3");
    int10(0x1A00);
    check_eq(BL(), 0x04, "DISPLAY_EGA: AH=1Ah reports DCC 04h (EGA colour)");

    m.set_display(dos_machine::DISPLAY_CGA_MDA);
    m.init_machine();
    int10(0x1A00);
    check_eq(BL(), 0x08, "DISPLAY_CGA_MDA: primary DCC 08h (VGA colour)");
    check_eq(BH(), 0x01, "DISPLAY_CGA_MDA: secondary DCC 01h (MDA)");

    m.set_display(dos_machine::DISPLAY_VGA);
    m.init_machine();
    mem.set_a20(true);
    check_eq(bda_w(bda::EQUIPMENT), 0x0005,
             "DISPLAY_VGA: equipment bits 4-5 = 00");
    int10(0x1A00);
    check_eq(BL(), 0x08, "DISPLAY_VGA: AH=1Ah reports DCC 08h (VGA colour)");
    check_eq(BH(), 0x00, "DISPLAY_VGA: no secondary adapter");

    // The BIOS answers VESA VBE 2.0 and mode 13h whatever this says.
    m.set_display(dos_machine::DISPLAY_CGA);
    m.init_machine();
    mem.set_a20(true);
    int10(0x1A00);
    diverge_eq(BL(), 0x02,
               "divergence: the default configuration reports DCC 02h (CGA) "
               "while the same BIOS implements mode 13h and VESA VBE 2.0");
  }

  //==========================================================================
  std::printf("\n");
  if (g_stuck)
    std::printf("NOTE: %d guest stub(s) never reached their HLT\n", g_stuck);
  if (g_exceptions)
    std::printf("NOTE: %d unexpected CPU exception(s) during a stub\n", g_exceptions);
  std::printf("known bugs still present: %d (expected %d)\n",
              g_bugs, KNOWN_BUGS_EXPECTED);
  bool baseline_ok = (g_bugs == KNOWN_BUGS_EXPECTED);
  if (!baseline_ok) {
    std::printf("FAIL: known-bug count is %d, baseline says %d - %s\n",
                g_bugs, KNOWN_BUGS_EXPECTED,
                g_bugs_fixed ? "a bug was fixed; lower KNOWN_BUGS_EXPECTED"
                             : "update the baseline deliberately");
  }
  if (g_failures == 0 && baseline_ok && g_stuck == 0 && g_exceptions == 0) {
    std::printf("ALL BIOS TESTS PASS (%d checks)\n", g_checks);
    return 0;
  }
  std::printf("BIOS TESTS FAILED: %d of %d checks failed\n", g_failures, g_checks);
  return 1;
}
