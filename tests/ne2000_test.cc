// ne2000_test.cc — unit harness for emu88/ne2000.cc (NE2000 / DP8390 NIC).
//
// Build & run:
//   g++ -std=c++20 -O2 -Wall -Wextra -I emu88 tests/ne2000_test.cc
//       emu88/ne2000.cc -o /tmp/ne2000_test && /tmp/ne2000_test
//
// To wire into the suite, add to tests/build.sh next to the other unit tests:
//   $CXX $CXXFLAGS tests/ne2000_test.cc emu88/ne2000.cc -o "$OUT/ne2000_test"
// and add ne2000_test to the `for t in opl_unit sb_unit ...` list in
// tests/run_suites.sh.  It needs no corpus, links one .cc file, and runs in
// well under a second.
//
// ---------------------------------------------------------------------------
// WHAT THIS COVERS
// ---------------------------------------------------------------------------
// emu88/ne2000.cc was 425 lines that every harness linking the DOS layer
// compiled and none of them ever executed.  This drives the class through its
// public API the way tests/opl_unit.cc and tests/uart_unit.cc drive theirs:
// iowrite/ioread/iowrite16/ioread16 on the 32 I/O offsets, receive(), and the
// on_transmit callback.  Nothing here reaches past the public interface — the
// register file, the 48 KB of card memory and the ring pointers are all private
// and are asserted only through the ports, which is how a driver sees them.
//
// Sections, in order:
//   1  reset state, register banking (pages 0/1/2/3) through the command reg
//   2  the MAC: set_mac -> PAR0-5 on page 1, and the doubled on-card PROM the
//      way NE2000.COM reads it (remote DMA of 32 bytes from address 0)
//   3  remote DMA: RSAR/RBCR setup, byte and word transfers both directions,
//      address auto-increment seen through CRDA0/1, the RDC bit, count
//      exhaustion, PROM write protection, and the PSTOP->PSTART wrap
//   4  transmit: frame written by remote DMA, TPSR/TBCR, the TXP command, the
//      exact bytes and length delivered to on_transmit, TSR and ISR.PTX
//   5  receive: the 4-byte DP8390 header, payload, CURR advance, BNRY, and a
//      read-back through remote DMA exactly as a driver does it
//   6  ring wrap across PSTART..PSTOP
//   6b a ring whose first page IS the first page of buffer RAM (PSTART = 0x40),
//      which is the only section that ever writes card address 4000h
//   7  ring overflow with BNRY parked
//   8  interrupts: IMR masking, irq_active(), ISR write-1-to-clear
//   9  receive filtering: unicast / broadcast / multicast / promiscuous /
//      monitor / stopped / runt
//
// The byte count in the receive header is `len + 4` — payload plus the 4-byte
// header, checked in section 5.  That is the value Linux's 8390.c and every
// NE2000 packet driver expect (`pkt_len = count - sizeof(hdr)`), and it is the
// right one HERE because dos_machine hands receive() a frame with no FCS.  A
// real DP8390 also stores the 4-byte FCS and counts it, so its count for the
// same wire frame is 4 higher; the absent FCS is pinned separately.
//
// ---------------------------------------------------------------------------
// THREE KINDS OF ASSERTION
// ---------------------------------------------------------------------------
// Same scheme as tests/fpu_test.cc, for the same reason: this is a functional
// model of a DP8390, not a gate-level one, and asserting whatever it happens to
// do would hide that.
//
//   check()   — behaviour that is correct.  A plausible bug flips it.  These
//               must pass.
//   diverge() — a value that provably differs from a real DP8390/NE2000.  The
//               assertion pins THIS implementation's value and the comment
//               above it names the gap.  These pass; if the value ever moves
//               the harness fails and somebody re-reads the comment.
//   bug()     — a defect with no design excuse.  The assertion states the
//               ARCHITECTURALLY CORRECT answer, so it is red on purpose, and
//               the count is held to KNOWN_BUGS_EXPECTED the way
//               tests/run_suites.sh holds SingleStepTests to SST_BASELINE.
//               Fixing one FAILS the harness and prints "lower
//               KNOWN_BUGS_EXPECTED", so an improvement can never go quietly
//               green.
//
// THE LEDGER IS EMPTY: KNOWN_BUGS_EXPECTED is 0.  Two defects came out of
// writing this file and both were fixed on 2026-08-27, with the assertion that
// caught each one staying exactly where it was and becoming a check():
//   - receive() stored the ring with only an `a < MEM_TOTAL' guard where
//     dma_write_byte refuses anything below BUF_START, so a card started before
//     CURR had been programmed wrote the frame over its own read-only MAC PROM.
//     CURR comes up 0 from reset(), so that is the state a driver is in between
//     STA and its first page-1 write.
//   - ioread16() on a register port returned one 8-bit register with a zero
//     high byte, while iowrite16() correctly split a register-port word into
//     two byte writes.  An INW of a register pair was half a read.
// The bug() machinery is left here for the next defect.
//
// ---------------------------------------------------------------------------
// NOT COVERED, AND WHY
// ---------------------------------------------------------------------------
// - Nothing here runs guest code.  This is the module in isolation; the port
//   decode in dos_machine.cc (ne2000_base .. +0x1F, and the 16-bit paths at
//   dos_machine.cc:1286 and :1312) and the IRQ delivery at dos_machine.cc:604
//   are not exercised by this file.  A guest-level test would need a packet
//   driver and a host network back end.
// - No timing.  Transmission completes inside the iowrite() that commands it,
//   so there is no window in which CR.TXP is set, no FIFO, and no collision or
//   deferral behaviour to test.  Section 4 pins that.
// - CNTR0/1/2 (frame-alignment, CRC and missed-packet counters) and ISR.CNT are
//   hardwired to zero in the implementation.  They are pinned as divergences,
//   not tested as counters, because there is nothing to count.
// - The multicast hash is not tested as a hash: MAR0-7 round-trip through page
//   1 and are then ignored by the filter.  Pinned in section 9.
// - Remote DMA in byte mode (DCR.WTS = 0) is not distinguished from word mode.
//   DCR is stored and read back and never consulted; pinned in section 3.
// - Bus width, IRQ number and the 8/16-bit slot type are not modelled at all,
//   so there is nothing to assert about them.
//
// ---------------------------------------------------------------------------
// SHOWN TO BE ABLE TO FAIL
// ---------------------------------------------------------------------------
// The standard commit f265310 set: a new gate is not trusted until it has been
// seen red.  22 single-point mutations were applied one at a time to a scratch
// copy of emu88/ne2000.cc (never the real file) and the harness was rebuilt and
// run against each.
//
// First pass: 18 of 20 killed.  The two survivors were real coverage holes —
// nothing read CLDA0/CLDA1 (page 0 offsets 1 and 2, which mirror the remote DMA
// address here), and nothing asserted that a page-2 write is ignored.  Both are
// closed; the re-applied mutations died.  Two further mutations added
// afterwards (drop the ISR_RST clear on start, drop the `rdar >= BUF_START`
// guard on DMA writes) also died.
//
// Final: 22 mutations, 22 killed, 0 survivors.  The list is in the report that
// accompanied this file; the ones worth naming here are the ones that killed
// only a single assertion, because they are what that assertion is for: swapping
// PSTART/PSTOP in the page-0 write switch, inverting the page-select shift,
// dropping the `rdar++' auto-increment, turning `isr &= ~value' into
// `isr |= value', `next > stop' instead of `next >= stop' in the ring wrap, and
// writing `total' instead of `total & 0xFF' into the header length byte.
//
// Six more were applied on 2026-08-27 against the two fixes below, and one of
// them SURVIVED at first: changing the receive-path guard from `a >= BUF_START'
// to `a > BUF_START' changed nothing, because every section used the
// conventional PSTART = 0x46 and so nothing ever wrote card address 4000h.
// Section 6b exists to close that; with it, all six die.  Total: 28 applied,
// 28 killed.
//
// Exit code is non-zero if any check()/diverge() fails, or if the number of
// known bugs still present is not exactly KNOWN_BUGS_EXPECTED.

#include "ne2000.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

//===========================================================================
// Bookkeeping
//===========================================================================

// Number of bug() assertions that are still failing.  See the header comment.
// Lower this (and turn the bug() call into a check()) when one is fixed.
static const int KNOWN_BUGS_EXPECTED = 0;

static int g_checks = 0;
static int g_failures = 0;
static int g_bug_asserts = 0;
static int g_bugs_hit = 0;
static int g_bugs_fixed = 0;

static void check(bool cond, const char *what) {
  g_checks++;
  if (!cond) {
    g_failures++;
    std::printf("  FAIL: %s\n", what);
  }
}

// A pinned divergence from a real DP8390/NE2000.  Same gate as check(): it
// asserts what THIS implementation does.  Separate name so the divergences are
// greppable.
static void diverge(bool cond, const char *what) {
  g_checks++;
  if (!cond) {
    g_failures++;
    std::printf("  FAIL (pinned divergence changed): %s\n", what);
  }
}

// `behaves_correctly` states the REAL-hardware behaviour, which is expected to
// be false today.  If it ever becomes true the bug was fixed and the baseline
// is stale — that fails the run, loudly.
// Unused while the ledger is empty; kept in place for the next defect.
[[maybe_unused]] static void bug(bool behaves_correctly, const char *what) {
  g_bug_asserts++;
  if (behaves_correctly) {
    g_bugs_fixed++;
    std::printf("  FIXED (lower KNOWN_BUGS_EXPECTED, make this a check()): %s\n", what);
  } else {
    g_bugs_hit++;
    std::printf("  KNOWN BUG: %s\n", what);
  }
}

//===========================================================================
// Register offsets, named the way the DP8390 datasheet names them.
// Several names share an offset because the pages overlay each other; that
// overlay is itself under test (section 1).
//===========================================================================

enum {
  R_CR     = 0x00,   // all pages

  // Page 0, write
  W_PSTART = 0x01, W_PSTOP = 0x02, W_BNRY  = 0x03, W_TPSR  = 0x04,
  W_TBCR0  = 0x05, W_TBCR1 = 0x06, W_ISR   = 0x07, W_RSAR0 = 0x08,
  W_RSAR1  = 0x09, W_RBCR0 = 0x0A, W_RBCR1 = 0x0B, W_RCR   = 0x0C,
  W_TCR    = 0x0D, W_DCR   = 0x0E, W_IMR   = 0x0F,

  // Page 0, read
  R_CLDA0  = 0x01, R_CLDA1 = 0x02, R_BNRY  = 0x03, R_TSR   = 0x04,
  R_NCR    = 0x05, R_FIFO  = 0x06, R_ISR   = 0x07, R_CRDA0 = 0x08,
  R_CRDA1  = 0x09, R_RSR   = 0x0C, R_CNTR0 = 0x0D, R_CNTR1 = 0x0E,
  R_CNTR2  = 0x0F,

  // Page 1
  P1_PAR0  = 0x01, P1_CURR = 0x07, P1_MAR0 = 0x08,

  // ASIC
  A_DATA   = 0x10, A_RESET = 0x1F,
};

// Command register bits.
enum {
  CR_STP = 0x01, CR_STA = 0x02, CR_TXP = 0x04,
  CR_RD_READ = 0x08, CR_RD_WRITE = 0x10, CR_RD_SEND = 0x18, CR_RD_ABORT = 0x20,
};

// ISR / IMR bits.
enum {
  I_PRX = 0x01, I_PTX = 0x02, I_RXE = 0x04, I_TXE = 0x08,
  I_OVW = 0x10, I_CNT = 0x20, I_RDC = 0x40, I_RST = 0x80,
};

// RCR bits.
enum { RCR_AB = 0x04, RCR_AM = 0x08, RCR_PRO = 0x10, RCR_MON = 0x20 };

//===========================================================================
// Test fixture: an ne2000 plus the handful of driver idioms every section uses.
//===========================================================================

static const uint8_t kMac[6]   = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
static const uint8_t kBcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static const uint8_t kMcast[6] = {0x01, 0x00, 0x5E, 0x00, 0x00, 0x01};
static const uint8_t kOther[6] = {0x00, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE};

struct Card {
  ne2000 nic;
  std::vector<uint8_t> tx;   // bytes handed to on_transmit, last call
  int tx_count = 0;

  Card() {
    nic.on_transmit = [this](const uint8_t *d, int n) {
      tx.assign(d, d + n);
      tx_count++;
    };
  }
  Card(const Card &) = delete;
  Card &operator=(const Card &) = delete;

  void w(int off, uint8_t v) { nic.iowrite(off, v); }
  uint8_t r(int off) { return nic.ioread(off); }

  // Select a register page the way a driver does: keep STA/STP, command the
  // remote DMA to abort/complete.  (Re-writing the CR with the RD field still
  // set to read/write would reload the DMA address from RSAR.)
  void sel(int pg) { w(R_CR, (uint8_t)((pg << 6) | CR_RD_ABORT | (r(R_CR) & 0x03))); }

  // Remote DMA read of `n` bytes from card address `addr`.  Preserves STA/STP
  // so a stopped card stays stopped.
  void dma_read(uint16_t addr, uint8_t *buf, int n) {
    sel(0);
    w(W_RBCR0, (uint8_t)(n & 0xFF));
    w(W_RBCR1, (uint8_t)((n >> 8) & 0xFF));
    w(W_RSAR0, (uint8_t)(addr & 0xFF));
    w(W_RSAR1, (uint8_t)(addr >> 8));
    w(R_CR, (uint8_t)(CR_RD_READ | (r(R_CR) & 0x03)));
    for (int i = 0; i < n; i++) buf[i] = r(A_DATA);
  }

  void dma_write(uint16_t addr, const uint8_t *buf, int n) {
    sel(0);
    w(W_RBCR0, (uint8_t)(n & 0xFF));
    w(W_RBCR1, (uint8_t)((n >> 8) & 0xFF));
    w(W_RSAR0, (uint8_t)(addr & 0xFF));
    w(W_RSAR1, (uint8_t)(addr >> 8));
    w(R_CR, (uint8_t)(CR_RD_WRITE | (r(R_CR) & 0x03)));
    for (int i = 0; i < n; i++) w(A_DATA, buf[i]);
  }

  uint8_t curr() { sel(1); uint8_t v = r(P1_CURR); sel(0); return v; }
  void set_curr(uint8_t v) { sel(1); w(P1_CURR, v); sel(0); }

  // The canonical NS8390 bring-up sequence: stop, program page 0, program
  // page 1 while still stopped, start, then drop loopback and set the real RCR.
  void init(uint8_t pstart, uint8_t pstop, uint8_t rcr) {
    nic.set_mac(kMac);
    w(R_CR, CR_STP | CR_RD_ABORT);     // 0x21: stop, page 0
    w(W_DCR, 0x49);                    // word-wide, no loopback, FIFO 8
    w(W_RBCR0, 0); w(W_RBCR1, 0);
    w(W_RCR, RCR_MON);                 // monitor while initialising
    w(W_TCR, 0x02);                    // internal loopback while initialising
    w(W_TPSR, 0x40);
    w(W_PSTART, pstart);
    w(W_BNRY, pstart);
    w(W_PSTOP, pstop);
    w(W_ISR, 0xFF);
    w(W_IMR, 0x00);
    sel(1);
    for (int i = 0; i < 6; i++) w(P1_PAR0 + i, kMac[i]);
    for (int i = 0; i < 8; i++) w(P1_MAR0 + i, 0x00);
    w(P1_CURR, (uint8_t)(pstart + 1));
    sel(0);
    w(R_CR, CR_STA | CR_RD_ABORT);     // 0x22: start, page 0
    w(W_TCR, 0x00);
    w(W_RCR, rcr);
  }
};

// Build an Ethernet frame: dst, src, type 0x0800, then a walking pattern.
//
// The length guard is not defensive programming for its own sake.  Two of the
// call sites derive `len` from what the card just reported, so a mutant that
// corrupts the ring can drive a nonsense length in here - and an out-of-range
// std::vector abort tells you nothing, on top of discarding every buffered
// line printed before it.  This says which it was.
static std::vector<uint8_t> frame(const uint8_t *dst, int len, uint8_t seed) {
  if (len < 1 || len > 16384) {
    std::printf("  FAIL: frame() asked for %d bytes — a card-derived length "
                "outside 1..16384\n", len);
    std::fflush(stdout);
    std::exit(1);
  }
  std::vector<uint8_t> f((size_t)len, 0);
  // Fill only what fits.  The runt tests in section 9 ask for frames SHORTER
  // than an Ethernet header on purpose, and the straight-line version of this -
  // two memcpys of 6 and an unconditional f[12]/f[13] - wrote two bytes past
  // the end of a 13-element vector every time it did.  -O2 kept quiet about it.
  for (int i = 0; i < len && i < 6; i++)  f[(size_t)i] = dst[i];
  for (int i = 6; i < len && i < 12; i++) f[(size_t)i] = kOther[i - 6];
  if (len > 12) f[12] = 0x08;
  if (len > 13) f[13] = 0x00;
  for (int i = 14; i < len; i++) f[(size_t)i] = (uint8_t)(seed + i * 3);
  return f;
}

static bool same(const uint8_t *a, const uint8_t *b, int n) {
  return std::memcmp(a, b, (size_t)n) == 0;
}

//===========================================================================
int main() {
  // Line-buffered, because this harness can be aborted by the code it is
  // testing: a mutation run found a mutant whose corrupted ring crashed a later
  // section, and the full-buffered stdout meant the FAIL line that had already
  // diagnosed it was discarded with the buffer.  A gate that cannot tell you
  // what it saw is most of the way to a gate that cannot fail.
  std::setvbuf(stdout, nullptr, _IOLBF, 0);

  //=========================================================================
  // 1. Reset state and register banking
  //=========================================================================
  {
    Card c;

    // The command register after reset: stopped, remote DMA aborted, page 0.
    check(c.r(R_CR) == (CR_STP | CR_RD_ABORT), "reset: CR == 0x21 (STP + DMA abort)");
    check(((c.r(R_CR) >> 6) & 3) == 0, "reset: page select is page 0");

    // ISR comes up with RST set and nothing else; nothing is masked in.
    check(c.r(R_ISR) == I_RST, "reset: ISR == 0x80 (RST alone)");
    check(!c.nic.irq_active(), "reset: IRQ not asserted (IMR == 0)");

    // Page 0 reads.  CLDA/CRDA follow the DMA address, which is 0.
    check(c.r(R_CLDA0) == 0x00, "reset: CLDA0 == 0");
    check(c.r(R_CLDA1) == 0x00, "reset: CLDA1 == 0");
    check(c.r(R_BNRY)  == 0x00, "reset: BNRY == 0");
    check(c.r(R_TSR)   == 0x00, "reset: TSR == 0");
    check(c.r(R_CRDA0) == 0x00, "reset: CRDA0 == 0");
    check(c.r(R_CRDA1) == 0x00, "reset: CRDA1 == 0");
    check(c.r(R_RSR)   == 0x00, "reset: RSR == 0");

    // Page 2 is where the page-0 *write* registers can be read back.
    c.sel(2);
    check(c.r(W_PSTART) == 0x00, "reset: PSTART == 0");
    check(c.r(W_PSTOP)  == 0x00, "reset: PSTOP == 0");
    check(c.r(W_TPSR)   == 0x00, "reset: TPSR == 0");
    check(c.r(W_TBCR0)  == 0x00 && c.r(W_TBCR1) == 0x00, "reset: TBCR == 0");
    check(c.r(W_RSAR0)  == 0x00 && c.r(W_RSAR1) == 0x00, "reset: RSAR == 0");
    check(c.r(W_RBCR0)  == 0x00 && c.r(W_RBCR1) == 0x00, "reset: RBCR == 0");
    check(c.r(W_RCR)    == 0x00, "reset: RCR == 0");
    check(c.r(W_TCR)    == 0x00, "reset: TCR == 0");
    check(c.r(W_DCR)    == 0x00, "reset: DCR == 0");
    check(c.r(W_IMR)    == 0x00, "reset: IMR == 0");
    c.sel(0);

    // Page 1 comes up zeroed, including CURR.  (Section 5's bug() is about what
    // happens if a driver starts the card while CURR is still this value.)
    c.sel(1);
    check(c.r(P1_CURR) == 0x00, "reset: CURR == 0");
    for (int i = 0; i < 8; i++) check(c.r(P1_MAR0 + i) == 0x00, "reset: MAR[i] == 0");
    c.sel(0);

    // The registers with hardwired values.  NCR and the FIFO are dead reads.
    check(c.r(R_NCR) == 0x00, "NCR (collision count) reads 0");
    check(c.r(R_FIFO) == 0x00, "FIFO port reads 0");

    // The tally counters do not exist.  A real DP8390 increments CNTR0/1/2 on
    // frame-alignment errors, CRC errors and missed packets, and raises ISR.CNT
    // when one wraps at 0xFF; here they are three `return 0` cases, so a driver
    // that reads them to clear them gets nothing and ISR.CNT is never set.
    diverge(c.r(R_CNTR0) == 0 && c.r(R_CNTR1) == 0 && c.r(R_CNTR2) == 0,
            "CNTR0/1/2 are hardwired to 0 (no tally counters)");

    // --- banking: the same offset is a different register on each page ------
    c.w(W_PSTART, 0x46);
    c.w(W_BNRY, 0x4A);
    c.sel(1);
    c.w(P1_PAR0, 0x11);                  // offset 1 on page 1 is PAR0
    check(c.r(P1_PAR0) == 0x11, "page 1: offset 1 reads back PAR0, not PSTART");
    c.w(P1_CURR, 0x47);                  // offset 7 on page 1 is CURR
    check(c.r(P1_CURR) == 0x47, "page 1: offset 7 reads back CURR, not ISR");
    c.sel(0);
    check(c.r(R_ISR) == I_RST, "page 0: offset 7 is still ISR after the page 1 write");
    c.sel(2);
    check(c.r(W_PSTART) == 0x46, "page 2: offset 1 mirrors PSTART (0x46)");
    c.sel(1);
    check(c.r(P1_PAR0) == 0x11, "page 1 value survives a trip through pages 0 and 2");
    c.sel(0);
    check(c.r(R_BNRY) == 0x4A, "page 0: offset 3 is BNRY on read as well as write");

    // Page 2 is read-only.  A write there must not reach the page-0 register.
    c.w(W_RCR, RCR_AB);
    c.sel(2);
    c.w(W_RCR, 0x55);
    check(c.r(W_RCR) == RCR_AB, "page 2 writes are ignored (RCR unchanged)");
    c.sel(0);

    // Page 3 does not exist on a DP8390 (PS1:PS0 = 11 is reserved).  Reads are
    // open bus and writes go nowhere.
    c.sel(3);
    check(c.r(0x01) == 0xFF && c.r(0x0F) == 0xFF, "page 3 reads 0xFF (no such page)");
    c.w(0x01, 0x99);
    c.sel(2);
    check(c.r(W_PSTART) == 0x46, "page 3 writes do not reach page 0");
    c.sel(0);

    // Offsets 0x11-0x1E are dead here.  On a real NE2000 the ASIC aliases the
    // data port across base+0x10..0x17 and the reset port across base+0x18..
    // 0x1F, so an INB from base+0x11 would return card memory, not 0xFF.
    diverge(c.r(0x11) == 0xFF && c.r(0x17) == 0xFF && c.r(0x1E) == 0xFF,
            "ports 0x11-0x1E read 0xFF (data/reset ports are not aliased)");

    // --- the reset port ----------------------------------------------------
    c.w(W_IMR, 0x3F);
    c.w(A_RESET, 0x00);
    check(c.r(R_CR) == (CR_STP | CR_RD_ABORT), "write to 0x1F resets the CR");
    check(c.r(R_ISR) == I_RST, "write to 0x1F sets ISR.RST again");
    c.sel(2);
    check(c.r(W_IMR) == 0x00 && c.r(W_PSTART) == 0x00, "write to 0x1F clears the register file");
    c.sel(0);

    // The classic NE2000 reset idiom is `outb(inb(base+0x1f), base+0x1f)`: the
    // read returns a latched value and the WRITE performs the reset.  Here the
    // read performs a reset of its own and returns 0.
    c.w(W_PSTART, 0x46);
    diverge(c.r(A_RESET) == 0x00, "read of 0x1F returns 0, not a latched byte");
    c.sel(2);
    diverge(c.r(W_PSTART) == 0x00, "read of 0x1F performs a reset by itself");
    c.sel(0);
  }

  //=========================================================================
  // 2. The MAC address: PAR registers and the on-card PROM
  //=========================================================================
  {
    Card c;

    // Before set_mac the PROM is the constructor's 0xFF fill.  A real card's
    // PROM is programmed at manufacture, so NE2000.COM probing a card whose
    // host never called set_mac finds no 0x57 0x57 signature and reports no
    // card at all.
    uint8_t prom[32];
    c.dma_read(0x0000, prom, 32);
    bool all_ff = true;
    for (int i = 0; i < 32; i++) if (prom[i] != 0xFF) all_ff = false;
    diverge(all_ff, "PROM is 0xFF until set_mac() is called (no factory contents)");

    c.nic.set_mac(kMac);

    // The driver's first read: PAR0-5 on page 1.
    c.sel(1);
    bool par_ok = true;
    for (int i = 0; i < 6; i++) if (c.r(P1_PAR0 + i) != kMac[i]) par_ok = false;
    check(par_ok, "set_mac: PAR0-5 read back the MAC on page 1");
    c.sel(0);

    // The way NE2000.COM actually reads it: 32-byte remote DMA from address 0,
    // where the 16-bit card returns each 8-bit PROM byte twice.
    c.dma_read(0x0000, prom, 32);
    bool doubled = true;
    for (int i = 0; i < 6; i++)
      if (prom[i * 2] != kMac[i] || prom[i * 2 + 1] != kMac[i]) doubled = false;
    check(doubled, "PROM: each of the 6 MAC bytes appears twice at offsets 0-11");

    uint8_t drv[6];
    for (int i = 0; i < 6; i++) drv[i] = prom[i * 2];
    check(same(drv, kMac, 6), "PROM: taking every other byte recovers the MAC");

    // The NE2000 signature the driver tests: PROM bytes 14 and 15, which land
    // at doubled offsets 28 and 30.
    check(prom[28] == 0x57 && prom[30] == 0x57, "PROM: 'WW' signature at doubled offsets 28/30");
    check(prom[29] == 0x57 && prom[31] == 0x57, "PROM: the signature bytes are doubled too");

    // Everything between the MAC and the signature is 0x57 as well.  On a real
    // card PROM bytes 6-13 are card-specific (usually 0x00), so doubled offsets
    // 12-27 would not be 'W'.  Harmless — no driver reads them — but it is not
    // what a PROM dump looks like.
    bool filler = true;
    for (int i = 12; i < 28; i++) if (prom[i] != 0x57) filler = false;
    diverge(filler, "PROM offsets 12-27 are filled with 0x57 rather than PROM bytes 6-13");

    // The PROM is read-only through the remote DMA: dma_write_byte refuses any
    // address below the 0x4000 buffer start.
    uint8_t poison[12];
    std::memset(poison, 0x00, sizeof(poison));
    c.dma_write(0x0000, poison, 12);
    c.dma_read(0x0000, prom, 12);
    check(prom[0] == kMac[0] && prom[10] == kMac[5],
          "remote DMA writes below 0x4000 are dropped (PROM is read-only)");

    // ...but the address counter and the byte count still ran.
    check(c.r(R_CRDA0) == 0x0C && c.r(R_CRDA1) == 0x00,
          "a dropped DMA write still advances the address counter");
  }

  //=========================================================================
  // 3. Remote DMA
  //=========================================================================
  {
    Card c;
    c.init(0x46, 0xC0, RCR_AB);

    // --- byte at a time, both directions -----------------------------------
    const uint8_t pat[8] = {0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x23, 0x45, 0x67};
    c.dma_write(0x4000, pat, 8);
    check(c.r(R_CRDA0) == 0x08 && c.r(R_CRDA1) == 0x40,
          "DMA write: CRDA advanced from 0x4000 to 0x4008");
    check((c.r(R_ISR) & I_RDC) != 0, "DMA write: ISR.RDC set when RBCR reaches 0");

    c.w(W_ISR, I_RDC);
    check((c.r(R_ISR) & I_RDC) == 0, "ISR.RDC cleared by writing 1 to it");

    uint8_t back[8];
    c.dma_read(0x4000, back, 8);
    check(same(back, pat, 8), "DMA read returns the bytes DMA write stored");
    check((c.r(R_ISR) & I_RDC) != 0, "DMA read: ISR.RDC set when RBCR reaches 0");

    // RDC is set exactly when the count runs out, not before.
    c.w(W_ISR, 0xFF);
    c.sel(0);
    c.w(W_RBCR0, 4); c.w(W_RBCR1, 0);
    c.w(W_RSAR0, 0x00); c.w(W_RSAR1, 0x40);
    c.w(R_CR, CR_RD_READ | CR_STA);
    check(c.r(R_CRDA0) == 0x00, "issuing the read command loads CRDA from RSAR");
    for (int i = 0; i < 3; i++) {
      check(c.r(A_DATA) == pat[i], "byte-at-a-time read returns the right byte");
      check((c.r(R_ISR) & I_RDC) == 0, "ISR.RDC stays clear while RBCR > 0");
    }
    check(c.r(R_CRDA0) == 0x03, "auto-increment: CRDA is start + 3 after 3 bytes");
    check(c.r(A_DATA) == pat[3], "fourth byte");
    check((c.r(R_ISR) & I_RDC) != 0, "ISR.RDC set on the byte that empties RBCR");

    // CLDA0/CLDA1 report the same counter as CRDA0/CRDA1 here.  On a real
    // DP8390 CLDA is the *local* DMA address — where the receive/transmit
    // engine is working inside card RAM — and moves independently of the host's
    // remote DMA.
    diverge(c.r(R_CLDA0) == c.r(R_CRDA0) && c.r(R_CLDA1) == c.r(R_CRDA1),
            "CLDA0/1 mirror the remote DMA address (no separate local DMA counter)");

    // Reading past the end of the transfer.  The datasheet's RDC is a status
    // bit, not an interlock: the remote DMA keeps running until it is aborted,
    // so a real card returns card RAM here.  This one gates the port hard.
    uint8_t extra = c.r(A_DATA);
    diverge(extra == 0xFF, "data port returns 0xFF once RBCR is exhausted");
    diverge(c.r(R_CRDA0) == 0x04, "the address counter does not advance past RBCR = 0");

    // --- word at a time ----------------------------------------------------
    c.w(W_ISR, 0xFF);
    c.w(W_RBCR0, 4); c.w(W_RBCR1, 0);
    c.w(W_RSAR0, 0x00); c.w(W_RSAR1, 0x41);
    c.w(R_CR, CR_RD_WRITE | CR_STA);
    c.nic.iowrite16(A_DATA, 0xBEEF);
    c.nic.iowrite16(A_DATA, 0x1234);
    check((c.r(R_ISR) & I_RDC) != 0, "two word writes exhaust a 4-byte RBCR");
    check(c.r(R_CRDA0) == 0x04 && c.r(R_CRDA1) == 0x41, "word writes advance CRDA by 2 each");

    uint8_t wb[4];
    c.dma_read(0x4100, wb, 4);
    check(wb[0] == 0xEF && wb[1] == 0xBE, "word write is little-endian in card RAM");
    check(wb[2] == 0x34 && wb[3] == 0x12, "second word likewise");

    c.w(W_ISR, 0xFF);
    c.w(W_RBCR0, 4); c.w(W_RBCR1, 0);
    c.w(W_RSAR0, 0x00); c.w(W_RSAR1, 0x41);
    c.w(R_CR, CR_RD_READ | CR_STA);
    check(c.nic.ioread16(A_DATA) == 0xBEEF, "word read returns the word that was written");
    check(c.nic.ioread16(A_DATA) == 0x1234, "second word read");
    check((c.r(R_ISR) & I_RDC) != 0, "word reads set ISR.RDC on exhaustion");

    // An odd RBCR against a word transfer: the low byte comes from card RAM,
    // the high byte is the exhausted-port 0xFF.  A real card in word mode moves
    // 16 bits at a time and would return both.
    c.w(W_ISR, 0xFF);
    c.w(W_RBCR0, 1); c.w(W_RBCR1, 0);
    c.w(W_RSAR0, 0x00); c.w(W_RSAR1, 0x41);
    c.w(R_CR, CR_RD_READ | CR_STA);
    diverge(c.nic.ioread16(A_DATA) == 0xFFEF, "16-bit read with RBCR = 1 returns 0xFF in the high byte");

    // DCR.WTS selects byte or word transfers on a real card.  Here DCR is
    // stored, read back on page 2, and never consulted: a 16-bit access always
    // moves two bytes.
    c.w(W_DCR, 0x48);                 // WTS = 0, i.e. byte mode
    c.w(W_ISR, 0xFF);
    c.w(W_RBCR0, 2); c.w(W_RBCR1, 0);
    c.w(W_RSAR0, 0x00); c.w(W_RSAR1, 0x41);
    c.w(R_CR, CR_RD_READ | CR_STA);
    diverge(c.nic.ioread16(A_DATA) == 0xBEEF, "16-bit DMA ignores DCR.WTS (byte mode still moves a word)");
    c.w(W_DCR, 0x49);

    // --- a 16-bit OUT to a register port splits into two register writes ----
    c.w(W_ISR, 0xFF);
    c.nic.iowrite16(W_RBCR0, 0x0140);   // RBCR0 = 0x40, RBCR1 = 0x01
    c.sel(2);
    check(c.r(W_RBCR0) == 0x40 && c.r(W_RBCR1) == 0x01,
          "16-bit write to a register port sets both halves of RBCR");
    c.sel(0);

    // Except at offset 0x0F, where the second byte would land on the data port
    // and is dropped instead.
    c.w(W_RSAR0, 0x00); c.w(W_RSAR1, 0x42);
    c.w(R_CR, CR_RD_WRITE | CR_STA);
    c.nic.iowrite16(W_IMR, 0xAA55);
    c.sel(2);
    check(c.r(W_IMR) == 0x55, "16-bit write to IMR sets IMR from the low byte");
    check(c.r(W_RBCR0) == 0x40 && c.r(W_RBCR1) == 0x01, "RBCR untouched...");
    c.sel(0);
    diverge(true, "...because the high byte of a word write to offset 0x0F is dropped, "
                  "not passed to the data port at 0x10");
    c.w(W_IMR, 0x00);

    // --- the read direction splits too -------------------------------------
    // iowrite16 turns a word write to a register port into two byte writes,
    // because that is what an ISA bus does with an 8-bit-decoded register file:
    // the card asserts IOCS16 only for the data port.  ioread16 used to return
    // ioread(offset) — 8 bits with a zero high byte — so an INW from base+0x03
    // lost TSR, and dos_machine.cc routes guest INW straight here, so a guest
    // saw it.  Fixed 2026-08-27.
    c.w(W_BNRY, 0x46);
    uint8_t pkt[64];
    std::memset(pkt, 0x5A, sizeof(pkt));
    c.dma_write(0x4000, pkt, 64);
    c.w(W_TPSR, 0x40);
    c.w(W_TBCR0, 64); c.w(W_TBCR1, 0);
    c.w(R_CR, CR_TXP | CR_STA | CR_RD_ABORT);
    check(c.r(R_TSR) == 0x01, "fixture for the INW bug: TSR == 0x01 after a transmit");
    uint16_t pair = c.nic.ioread16(R_BNRY);
    check(pair == 0x0146, "INW from base+0x03 returns BNRY | (TSR << 8), not just BNRY");

    // --- the remote DMA wraps at PSTOP -> PSTART ---------------------------
    // The ring boundary applies to the host's DMA too, which is what lets a
    // driver read a packet that straddles the end of the ring in one transfer.
    Card d;
    d.init(0x46, 0x48, RCR_AB);        // a 2-page ring: 0x4600..0x4800
    const uint8_t tail[2] = {0x11, 0x22};
    const uint8_t head[2] = {0x33, 0x44};
    d.dma_write(0x47FE, tail, 2);
    d.dma_write(0x4600, head, 2);
    uint8_t wrapped[4];
    d.dma_read(0x47FE, wrapped, 4);
    check(wrapped[0] == 0x11 && wrapped[1] == 0x22, "DMA read: the two bytes before PSTOP");
    check(wrapped[2] == 0x33 && wrapped[3] == 0x44, "DMA read wraps PSTOP -> PSTART mid-transfer");
    check(d.r(R_CRDA0) == 0x02 && d.r(R_CRDA1) == 0x46, "CRDA after the wrap is PSTART + 2");

    // The "send packet" remote DMA command (CR RD field = 3) is accepted and
    // does nothing at all: no address is loaded, no bytes move, RDC is never
    // set.  A real card DMAs the packet at BNRY+1 out of the ring.
    d.w(W_ISR, 0xFF);
    d.w(R_CR, CR_RD_SEND | CR_STA);
    diverge((d.r(R_ISR) & I_RDC) == 0 && d.r(A_DATA) == 0xFF,
            "CR remote-DMA command 3 (send packet) is a no-op");
  }

  //=========================================================================
  // 4. Transmit
  //=========================================================================
  {
    Card c;
    c.init(0x46, 0xC0, RCR_AB);

    // Load a frame into the transmit buffer at page 0x40 by remote DMA, then
    // command the transmit exactly as a driver does.
    std::vector<uint8_t> f = frame(kOther, 64, 0x10);
    c.dma_write(0x4000, f.data(), 64);
    c.w(W_ISR, 0xFF);
    c.w(W_TPSR, 0x40);
    c.w(W_TBCR0, 64);
    c.w(W_TBCR1, 0);
    c.w(R_CR, CR_TXP | CR_STA | CR_RD_ABORT);

    check(c.tx_count == 1, "TXP delivers exactly one frame to on_transmit");
    check((int)c.tx.size() == 64, "on_transmit length is TBCR (64)");
    check(same(c.tx.data(), f.data(), 64), "on_transmit bytes are the bytes DMA'd into card RAM");
    check(c.r(R_TSR) == 0x01, "TSR == 0x01 (packet transmitted OK)");
    check((c.r(R_ISR) & I_PTX) != 0, "ISR.PTX set after transmit");
    check((c.r(R_ISR) & I_TXE) == 0, "ISR.TXE clear after a good transmit");

    // TXP never sticks: the transmit completes inside the OUT that starts it.
    // A real card is still shifting bits out for ~50us at 10 Mbit and a driver
    // polling CR sees TXP set; here the poll can never observe it.
    check(c.r(R_CR) == (CR_STA | CR_RD_ABORT), "CR reads back with TXP cleared");
    diverge(true, "transmission is instantaneous: there is no window in which CR.TXP reads 1");

    // A second transmit of a different length from a different page.
    std::vector<uint8_t> g = frame(kBcast, 1514, 0x77);
    c.dma_write(0x4100, g.data(), 1514);
    c.w(W_ISR, 0xFF);
    c.w(W_TPSR, 0x41);
    c.w(W_TBCR0, (uint8_t)(1514 & 0xFF));
    c.w(W_TBCR1, (uint8_t)(1514 >> 8));
    c.w(R_CR, CR_TXP | CR_STA | CR_RD_ABORT);
    check(c.tx_count == 2, "second TXP transmits again");
    check((int)c.tx.size() == 1514, "a full-size frame keeps its length");
    check(same(c.tx.data(), g.data(), 1514), "a full-size frame keeps its bytes");

    // TBCR = 0 is a programming error: no frame goes out and TXE is raised.
    c.w(W_ISR, 0xFF);
    c.w(W_TBCR0, 0); c.w(W_TBCR1, 0);
    c.w(R_CR, CR_TXP | CR_STA | CR_RD_ABORT);
    check(c.tx_count == 2, "TBCR = 0 transmits nothing");
    check((c.r(R_ISR) & I_TXE) != 0, "TBCR = 0 raises ISR.TXE");
    check((c.r(R_ISR) & I_PTX) == 0, "TBCR = 0 does not raise ISR.PTX");
    // A real chip explains a TXE through TSR — ABT, FU, CRS or CDH.  Here TSR
    // is cleared to 0, so the driver is told an error happened and given no
    // reason for it.
    diverge(c.r(R_TSR) == 0x00, "TSR reads 0x00 on the transmit-error path (no ABT/FU/CRS bit)");

    // Over-long transmits are refused the same way.
    c.w(W_ISR, 0xFF);
    c.w(W_TBCR0, (uint8_t)(1601 & 0xFF)); c.w(W_TBCR1, (uint8_t)(1601 >> 8));
    c.w(R_CR, CR_TXP | CR_STA | CR_RD_ABORT);
    check(c.tx_count == 2, "TBCR > 1600 transmits nothing");
    check((c.r(R_ISR) & I_TXE) != 0, "TBCR > 1600 raises ISR.TXE");

    // TXP without STA does nothing: the NIC has to be started.
    c.w(W_ISR, 0xFF);
    c.w(W_TBCR0, 64); c.w(W_TBCR1, 0);
    c.w(W_TPSR, 0x40);
    c.w(R_CR, CR_TXP | CR_STP | CR_RD_ABORT);
    check(c.tx_count == 2, "TXP with STP instead of STA transmits nothing");
    check(c.r(R_ISR) == 0x00, "a refused transmit sets no ISR bit at all");
    c.w(R_CR, CR_STA | CR_RD_ABORT);

    // Internal loopback (TCR.LB != 0) completes without putting anything on the
    // wire.  A real DP8390 loops the frame back through the receive path so the
    // driver can check it — that is what the loopback modes exist for — and
    // here nothing comes back.
    c.w(W_ISR, 0xFF);
    c.w(W_TCR, 0x02);
    c.w(R_CR, CR_TXP | CR_STA | CR_RD_ABORT);
    check(c.tx_count == 2, "loopback mode does not put the frame on the wire");
    check(c.r(R_TSR) == 0x01, "loopback: TSR == 0x01");
    check((c.r(R_ISR) & I_PTX) != 0, "loopback: ISR.PTX set");
    diverge((c.r(R_ISR) & I_PRX) == 0 && c.curr() == 0x47,
            "internal loopback does not feed the frame back into the receive ring");
    c.w(W_TCR, 0x00);
  }
  {
    // A transmit that runs off the end of the ring wraps to PSTART.  PSTART/
    // PSTOP bound the RECEIVE ring on a real DP8390; the transmit local DMA
    // reads linearly from TPSR for TBCR bytes and does not consult them.  No
    // driver puts its transmit buffer inside the receive ring, so this is a
    // difference in an unreachable corner rather than a defect — pinned so it
    // stays visible.
    Card c;
    c.init(0x46, 0x48, RCR_AB);        // ring 0x4600..0x4800
    std::vector<uint8_t> a(256), b(128);
    for (int i = 0; i < 256; i++) a[(size_t)i] = (uint8_t)(0xA0 + i);
    for (int i = 0; i < 128; i++) b[(size_t)i] = (uint8_t)(0x50 + i);
    c.dma_write(0x4700, a.data(), 256);
    c.dma_write(0x4600, b.data(), 128);
    c.w(W_TPSR, 0x47);
    c.w(W_TBCR0, (uint8_t)(384 & 0xFF)); c.w(W_TBCR1, (uint8_t)(384 >> 8));
    c.w(R_CR, CR_TXP | CR_STA | CR_RD_ABORT);
    check((int)c.tx.size() == 384, "wrapped transmit still sends TBCR bytes");
    diverge(same(c.tx.data(), a.data(), 256) && same(c.tx.data() + 256, b.data(), 128),
            "the transmit DMA wraps at PSTOP -> PSTART like the receive ring does");
  }

  //=========================================================================
  // 5. Receive
  //=========================================================================
  {
    Card c;
    c.init(0x46, 0xC0, RCR_AB);
    c.w(W_ISR, 0xFF);

    std::vector<uint8_t> f = frame(kMac, 60, 0x31);
    c.nic.receive(f.data(), 60);

    check((c.r(R_ISR) & I_PRX) != 0, "receive: ISR.PRX set");
    check(c.r(R_RSR) == 0x01, "receive: RSR == 0x01 (packet received OK)");
    check(c.curr() == 0x48, "receive: CURR advanced from 0x47 to 0x48 (one page)");
    check(c.r(R_BNRY) == 0x46, "receive: BNRY is the driver's, and the card did not move it");

    // Read it back the way a driver does: header first, from page BNRY+1.
    uint8_t hdr[4];
    c.dma_read(0x4700, hdr, 4);
    check(hdr[0] == 0x01, "header[0] is the receive status byte (PRX)");
    check(hdr[1] == 0x48, "header[1] is the next-page pointer (0x48)");
    // The DP8390 byte count includes the 4-byte header.  Linux's 8390.c does
    // `pkt_len = rx_frame.count - sizeof(struct e8390_pkt_hdr)`, so 60 + 4 is
    // what makes a driver read back exactly the 60 bytes handed to receive().
    check(hdr[2] == 64 && hdr[3] == 0, "header[2:3] is len + 4 = 64, header included");

    uint8_t body[64];
    c.dma_read(0x4700, body, 64);
    check(same(body + 4, f.data(), 60), "the payload follows the header byte for byte");

    // No FCS.  A real card writes the 4-byte CRC after the payload and counts
    // it, so the same wire frame reports 68 there.  dos_machine hands over
    // frames with no FCS, so counting one would report bytes that do not exist.
    uint8_t after[8];
    c.dma_read((uint16_t)(0x4700 + 64), after, 8);
    bool untouched = true;
    for (int i = 0; i < 8; i++) if (after[i] != 0xFF) untouched = false;
    diverge(untouched, "no FCS is stored after the payload (the 4 bytes there are untouched RAM)");

    // The next page is untouched: a 64-byte frame occupies one page.
    uint8_t nextpage[4];
    c.dma_read(0x4800, nextpage, 4);
    check(nextpage[0] == 0xFF && nextpage[3] == 0xFF, "the frame did not spill into page 0x48");

    // --- a second frame, two pages long ------------------------------------
    c.w(W_BNRY, 0x47);                 // the driver consumed page 0x47
    c.w(W_ISR, 0xFF);
    std::vector<uint8_t> g = frame(kMac, 300, 0x80);
    c.nic.receive(g.data(), 300);
    check(c.curr() == 0x4A, "a 300-byte frame takes two pages: CURR 0x48 -> 0x4A");

    uint8_t big[304];
    c.dma_read(0x4800, big, 304);
    check(big[0] == 0x01, "second frame: status byte");
    check(big[1] == 0x4A, "second frame: next-page pointer is 0x4A");
    check(big[2] == 0x30 && big[3] == 0x01, "second frame: byte count 304 low/high");
    check(same(big + 4, g.data(), 300), "second frame: 300 payload bytes across two pages");

    // BNRY is entirely the driver's register; the card only reads it.
    c.w(W_BNRY, 0x49);
    check(c.r(R_BNRY) == 0x49, "BNRY round-trips through page 0");

    // --- the whole driver loop: read until BNRY+1 == CURR -------------------
    c.w(W_ISR, 0xFF);
    for (int k = 0; k < 3; k++) {
      std::vector<uint8_t> h = frame(kMac, 100 + k * 40, (uint8_t)(0xC0 + k));
      c.nic.receive(h.data(), 100 + k * 40);
    }
    int drained = 0;
    uint8_t bn = c.r(R_BNRY);
    while (true) {
      uint8_t next_frame = (uint8_t)(bn + 1);
      if (next_frame >= 0xC0) next_frame = 0x46;
      if (next_frame == c.curr()) break;
      uint8_t h4[4];
      c.dma_read((uint16_t)(next_frame << 8), h4, 4);
      int count = h4[2] | (h4[3] << 8);
      std::vector<uint8_t> got((size_t)count);
      c.dma_read((uint16_t)(next_frame << 8), got.data(), count);
      std::vector<uint8_t> want = frame(kMac, 100 + drained * 40, (uint8_t)(0xC0 + drained));
      check(count == 100 + drained * 40 + 4, "drain loop: byte count matches the frame + header");
      check(same(got.data() + 4, want.data(), count - 4), "drain loop: payload matches");
      bn = (uint8_t)(h4[1] - 1);
      if (h4[1] == 0x46) bn = 0xBF;
      c.w(W_BNRY, bn);
      drained++;
      if (drained > 8) break;
    }
    check(drained == 3, "drain loop consumed exactly the three queued frames");
    check((uint8_t)(c.r(R_BNRY) + 1) == c.curr(), "after draining, BNRY + 1 == CURR");
  }

  //=========================================================================
  // 5b. BUG: an unprogrammed CURR lets the receive path eat the MAC PROM
  //=========================================================================
  {
    // dma_write_byte refuses any address below BUF_START (0x4000) so the host
    // cannot write the PROM.  receive() writes the ring through a lambda whose
    // only guard is `a < MEM_TOTAL`, so it can.  CURR is 0 out of reset, which
    // makes page 0 — the PROM — the destination for the first frame if the
    // driver starts the card before programming page 1.  The MAC the driver is
    // about to read out of the PROM is then the frame's own header.
    Card c;
    c.nic.set_mac(kMac);
    c.w(R_CR, CR_STP | CR_RD_ABORT);
    c.w(W_PSTART, 0x46);
    c.w(W_PSTOP, 0xC0);
    c.w(W_BNRY, 0x46);
    c.w(W_RCR, RCR_AB);
    c.w(R_CR, CR_STA | CR_RD_ABORT);   // started, CURR still 0

    std::vector<uint8_t> f = frame(kMac, 60, 0x22);
    c.nic.receive(f.data(), 60);
    check((c.r(R_ISR) & I_PRX) != 0, "fixture: the frame was accepted with CURR = 0");

    uint8_t prom[12];
    c.dma_read(0x0000, prom, 12);
    bool intact = true;
    for (int i = 0; i < 6; i++)
      if (prom[i * 2] != kMac[i] || prom[i * 2 + 1] != kMac[i]) intact = false;
    check(intact, "a received frame must not overwrite the read-only MAC PROM at address 0");
  }

  //=========================================================================
  // 6. Ring wrap
  //=========================================================================
  {
    Card c;
    c.init(0x46, 0x4A, RCR_AB);        // a 4-page ring: 0x46, 0x47, 0x48, 0x49
    c.w(W_ISR, 0xFF);

    int wraps = 0;
    bool in_range = true;
    uint8_t seen_next[8];
    for (int k = 0; k < 8; k++) {
      uint8_t at = c.curr();
      std::vector<uint8_t> f = frame(kMac, 60, (uint8_t)(0x40 + k));
      c.nic.receive(f.data(), 60);
      check(c.curr() != at, "ring wrap: each frame advances CURR");

      uint8_t h4[4];
      c.dma_read((uint16_t)(at << 8), h4, 4);
      seen_next[k] = h4[1];
      if (h4[1] < 0x46 || h4[1] >= 0x4A) in_range = false;
      if (h4[1] < at) wraps++;

      uint8_t body[64];
      c.dma_read((uint16_t)(at << 8), body, 64);
      check(same(body + 4, f.data(), 60), "ring wrap: the payload survives the wrap");

      // Consume it, the way the drain loop above does.
      uint8_t bn = (uint8_t)(h4[1] - 1);
      if (h4[1] == 0x46) bn = 0x49;
      c.w(W_BNRY, bn);
    }
    check(in_range, "ring wrap: every next-page pointer stays inside PSTART..PSTOP");
    check(wraps == 2, "ring wrap: eight 1-page frames through a 4-page ring wrapped twice");
    check(seen_next[2] == 0x46, "ring wrap: the frame on page 0x49 points back at PSTART");
    check(c.curr() >= 0x46 && c.curr() < 0x4A, "ring wrap: CURR stays inside the ring");

    // The wrap is at PSTOP exactly: a frame ending on the last page points at
    // PSTART, not past it.
    check(seen_next[6] == 0x46, "ring wrap: and again on the second lap");
  }

  //=========================================================================
  // 6b. A ring that starts at the first byte of buffer RAM
  //=========================================================================
  // PSTART = 0x40 puts the ring's first byte at BUF_START exactly.  Every other
  // section here uses the conventional 0x46, so nothing else ever writes card
  // address 0x4000 - which made `>=` versus `>` in the receive-path guard an
  // undetectable change.  It is a legal configuration: PSTART only has to be at
  // or above the first buffer page, and a driver that puts its transmit buffer
  // elsewhere can start the receive ring here.
  {
    Card c;
    c.init(0x40, 0x43, RCR_AB);        // 3 pages: 0x40, 0x41, 0x42
    c.w(W_ISR, 0xFF);
    // init leaves CURR = PSTART + 1 and BNRY = PSTART, which is the DP8390
    // convention that keeps "empty" distinguishable from "full".  So two frames
    // have to go round before CURR reaches 0x40 and a write actually touches
    // card address 4000h.
    for (int k = 0; k < 2; k++) {
      std::vector<uint8_t> pre = frame(kMac, 60, (uint8_t)(0x90 + k));
      c.nic.receive(pre.data(), 60);
      uint8_t h[4];
      c.dma_read((uint16_t)((0x41 + k) << 8), h, 4);
      c.w(W_BNRY, (uint8_t)(0x41 + k));   // drain it
    }
    check(c.curr() == 0x40, "PSTART=0x40: CURR wrapped round to PSTART");

    std::vector<uint8_t> f = frame(kMac, 60, 0x71);
    c.nic.receive(f.data(), 60);
    check((c.r(R_ISR) & I_PRX) != 0, "PSTART=0x40: the frame on the first page is accepted");

    uint8_t body[64];
    c.dma_read(0x4000, body, 64);
    check(body[0] == 0x01, "PSTART=0x40: the status byte lands at card address 4000h");
    check(body[1] == 0x41, "PSTART=0x40: the next-page pointer is 41h");
    check((body[2] | (body[3] << 8)) == 64, "PSTART=0x40: the byte count is 64");
    check(same(body + 4, f.data(), 60),
          "PSTART=0x40: the payload starts at 4004h, byte for byte");
    check(c.curr() == 0x41, "PSTART=0x40: CURR advanced one page");
  }

  //=========================================================================
  // 7. Ring overflow
  //=========================================================================
  {
    Card c;
    c.init(0x46, 0x4A, RCR_AB);        // 4 pages, BNRY parked at 0x46
    c.w(W_ISR, 0xFF);

    std::vector<uint8_t> f = frame(kMac, 60, 0x01);
    c.nic.receive(f.data(), 60);
    check(c.curr() == 0x48, "overflow: first frame accepted");
    c.nic.receive(f.data(), 60);
    check(c.curr() == 0x49, "overflow: second frame accepted");
    check((c.r(R_ISR) & I_OVW) == 0, "overflow: OVW still clear with a page to spare");

    // The third has nowhere to go: CURR would reach BNRY.
    c.w(W_ISR, 0xFF);
    c.nic.receive(f.data(), 60);
    check((c.r(R_ISR) & I_OVW) != 0, "overflow: ISR.OVW set when the ring is full");
    check((c.r(R_ISR) & I_PRX) == 0, "overflow: the dropped frame does not raise PRX");
    check(c.curr() == 0x49, "overflow: CURR does not move for a dropped frame");

    uint8_t page[8];
    c.dma_read(0x4900, page, 8);
    bool blank = true;
    for (int i = 0; i < 8; i++) if (page[i] != 0xFF) blank = false;
    check(blank, "overflow: nothing was written to the page that had no room");

    // What a real DP8390 does next is stop receiving until the driver runs the
    // overflow-recovery sequence (stop, clear RBCR, loopback, start, drain the
    // ring, clear OVW, leave loopback).  Here the card simply resumes the
    // instant BNRY moves, and the frame it dropped is not counted anywhere:
    // CNTR2 is the missed-packet counter and is hardwired to 0.
    diverge(c.r(R_CNTR2) == 0x00, "overflow: the missed frame does not appear in CNTR2");
    diverge((c.r(R_ISR) & I_RXE) == 0, "overflow: no receive error is reported alongside OVW");
    diverge(c.r(R_RSR) == 0x01, "overflow: RSR still shows the last GOOD frame's status");

    c.w(W_BNRY, 0x47);                 // driver consumes one page
    c.w(W_ISR, 0xFF);
    c.nic.receive(f.data(), 60);
    diverge((c.r(R_ISR) & I_PRX) != 0 && c.curr() == 0x46,
            "overflow: reception resumes as soon as BNRY moves, with no recovery sequence");
  }

  //=========================================================================
  // 8. Interrupts
  //=========================================================================
  {
    Card c;
    c.init(0x46, 0xC0, RCR_AB);
    c.w(W_ISR, 0xFF);
    c.w(W_IMR, 0x00);

    std::vector<uint8_t> f = frame(kMac, 60, 0x05);
    c.nic.receive(f.data(), 60);
    check((c.r(R_ISR) & I_PRX) != 0, "ISR.PRX latches even when IMR masks it");
    check(!c.nic.irq_active(), "irq_active() false while IMR = 0");

    c.w(W_IMR, I_PRX);
    check(c.nic.irq_active(), "irq_active() true once IMR.PRX is set over a latched PRX");

    // Write-1-to-clear, one bit at a time.
    c.w(W_ISR, I_PTX);
    check((c.r(R_ISR) & I_PRX) != 0, "writing a 0 bit to ISR leaves PRX alone");
    check(c.nic.irq_active(), "and the IRQ is still asserted");
    c.w(W_ISR, I_PRX);
    check((c.r(R_ISR) & I_PRX) == 0, "writing a 1 bit to ISR clears PRX");
    check(!c.nic.irq_active(), "clearing the only unmasked bit drops the IRQ");

    // Several bits at once, cleared selectively.
    c.w(W_ISR, 0xFF);
    c.nic.receive(f.data(), 60);                       // PRX
    c.w(W_TPSR, 0x40);
    c.w(W_TBCR0, 64); c.w(W_TBCR1, 0);
    c.w(R_CR, CR_TXP | CR_STA | CR_RD_ABORT);          // PTX
    uint8_t bufx[4];
    c.dma_read(0x4000, bufx, 4);                       // RDC
    check((c.r(R_ISR) & (I_PRX | I_PTX | I_RDC)) == (I_PRX | I_PTX | I_RDC),
          "PRX, PTX and RDC latch together");
    c.w(W_ISR, I_PTX);
    check((c.r(R_ISR) & (I_PRX | I_PTX | I_RDC)) == (I_PRX | I_RDC),
          "clearing PTX leaves PRX and RDC set");
    c.w(W_ISR, I_PRX | I_RDC);
    check((c.r(R_ISR) & (I_PRX | I_PTX | I_RDC)) == 0, "clearing the other two empties the ISR");

    // The mask is an AND, not a latch: masking a set bit drops the line.
    c.w(W_ISR, 0xFF);
    c.nic.receive(f.data(), 60);
    c.w(W_IMR, I_PRX);
    check(c.nic.irq_active(), "IMR.PRX over a set PRX asserts");
    c.w(W_IMR, I_PTX);
    check(!c.nic.irq_active(), "moving the mask to PTX de-asserts without touching the ISR");
    check((c.r(R_ISR) & I_PRX) != 0, "and PRX is still latched underneath");
    c.w(W_IMR, 0x00);
  }
  {
    // ISR.RST is documented as NIC state — set when the NIC enters the reset
    // state, cleared when a start command is issued — rather than a
    // write-1-to-clear event bit.  Here `isr &= ~value` clears it like any
    // other bit, which every driver does on the way past with ISR = 0xFF.
    Card c;
    check(c.r(R_ISR) == I_RST, "fixture: RST set out of reset");
    c.w(W_ISR, I_RST);
    diverge(c.r(R_ISR) == 0x00, "writing 1 to ISR.RST clears it");

    // A start command clears RST, which is the documented behaviour.
    Card d;
    d.w(R_CR, CR_STA | CR_RD_ABORT);
    check(d.r(R_ISR) == 0x00, "a start command clears ISR.RST");

    Card e;
    e.w(R_CR, CR_STA | CR_STP | CR_RD_ABORT);
    check(e.r(R_ISR) == I_RST, "STA together with STP does not clear ISR.RST");

    // IMR has seven bits on a real DP8390; bit 7 is reserved and there is no
    // way to interrupt on RST.  Here irq_active() is a plain `isr & imr`, so
    // IMR = 0x80 asserts the line on the reset status bit.
    Card g;
    g.w(W_IMR, 0x80);
    diverge(g.nic.irq_active(), "IMR bit 7 is honoured and asserts the IRQ on ISR.RST");
  }

  //=========================================================================
  // 9. Receive filtering
  //=========================================================================
  {
    // A helper that runs one frame through a freshly initialised card and says
    // whether it landed in the ring.
    auto accepts = [](const uint8_t *dst, int len, uint8_t rcr, bool started,
                      const uint8_t *mar) -> bool {
      Card c;
      c.init(0x46, 0xC0, rcr);
      if (mar) {
        c.sel(1);
        for (int i = 0; i < 8; i++) c.w(P1_MAR0 + i, mar[i]);
        c.sel(0);
      }
      if (!started) c.w(R_CR, CR_STP | CR_RD_ABORT);
      c.w(W_ISR, 0xFF);
      std::vector<uint8_t> f = frame(dst, len, 0x60);
      c.nic.receive(f.data(), len);
      return (c.r(R_ISR) & I_PRX) != 0 && c.curr() != 0x47;
    };

    // Unicast to our own PAR is accepted with RCR = 0: that is the default
    // address filter and it needs no enable bit.
    check(accepts(kMac, 60, 0x00, true, nullptr), "unicast to PAR accepted with RCR = 0");
    check(!accepts(kOther, 60, 0x00, true, nullptr), "unicast to another station rejected");
    check(!accepts(kBcast, 60, 0x00, true, nullptr), "broadcast rejected without RCR.AB");
    check(accepts(kBcast, 60, RCR_AB, true, nullptr), "broadcast accepted with RCR.AB");
    check(!accepts(kMcast, 60, RCR_AB, true, nullptr), "multicast rejected without RCR.AM");
    check(accepts(kMcast, 60, RCR_AB | RCR_AM, true, nullptr), "multicast accepted with RCR.AM");
    check(!accepts(kOther, 60, RCR_AB | RCR_AM, true, nullptr),
          "a foreign unicast is still rejected with AB and AM set");
    check(accepts(kOther, 60, RCR_PRO, true, nullptr), "RCR.PRO accepts a foreign unicast");
    check(accepts(kMac, 60, RCR_PRO, true, nullptr), "RCR.PRO still accepts our own");
    check(!accepts(kMac, 60, RCR_MON, true, nullptr), "RCR.MON stores nothing");
    check(!accepts(kMac, 60, RCR_PRO | RCR_MON, true, nullptr), "RCR.MON beats RCR.PRO");
    check(!accepts(kMac, 60, RCR_AB, false, nullptr), "a stopped NIC (CR.STP) stores nothing");

    // The 64-bit multicast hash does not exist.  MAR0-7 round-trip through page
    // 1 (asserted in section 1) and the filter never looks at them, so RCR.AM
    // is an accept-all-multicast switch.  A real DP8390 hashes the destination
    // address with CRC-32 and tests the corresponding MAR bit, so an all-zero
    // MAR rejects every multicast address.
    const uint8_t mar_zero[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    diverge(accepts(kMcast, 60, RCR_AB | RCR_AM, true, mar_zero),
            "multicast passes on RCR.AM alone; MAR0-7 are never consulted");

    // Short frames are dropped with no trace.  A real card stores the runt and
    // flags it, or counts it — either way the driver can tell it happened.
    check(!accepts(kMac, 13, RCR_AB, true, nullptr), "a 13-byte frame is dropped");
    check(accepts(kMac, 14, RCR_AB, true, nullptr), "a 14-byte frame (header only) is stored");
    {
      Card c;
      c.init(0x46, 0xC0, RCR_AB);
      c.w(W_ISR, 0xFF);
      std::vector<uint8_t> f = frame(kMac, 13, 0x11);
      c.nic.receive(f.data(), 13);
      diverge(c.r(R_ISR) == 0x00 && c.r(R_RSR) == 0x00 && c.r(R_CNTR0) == 0x00,
              "a runt is dropped silently: no ISR bit, no RSR status, no counter");
    }

    // The receive path checks CR.STP but not CR.STA, so a card that has been
    // written a CR with neither bit set still receives.
    {
      Card c;
      c.init(0x46, 0xC0, RCR_AB);
      c.w(R_CR, CR_RD_ABORT);          // neither STA nor STP
      c.w(W_ISR, 0xFF);
      std::vector<uint8_t> f = frame(kMac, 60, 0x12);
      c.nic.receive(f.data(), 60);
      diverge((c.r(R_ISR) & I_PRX) != 0, "receive only tests CR.STP, never CR.STA");
    }

    // Oversize frames are not filtered either: anything that fits in the ring
    // is stored, so a 2000-byte frame lands in the ring with a byte count a
    // real card could not have produced.
    {
      Card c;
      c.init(0x46, 0xC0, RCR_AB);
      c.w(W_ISR, 0xFF);
      std::vector<uint8_t> f = frame(kMac, 2000, 0x13);
      c.nic.receive(f.data(), 2000);
      uint8_t h4[4];
      c.dma_read(0x4700, h4, 4);
      diverge((c.r(R_ISR) & I_PRX) != 0 && (h4[2] | (h4[3] << 8)) == 2004,
              "a 2000-byte frame is stored whole (no maximum-length check)");
    }
  }

  //=========================================================================
  std::printf("\n");
  if (g_bug_asserts) {
    std::printf("known bugs still present: %d (expected %d)\n",
                g_bugs_hit, KNOWN_BUGS_EXPECTED);
  }
  bool baseline_ok = (g_bugs_hit == KNOWN_BUGS_EXPECTED);
  if (!baseline_ok) {
    std::printf("FAIL: known-bug count is %d, baseline says %d — %s\n",
                g_bugs_hit, KNOWN_BUGS_EXPECTED,
                g_bugs_fixed ? "a bug was fixed; lower KNOWN_BUGS_EXPECTED"
                             : "update the baseline deliberately");
  }
  if (g_failures == 0 && baseline_ok) {
    std::printf("ALL NE2000 TESTS PASS (%d checks)\n", g_checks);
    return 0;
  }
  std::printf("NE2000 TESTS FAILED: %d of %d checks failed\n", g_failures, g_checks);
  return 1;
}
