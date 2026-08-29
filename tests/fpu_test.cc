// fpu_test.cc — unit harness for emu88/emu88_fpu.cc (x87 FPU emulation).
//
// Build & run:
//   g++ -std=c++20 -O2 -Wall -Wextra -I emu88 tests/fpu_test.cc
//       emu88/emu88.cc emu88/emu88_pmode.cc emu88/emu88_fpu.cc
//       emu88/emu88_mem.cc -o /tmp/fpu_test && /tmp/fpu_test
//
// To wire into the suite, add to tests/build.sh next to the other unit tests:
//   $CXX $CXXFLAGS tests/fpu_test.cc $CORE -o "$OUT/fpu_test" && echo "built $OUT/fpu_test"
// and add fpu_test to the `for t in opl_unit sb_unit ...` list in
// tests/run_suites.sh.  It needs no corpus and runs in well under a second.
//
// Everything here is driven by EXECUTING REAL OPCODE BYTES: the encoding is
// stored at CS:0, IP is pointed at it and emu88::execute() runs it.  That
// exercises the 0xD8-0xDF escape dispatch, the modrm decoder (16-bit disp16,
// [BX+disp8], a segment override, and one 0x67 32-bit-addressing form) and the
// FPU handler together, rather than poking execute_fpu() behind the decoder's
// back.
//
// ---------------------------------------------------------------------------
// WHAT THIS EMULATOR IS, AND WHAT THESE TESTS THEREFORE ASSERT
// ---------------------------------------------------------------------------
// emu88.h declares:  struct FPUState { f80 regs[8]; uint8_t tags[8];
//                                      uint16_t cw; uint16_t sw; ... }
// The x87 register stack is 80-BIT DOUBLE EXTENDED PRECISION, implemented as
// an integer soft float in emu88/emu88_f80.h.  It used to be host `double`,
// and this file used to carry thirty-one diverge() assertions pinning the
// places where 53 mantissa bits showed through.  There are none left: every
// one of them is an ordinary check() on the 387's answer now.
//
// What that changed, concretely, and what this file therefore asserts:
//
//   - FLD/FSTP m80real are ten-byte moves, so NaN payloads, signalling NaNs,
//     denormals and the unsupported encodings survive a round trip (section 5);
//   - the seven FLD constants are the 387's ROM values to all 64 bits, not
//     doubles widened (section 6);
//   - pushing onto a live register is a stack overflow and reading an empty
//     one is a stack underflow, with the IE|SF and C1 a 387 reports (section 2);
//   - precision control rounds the significand to 24 or 53 bits inside the
//     full 15-bit exponent range, which is a thing only a soft float can do
//     (section 13);
//   - FPREM reduces a large exponent difference a bite at a time and reports
//     C2, and leaves the quotient bits in C0/C3/C1 (section 12);
//   - the transcendentals are evaluated in the emulator's own arithmetic
//     rather than through the host's double libm, to a worst observed 4 ulp
//     of a 64-bit significand (section 14; tests/f80_unit.cc measures it);
//   - FNSTENV and FLDENV write and read all seven environment fields, in the
//     layout the operand size and CR0.PE select, with the tag word in physical
//     register order (section 17);
//   - the denormal, unsupported, overflow and gradual-underflow classes exist
//     at all, and section 21 is about nothing else.
//
// So there are two kinds of assertion left in this file, not three:
//
//   check()   — this implementation is right, and a plausible bug flips it.
//   bug()     — a defect this harness records deliberately.  The assertion
//               states the CORRECT (real-387) behaviour, is expected to fail,
//               is reported as "KNOWN BUG", and is held to a baseline count
//               exactly the way tests/run_suites.sh holds SingleStepTests to
//               SST_BASELINE: if a bug gets fixed the count drops, the harness
//               FAILS, and KNOWN_BUGS_EXPECTED has to be lowered deliberately.
//               A known bug can therefore never quietly become "the way it
//               works".  THE LEDGER IS EMPTY: KNOWN_BUGS_EXPECTED is 0.
//
// diverge() is kept, unused, for the same reason bug() is: it is the shape a
// future deliberate divergence should take.  What it must NOT be used for
// again is a register format.
//
// WHAT THIS FILE STILL DOES NOT COVER
// -----------------------------------
// The arithmetic itself.  This harness drives real opcode bytes through the
// decoder and checks what lands in the register file; it does not grade
// add/sub/mul/div/sqrt against anything.  tests/f80_unit.cc does that, against
// the host's own x87.  The two are complementary and neither replaces the
// other: this one owns the decode, the stack and the status word, that one
// owns the numbers.
//
// Unmasked exceptions used to be absent from this file, because they were
// absent from emu88.  They are here now: section 17b covers ES and B as a
// RECOMPUTED function of the status and control words rather than a latch, and
// section 19b covers #MF delivery - the deferral to the next waiting
// instruction, the ten no-wait encodings, and the priority against an operand
// fault.  What this file still cannot reach is the AT's IRQ13 route, because
// that lives in dos_machine and this harness builds a bare emu88;
// tests/bios_test.cc drives that end to end.
//
// Exit code is non-zero if any check()/diverge() fails, or if the number of
// known bugs still present is not exactly KNOWN_BUGS_EXPECTED.

#include "emu88.h"
#include "emu88_mem.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>

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

// A pinned divergence from real x87.  Same gate as check(): it asserts what
// THIS implementation does.  Separate name so the divergences are greppable.
[[maybe_unused]] static void diverge(bool cond, const char *what) {
  g_checks++;
  if (!cond) {
    g_failures++;
    std::printf("  FAIL (pinned divergence changed): %s\n", what);
  }
}

// `behaves_correctly` states the REAL-387 behaviour, which is expected to be
// false today.  If it ever becomes true the bug was fixed and the baseline is
// stale — that fails the run, loudly.
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
// Test machine: 16MB flat RAM, A20 on (the convention in tests/sst386.cc)
//===========================================================================

class TestMem : public emu88_mem {
public:
  TestMem() : emu88_mem(0x1000000) { set_a20(true); }
};

class TestCpu : public emu88 {
public:
  TestCpu(emu88_mem *m) : emu88(m) {}
  emu88_uint8 port_in(emu88_uint16) override { return 0xFF; }
  emu88_uint16 port_in16(emu88_uint16) override { return 0xFFFF; }
  void port_out(emu88_uint16, emu88_uint8) override {}
  void port_out16(emu88_uint16, emu88_uint16) override {}
};

static TestMem *mem;
static TestCpu *cpu;

static const uint16_t CS_SEL = 0x2000, DS_SEL = 0x3000, ES_SEL = 0x3100;
static const uint32_t CS_LIN = 0x20000, DS_LIN = 0x30000, ES_LIN = 0x31000;

static void setup() {
  cpu->reset();                       // reset() calls fpu_init()
  cpu->cpu_type = emu88::CPU_386;
  cpu->lock_ud = true;
  cpu->load_segment_real(emu88::seg_CS, CS_SEL);
  cpu->load_segment_real(emu88::seg_DS, DS_SEL);
  cpu->load_segment_real(emu88::seg_ES, ES_SEL);
  cpu->load_segment_real(emu88::seg_SS, DS_SEL);
  cpu->set_reg16(emu88::reg_SP, 0xFFF0);
  cpu->flags = 0x0002;
}

// Execute exactly one instruction assembled at CS:0000.
static void run(std::initializer_list<uint8_t> bytes) {
  uint32_t a = CS_LIN;
  for (uint8_t b : bytes) mem->store_mem(a++, b);
  cpu->ip = 0;
  cpu->execute();
}

//===========================================================================
// Encodings.  mod=00 rm=110 is [disp16] in DS — the shortest memory form.
//===========================================================================

static uint8_t mrm_disp16(uint8_t reg) { return (uint8_t)(0x06 | (reg << 3)); }

// esc /reg with a DS:disp16 operand.
static void opm(uint8_t esc, uint8_t reg, uint16_t off) {
  run({esc, mrm_disp16(reg), (uint8_t)(off & 0xFF), (uint8_t)(off >> 8)});
}
// esc with an explicit register-form modrm byte (>= 0xC0).
static void opr(uint8_t esc, uint8_t modrm) { run({esc, modrm}); }

//===========================================================================
// Guest memory helpers (all relative to DS unless stated)
//===========================================================================

static void wr8(uint16_t off, uint8_t v)    { mem->store_mem(DS_LIN + off, v); }
static uint8_t rd8(uint16_t off)            { return mem->fetch_mem(DS_LIN + off); }
static void wr16(uint16_t off, uint16_t v)  { mem->store_mem16(DS_LIN + off, v); }
static uint16_t rd16(uint16_t off)          { return mem->fetch_mem16(DS_LIN + off); }
static void wr32(uint16_t off, uint32_t v)  { mem->store_mem32(DS_LIN + off, v); }
static uint32_t rd32(uint16_t off)          { return mem->fetch_mem32(DS_LIN + off); }
static void wr64(uint16_t off, uint64_t v) {
  wr32(off, (uint32_t)v); wr32((uint16_t)(off + 4), (uint32_t)(v >> 32));
}
static uint64_t rd64(uint16_t off) {
  return (uint64_t)rd32(off) | ((uint64_t)rd32((uint16_t)(off + 4)) << 32);
}

static uint64_t dbits(double d) { uint64_t r; std::memcpy(&r, &d, 8); return r; }
static double   dfrom(uint64_t r) { double d; std::memcpy(&d, &r, 8); return d; }
static uint32_t fbits(float f)  { uint32_t r; std::memcpy(&r, &f, 4); return r; }
static float    ffrom(uint32_t r) { float f; std::memcpy(&f, &r, 4); return f; }

static void wrf(uint16_t off, float f)   { wr32(off, fbits(f)); }
static float rdf(uint16_t off)           { return ffrom(rd32(off)); }
static void wrd(uint16_t off, double d)  { wr64(off, dbits(d)); }
static double rdd(uint16_t off)          { return dfrom(rd64(off)); }

// 80-bit real: 8 mantissa bytes then the exponent/sign word.
static void wr80(uint16_t off, uint64_t mant, uint16_t exp_sign) {
  for (int i = 0; i < 8; i++) wr8((uint16_t)(off + i), (uint8_t)(mant >> (i * 8)));
  wr16((uint16_t)(off + 8), exp_sign);
}
static uint64_t rd80_mant(uint16_t off) {
  uint64_t m = 0;
  for (int i = 0; i < 8; i++) m |= (uint64_t)rd8((uint16_t)(off + i)) << (i * 8);
  return m;
}
static uint16_t rd80_exp(uint16_t off) { return rd16((uint16_t)(off + 8)); }

//===========================================================================
// FPU state accessors
//===========================================================================

static int      ftop()        { return (cpu->fpu.sw >> 11) & 7; }
static f80      stf(int i)    { return cpu->fpu.regs[(ftop() + i) & 7]; }
static uint8_t  tg(int i)     { return cpu->fpu.tags[(ftop() + i) & 7]; }
static uint16_t sw()          { return cpu->fpu.sw; }
static uint16_t cw()          { return cpu->fpu.cw; }

// The register file is 80-bit now.  Most of this file compares against
// ordinary double literals - 13.0, 2.5, -7.0, 0.4 - and for those the
// narrowing IS the right reading: each is the exact 64-bit result rounded to
// 53 bits, so the assertion means the same thing it always did.  st() keeps
// its name and its type; the assertions that need the extra eleven bits say so
// by using st_is() instead.
static double st(int i) {
  f80_ctx t = f80_ctx_make(0x037F);
  double d;
  uint64_t b = f80_to_f64(stf(i), t);
  std::memcpy(&d, &b, 8);
  return d;
}
// Exact 80-bit compare against the stored significand and sign/exponent word.
static bool st_is(int i, uint16_t se, uint64_t sig) {
  f80 v = stf(i);
  return v.se == se && v.sig == sig;
}
static bool reg_is(int p, uint16_t se, uint64_t sig) {
  return cpu->fpu.regs[p].se == se && cpu->fpu.regs[p].sig == sig;
}
// The real indefinite QNaN: the masked #IA result everywhere it can occur.
static bool st_indef(int i) { return st_is(i, 0xFFFF, 0xC000000000000000ULL); }
// Seed an exact 80-bit value into ST(0) through a real FLD m80real.
static const uint16_t SCRATCH80 = 0x0160;
static void push80(uint16_t se, uint64_t sig);

enum { TAG_VALID = 0, TAG_ZERO = 1, TAG_SPECIAL = 2, TAG_EMPTY = 3 };

// C3 C2 C1 C0 packed into bits 3..0 — one readable number per condition code.
static int cc() {
  uint16_t s = cpu->fpu.sw;
  return (((s >> 14) & 1) << 3) | (((s >> 10) & 1) << 2) |
         (((s >> 9) & 1) << 1) | ((s >> 8) & 1);
}
#define CC_GT      0x0                 // C3=0 C2=0 C0=0
#define CC_LT      0x1                 // C0
#define CC_EQ      0x8                 // C3
#define CC_UNORD   0xD                 // C3 C2 C0

static const uint16_t SW_IE = 0x0001, SW_DE = 0x0002, SW_ZE = 0x0004,
                     SW_OE = 0x0008, SW_UE = 0x0010, SW_PE = 0x0020,
                     SW_SF = 0x0040, SW_ES = 0x0080, SW_B  = 0x8000;
static const uint16_t SW_C1 = 0x0200, SW_C2 = 0x0400;

// Mnemonic shorthands used a lot below.
static void FNINIT()  { opr(0xDB, 0xE3); }
static void FLD1()    { opr(0xD9, 0xE8); }
static void FLDZ()    { opr(0xD9, 0xEE); }
static void FLDm64(uint16_t o) { opm(0xDD, 0, o); }   // DD /0
static void FSTPm64(uint16_t o){ opm(0xDD, 3, o); }   // DD /3
static void FSTPm80(uint16_t o){ opm(0xDB, 7, o); }   // DB /7
static void FLDm80(uint16_t o) { opm(0xDB, 5, o); }   // DB /5
static void FSTPst0() { opr(0xDD, 0xD8); }            // FSTP ST(0) = pop

// Push a double onto the guest FPU stack by executing a real FLD m64real.
static const uint16_t SCRATCH64 = 0x0110;
static void push(double v) { wrd(SCRATCH64, v); FLDm64(SCRATCH64); }
static void push80(uint16_t se, uint64_t sig) {
  wr80(SCRATCH80, sig, se);
  FLDm80(SCRATCH80);
}

//===========================================================================

int main() {
  TestMem m; TestCpu c(&m);
  mem = &m; cpu = &c;
  setup();

  //=========================================================================
  // 1. fpu_init(), FNINIT, FNCLEX
  //=========================================================================
  {
    check(cw() == 0x037F, "fpu_init: CW = 0x037F (masked, 64-bit PC, round nearest)");
    check(sw() == 0x0000, "fpu_init: SW = 0");
    check(ftop() == 0, "fpu_init: TOP = 0");
    bool all_empty = true, all_zero = true;
    for (int i = 0; i < 8; i++) {
      if (cpu->fpu.tags[i] != TAG_EMPTY) all_empty = false;
      if (!reg_is(i, 0x0000, 0)) all_zero = false;
    }
    check(all_empty, "reset: all 8 tags are TAG_EMPTY");
    check(all_zero, "reset: all 8 registers are +0.0");

    // RESET and FNINIT are not the same thing.  RESET gives the data
    // registers +0.0; FNINIT marks them empty and leaves their CONTENTS
    // alone, which is visible through an FNSAVE taken afterwards.
    push80(0x4000, 0xDEADBEEFDEADBEEFULL);
    int phys = ftop();
    FNINIT();
    check(tg(0) == TAG_EMPTY, "FNINIT empties the tag");
    check(reg_is(phys, 0x4000, 0xDEADBEEFDEADBEEFULL),
          "...but leaves the register's contents where they were");

    // Dirty every field, then FNINIT must restore all of it.
    push(1.5); push(-2.5); push(3.5);
    cpu->fpu.cw = 0x0F3F;
    cpu->fpu.sw |= SW_ZE | SW_IE;
    check(ftop() == 5, "three pushes moved TOP to 5");
    FNINIT();
    check(cw() == 0x037F, "FNINIT restores CW 0x037F");
    check(sw() == 0x0000, "FNINIT clears SW (TOP back to 0)");
    check(tg(0) == TAG_EMPTY && tg(3) == TAG_EMPTY, "FNINIT empties the tags");

    // FNCLEX clears the exception bits, leaves TOP and C3 alone.
    push(7.0);                       // TOP = 7
    cpu->fpu.sw |= SW_ZE | SW_IE | SW_SF | SW_ES | SW_B;
    cpu->fpu.sw |= 0x4000;           // C3
    opr(0xDB, 0xE2);                 // FNCLEX
    check((sw() & (SW_ZE | SW_IE | SW_SF | SW_ES | SW_B)) == 0,
          "FNCLEX clears IE/ZE/SF/ES/B");
    check(ftop() == 7, "FNCLEX preserves TOP");
    check((sw() & 0x4000) != 0, "FNCLEX preserves C3 (this impl leaves C0-C3)");
    check(st(0) == 7.0, "FNCLEX does not disturb ST(0)");
  }

  //=========================================================================
  // 2. Stack discipline: TOP, wraparound, tags, overflow
  //=========================================================================
  {
    FNINIT();
    const int want_top[8] = {7, 6, 5, 4, 3, 2, 1, 0};
    bool top_ok = true, tag_ok = true;
    for (int i = 0; i < 8; i++) {
      push((double)(i + 1));
      if (ftop() != want_top[i]) top_ok = false;
      if (tg(0) != TAG_VALID) tag_ok = false;
    }
    check(top_ok, "8 pushes walk TOP 7,6,5,4,3,2,1,0 (wraps past 0)");
    check(tag_ok, "each push tags the new ST(0) TAG_VALID");
    check(st(0) == 8.0 && st(7) == 1.0, "full stack: ST(0)=8, ST(7)=1");
    check(reg_is(0, 0x4002, 0x8000000000000000ULL) &&
          reg_is(7, 0x3FFF, 0x8000000000000000ULL),
          "physical regs[0]=8 (last push), regs[7]=1 (first push)");

    // A 9th push is a stack overflow.  TOP still decrements, but the
    // destination receives the indefinite QNaN rather than the operand, and
    // IE|SF are raised with C1 SET - C1 is what tells an overflow from an
    // underflow, and the four assertions below pinned its absence until the
    // register file was rewritten.
    push(99.0);
    check(ftop() == 7, "9th push still moves TOP to 7");
    check(st_indef(0), "9th push writes the indefinite, not the operand");
    check((sw() & (SW_IE | SW_SF)) == (SW_IE | SW_SF), "9th push sets IE and SF");
    check((sw() & SW_C1) != 0, "9th push sets C1: this is an OVERFLOW");

    // And the other direction: reading an empty register is a stack
    // underflow, which sets the same two flags and CLEARS C1.
    FNINIT();
    push(1.0);
    opr(0xD8, 0xC1);                  // FADD ST(0), ST(1) — ST(1) is empty
    check((sw() & (SW_IE | SW_SF)) == (SW_IE | SW_SF), "reading an empty register sets IE and SF");
    check((sw() & SW_C1) == 0, "stack underflow clears C1");
    check(st_indef(0), "the underflowing operand delivers the indefinite");

    // FCHS and FABS are the two instructions that reach the sign bit without
    // going through an f80_* routine, so they are the two that can deform the
    // substitute they were just handed.  The masked #IS response is the
    // indefinite as it is - sign set - not a sign-manipulated copy of it.
    // Checked against the host: fninit; fchs; fnsave leaves FFFF:C000...
    FNINIT();
    opr(0xD9, 0xE0);                  // FCHS on an empty stack
    check((sw() & (SW_IE | SW_SF)) == (SW_IE | SW_SF), "FCHS on empty ST(0) sets IE and SF");
    check(st_indef(0), "FCHS on empty ST(0) leaves the indefinite, not a positive QNaN");
    FNINIT();
    opr(0xD9, 0xE1);                  // FABS on an empty stack
    check((sw() & (SW_IE | SW_SF)) == (SW_IE | SW_SF), "FABS on empty ST(0) sets IE and SF");
    check(st_indef(0), "FABS on empty ST(0) leaves the indefinite, not a positive QNaN");

    // The masked #IS response is the indefinite UNCONDITIONALLY, and that is
    // not what falls out of substituting it for the empty operand: the f80
    // primitive then runs its ordinary two-NaN tie-break on the substitute, so
    // any QNaN in the live register with a significand above C000000000000000
    // outranks it and lands in the destination.  Measured on the host, which
    // delivers FFFF:C000000000000000 there.
    FNINIT();
    push80(0x7FFF, 0xFFFFFFFFFFFFFFFFULL);      // ST(0) = a big QNaN
    push(1.0);                                  // ST(0) = 1.0, ST(1) = the QNaN
    opr(0xDD, 0xC0);                            // FFREE ST(0)
    opr(0xD8, 0xC1);                            // FADD ST(0), ST(1)
    check(st_indef(0), "a stack underflow outranks the surviving NaN operand");
    check((sw() & (SW_IE | SW_SF)) == (SW_IE | SW_SF), "...and reports IE|SF");
    check((sw() & SW_PE) == 0, "...and raises no PE for an operation it aborted");

    // A signalling NaN arriving as an m32 OPERAND must not be quieted before
    // the two-NaN tie-break runs.  The rule compares significands, and setting
    // the quiet bit first lifts the memory NaN above ST(0) and hands it the
    // result.  Measured on the host: ST(0)=C000000000000001 against the m32
    // SNaN 7F800001 delivers ST(0)'s NaN, and emu88 delivered the memory one.
    // The control below is the same shape with a QUIET memory NaN, where the
    // memory NaN legitimately does win.
    FNINIT();
    push80(0x7FFF, 0xC000000000000001ULL);      // ST(0) = a QNaN, small payload
    wr32(SCRATCH64, 0x7F800001u);               // an m32 SNaN
    opm(0xD8, 0, SCRATCH64);                    // FADD m32real
    check(st_is(0, 0x7FFF, 0xC000000000000001ULL),
          "an m32 SNaN operand does not outrank ST(0) by being quieted first");
    check((sw() & SW_IE) != 0, "...and the SNaN still raises #IA");
    FNINIT();
    push80(0x7FFF, 0xC000000000000001ULL);
    wr32(SCRATCH64, 0x7FC00001u);               // an m32 QUIET NaN, larger
    opm(0xD8, 0, SCRATCH64);
    check(st_is(0, 0x7FFF, 0xC000010000000000ULL),
          "...while a larger quiet m32 NaN still wins on significand");

    // #D is the lowest-priority report and a higher-priority one displaces it.
    // The memory-operand helpers raise DE into the same context BEFORE the
    // arithmetic decides to raise #IS, so a denormal operand used to be
    // reported beside a stack fault.  The host reports IE alone for both of
    // these, and DE|PE for the same FADD against a live ST(0).
    FNINIT();
    for (int i = 0; i < 8; i++) push(1.0);      // stack full
    wr32(SCRATCH64, 0x802CCD94u);               // a denormal single
    opm(0xD9, 0, SCRATCH64);                    // FLD m32real -> stack overflow
    check((sw() & SW_IE) != 0, "FLD m32 of a denormal onto a full stack raises IE");
    check((sw() & SW_DE) == 0, "...and the stack fault suppresses #D");

    FNINIT();
    wr32(SCRATCH64, 0x802CCD94u);
    opm(0xD8, 0, SCRATCH64);                    // FADD m32real, ST(0) empty
    check((sw() & SW_IE) != 0, "FADD m32 of a denormal with ST(0) empty raises IE");
    check((sw() & SW_DE) == 0, "...and the stack fault suppresses #D there too");

    FNINIT();                                   // the control: a live ST(0)
    push(1.0);
    wr32(SCRATCH64, 0x802CCD94u);
    opm(0xD8, 0, SCRATCH64);
    check((sw() & SW_DE) != 0, "...while a live ST(0) still reports the denormal");

    // When ONE instruction raises both faults - it reads an empty register and
    // then pushes onto a full stack - the FIRST one owns C1.  fpu_get already
    // latched an underflow and refused to be overwritten; fpu_push did not,
    // so the later overflow displaced it and C1 came out set.  Measured on the
    // host for FLD ST(i), FXTRACT and FPTAN: SW=3841, C1 clear.
    FNINIT();
    for (int i = 0; i < 8; i++) push(1.0);      // stack full, TOP = 0
    opr(0xDD, 0xC0);                            // FFREE ST(0): empty it, still 7 live
    opr(0xD9, 0xC0);                            // FLD ST(0): reads empty, pushes onto full
    check((sw() & (SW_IE | SW_SF)) == (SW_IE | SW_SF),
          "reading empty and then pushing onto full raises IE and SF");
    check((sw() & SW_C1) == 0,
          "...and C1 reports the FIRST fault, the underflow, not the overflow");

    // The two-result instructions write one result and push the other, so a
    // stack fault has to claim BOTH destinations - and the arithmetic they had
    // already finished has to go with it.  FPTAN and FSINCOS in particular had
    // the transcendental's own inexactness sitting in the context, which
    // reached the status word as a PE the instruction never earned.  Measured
    // on the host for all three, on a full stack and an empty one: SW is IE|SF
    // plus the C1 direction bit, PE clear, and both registers the indefinite.
    {
      static const uint8_t two_op[3] = { 0xF4, 0xF2, 0xFB };
      static const char *two_nm[3]   = { "FXTRACT", "FPTAN", "FSINCOS" };
      for (int k = 0; k < 3; k++) {
        char msg[96];
        FNINIT();
        for (int i = 0; i < 8; i++) push(1.0);      // stack full -> overflow
        opr(0xD9, two_op[k]);
        std::snprintf(msg, sizeof msg, "%s on a full stack: both results indefinite", two_nm[k]);
        check(st_indef(0) && st_indef(1), msg);
        std::snprintf(msg, sizeof msg, "%s on a full stack: IE|SF with C1 set", two_nm[k]);
        check((sw() & (SW_IE | SW_SF | SW_C1)) == (SW_IE | SW_SF | SW_C1), msg);
        std::snprintf(msg, sizeof msg, "%s on a full stack raises no PE", two_nm[k]);
        check((sw() & SW_PE) == 0, msg);

        FNINIT();                                   // empty stack -> underflow
        opr(0xD9, two_op[k]);
        std::snprintf(msg, sizeof msg, "%s on an empty stack: both results indefinite", two_nm[k]);
        check(st_indef(0) && st_indef(1), msg);
        std::snprintf(msg, sizeof msg, "%s on an empty stack: C1 clear, no PE", two_nm[k]);
        check((sw() & (SW_C1 | SW_PE)) == 0, msg);
      }
    }

    // FCMOVcc reads BOTH operands whatever the condition says, so an empty
    // source is a stack underflow even on the path that moves nothing.
    FNINIT();
    push(1.0);
    cpu->flags |= 0x0001;                       // CF = 1
    opr(0xDB, 0xC1);                            // FCMOVNB ST(0), ST(1): !CF, false
    check((sw() & (SW_IE | SW_SF)) == (SW_IE | SW_SF),
          "FCMOVcc reads its source even when the condition is false");
    // And #IS owns the destination: the indefinite lands in ST(0) whichever
    // operand was empty and whatever the condition decided, rather than the
    // other operand's real value.  Measured on the host for both DA and DB and
    // for CF set and clear.
    check(st_indef(0), "FCMOVcc on a stack underflow delivers the indefinite");
    check(tg(0) == TAG_SPECIAL, "...and tags the destination SPECIAL");
    FNINIT();
    push(1.0);                                  // ST(1) empty, condition TRUE
    opr(0xDA, 0xC1);                            // FCMOVB ST(0), ST(1): CF = 1
    check(st_indef(0), "...on the taken path too");
    cpu->flags &= (uint16_t)~0x0001;

    // The FCOMI family CLEARS C1 rather than leaving it alone.
    FNINIT();
    push(1.0); push(2.0);
    cpu->fpu.sw |= SW_C1;
    opr(0xDB, 0xF1);                            // FCOMI ST(0), ST(1)
    check((sw() & SW_C1) == 0, "FCOMI clears C1");
    FNINIT();
    push(1.0); push(2.0);
    cpu->fpu.sw |= SW_C1;
    opr(0xDF, 0xF1);                            // FCOMIP ST(0), ST(1)
    check((sw() & SW_C1) == 0, "FCOMIP clears C1 too");

    // Popping: FSTP ST(0) empties the slot and advances TOP.
    FNINIT();
    push(4.0);                        // TOP = 7
    push(5.0);                        // TOP = 6
    check(tg(0) == TAG_VALID && tg(1) == TAG_VALID, "two live registers tagged valid");
    FSTPst0();
    check(ftop() == 7, "FSTP ST(0) advanced TOP 6 -> 7");
    check(cpu->fpu.tags[6] == TAG_EMPTY, "FSTP ST(0) tagged the vacated slot EMPTY");
    check(st(0) == 4.0, "after the pop ST(0) is the older value");

    // FINCSTP / FDECSTP move TOP only; tags and values are untouched.
    FNINIT();
    push(1.0); push(2.0);             // TOP=6, ST(0)=2, ST(1)=1
    opr(0xD9, 0xF7);                  // FINCSTP
    check(ftop() == 7, "FINCSTP: TOP 6 -> 7");
    check(st(0) == 1.0, "FINCSTP: ST(0) is now the old ST(1)");
    check(cpu->fpu.tags[6] == TAG_VALID, "FINCSTP does not empty the skipped tag");
    opr(0xD9, 0xF6);                  // FDECSTP
    check(ftop() == 6, "FDECSTP: TOP 7 -> 6");
    check(st(0) == 2.0, "FDECSTP: ST(0) is the old value again");
    opr(0xD9, 0xF6); opr(0xD9, 0xF6); opr(0xD9, 0xF6);
    opr(0xD9, 0xF6); opr(0xD9, 0xF6); opr(0xD9, 0xF6); opr(0xD9, 0xF6);
    check(ftop() == 7, "seven more FDECSTP wrap TOP 6 -> 7 (mod 8)");

    // FFREE marks a slot empty without moving TOP or clearing the value.
    FNINIT();
    push(1.0); push(2.0); push(3.0);  // TOP=5: ST0=3 ST1=2 ST2=1
    opr(0xDD, 0xC1);                  // FFREE ST(1)
    check(ftop() == 5, "FFREE does not move TOP");
    check(tg(1) == TAG_EMPTY, "FFREE ST(1) tags it EMPTY");
    check(st(1) == 2.0, "FFREE leaves the register contents alone");
    check(tg(0) == TAG_VALID && tg(2) == TAG_VALID, "FFREE ST(1) touched no other tag");

    // Every register-form decode range in the handler has the shape
    // "op2 >= 0xN0 && op2 <= 0xN7".  ST(1) only ever probes the low end, so
    // the upper bound of each range is untested unless index 7 is issued too.
    // FFREE ST(7) (DD C7) is the top of the DD C0-C7 range.
    FNINIT();
    for (int i = 1; i <= 8; i++) push((double)i);   // full stack: ST0=8 ... ST7=1
    check(ftop() == 0 && st(0) == 8.0 && st(7) == 1.0,
          "eight pushes leave a full stack with ST(0)=8 and ST(7)=1");
    opr(0xDD, 0xC7);                  // FFREE ST(7)
    check(tg(7) == TAG_EMPTY, "FFREE ST(7) tags the deepest register EMPTY");
    check(ftop() == 0, "FFREE ST(7) does not move TOP");
    check(tg(0) == TAG_VALID && tg(6) == TAG_VALID, "FFREE ST(7) touched no other tag");

    // Tag classification.
    FNINIT();
    push(0.0);   check(tg(0) == TAG_ZERO,    "compute_tag(+0.0) = TAG_ZERO");
    push(-0.0);  check(tg(0) == TAG_ZERO,    "compute_tag(-0.0) = TAG_ZERO");
    push(INFINITY);  check(tg(0) == TAG_SPECIAL, "compute_tag(+inf) = TAG_SPECIAL");
    push(dfrom(0x7FF8000000000000ULL));
    check(tg(0) == TAG_SPECIAL, "compute_tag(NaN) = TAG_SPECIAL");
    push(1e-300); check(tg(0) == TAG_VALID, "compute_tag(tiny normal) = TAG_VALID");
    // A subnormal DOUBLE is not a subnormal in the 80-bit file: FLD m64real
    // normalises it into the wider exponent range, so TAG_VALID is the right
    // answer and always was.  What is new is that the load reports #D, and
    // that a genuine 80-bit denormal - which no double can express and which
    // this harness could not previously construct - tags SPECIAL.
    FNINIT(); push(5e-324);
    check(tg(0) == TAG_VALID, "a subnormal double normalises to a VALID extended value");
    check((sw() & SW_DE) != 0, "...and the load raises #D");
    check(st_is(0, 0x3BCD, 0x8000000000000000ULL), "5e-324 normalises to 2^-1074");
    FNINIT(); push80(0x0000, 0x0000000000000001ULL);
    check(tg(0) == TAG_SPECIAL, "a true 80-bit denormal tags SPECIAL");
    check(st_is(0, 0x0000, 0x0000000000000001ULL), "...and is held verbatim");
    FNINIT(); push80(0x4000, 0x4000000000000000ULL);
    check(tg(0) == TAG_SPECIAL, "an unnormal (J clear, exponent non-zero) tags SPECIAL");
  }

  //=========================================================================
  // 3. FLD / FST / FSTP through m32real, m64real, register-to-register
  //=========================================================================
  {
    FNINIT();
    wrf(0x0100, 1.5f);
    opm(0xD9, 0, 0x0100);             // FLD m32real
    check(st(0) == 1.5, "FLD m32real 1.5f");
    check(ftop() == 7, "FLD m32real pushed");

    // A float that is not exactly a double: FLD must widen, not re-round.
    FNINIT();
    wrf(0x0100, 0.1f);
    opm(0xD9, 0, 0x0100);
    check(dbits(st(0)) == 0x3FB99999A0000000ULL,
          "FLD m32real 0.1f widens to (double)0.1f exactly");

    // FST m32real narrows and does NOT pop; FSTP does pop.
    FNINIT();
    push(1.0 / 3.0);
    wr32(0x0100, 0xDEADBEEF);
    opm(0xD9, 2, 0x0100);             // FST m32real
    check(fbits(rdf(0x0100)) == 0x3EAAAAABU, "FST m32real stores float(1/3) = 0x3EAAAAAB");
    check(ftop() == 7, "FST m32real does not pop");
    check(st(0) == 1.0 / 3.0, "FST m32real leaves ST(0) at full double precision");
    wr32(0x0100, 0);
    opm(0xD9, 3, 0x0100);             // FSTP m32real
    check(fbits(rdf(0x0100)) == 0x3EAAAAABU, "FSTP m32real stored the same float");
    check(ftop() == 0, "FSTP m32real popped");
    check(cpu->fpu.tags[7] == TAG_EMPTY, "FSTP m32real emptied the tag");

    // m64real is the register's native format: bit-exact both ways.
    FNINIT();
    const uint64_t PI53 = 0x400921FB54442D18ULL;
    wr64(0x0110, PI53);
    FLDm64(0x0110);
    check(dbits(st(0)) == PI53, "FLD m64real is bit-exact");
    wr64(0x0118, 0);
    opm(0xDD, 2, 0x0118);             // FST m64real
    check(rd64(0x0118) == PI53, "FST m64real is bit-exact");
    check(ftop() == 7, "FST m64real does not pop");
    FSTPm64(0x0118);
    check(ftop() == 0, "FSTP m64real popped");

    // FLD ST(i) (D9 C0+i) duplicates; FST/FSTP ST(i) (DD D0+i / DD D8+i) copy.
    FNINIT();
    push(11.0); push(22.0);           // ST0=22 ST1=11
    opr(0xD9, 0xC1);                  // FLD ST(1)
    check(st(0) == 11.0, "FLD ST(1) pushed a copy of ST(1)");
    check(st(1) == 22.0 && st(2) == 11.0, "FLD ST(1) left the stack below intact");
    check(tg(0) == TAG_VALID, "FLD ST(i) tags the new top valid");

    // FLD ST(7) (D9 C7) — the far end of the D9 C0-C7 range.  On a FULL
    // stack this is a stack overflow, which is what it is on hardware and what
    // this harness could not express while pushes were unconditional.
    FNINIT();
    for (int i = 1; i <= 8; i++) push((double)i);   // ST0=8 ... ST7=1
    opr(0xD9, 0xC7);                  // FLD ST(7)
    check(ftop() == 7, "FLD ST(7) pushed (TOP 0 -> 7)");
    check(st_indef(0), "FLD ST(7) onto a full stack overflows: indefinite");
    check((sw() & (SW_IE | SW_SF | SW_C1)) == (SW_IE | SW_SF | SW_C1),
          "FLD ST(7) onto a full stack sets IE, SF and C1");
    check(st(1) == 8.0, "FLD ST(7) left the old ST(0) directly below it");

    // The same encoding with room on the stack does what it always did.
    FNINIT();
    for (int i = 1; i <= 7; i++) push((double)i);   // ST0=7 ... ST6=1, ST7 empty
    opr(0xD9, 0xC6);                  // FLD ST(6)
    check(st(0) == 1.0, "FLD ST(6) copied the deepest live register to the top");
    check(tg(0) == TAG_VALID, "FLD ST(i) tags the new top valid");

    FNINIT();
    push(1.0); push(2.0); push(3.0);  // ST0=3 ST1=2 ST2=1
    opr(0xDD, 0xD2);                  // FST ST(2)
    check(st(2) == 3.0, "FST ST(2) copied ST(0) into ST(2)");
    check(ftop() == 5, "FST ST(i) does not pop");
    opr(0xDD, 0xDA);                  // FSTP ST(2)
    check(ftop() == 6, "FSTP ST(i) pops");
    check(st(1) == 3.0, "FSTP ST(2) wrote ST(0) into the old ST(2)");

    // FST ST(7) / FSTP ST(7) — the far ends of the DD D0-D7 and DD D8-DF ranges.
    FNINIT();
    for (int i = 1; i <= 8; i++) push((double)i);   // ST0=8 ... ST7=1
    opr(0xDD, 0xD7);                  // FST ST(7)
    check(st(7) == 8.0, "FST ST(7) copied ST(0) into the deepest register");
    check(ftop() == 0, "FST ST(7) does not pop");
    FNINIT();
    for (int i = 1; i <= 8; i++) push((double)i);
    opr(0xDD, 0xDF);                  // FSTP ST(7)
    check(ftop() == 1, "FSTP ST(7) pops");
    check(st(6) == 8.0, "FSTP ST(7) wrote ST(0) into the old ST(7)");

    // FST ST(i) must carry the tag across, not just the value.
    FNINIT();
    push(1.0); push(0.0);             // ST0 = +0 (TAG_ZERO), ST1 = 1 (TAG_VALID)
    opr(0xDD, 0xD1);                  // FST ST(1)
    check(tg(1) == TAG_ZERO, "FST ST(i) copies the tag as well as the value");

    // FXCH swaps value and tag.
    FNINIT();
    push(0.0); push(5.0);             // ST0=5 valid, ST1=0 zero
    opr(0xD9, 0xC9);                  // FXCH ST(1)
    check(st(0) == 0.0 && st(1) == 5.0, "FXCH ST(1) swapped the values");
    check(tg(0) == TAG_ZERO && tg(1) == TAG_VALID, "FXCH ST(1) swapped the tags");
    opr(0xD9, 0xC9);
    check(st(0) == 5.0, "FXCH ST(1) twice is identity");

    // FXCH ST(7) — the far end of the D9 C8-CF range.
    FNINIT();
    for (int i = 1; i <= 8; i++) push((double)i);   // ST0=8 ... ST7=1
    opr(0xD9, 0xCF);                  // FXCH ST(7)
    check(st(0) == 1.0 && st(7) == 8.0, "FXCH ST(7) swapped the ends of a full stack");
    check(ftop() == 0, "FXCH ST(7) does not move TOP");

    // FNOP.
    FNINIT(); push(3.0);
    uint16_t before = sw();
    opr(0xD9, 0xD0);                  // FNOP
    check(sw() == before && st(0) == 3.0, "FNOP changes nothing");
  }

  //=========================================================================
  // 4. The modrm paths: [BX+disp8], a segment override, 0x67 32-bit addressing
  //=========================================================================
  {
    FNINIT();
    wrd(0x0140, 6.25);
    cpu->set_reg16(emu88::reg_BX, 0x0100);
    run({0xDD, 0x47, 0x40});          // FLD m64real [BX+0x40]  (mod=01 rm=111)
    check(st(0) == 6.25, "FLD m64real via [BX+disp8]");

    // ES:0x0150 holds a different value than DS:0x0150.
    FNINIT();
    wrd(0x0150, 1.0);
    mem->store_mem32(ES_LIN + 0x0150, (uint32_t)dbits(9.75));
    mem->store_mem32(ES_LIN + 0x0154, (uint32_t)(dbits(9.75) >> 32));
    run({0x26, 0xDD, mrm_disp16(0), 0x50, 0x01});   // ES: FLD m64real [0x0150]
    check(st(0) == 9.75, "segment override (26h) is honoured by the FPU operand");
    FNINIT();
    opm(0xDD, 0, 0x0150);
    check(st(0) == 1.0, "without the override the same offset reads DS");

    // 0x67 selects decode_modrm_32; mod=00 rm=101 is [disp32].
    FNINIT();
    wrd(0x0160, -3.5);
    run({0x67, 0xDD, 0x05, 0x60, 0x01, 0x00, 0x00});
    check(st(0) == -3.5, "FLD m64real via 32-bit addressing ([disp32])");
  }

  //=========================================================================
  // 5. m80real — the path that used to be lossy, and no longer is
  //=========================================================================
  {
    // Exact encodings out of fpu_write_m80real.
    FNINIT();
    push(1.0);
    FSTPm80(0x0120);
    check(rd80_exp(0x0120) == 0x3FFF, "FSTP m80real 1.0: exponent/sign = 0x3FFF");
    check(rd80_mant(0x0120) == 0x8000000000000000ULL,
          "FSTP m80real 1.0: mantissa = 0x8000000000000000 (J bit only)");
    push(-2.5);
    FSTPm80(0x0120);
    check(rd80_exp(0x0120) == 0xC000, "FSTP m80real -2.5: exponent/sign = 0xC000");
    check(rd80_mant(0x0120) == 0xA000000000000000ULL,
          "FSTP m80real -2.5: mantissa = 0xA000000000000000");

    // double -> m80 -> double is lossless for normals: the 53-bit mantissa
    // fits in the 64-bit field with room to spare.
    const double rt[] = {1.0 / 3.0, -1234.5678e9, 1e-300, 3.141592653589793,
                         65536.0, -7.0};
    bool rt_ok = true;
    for (double v : rt) {
      FNINIT(); push(v); FSTPm80(0x0120); FLDm80(0x0120);
      if (dbits(st(0)) != dbits(v)) rt_ok = false;
    }
    check(rt_ok, "double -> m80real -> double round-trips bit-exactly (6 values)");

    // The other direction is where the 53-bit register loses.  1 + 2^-52 is
    // the smallest step a double can take above 1.0, so it survives.
    FNINIT();
    wr80(0x0120, 0x8000000000000800ULL, 0x3FFF);   // 1 + 2^-52
    FLDm80(0x0120);
    check(dbits(st(0)) == 0x3FF0000000000001ULL,
          "FLD m80real 1+2^-52 loads exactly (fits in 53 bits)");
    FSTPm80(0x0130);
    check(rd80_mant(0x0130) == 0x8000000000000800ULL && rd80_exp(0x0130) == 0x3FFF,
          "and stores back byte-identically");

    // 1 + 2^-53 needs a 54-bit mantissa.  A real 387 keeps it; a double cannot.
    FNINIT();
    wr80(0x0120, 0x8000000000000400ULL, 0x3FFF);   // 1 + 2^-53
    FLDm80(0x0120);
    check(st_is(0, 0x3FFF, 0x8000000000000400ULL),
          "FLD m80real 1+2^-53 is held exactly (64-bit significand)");
    check(st(0) == 1.0, "...and still narrows to 1.0 when read back as a double");
    FSTPm80(0x0130);
    check(rd80_mant(0x0130) == 0x8000000000000400ULL && rd80_exp(0x0130) == 0x3FFF,
          "storing it back is byte-identical: the m80 path is a ten-byte move");

    // Specials.
    FNINIT();
    wr80(0x0120, 0x8000000000000000ULL, 0x7FFF);   // +inf
    FLDm80(0x0120);
    check(std::isinf(st(0)) && st(0) > 0, "FLD m80real +infinity");
    wr80(0x0120, 0x8000000000000000ULL, 0xFFFF);   // -inf
    FLDm80(0x0120);
    check(std::isinf(st(0)) && st(0) < 0, "FLD m80real -infinity");
    FSTPm80(0x0130);
    check(rd80_exp(0x0130) == 0xFFFF && rd80_mant(0x0130) == 0x8000000000000000ULL,
          "FSTP m80real -infinity encodes 0xFFFF / 0x8000000000000000");
    FSTPst0();                                     // drop the +inf

    FNINIT();
    wr80(0x0120, 0xC000000000000000ULL, 0x7FFF);   // QNaN
    FLDm80(0x0120);
    check(std::isnan(st(0)), "FLD m80real QNaN");
    // Sign and the 62-bit payload both survive, in both directions.
    FNINIT();
    wr80(0x0120, 0xC123456789ABCDEFULL, 0xFFFF);   // negative NaN, payload set
    FLDm80(0x0120);
    check(st_is(0, 0xFFFF, 0xC123456789ABCDEFULL),
          "FLD m80real keeps a negative NaN's sign and payload");
    check(std::signbit(st(0)), "...and it still narrows to a negative double NaN");
    FSTPm80(0x0130);
    check(rd80_exp(0x0130) == 0xFFFF && rd80_mant(0x0130) == 0xC123456789ABCDEFULL,
          "FSTP m80real writes the same ten bytes back");

    // A SIGNALLING NaN loads through m80real without raising anything - the
    // load is a byte move, not a conversion - and is quieted, with #IA, by the
    // first arithmetic that touches it.
    FNINIT();
    push80(0x7FFF, 0xA123456789ABCDEFULL);         // bit 62 clear: SNaN
    check(st_is(0, 0x7FFF, 0xA123456789ABCDEFULL), "FLD m80real of an SNaN is verbatim");
    check((sw() & SW_IE) == 0, "...and raises nothing");
    push(1.0);
    opr(0xDE, 0xC1);                               // FADDP ST(1), ST(0)
    check((sw() & SW_IE) != 0, "arithmetic on an SNaN raises #IA");
    check(st_is(0, 0x7FFF, 0xE123456789ABCDEFULL), "...and quiets it by setting bit 62");

    // Signed zero survives both ways.
    FNINIT();
    push(-0.0);
    FSTPm80(0x0120);
    check(rd80_exp(0x0120) == 0x8000 && rd80_mant(0x0120) == 0,
          "FSTP m80real -0.0 encodes 0x8000 / 0");
    FLDm80(0x0120);
    check(st(0) == 0.0 && std::signbit(st(0)), "FLD m80real -0.0 keeps the sign");
    wr80(0x0120, 0, 0x0000);
    FNINIT(); FLDm80(0x0120);
    check(st(0) == 0.0 && !std::signbit(st(0)), "FLD m80real +0.0");

    // A subnormal double has no implied 1, so it has to be normalised on the
    // way out: 5e-324 is 2^-1074 * 1.0, giving exponent -1074 + 16383 = 0x3BCD
    // and the J bit alone in the mantissa.
    FNINIT();
    push(5e-324);
    FSTPm80(0x0120);
    check(rd80_exp(0x0120) == 0x3BCD,
          "fpu_write_m80real(5e-324) encodes exponent 0x3BCD");
    FLDm80(0x0120);
    check(st(0) == 5e-324,
          "m80real round-trip of a subnormal double is lossless");
  }

  //=========================================================================
  // 6. FLD constants
  //=========================================================================
  {
    // A 387 loads these from an 80-bit ROM, and so does this: they are exact
    // 64-bit constants now, not host-library doubles narrowed to 53 bits.
    // The dbits() checks below are what those constants NARROW to, and the
    // exact 80-bit patterns are asserted further down.
    FNINIT();
    FLD1();  check(dbits(st(0)) == 0x3FF0000000000000ULL, "FLD1 = 1.0");
    check(tg(0) == TAG_VALID, "FLD1 tags valid");
    FNINIT();
    FLDZ();  check(dbits(st(0)) == 0x0000000000000000ULL, "FLDZ = +0.0");
    check(tg(0) == TAG_ZERO, "FLDZ tags zero");
    FNINIT();
    opr(0xD9, 0xEB); check(dbits(st(0)) == 0x400921FB54442D18ULL, "FLDPI = M_PI");
    FNINIT();
    opr(0xD9, 0xE9); check(dbits(st(0)) == 0x400A934F0979A371ULL, "FLDL2T = log2(10)");
    FNINIT();
    opr(0xD9, 0xEA); check(dbits(st(0)) == 0x3FF71547652B82FEULL, "FLDL2E = log2(e)");
    FNINIT();
    opr(0xD9, 0xEC); check(dbits(st(0)) == 0x3FD34413509F79FFULL, "FLDLG2 = log10(2)");
    FNINIT();
    opr(0xD9, 0xED); check(dbits(st(0)) == 0x3FE62E42FEFA39EFULL, "FLDLN2 = ln(2)");
    // The constants are the 387's ROM values now, to all 64 bits.  Loading
    // them as doubles cost the low eleven, which is what this pinned.
    struct { uint8_t op; uint16_t se; uint64_t sig; const char *name; } romc[] = {
      { 0xE8, 0x3FFF, 0x8000000000000000ULL, "FLD1" },
      { 0xE9, 0x4000, 0xD49A784BCD1B8AFEULL, "FLDL2T" },
      { 0xEA, 0x3FFF, 0xB8AA3B295C17F0BCULL, "FLDL2E" },
      { 0xEB, 0x4000, 0xC90FDAA22168C235ULL, "FLDPI" },
      { 0xEC, 0x3FFD, 0x9A209A84FBCFF799ULL, "FLDLG2" },
      { 0xED, 0x3FFE, 0xB17217F7D1CF79ACULL, "FLDLN2" },
      { 0xEE, 0x0000, 0x0000000000000000ULL, "FLDZ" },
    };
    bool rom_ok = true, rom_mem_ok = true;
    for (auto &k : romc) {
      FNINIT();
      opr(0xD9, k.op);
      if (!st_is(0, k.se, k.sig)) rom_ok = false;
      FSTPm80(0x0120);
      if (rd80_mant(0x0120) != k.sig || rd80_exp(0x0120) != k.se) rom_mem_ok = false;
    }
    check(rom_ok, "all seven FLD constants are the 387 ROM values to 64 bits");
    check(rom_mem_ok, "...and store back through m80real unchanged");

    // All seven push, in order, without disturbing each other.
    FNINIT();
    opr(0xD9, 0xE8); opr(0xD9, 0xE9); opr(0xD9, 0xEA); opr(0xD9, 0xEB);
    opr(0xD9, 0xEC); opr(0xD9, 0xED); opr(0xD9, 0xEE);
    check(ftop() == 1, "seven constant loads advanced TOP by 7");
    check(st(0) == 0.0 && dbits(st(6)) == 0x3FF0000000000000ULL,
          "constant stack reads FLDZ at ST(0), FLD1 at ST(6)");
  }

  //=========================================================================
  // 7. Arithmetic and operand order
  //=========================================================================
  {
    // D8 with m32real.
    FNINIT(); push(10.0); wrf(0x0100, 3.0f);
    opm(0xD8, 0, 0x0100); check(st(0) == 13.0, "FADD m32real: 10+3");
    FNINIT(); push(10.0); opm(0xD8, 1, 0x0100); check(st(0) == 30.0, "FMUL m32real: 10*3");
    FNINIT(); push(10.0); opm(0xD8, 4, 0x0100); check(st(0) == 7.0,  "FSUB m32real: ST0-mem = 7");
    FNINIT(); push(10.0); opm(0xD8, 5, 0x0100); check(st(0) == -7.0, "FSUBR m32real: mem-ST0 = -7");
    wrf(0x0100, 4.0f);
    FNINIT(); push(10.0); opm(0xD8, 6, 0x0100); check(st(0) == 2.5,  "FDIV m32real: ST0/mem = 2.5");
    FNINIT(); push(10.0); opm(0xD8, 7, 0x0100); check(st(0) == 0.4,  "FDIVR m32real: mem/ST0 = 0.4");

    // DC with m64real.  (0x0180, not SCRATCH64 — push() writes SCRATCH64.)
    wrd(0x0180, 4.0);
    FNINIT(); push(10.0); opm(0xDC, 0, 0x0180); check(st(0) == 14.0, "FADD m64real");
    FNINIT(); push(10.0); opm(0xDC, 1, 0x0180); check(st(0) == 40.0, "FMUL m64real");
    FNINIT(); push(10.0); opm(0xDC, 4, 0x0180); check(st(0) == 6.0,  "FSUB m64real: ST0-mem");
    FNINIT(); push(10.0); opm(0xDC, 5, 0x0180); check(st(0) == -6.0, "FSUBR m64real: mem-ST0");
    FNINIT(); push(10.0); opm(0xDC, 6, 0x0180); check(st(0) == 2.5,  "FDIV m64real: ST0/mem");
    FNINIT(); push(10.0); opm(0xDC, 7, 0x0180); check(st(0) == 0.4,  "FDIVR m64real: mem/ST0");
    FNINIT(); push(10.0); opm(0xDC, 2, 0x0180); check(cc() == CC_GT, "FCOM m64real 10 > 4");
    FNINIT(); push(10.0); opm(0xDC, 3, 0x0180);
    check(cc() == CC_GT && ftop() == 0, "FCOMP m64real compares then pops");

    // D8 register form: destination ST(0), source ST(i).
    FNINIT(); push(3.0); push(10.0);        // ST0=10 ST1=3
    opr(0xD8, 0xE1); check(st(0) == 7.0,  "D8 E1 FSUB ST(0),ST(1) = 10-3");
    FNINIT(); push(3.0); push(10.0);
    opr(0xD8, 0xE9); check(st(0) == -7.0, "D8 E9 FSUBR ST(0),ST(1) = 3-10");
    FNINIT(); push(4.0); push(10.0);
    opr(0xD8, 0xF1); check(st(0) == 2.5,  "D8 F1 FDIV ST(0),ST(1) = 10/4");
    FNINIT(); push(4.0); push(10.0);
    opr(0xD8, 0xF9); check(st(0) == 0.4,  "D8 F9 FDIVR ST(0),ST(1) = 4/10");
    FNINIT(); push(3.0); push(10.0);
    opr(0xD8, 0xC1); check(st(0) == 13.0, "D8 C1 FADD ST(0),ST(1)");
    FNINIT(); push(3.0); push(10.0);
    opr(0xD8, 0xC9); check(st(0) == 30.0, "D8 C9 FMUL ST(0),ST(1)");

    // DC register form: destination ST(i), source ST(0) — and the SUB/SUBR,
    // DIV/DIVR reg encodings are swapped relative to D8.  This is the classic
    // place to have the operands backwards, so assert non-commutative cases.
    FNINIT(); push(3.0); push(10.0);        // ST0=10 ST1=3
    opr(0xDC, 0xE1); check(st(1) == 7.0,  "DC E1 FSUBR ST(1),ST(0): ST1 = ST0-ST1 = 7");
    check(st(0) == 10.0, "DC E1 leaves ST(0) alone");
    FNINIT(); push(3.0); push(10.0);
    opr(0xDC, 0xE9); check(st(1) == -7.0, "DC E9 FSUB ST(1),ST(0): ST1 = ST1-ST0 = -7");
    FNINIT(); push(4.0); push(10.0);
    opr(0xDC, 0xF1); check(st(1) == 2.5,  "DC F1 FDIVR ST(1),ST(0): ST1 = ST0/ST1 = 2.5");
    FNINIT(); push(4.0); push(10.0);
    opr(0xDC, 0xF9); check(st(1) == 0.4,  "DC F9 FDIV ST(1),ST(0): ST1 = ST1/ST0 = 0.4");
    FNINIT(); push(3.0); push(10.0);
    opr(0xDC, 0xC1); check(st(1) == 13.0, "DC C1 FADD ST(1),ST(0)");
    FNINIT(); push(3.0); push(10.0);
    opr(0xDC, 0xC9); check(st(1) == 30.0, "DC C9 FMUL ST(1),ST(0)");

    // DE register form: same as DC, then pop.
    FNINIT(); push(3.0); push(10.0);
    opr(0xDE, 0xC1); check(st(0) == 13.0 && ftop() == 7, "DE C1 FADDP ST(1),ST(0)");
    FNINIT(); push(3.0); push(10.0);
    opr(0xDE, 0xC9); check(st(0) == 30.0 && ftop() == 7, "DE C9 FMULP ST(1),ST(0)");
    FNINIT(); push(3.0); push(10.0);
    opr(0xDE, 0xE1); check(st(0) == 7.0 && ftop() == 7,
                           "DE E1 FSUBRP ST(1),ST(0): ST1 = ST0-ST1, pop");
    FNINIT(); push(3.0); push(10.0);
    opr(0xDE, 0xE9); check(st(0) == -7.0 && ftop() == 7,
                           "DE E9 FSUBP ST(1),ST(0): ST1 = ST1-ST0, pop");
    FNINIT(); push(4.0); push(10.0);
    opr(0xDE, 0xF1); check(st(0) == 2.5 && ftop() == 7,
                           "DE F1 FDIVRP ST(1),ST(0): ST1 = ST0/ST1, pop");
    FNINIT(); push(4.0); push(10.0);
    opr(0xDE, 0xF9); check(st(0) == 0.4 && ftop() == 7,
                           "DE F9 FDIVP ST(1),ST(0): ST1 = ST1/ST0, pop");
    check(cpu->fpu.tags[6] == TAG_EMPTY, "the P forms empty the popped slot");

    // FADDP to a deeper register.
    FNINIT(); push(100.0); push(2.0); push(1.0);   // ST0=1 ST1=2 ST2=100
    opr(0xDE, 0xC2); check(st(0) == 2.0 && st(1) == 101.0,
                           "DE C2 FADDP ST(2),ST(0): 100+1, then pop");

    // Integer operands: DA is m32int, DE is m16int.
    FNINIT(); push(10.0); wr32(0x0100, (uint32_t)(int32_t)-3);
    opm(0xDA, 0, 0x0100); check(st(0) == 7.0, "FIADD m32int (-3): sign-extended");
    FNINIT(); push(10.0); opm(0xDA, 4, 0x0100); check(st(0) == 13.0, "FISUB m32int: ST0-(-3)");
    FNINIT(); push(10.0); opm(0xDA, 5, 0x0100); check(st(0) == -13.0, "FISUBR m32int: (-3)-ST0");
    FNINIT(); push(10.0); wr32(0x0100, 4);
    opm(0xDA, 6, 0x0100); check(st(0) == 2.5, "FIDIV m32int: ST0/4");
    FNINIT(); push(10.0); opm(0xDA, 7, 0x0100); check(st(0) == 0.4, "FIDIVR m32int: 4/ST0");
    FNINIT(); push(10.0); opm(0xDA, 1, 0x0100); check(st(0) == 40.0, "FIMUL m32int");

    FNINIT(); push(10.0); wr16(0x0100, (uint16_t)(int16_t)-2);
    opm(0xDE, 0, 0x0100); check(st(0) == 8.0, "FIADD m16int (-2): sign-extended");
    FNINIT(); push(10.0); opm(0xDE, 4, 0x0100); check(st(0) == 12.0, "FISUB m16int");
    FNINIT(); push(10.0); opm(0xDE, 5, 0x0100); check(st(0) == -12.0, "FISUBR m16int");
    FNINIT(); push(10.0); wr16(0x0100, 4);
    opm(0xDE, 6, 0x0100); check(st(0) == 2.5, "FIDIV m16int");
    FNINIT(); push(10.0); opm(0xDE, 7, 0x0100); check(st(0) == 0.4, "FIDIVR m16int");
    FNINIT(); push(10.0); opm(0xDE, 1, 0x0100); check(st(0) == 40.0, "FIMUL m16int");

    // Both integer paths share one guard deciding which /reg forms recompute
    // TAG(0) afterwards — a single comparison covering FIADD/FIMUL and
    // FISUB/FISUBR/FIDIV/FIDIVR but not the two compares.  The value tests
    // above never look at the tag, so drive ST(0) somewhere the tag has to
    // change and read it.
    FNINIT(); push(10.0); wr32(0x0100, 10);
    opm(0xDA, 4, 0x0100);
    check(st(0) == 0.0, "FISUB m32int 10-10 = 0");
    check(tg(0) == TAG_ZERO, "FISUB m32int retags ST(0) TAG_ZERO");
    FNINIT(); push(10.0); wr16(0x0100, 10);
    opm(0xDE, 4, 0x0100);
    check(st(0) == 0.0, "FISUB m16int 10-10 = 0");
    check(tg(0) == TAG_ZERO, "FISUB m16int retags ST(0) TAG_ZERO");
    FNINIT(); push(0.0); wr32(0x0100, 3);
    opm(0xDA, 0, 0x0100);
    check(st(0) == 3.0 && tg(0) == TAG_VALID, "FIADD m32int retags a zero ST(0) valid");
    FNINIT(); push(0.0); wr16(0x0100, 3);
    opm(0xDE, 0, 0x0100);
    check(st(0) == 3.0 && tg(0) == TAG_VALID, "FIADD m16int retags a zero ST(0) valid");

    // Divide by zero: ZE, and the result carries the dividend's sign.
    FNINIT(); push(-6.0); wrf(0x0100, 0.0f);
    opm(0xD8, 6, 0x0100);
    check((sw() & SW_ZE) != 0, "FDIV by zero sets ZE");
    check(std::isinf(st(0)) && st(0) < 0, "FDIV -6/0 = -infinity");
    check(tg(0) == TAG_SPECIAL, "the infinity is tagged TAG_SPECIAL");
    FNINIT(); push(0.0); wrf(0x0100, -5.0f);
    opm(0xD8, 7, 0x0100);
    check((sw() & SW_ZE) != 0, "FDIVR with ST(0)=0 sets ZE");
    check(std::isinf(st(0)) && st(0) < 0, "FDIVR -5/0 = -infinity");
    FNINIT(); push(2.0); push(0.0);         // ST0=0 ST1=2
    opr(0xDC, 0xF9);                        // FDIV ST(1),ST(0) -> ST1 = 2/0
    check(std::isinf(st(1)) && st(1) > 0 && (sw() & SW_ZE) != 0,
          "DC F9 FDIV ST(1),ST(0) by zero: +infinity and ZE");

    // 0/0 is an invalid operation (#IA, result QNaN), not a zero-divide.
    FNINIT(); push(0.0); wrf(0x0100, 0.0f);
    opm(0xD8, 6, 0x0100);
    check(std::isnan(st(0)), "FDIV 0/0 gives a QNaN, not an infinity");

    // The integer divide paths sign the infinity exactly the way the
    // m32real/m64real paths twenty lines above do, and raise ZE and retag the
    // result TAG_SPECIAL with it.  All four integer divide-by-zero branches
    // are exercised here.
    FNINIT(); push(-6.0); wr32(0x0100, 0);
    opm(0xDA, 6, 0x0100);
    check((sw() & SW_ZE) != 0, "FIDIV m32int by zero sets ZE");
    check(tg(0) == TAG_SPECIAL, "FIDIV m32int by zero tags the infinity TAG_SPECIAL");
    check(std::isinf(st(0)) && st(0) < 0, "FIDIV -6 by 0 = -infinity");
    FNINIT(); push(0.0); wr16(0x0100, (uint16_t)(int16_t)-5);
    opm(0xDE, 7, 0x0100);
    check((sw() & SW_ZE) != 0, "FIDIVR m16int with ST(0)=0 sets ZE");
    check(tg(0) == TAG_SPECIAL, "FIDIVR m16int with ST(0)=0 tags the infinity TAG_SPECIAL");
    check(std::isinf(st(0)) && st(0) < 0, "FIDIVR -5 by 0 = -infinity");
    // The other two of the four integer divide-by-zero branches.
    FNINIT(); push(0.0); wr32(0x0100, 7);
    opm(0xDA, 7, 0x0100);
    check((sw() & SW_ZE) != 0, "FIDIVR m32int with ST(0)=0 sets ZE");
    check(std::isinf(st(0)) && tg(0) == TAG_SPECIAL,
          "FIDIVR m32int with ST(0)=0 yields an infinity tagged TAG_SPECIAL");
    FNINIT(); push(-6.0); wr16(0x0100, 0);
    opm(0xDE, 6, 0x0100);
    check((sw() & SW_ZE) != 0, "FIDIV m16int by zero sets ZE");
    check(std::isinf(st(0)) && tg(0) == TAG_SPECIAL,
          "FIDIV m16int by zero yields an infinity tagged TAG_SPECIAL");
  }

  //=========================================================================
  // 8. FILD / FIST / FISTP / FISTTP
  //=========================================================================
  {
    FNINIT(); wr16(0x0100, (uint16_t)(int16_t)-1234);
    opm(0xDF, 0, 0x0100); check(st(0) == -1234.0, "FILD m16int -1234");
    FNINIT(); wr32(0x0100, (uint32_t)(int32_t)-123456789);
    opm(0xDB, 0, 0x0100); check(st(0) == -123456789.0, "FILD m32int -123456789");
    FNINIT(); wr64(0x0110, (uint64_t)(int64_t)-1234567890123LL);
    opm(0xDF, 5, 0x0110); check(st(0) == -1234567890123.0, "FILD m64int -1234567890123");

    // Store back.  Default rounding is round-to-nearest-even.
    FNINIT(); push(-5.5);
    wr16(0x0100, 0xFFFF);
    opm(0xDF, 2, 0x0100);
    check((int16_t)rd16(0x0100) == -6, "FIST m16int -5.5 -> -6 (nearest-even)");
    check(ftop() == 7, "FIST m16int does not pop");
    FNINIT(); push(-4.5);
    opm(0xDF, 3, 0x0100);
    check((int16_t)rd16(0x0100) == -4, "FISTP m16int -4.5 -> -4 (nearest-even)");
    check(ftop() == 0, "FISTP m16int pops");

    FNINIT(); push(2.5);
    opm(0xDB, 2, 0x0100); check((int32_t)rd32(0x0100) == 2, "FIST m32int 2.5 -> 2");
    FNINIT(); push(3.5);
    opm(0xDB, 3, 0x0100);
    check((int32_t)rd32(0x0100) == 4 && ftop() == 0, "FISTP m32int 3.5 -> 4, pops");

    FNINIT(); push(-9007199254740992.0);       // -2^53
    opm(0xDF, 7, 0x0110);
    check((int64_t)rd64(0x0110) == -9007199254740992LL, "FISTP m64int -2^53");
    check(ftop() == 0, "FISTP m64int pops");

    // FISTTP truncates toward zero whatever the rounding-control field says.
    FNINIT(); push(-2.9);
    opm(0xDB, 1, 0x0100);
    check((int32_t)rd32(0x0100) == -2 && ftop() == 0, "FISTTP m32int -2.9 -> -2, pops");
    FNINIT(); push(2.9);
    opm(0xDF, 1, 0x0100);
    check((int16_t)rd16(0x0100) == 2 && ftop() == 0, "FISTTP m16int 2.9 -> 2, pops");
    FNINIT(); push(-2.9);
    opm(0xDD, 1, 0x0110);
    check((int64_t)rd64(0x0110) == -2 && ftop() == 0, "FISTTP m64int -2.9 -> -2, pops");

    // A 64-bit significand holds every 64-bit integer whose magnitude fits,
    // so these round-trip exactly now.  2^53+1 is the value that used to fall
    // off the double stack; INT64_MAX is the one that could not be tested at
    // all, because the in-range boundary had to be approximated.
    FNINIT(); wr64(0x0110, 9007199254740993ULL);   // 2^53 + 1
    opm(0xDF, 5, 0x0110);
    check(st_is(0, 0x4034, 0x8000000000000400ULL), "FILD m64int 2^53+1 is exact");
    opm(0xDF, 7, 0x0118);
    check((int64_t)rd64(0x0118) == 9007199254740993LL, "FISTP m64int returns it unchanged");
    check((sw() & SW_PE) == 0, "...with no precision loss to report");
    FNINIT(); wr64(0x0110, 0x7FFFFFFFFFFFFFFFULL);  // INT64_MAX
    opm(0xDF, 5, 0x0110);
    check(st_is(0, 0x403D, 0xFFFFFFFFFFFFFFFEULL), "FILD m64int of INT64_MAX is exact");
    opm(0xDF, 7, 0x0118);
    check((int64_t)rd64(0x0118) == 0x7FFFFFFFFFFFFFFFLL, "FISTP m64int round-trips INT64_MAX");
    check((sw() & SW_IE) == 0, "...and does not call it out of range");
    // The other end.  -2^63 IS in range, and it is the one value whose
    // negation cannot be done in a signed int64 - a UBSan report, not a wrong
    // answer, which is exactly the class of defect the oracle cannot see.
    FNINIT();
    push80(0xC03E, 0x8000000000000000ULL);      // -2^63, exactly
    opm(0xDF, 7, 0x0118);
    check(rd64(0x0118) == 0x8000000000000000ULL, "FISTP m64int stores -2^63 exactly");
    check((sw() & SW_IE) == 0, "...and does not call INT64_MIN out of range");
    FNINIT(); wr64(0x0110, 0x8000000000000000ULL);
    opm(0xDF, 5, 0x0110);
    check(st_is(0, 0xC03E, 0x8000000000000000ULL), "FILD m64int loads INT64_MIN exactly");

    // Out of range, a NaN, an infinity.  A 387 raises #IA and, with #IA masked
    // (which is the only way this host runs one), stores the INTEGER INDEFINITE
    // value: the most negative integer of the destination width.
    //
    // This block did not exist until 2026-08-27, and the header of this file
    // and tests/README.md both said why: every one of these paths cast a double
    // straight to int16_t/int32_t/int64_t, which is undefined behaviour rather
    // than a defined result, so a test here would have been testing the
    // compiler.  It is a defined result now, so it is asserted.
    struct IntCase { double v; const char *what; };
    static const IntCase over32[] = {
      { 2147483648.0,  "2^31, one past INT32_MAX" },
      { -2147483649.0, "-2^31-1, one below INT32_MIN" },
      { 1e300,         "1e300" },
      { INFINITY,      "+infinity" },
      { -INFINITY,     "-infinity" },
      { NAN,           "a NaN" },
    };
    for (const auto &c : over32) {
      char msg[144];
      FNINIT(); push(c.v); wr32(0x0100, 0x5A5A5A5A);
      opm(0xDB, 3, 0x0100);                       // FISTP m32int
      std::snprintf(msg, sizeof msg, "FISTP m32int of %s stores the integer indefinite 80000000h", c.what);
      check(rd32(0x0100) == 0x80000000u, msg);
      std::snprintf(msg, sizeof msg, "FISTP m32int of %s raises IE", c.what);
      check((sw() & SW_IE) != 0, msg);
    }
    // The exact boundaries on the other side: the largest and smallest values
    // that DO fit must still store normally and raise nothing.
    FNINIT(); push(2147483647.0); opm(0xDB, 3, 0x0100);
    check((int32_t)rd32(0x0100) == 2147483647 && (sw() & SW_IE) == 0,
          "FISTP m32int of INT32_MAX stores it and raises nothing");
    FNINIT(); push(-2147483648.0); opm(0xDB, 3, 0x0100);
    check((int32_t)rd32(0x0100) == (-2147483647 - 1) && (sw() & SW_IE) == 0,
          "FISTP m32int of INT32_MIN stores it and raises nothing");
    // Rounding decides the range: 2147483647.6 rounds to 2^31, which does not
    // fit, so the check has to be made after rounding rather than before.
    FNINIT(); push(2147483647.6); opm(0xDB, 3, 0x0100);
    check(rd32(0x0100) == 0x80000000u && (sw() & SW_IE) != 0,
          "FISTP m32int of 2147483647.6 is out of range AFTER rounding");

    FNINIT(); push(32768.0); opm(0xDF, 3, 0x0100);      // FISTP m16int
    check(rd16(0x0100) == 0x8000 && (sw() & SW_IE) != 0,
          "FISTP m16int of 2^15 stores the integer indefinite 8000h and raises IE");
    FNINIT(); push(32767.0); opm(0xDF, 3, 0x0100);
    check((int16_t)rd16(0x0100) == 32767 && (sw() & SW_IE) == 0,
          "FISTP m16int of INT16_MAX stores it and raises nothing");
    FNINIT(); push(-32769.0); opm(0xDF, 3, 0x0100);
    check(rd16(0x0100) == 0x8000 && (sw() & SW_IE) != 0,
          "FISTP m16int of -2^15-1 stores the integer indefinite and raises IE");

    FNINIT(); push(1e300); opm(0xDF, 7, 0x0110);        // FISTP m64int
    check(rd64(0x0110) == 0x8000000000000000ULL && (sw() & SW_IE) != 0,
          "FISTP m64int of 1e300 stores the integer indefinite and raises IE");
    // The 64-bit boundary, both sides.  -2^63 is exactly representable as a
    // double and IS in range; +2^63 is exactly representable and is NOT, because
    // INT64_MAX is not.  The pair matters: a bound written `v > hi' instead of
    // `v >= hi' passes every other case in this block.
    FNINIT(); push(-9223372036854775808.0); opm(0xDF, 7, 0x0110);
    check(rd64(0x0110) == 0x8000000000000000ULL && (sw() & SW_IE) == 0,
          "FISTP m64int of -2^63 stores INT64_MIN and raises nothing - it is in range");
    FNINIT(); push(9223372036854775808.0); opm(0xDF, 7, 0x0110);
    check(rd64(0x0110) == 0x8000000000000000ULL && (sw() & SW_IE) != 0,
          "FISTP m64int of +2^63 is out of range: integer indefinite and IE");
    // The largest double below 2^63 is 2^63 - 1024, which fits.
    FNINIT(); push(9223372036854774784.0); opm(0xDF, 7, 0x0110);
    check(rd64(0x0110) == 9223372036854774784ULL && (sw() & SW_IE) == 0,
          "FISTP m64int of the largest double below 2^63 stores it and raises nothing");
    FNINIT(); push(NAN); opm(0xDD, 1, 0x0110);          // FISTTP m64int
    check(rd64(0x0110) == 0x8000000000000000ULL && (sw() & SW_IE) != 0,
          "FISTTP m64int of a NaN stores the integer indefinite and raises IE");
    FNINIT(); push(-1e30); opm(0xDF, 1, 0x0100);        // FISTTP m16int
    check(rd16(0x0100) == 0x8000 && (sw() & SW_IE) != 0,
          "FISTTP m16int of -1e30 stores the integer indefinite and raises IE");
    FNINIT(); push(1e30); opm(0xDB, 1, 0x0100);         // FISTTP m32int
    check(rd32(0x0100) == 0x80000000u && (sw() & SW_IE) != 0,
          "FISTTP m32int of 1e30 stores the integer indefinite and raises IE");
    FNINIT(); push(1e30); opm(0xDB, 2, 0x0100);         // FIST m32int (no pop)
    check(rd32(0x0100) == 0x80000000u && (sw() & SW_IE) != 0 && ftop() == 7,
          "FIST m32int of 1e30 stores the integer indefinite, raises IE, does not pop");
    FNINIT(); push(1e30); opm(0xDF, 2, 0x0100);         // FIST m16int (no pop)
    check(rd16(0x0100) == 0x8000 && (sw() & SW_IE) != 0 && ftop() == 7,
          "FIST m16int of 1e30 stores the integer indefinite, raises IE, does not pop");
  }

  //=========================================================================
  // 9. Compares: C3/C2/C0, FSTSW AX
  //=========================================================================
  {
    // FCOM against memory.
    FNINIT(); push(1.0); wrf(0x0100, 2.0f);
    opm(0xD8, 2, 0x0100); check(cc() == CC_LT, "FCOM m32real 1 < 2: C3=0 C2=0 C0=1");
    FNINIT(); push(2.0); opm(0xD8, 2, 0x0100); check(cc() == CC_EQ, "FCOM m32real 2 == 2: C3=1");
    FNINIT(); push(3.0); opm(0xD8, 2, 0x0100); check(cc() == CC_GT, "FCOM m32real 3 > 2: all clear");
    FNINIT(); push(dfrom(0x7FF8000000000000ULL));
    opm(0xD8, 2, 0x0100); check(cc() == CC_UNORD, "FCOM NaN: C3=1 C2=1 C0=1 (unordered)");
    check(ftop() == 7, "FCOM does not pop");

    // FCOMP pops; FCOMPP (DE D9) pops twice.
    FNINIT(); push(1.0); wrf(0x0100, 2.0f);
    opm(0xD8, 3, 0x0100);
    check(cc() == CC_LT && ftop() == 0, "FCOMP m32real compares then pops");
    FNINIT(); push(2.0); push(1.0);            // ST0=1 ST1=2
    opr(0xDE, 0xD9);
    check(cc() == CC_LT && ftop() == 0, "FCOMPP: ST0<ST1, pops both");
    check(cpu->fpu.tags[7] == TAG_EMPTY && cpu->fpu.tags[6] == TAG_EMPTY,
          "FCOMPP emptied both slots");

    // The undocumented FCOM/FCOMP aliases in the DC and DE register forms.
    // DC D0+i / DC D8+i decode to reg = 2 / reg = 3, and DE D0+i to reg = 2 —
    // ranges the arithmetic tests in section 7 never reach, because they only
    // ever issue modrm C1/C9/E1/E9/F1/F9.  Unlike the DC arithmetic, these
    // compare ST(0) AGAINST ST(i): the destination-is-ST(i) convention does
    // not apply, so a swapped operand order shows up as an inverted C0/C3.
    FNINIT(); push(2.0); push(1.0);            // ST0=1 ST1=2
    opr(0xDC, 0xD1);
    check(cc() == CC_LT, "DC D1 FCOM ST(1): 1 < 2 (ST(0) is the left operand)");
    check(ftop() == 6, "DC D1 FCOM does not pop");
    FNINIT(); push(1.0); push(2.0);            // ST0=2 ST1=1
    opr(0xDC, 0xD1);
    check(cc() == CC_GT, "DC D1 FCOM ST(1): 2 > 1");
    FNINIT(); push(2.0); push(1.0);
    opr(0xDC, 0xD9);
    check(cc() == CC_LT && ftop() == 7, "DC D9 FCOMP ST(1): compares then pops");
    FNINIT(); push(2.0); push(1.0);
    opr(0xDE, 0xD1);
    check(cc() == CC_LT, "DE D1 FCOMP ST(1): 1 < 2");
    check(ftop() == 7, "DE D1 FCOMP pops (the DE alias is the popping form)");
    check(cpu->fpu.tags[6] == TAG_EMPTY, "DE D1 FCOMP emptied the vacated slot");

    // FICOM / FICOMP.
    FNINIT(); push(5.0); wr32(0x0100, 7);
    opm(0xDA, 2, 0x0100); check(cc() == CC_LT, "FICOM m32int 5 < 7");
    FNINIT(); push(5.0); wr16(0x0100, 5);
    opm(0xDE, 3, 0x0100); check(cc() == CC_EQ && ftop() == 0, "FICOMP m16int 5 == 5, pops");

    // FTST compares against +0.0.
    FNINIT(); push(-1.0); opr(0xD9, 0xE4); check(cc() == CC_LT, "FTST -1 < 0");
    FNINIT(); push(0.0);  opr(0xD9, 0xE4); check(cc() == CC_EQ, "FTST 0 == 0");
    FNINIT(); push(-0.0); opr(0xD9, 0xE4); check(cc() == CC_EQ, "FTST -0 == 0");
    FNINIT(); push(1e-300); opr(0xD9, 0xE4); check(cc() == CC_GT, "FTST tiny > 0");

    // FUCOM / FUCOMP / FUCOMPP.
    FNINIT(); push(9.0); push(4.0);            // ST0=4 ST1=9
    opr(0xDD, 0xE1); check(cc() == CC_LT && ftop() == 6, "FUCOM ST(1): 4 < 9, no pop");
    opr(0xDD, 0xE9); check(cc() == CC_LT && ftop() == 7, "FUCOMP ST(1): pops");
    FNINIT(); push(4.0); push(4.0);
    opr(0xDA, 0xE9); check(cc() == CC_EQ && ftop() == 0, "FUCOMPP (DA E9): equal, pops both");
    FNINIT(); push(1.0); push(dfrom(0x7FF8000000000000ULL));
    opr(0xDD, 0xE1); check(cc() == CC_UNORD, "FUCOM with NaN is unordered");

    // Index 7 of the DD E0-E7 and DD E8-EF ranges.  The stack is filled in
    // DESCENDING order so the answer is "less than": CC_GT is all-bits-clear
    // and is exactly what a decode that ignored the opcode would leave behind,
    // so an ST(0) > ST(7) case here would pass without the compare running.
    FNINIT();
    for (int i = 8; i >= 1; i--) push((double)i);   // ST0=1 ... ST7=8
    opr(0xDD, 0xE7);
    check(cc() == CC_LT && ftop() == 0, "FUCOM ST(7): 1 < 8, no pop");
    opr(0xDD, 0xEF);
    check(cc() == CC_LT && ftop() == 1, "FUCOMP ST(7): compares then pops");

    // FNSTSW AX: the whole status word lands in AX, TOP and all.
    FNINIT(); push(2.0); push(1.0);            // ST0=1 ST1=2, TOP=6
    opr(0xDE, 0xD9);                           // FCOMPP -> C0 set, TOP=0
    cpu->set_reg32(emu88::reg_AX, 0x12345678);
    opr(0xDF, 0xE0);                           // FNSTSW AX
    uint16_t ax = cpu->get_reg16(emu88::reg_AX);
    check(ax == sw(), "FNSTSW AX copies the status word verbatim");
    check((ax & 0x0100) != 0, "FNSTSW AX: C0 reaches AH bit 0");
    check((ax & 0x4000) == 0, "FNSTSW AX: C3 clear for less-than");
    check((cpu->get_reg32(emu88::reg_AX) >> 16) == 0x1234,
          "FNSTSW AX writes AX only, leaving the high half of EAX");
    FNINIT(); push(5.0); push(5.0);            // TOP=6
    opr(0xDD, 0xE1);                           // FUCOM ST(1) -> equal
    opr(0xDF, 0xE0);
    ax = cpu->get_reg16(emu88::reg_AX);
    check((ax & 0x4500) == 0x4000, "FSTSW AX: equal is C3 only in AH");
    check(((ax >> 11) & 7) == 6, "FSTSW AX: TOP=6 shows up in AH bits 3-5");

    // FNSTSW m16 writes the same word to memory.
    wr16(0x0100, 0);
    opm(0xDD, 7, 0x0100);
    check(rd16(0x0100) == sw(), "FNSTSW m16 stores the status word");

    // Intel specifies that FCOM/FUCOM/FTST clear C1 along with C0/C2/C3, so a
    // C1 left over from FXAM does not survive into the next FSTSW AX.
    FNINIT(); push(-1.0);
    opr(0xD9, 0xE5);                            // FXAM -> C1 = sign = 1
    check((sw() & 0x0200) != 0, "FXAM on a negative set C1 (setup for the C1 check)");
    push(3.0);
    opr(0xD8, 0xD1);                            // FCOM ST(1)
    check((sw() & 0x0200) == 0, "FCOM clears C1");
  }

  //=========================================================================
  // 10. FXAM
  //=========================================================================
  {
    FNINIT();
    opr(0xD9, 0xE5); check(cc() == 0x9, "FXAM empty: C3=1 C2=0 C1=0 C0=1");
    FNINIT(); push(0.0);
    opr(0xD9, 0xE5); check(cc() == 0x8, "FXAM +0: C3=1 C2=0 C1=0 C0=0");
    FNINIT(); push(-0.0);
    opr(0xD9, 0xE5); check(cc() == 0xA, "FXAM -0: C3=1 C1=1");
    FNINIT(); push(1.0);
    opr(0xD9, 0xE5); check(cc() == 0x4, "FXAM normal: C2=1");
    FNINIT(); push(-1.0);
    opr(0xD9, 0xE5); check(cc() == 0x6, "FXAM negative normal: C2=1 C1=1");
    FNINIT(); push(INFINITY);
    opr(0xD9, 0xE5); check(cc() == 0x5, "FXAM +inf: C2=1 C0=1");
    FNINIT(); push(-INFINITY);
    opr(0xD9, 0xE5); check(cc() == 0x7, "FXAM -inf: C2=1 C1=1 C0=1");
    FNINIT(); push(dfrom(0x7FF8000000000000ULL));
    opr(0xD9, 0xE5); check(cc() == 0x1, "FXAM NaN: C0=1");
    FNINIT(); push(dfrom(0xFFF8000000000000ULL));
    opr(0xD9, 0xE5); check(cc() == 0x3, "FXAM -NaN: C1=1 C0=1");
    // FXAM reads the TAG, not the value: FFREE makes a live register "empty".
    FNINIT(); push(1.0);
    opr(0xDD, 0xC0);                            // FFREE ST(0)
    opr(0xD9, 0xE5); check(cc() == 0x9, "FXAM after FFREE ST(0) reports empty");
    // A subnormal double is a normal once widened, so C2 alone is right for
    // it.  The denormal and unsupported classes need genuine 80-bit operands.
    FNINIT(); push(5e-324);
    opr(0xD9, 0xE5);
    check(cc() == 0x4, "FXAM on a widened subnormal double reports normal (C2)");
    FNINIT(); push80(0x0000, 0x0000000000000001ULL);
    opr(0xD9, 0xE5);
    check(cc() == 0xC, "FXAM on a true 80-bit denormal reports C3|C2");
    FNINIT(); push80(0x8000, 0x0000000000000001ULL);
    opr(0xD9, 0xE5);
    check(cc() == 0xE, "...and C1 carries its sign");
    FNINIT(); push80(0x4000, 0x4000000000000000ULL);
    opr(0xD9, 0xE5);
    check(cc() == 0x0, "FXAM on an unnormal reports the unsupported class (all clear)");
  }

  //=========================================================================
  // 11. FCHS / FABS / FSQRT / FRNDINT / FSCALE / FXTRACT
  //=========================================================================
  {
    FNINIT(); push(2.5);
    opr(0xD9, 0xE0); check(st(0) == -2.5, "FCHS 2.5 -> -2.5");
    opr(0xD9, 0xE0); check(st(0) == 2.5,  "FCHS twice is identity");
    FNINIT(); push(0.0);
    opr(0xD9, 0xE0);
    check(st(0) == 0.0 && std::signbit(st(0)), "FCHS +0 -> -0");
    check(tg(0) == TAG_ZERO, "FCHS -0 stays TAG_ZERO");
    FNINIT(); push(INFINITY);
    opr(0xD9, 0xE0);
    check(std::isinf(st(0)) && st(0) < 0 && tg(0) == TAG_SPECIAL, "FCHS +inf -> -inf");

    FNINIT(); push(-3.75);
    opr(0xD9, 0xE1); check(st(0) == 3.75, "FABS -3.75 -> 3.75");
    opr(0xD9, 0xE1); check(st(0) == 3.75, "FABS is idempotent");
    FNINIT(); push(-0.0);
    opr(0xD9, 0xE1); check(st(0) == 0.0 && !std::signbit(st(0)), "FABS -0 -> +0");

    FNINIT(); push(2.0);
    opr(0xD9, 0xFA); check(dbits(st(0)) == 0x3FF6A09E667F3BCDULL, "FSQRT 2 = 1.4142135623730951");
    FNINIT(); push(144.0);
    opr(0xD9, 0xFA); check(st(0) == 12.0, "FSQRT 144 = 12 exactly");
    FNINIT(); push(0.0);
    opr(0xD9, 0xFA); check(st(0) == 0.0 && tg(0) == TAG_ZERO, "FSQRT 0 = 0");
    // sqrt of a negative is an invalid operation: #IA, and the indefinite.
    FNINIT(); push(-4.0);
    opr(0xD9, 0xFA);
    check(st_indef(0), "FSQRT -4 delivers the real indefinite");
    check((sw() & SW_IE) != 0, "FSQRT of a negative raises #IA");
    // sqrt of a negative ZERO is not: it is -0, exactly, with nothing raised.
    FNINIT(); push(-0.0);
    opr(0xD9, 0xFA);
    check(st_is(0, 0x8000, 0) && (sw() & SW_IE) == 0, "FSQRT -0 = -0, no flag");
    // And the significand really is 64 bits wide now.
    FNINIT(); push(2.0);
    opr(0xD9, 0xFA);
    check(st_is(0, 0x3FFF, 0xB504F333F9DE6484ULL), "FSQRT 2 to all 64 significand bits");

    // FRNDINT under all four rounding modes (see section 13 for FLDCW).
    FNINIT(); push(2.5);
    opr(0xD9, 0xFC); check(st(0) == 2.0, "FRNDINT 2.5 -> 2 (nearest-even)");
    FNINIT(); push(3.5);
    opr(0xD9, 0xFC); check(st(0) == 4.0, "FRNDINT 3.5 -> 4 (nearest-even)");
    FNINIT(); push(-2.5);
    opr(0xD9, 0xFC); check(st(0) == -2.0, "FRNDINT -2.5 -> -2 (nearest-even)");
    FNINIT(); push(2.7);
    opr(0xD9, 0xFC); check(st(0) == 3.0, "FRNDINT 2.7 -> 3");
    check(tg(0) == TAG_VALID, "FRNDINT retags the result");

    // FSCALE: ST(0) *= 2^trunc(ST(1)).
    FNINIT(); push(10.0); push(1.0);            // ST0=1 ST1=10
    opr(0xD9, 0xFD); check(st(0) == 1024.0, "FSCALE 1 * 2^10 = 1024");
    check(st(1) == 10.0, "FSCALE leaves ST(1) alone");
    FNINIT(); push(-3.7); push(1.0);
    opr(0xD9, 0xFD); check(st(0) == 0.125, "FSCALE truncates the exponent: 2^-3");
    FNINIT(); push(0.0); push(7.5);
    opr(0xD9, 0xFD); check(st(0) == 7.5, "FSCALE by 0 is a no-op");

    // FXTRACT: 12 = 1.5 * 2^3.
    FNINIT(); push(12.0);
    opr(0xD9, 0xF4);
    check(st(0) == 1.5, "FXTRACT 12: significand 1.5 on top");
    check(st(1) == 3.0, "FXTRACT 12: exponent 3 below it");
    check(ftop() == 6, "FXTRACT pushed (TOP moved by one)");
    FNINIT(); push(-0.75);
    opr(0xD9, 0xF4);
    check(st(0) == -1.5 && st(1) == -1.0, "FXTRACT -0.75 = -1.5 * 2^-1");
    FNINIT(); push(0.0);
    opr(0xD9, 0xF4);
    check(std::isinf(st(1)) && st(1) < 0 && st(0) == 0.0,
          "FXTRACT 0 gives exponent -infinity and significand 0");
    check((sw() & SW_ZE) != 0, "FXTRACT 0 raises #Z");
    FNINIT(); push80(0x7FFF, 0x8000000000000000ULL);   // +inf
    opr(0xD9, 0xF4);
    check(std::isinf(st(1)) && st(1) > 0 && std::isinf(st(0)) && st(0) > 0,
          "FXTRACT of an infinity gives +inf and +inf");
    FNINIT(); push80(0x0000, 0x0000000000000001ULL);   // the smallest denormal
    opr(0xD9, 0xF4);
    check(st(1) == -16445.0, "FXTRACT normalises a denormal: exponent -16445");
    check((sw() & SW_DE) != 0, "...and reports #D");
  }

  //=========================================================================
  // 12. FPREM / FPREM1
  //=========================================================================
  {
    FNINIT(); push(4.0); push(13.0);            // ST0=13 ST1=4
    opr(0xD9, 0xF8); check(st(0) == 1.0, "FPREM 13 mod 4 = 1");
    FNINIT(); push(4.0); push(-13.0);
    opr(0xD9, 0xF8); check(st(0) == -1.0, "FPREM -13 mod 4 = -1 (sign of the dividend)");
    FNINIT(); push(4.0); push(14.0);
    opr(0xD9, 0xF8); check(st(0) == 2.0, "FPREM 14 mod 4 = 2 (truncated quotient)");
    FNINIT(); push(0.0); push(5.0);
    opr(0xD9, 0xF8);
    check(st_indef(0) && (sw() & SW_IE) != 0,
          "FPREM of a zero divisor is #IA and the indefinite");

    FNINIT(); push(4.0); push(13.0);
    opr(0xD9, 0xF5); check(st(0) == 1.0, "FPREM1 13 rem 4 = 1");
    FNINIT(); push(4.0); push(14.0);
    opr(0xD9, 0xF5); check(st(0) == -2.0, "FPREM1 14 rem 4 = -2 (rounded quotient 4)");
    check(st(1) == 4.0, "FPREM1 leaves ST(1) alone");

    // FPREM1 rounds the quotient to NEAREST EVEN (IEEE remainder), so 10 rem 4
    // takes q=2, not the half-away-from-zero q=3.
    FNINIT(); push(4.0); push(10.0);
    opr(0xD9, 0xF5);
    check(st(0) == 2.0, "FPREM1 10 rem 4 = 2 (q=2, ties-to-even)");

    // A large exponent difference is reduced a bite at a time, with C2 set to
    // say "call me again".  Software is expected to loop on C2, and the
    // converged answer is the exact remainder - which a single divide through
    // a 53-bit quotient could never produce for 2^100 mod 3.
    FNINIT(); push(3.0); push(ldexp(1.0, 100));
    opr(0xD9, 0xF8);
    check((sw() & SW_C2) != 0, "FPREM 2^100 mod 3 reports a PARTIAL reduction (C2)");
    int spins = 0;
    while ((sw() & SW_C2) && spins < 64) { opr(0xD9, 0xF8); spins++; }
    check((sw() & SW_C2) == 0, "FPREM converges");
    check(st(0) == 1.0, "...to the true remainder 2^100 mod 3 = 1");

    // The quotient bits a 387 leaves in C0, C3 and C1 (Q2, Q1, Q0).
    FNINIT(); push(4.0); push(13.0);            // 13 = 3*4 + 1, so Q = 3
    cpu->fpu.sw &= (uint16_t)~0x4700;
    opr(0xD9, 0xF8);
    check((sw() & 0x4700) == 0x4200,
          "FPREM 13 mod 4 leaves Q = 3 in C0:C3:C1 (C3 and C1 set)");
    FNINIT(); push(4.0); push(22.0);            // 22 = 5*4 + 2, so Q = 5
    cpu->fpu.sw &= (uint16_t)~0x4700;
    opr(0xD9, 0xF8);
    check(st(0) == 2.0 && (sw() & 0x4700) == 0x0300,
          "FPREM 22 mod 4 leaves Q = 5 (C0 and C1 set)");
  }

  //=========================================================================
  // 13. FLDCW / FNSTCW and the rounding-control field
  //=========================================================================
  {
    FNINIT();
    wr16(0x0100, 0x0F7F);
    opm(0xD9, 5, 0x0100);                       // FLDCW
    check(cw() == 0x0F7F, "FLDCW loads the control word");
    wr16(0x0102, 0);
    opm(0xD9, 7, 0x0102);                       // FNSTCW
    check(rd16(0x0102) == 0x0F7F, "FNSTCW stores it back");

    struct { uint16_t cwv; const char *name; double r25, rm25, r27, rm27; } rc[] = {
      {0x037F, "RC=00 nearest",  2.0, -2.0, 3.0, -3.0},
      {0x077F, "RC=01 down",     2.0, -3.0, 2.0, -3.0},
      {0x0B7F, "RC=10 up",       3.0, -2.0, 3.0, -2.0},
      {0x0F7F, "RC=11 truncate", 2.0, -2.0, 2.0, -2.0},
    };
    char buf[128];
    for (auto &r : rc) {
      FNINIT();
      wr16(0x0100, r.cwv); opm(0xD9, 5, 0x0100);
      push(2.5);  opr(0xD9, 0xFC);
      std::snprintf(buf, sizeof buf, "FRNDINT 2.5 under %s", r.name);
      check(st(0) == r.r25, buf);
      FNINIT(); wr16(0x0100, r.cwv); opm(0xD9, 5, 0x0100);
      push(-2.5); opr(0xD9, 0xFC);
      std::snprintf(buf, sizeof buf, "FRNDINT -2.5 under %s", r.name);
      check(st(0) == r.rm25, buf);
      FNINIT(); wr16(0x0100, r.cwv); opm(0xD9, 5, 0x0100);
      push(2.7);  opr(0xD9, 0xFC);
      std::snprintf(buf, sizeof buf, "FRNDINT 2.7 under %s", r.name);
      check(st(0) == r.r27, buf);
      FNINIT(); wr16(0x0100, r.cwv); opm(0xD9, 5, 0x0100);
      push(-2.7); opr(0xD9, 0xFC);
      std::snprintf(buf, sizeof buf, "FRNDINT -2.7 under %s", r.name);
      check(st(0) == r.rm27, buf);
    }

    // FIST honours RC; FISTTP must not.
    FNINIT(); wr16(0x0100, 0x0B7F); opm(0xD9, 5, 0x0100);   // round up
    push(2.1); opm(0xDB, 3, 0x0104);
    check((int32_t)rd32(0x0104) == 3, "FISTP m32int 2.1 under RC=up gives 3");
    FNINIT(); wr16(0x0100, 0x0B7F); opm(0xD9, 5, 0x0100);
    push(2.9); opm(0xDB, 1, 0x0104);
    check((int32_t)rd32(0x0104) == 2, "FISTTP m32int 2.9 under RC=up still truncates to 2");

    // FLDCW must not disturb anything else.
    FNINIT(); push(6.0);
    wr16(0x0100, 0x0F7F); opm(0xD9, 5, 0x0100);
    check(st(0) == 6.0 && ftop() == 7, "FLDCW leaves the stack alone");

    // Precision control rounds the SIGNIFICAND to 24 or 53 bits while leaving
    // the exponent range at the full fifteen - which is why it needs a soft
    // float and could not be done at all while the file was host doubles.
    FNINIT();
    wr16(0x0100, 0x003F);                       // PC = 00 (single), RC = nearest
    opm(0xD9, 5, 0x0100);
    push(1.0); wrd(0x0110, ldexp(1.0, -30)); opm(0xDC, 0, 0x0110);
    check(st(0) == 1.0, "PC=24: 1 + 2^-30 rounds away entirely");
    check((sw() & SW_PE) != 0, "...and reports #P");
    // PC=53 keeps 2^-52 and loses 2^-60; PC=64 keeps both.
    FNINIT();
    wr16(0x0100, 0x023F); opm(0xD9, 5, 0x0100); // PC = 10 (double)
    push(1.0); wrd(0x0110, ldexp(1.0, -60)); opm(0xDC, 0, 0x0110);
    check(st(0) == 1.0, "PC=53: 1 + 2^-60 rounds to 1.0");
    FNINIT();
    wr16(0x0100, 0x023F); opm(0xD9, 5, 0x0100);
    push(1.0); wrd(0x0110, ldexp(1.0, -52)); opm(0xDC, 0, 0x0110);
    check(st_is(0, 0x3FFF, 0x8000000000000800ULL), "PC=53: 1 + 2^-52 survives");
    FNINIT();
    wr16(0x0100, 0x037F); opm(0xD9, 5, 0x0100); // PC = 11 (extended)
    push(1.0); wrd(0x0110, ldexp(1.0, -60)); opm(0xDC, 0, 0x0110);
    check(st_is(0, 0x3FFF, 0x8000000000000008ULL),
          "PC=64: 1 + 2^-60 survives, which no double register could hold");
    // Precision control reaches FADD/FSUB/FMUL/FDIV/FSQRT and NOTHING else.
    // FSCALE only moves an exponent, so it keeps all 64 significand bits even
    // at PC=24 - which is what the hardware does, and is not what this did
    // until the oracle's FSCALE grid was widened past PC=64.
    FNINIT();
    wr16(0x0100, 0x003F); opm(0xD9, 5, 0x0100); // PC = 24
    push(4.0);
    push80(0x3FFF, 0xA71C3D5AAB33E088ULL);      // a full 64-bit significand
    opr(0xD9, 0xFD);                            // FSCALE: ST(0) * 2^trunc(ST(1))
    check(st_is(0, 0x4003, 0xA71C3D5AAB33E088ULL),
          "FSCALE keeps all 64 significand bits under PC=24");
    FNINIT();
    wr16(0x0100, 0x003F); opm(0xD9, 5, 0x0100);
    push80(0x3FFF, 0xA71C3D5AAB33E088ULL);
    opr(0xD9, 0xFC);                            // FRNDINT
    check(st_is(0, 0x3FFF, 0x8000000000000000ULL),
          "FRNDINT ignores PC too: 1.30... rounds to 1, not to a 24-bit value");
    FNINIT();

    // The exponent range does NOT narrow with PC: 2^-200 is still a normal.
    FNINIT();
    wr16(0x0100, 0x003F); opm(0xD9, 5, 0x0100);
    push(ldexp(1.0, -200)); push(ldexp(1.0, -200)); opr(0xDE, 0xC1);
    check(st_is(0, 0x3F38, 0x8000000000000000ULL),
          "PC=24 rounds the significand only: 2^-199 is still exact");
    FNINIT();
  }

  //=========================================================================
  // 14. Transcendentals
  //=========================================================================
  {
    FNINIT(); push(3.0);
    opr(0xD9, 0xF0); check(st(0) == 7.0, "F2XM1 3 = 2^3-1 = 7");
    FNINIT(); push(0.0);
    opr(0xD9, 0xF0); check(st(0) == 0.0, "F2XM1 0 = 0");
    FNINIT(); push(-1.0);
    opr(0xD9, 0xF0); check(st(0) == -0.5, "F2XM1 -1 = -0.5");

    FNINIT(); push(3.0); push(8.0);             // ST0=8 ST1=3
    opr(0xD9, 0xF1);
    check(st(0) == 9.0, "FYL2X: 3*log2(8) = 9, result in ST(0)");
    check(ftop() == 7, "FYL2X popped once");
    FNINIT(); push(1.0); push(7.0);
    opr(0xD9, 0xF9);
    check(st(0) == 3.0, "FYL2XP1: 1*log2(7+1) = 3");
    check(ftop() == 7, "FYL2XP1 popped once");

    FNINIT(); push(0.0);
    opr(0xD9, 0xFE); check(st(0) == 0.0, "FSIN 0 = 0");
    FNINIT(); push(0.0);
    opr(0xD9, 0xFF); check(st(0) == 1.0, "FCOS 0 = 1");
    FNINIT(); push(M_PI / 2);
    opr(0xD9, 0xFE); check(st(0) == 1.0, "FSIN pi/2 = 1");
    FNINIT(); push(0.0);
    opr(0xD9, 0xFB);                            // FSINCOS
    check(st(0) == 1.0, "FSINCOS 0: cos on top");
    check(st(1) == 0.0, "FSINCOS 0: sin below");
    check(ftop() == 6, "FSINCOS pushed one register");
    FNINIT(); push(0.0);
    opr(0xD9, 0xF2);                            // FPTAN
    check(st(0) == 1.0, "FPTAN 0: the pushed 1.0 is on top");
    check(st(1) == 0.0, "FPTAN 0: tan(0) below it");
    check(ftop() == 6, "FPTAN pushed one register");
    check((sw() & 0x0400) == 0, "FPTAN clears C2 (reduction complete)");
    FNINIT(); push(1.0); push(1.0);
    opr(0xD9, 0xF3);                            // FPATAN
    check(dbits(st(0)) == 0x3FE921FB54442D18ULL, "FPATAN 1,1 = pi/4");
    check(ftop() == 7, "FPATAN popped once");
    FNINIT(); push(-1.0); push(0.0);            // ST1 = -1, ST0 = 0
    opr(0xD9, 0xF3);
    check(st(0) == -M_PI / 2, "FPATAN is atan2(ST(1),ST(0)), not atan2(ST(0),ST(1))");

    // |x| >= 2^63 is out of range: C2 is SET and the operand is left alone.
    // The fixtures pre-set C2 so a decode that does nothing cannot pass by
    // accident; the assertions now want it still set for the opposite reason.
    FNINIT(); push(ldexp(1.0, 70));
    opr(0xD9, 0xFE);
    check((sw() & SW_C2) != 0, "FSIN of 2^70 sets C2: out of range");
    check(st(0) == ldexp(1.0, 70), "FSIN of 2^70 leaves the argument alone");
    FNINIT(); push(ldexp(1.0, 70));
    opr(0xD9, 0xFF);
    check((sw() & SW_C2) != 0, "FCOS of 2^70 sets C2 as well");
    check(st(0) == ldexp(1.0, 70), "FCOS of 2^70 leaves the argument alone");
    FNINIT(); push(ldexp(1.0, 70));
    opr(0xD9, 0xF2);
    check((sw() & SW_C2) != 0 && ftop() == 7, "FPTAN of 2^70 sets C2 and does not push");
    FNINIT(); push(ldexp(1.0, 70));
    opr(0xD9, 0xFB);
    check((sw() & SW_C2) != 0 && ftop() == 7, "FSINCOS of 2^70 sets C2 and does not push");
    // Just inside the range the reduction has to be right to many more bits
    // than the argument itself carries.
    FNINIT(); push(ldexp(1.0, 62));
    opr(0xD9, 0xFE);
    check((sw() & SW_C2) == 0, "FSIN of 2^62 is in range");
    check(std::fabs(st(0) - (-0.70292244361920888)) < 1e-15,
          "...and reduces correctly: sin(2^62) = -0.702922443619209");

    // F2XM1 and FYL2XP1 exist on real hardware precisely to keep precision
    // near zero.  Computed as pow(2,x)-1 and log2(x+1) in double they returned
    // exactly zero for x = 2^-60; the answers below are correct to the last
    // few bits of a 64-bit significand.
    const double tiny = ldexp(1.0, -60);
    FNINIT(); push(tiny);
    opr(0xD9, 0xF0);
    check(st(0) != 0.0, "F2XM1 2^-60 is not zero");
    check(std::fabs(st(0) / std::expm1(tiny * M_LN2) - 1.0) < 1e-15,
          "F2XM1 2^-60 = 2^-60 * ln2, to within a double's resolution");
    FNINIT(); push(1.0); push(tiny);
    opr(0xD9, 0xF9);
    check(st(0) != 0.0, "FYL2XP1 2^-60 is not zero");
    check(std::fabs(st(0) / (tiny / M_LN2) - 1.0) < 1e-15,
          "FYL2XP1 2^-60 = 2^-60 / ln2, to within a double's resolution");

    // FYL2X of zero: the value a 387 produces, but without the ZE flag.
    FNINIT(); push(2.0); push(0.0);
    opr(0xD9, 0xF1);
    check(std::isinf(st(0)) && st(0) < 0, "FYL2X 2*log2(0) = -infinity");
    check((sw() & SW_ZE) != 0, "FYL2X of zero raises #Z");
    FNINIT(); push(2.0); push(-1.0);
    opr(0xD9, 0xF1);
    check(st_indef(0) && (sw() & SW_IE) != 0, "FYL2X of a negative is #IA");
  }

  //=========================================================================
  // 15. FCOMI / FUCOMI / FCOMIP / FUCOMIP — the INTEGER flags
  //=========================================================================
  {
    struct { double a, b; bool cf, zf, pf; const char *name; } t[] = {
      {1.0, 2.0, true,  false, false, "less than"},
      {2.0, 2.0, false, true,  false, "equal"},
      {3.0, 2.0, false, false, false, "greater than"},
    };
    char buf[128];
    for (auto &e : t) {
      FNINIT(); push(e.b); push(e.a);           // ST0 = a, ST1 = b
      cpu->flags = 0x0002 | emu88::FLAG_CF | emu88::FLAG_ZF | emu88::FLAG_PF;
      opr(0xDB, 0xF1);                          // FCOMI ST(0),ST(1)
      std::snprintf(buf, sizeof buf, "FCOMI %s: CF", e.name);
      check(cpu->get_flag(emu88::FLAG_CF) == e.cf, buf);
      std::snprintf(buf, sizeof buf, "FCOMI %s: ZF", e.name);
      check(cpu->get_flag(emu88::FLAG_ZF) == e.zf, buf);
      std::snprintf(buf, sizeof buf, "FCOMI %s: PF", e.name);
      check(cpu->get_flag(emu88::FLAG_PF) == e.pf, buf);
      check(ftop() == 6, "FCOMI does not pop");
    }
    FNINIT(); push(1.0); push(dfrom(0x7FF8000000000000ULL));
    cpu->flags = 0x0002;
    opr(0xDB, 0xF1);
    check(cpu->get_flag(emu88::FLAG_CF) && cpu->get_flag(emu88::FLAG_ZF) &&
          cpu->get_flag(emu88::FLAG_PF), "FCOMI with NaN sets CF, ZF and PF");
    check(cc() == 0, "FCOMI writes the EFLAGS, not C0/C2/C3");

    FNINIT(); push(2.0); push(1.0);
    cpu->flags = 0x0002;
    opr(0xDB, 0xE9);                            // FUCOMI ST(0),ST(1)
    check(cpu->get_flag(emu88::FLAG_CF) && !cpu->get_flag(emu88::FLAG_ZF),
          "FUCOMI 1 < 2 sets CF");
    check(ftop() == 6, "FUCOMI does not pop");

    FNINIT(); push(2.0); push(3.0);
    cpu->flags = 0x0002 | emu88::FLAG_CF;
    opr(0xDF, 0xF1);                            // FCOMIP ST(0),ST(1)
    check(!cpu->get_flag(emu88::FLAG_CF), "FCOMIP 3 > 2 clears CF");
    check(ftop() == 7, "FCOMIP pops");
    FNINIT(); push(2.0); push(2.0);
    cpu->flags = 0x0002;
    opr(0xDF, 0xE9);                            // FUCOMIP ST(0),ST(1)
    check(cpu->get_flag(emu88::FLAG_ZF), "FUCOMIP equal sets ZF");
    check(ftop() == 7, "FUCOMIP pops");

    // Intel specifies OF, SF and AF are cleared by all four of these.
    FNINIT(); push(2.0); push(1.0);
    cpu->flags = 0x0002 | emu88::FLAG_OF | emu88::FLAG_SF | emu88::FLAG_AF;
    opr(0xDB, 0xF1);
    check((cpu->flags & (emu88::FLAG_OF | emu88::FLAG_SF | emu88::FLAG_AF)) == 0,
          "FCOMI clears OF, SF and AF");
  }

  //=========================================================================
  // 16. FCMOVcc
  //=========================================================================
  {
    struct { uint8_t esc, modrm; uint16_t flags; bool expect; const char *name; } fc[] = {
      {0xDA, 0xC1, emu88::FLAG_CF, true,  "FCMOVB   with CF=1 moves"},
      {0xDA, 0xC1, 0,              false, "FCMOVB   with CF=0 does not"},
      {0xDA, 0xC9, emu88::FLAG_ZF, true,  "FCMOVE   with ZF=1 moves"},
      {0xDA, 0xC9, 0,              false, "FCMOVE   with ZF=0 does not"},
      {0xDA, 0xD1, emu88::FLAG_ZF, true,  "FCMOVBE  with ZF=1 moves"},
      {0xDA, 0xD1, emu88::FLAG_CF, true,  "FCMOVBE  with CF=1 moves"},
      {0xDA, 0xD1, 0,              false, "FCMOVBE  with CF=0 ZF=0 does not"},
      {0xDA, 0xD9, emu88::FLAG_PF, true,  "FCMOVU   with PF=1 moves"},
      {0xDA, 0xD9, 0,              false, "FCMOVU   with PF=0 does not"},
      {0xDB, 0xC1, 0,              true,  "FCMOVNB  with CF=0 moves"},
      {0xDB, 0xC1, emu88::FLAG_CF, false, "FCMOVNB  with CF=1 does not"},
      {0xDB, 0xC9, 0,              true,  "FCMOVNE  with ZF=0 moves"},
      {0xDB, 0xC9, emu88::FLAG_ZF, false, "FCMOVNE  with ZF=1 does not"},
      {0xDB, 0xD1, 0,              true,  "FCMOVNBE with CF=0 ZF=0 moves"},
      {0xDB, 0xD1, emu88::FLAG_ZF, false, "FCMOVNBE with ZF=1 does not"},
      {0xDB, 0xD1, emu88::FLAG_CF, false, "FCMOVNBE with CF=1 does not"},
      {0xDB, 0xD9, 0,              true,  "FCMOVNU  with PF=0 moves"},
      {0xDB, 0xD9, emu88::FLAG_PF, false, "FCMOVNU  with PF=1 does not"},
    };
    for (auto &e : fc) {
      FNINIT(); push(7.0); push(1.0);           // ST0 = 1, ST1 = 7
      cpu->flags = (uint16_t)(0x0002 | e.flags);
      opr(e.esc, e.modrm);
      check(st(0) == (e.expect ? 7.0 : 1.0), e.name);
      check(ftop() == 6, "FCMOVcc never pops");
    }
    // The tag travels with the value.
    FNINIT(); push(0.0); push(1.0);             // ST1 tagged ZERO
    cpu->flags = 0x0002 | emu88::FLAG_CF;
    opr(0xDA, 0xC1);
    check(tg(0) == TAG_ZERO, "FCMOVcc copies the tag along with the value");
    // Driven by a real STC rather than a poked flag, to prove the path.
    FNINIT(); push(7.0); push(1.0);
    cpu->flags = 0x0002;
    run({0xF9});                                // STC
    opr(0xDA, 0xC1);
    check(st(0) == 7.0, "FCMOVB after a real STC instruction moves");
  }

  //=========================================================================
  // 17. FNSAVE / FRSTOR and FNSTENV / FLDENV
  //=========================================================================
  {
    const uint16_t SAVE = 0x0200;
    for (int i = 0; i < 120; i++) wr8((uint16_t)(SAVE + i), 0xFF);
    FNINIT();
    wr16(0x0100, 0x0B7F); opm(0xD9, 5, 0x0100); // CW = round up
    push(1.5); push(-2.25); push(3.0);          // TOP = 5
    opr(0xDD, 0xC1);                            // FFREE ST(1) -> a hole
    uint16_t saved_sw = sw();
    opm(0xDD, 6, SAVE);                         // FNSAVE (16-bit form)
    check(rd16(SAVE + 0) == 0x0B7F, "FNSAVE writes CW at +0");
    check(rd16(SAVE + 2) == saved_sw, "FNSAVE writes SW at +2");
    // The tag word is in PHYSICAL register order.  TOP is 5, so ST(0), ST(1)
    // and ST(2) are FPR5, FPR6 and FPR7, and the FFREE hole is FPR6:
    //   FPR0..4 empty(3), FPR5 valid(0), FPR6 empty(3), FPR7 valid(0)
    check(rd16(SAVE + 4) == 0x33FF,
          "FNSAVE tag word is in physical register order (0x33FF)");

    // FIP and FDP are real now, not zeros.  The last pointer-updating
    // instruction was the FFREE at CS:0000; the last memory operand was the
    // FLD m64real that pushed 3.0 out of DS:SCRATCH64.  In real mode the
    // environment carries 20-bit LINEAR addresses split across two words, with
    // the 11-bit opcode sharing the second one.
    {
      uint32_t ilin = CS_LIN;                      // insn_ip is 0 for every run()
      uint32_t dlin = DS_LIN + SCRATCH64;
      uint16_t fop  = (uint16_t)(((0xDD & 7) << 8) | 0xC1);   // FFREE ST(1)
      check(rd16(SAVE + 6) == (uint16_t)ilin, "FNSAVE writes FIP[15:0] at +6");
      check(rd16(SAVE + 8) == (uint16_t)((((ilin >> 16) & 0x0F) << 12) | fop),
            "FNSAVE writes FIP[19:16] and the 11-bit opcode at +8");
      check(rd16(SAVE + 10) == (uint16_t)dlin, "FNSAVE writes FDP[15:0] at +10");
      check(rd16(SAVE + 12) == (uint16_t)((((dlin >> 16) & 0x0F) << 12)),
            "FNSAVE writes FDP[19:16] at +12");
    }
    check(rd80_exp((uint16_t)(SAVE + 14)) == 0x4000 &&
          rd80_mant((uint16_t)(SAVE + 14)) == 0xC000000000000000ULL,
          "FNSAVE stores ST(0)=3.0 first, as an 80-bit real");
    check(cw() == 0x037F && sw() == 0 && tg(0) == TAG_EMPTY,
          "FNSAVE re-initialises the FPU afterwards");

    opm(0xDD, 4, SAVE);                         // FRSTOR
    check(cw() == 0x0B7F, "FRSTOR restores CW");
    check(sw() == saved_sw, "FRSTOR restores SW (and therefore TOP)");
    check(ftop() == 5, "FRSTOR restored TOP = 5");
    check(st(0) == 3.0 && st(2) == 1.5, "FRSTOR restored the register values");
    check(tg(0) == TAG_VALID && tg(1) == TAG_EMPTY && tg(2) == TAG_VALID,
          "FRSTOR restored the tags, hole included");
    check(rd16(SAVE + 4) == 0x33FF,
          "the restored image still carries the physical-order tag word");

    // A tag word that DISAGREES with the register data does not round-trip the
    // way it was written.  A real x87 keeps only the EMPTY/non-EMPTY decision
    // from the image and re-derives valid/zero/special from what is actually in
    // each register.  Measured on the host with a hand-built 108-byte image:
    // tag word 0x0000 (all VALID) over +0.0, 1.0, an SNaN and a denormal comes
    // back 0x55A1; 0x5555 and 0xAAAA over eight 1.0s both come back 0x0000;
    // and 0xFFFF and 0x0003 come back unchanged, because EMPTY is honoured.
    {
      const uint16_t IMG = 0x0600;
      struct { uint16_t se; uint64_t sig; } rv[8] = {
        { 0x0000, 0x0000000000000000ULL },   // +0.0      -> ZERO
        { 0x3FFF, 0x8000000000000000ULL },   // 1.0       -> VALID
        { 0x7FFF, 0x8000000000000001ULL },   // SNaN      -> SPECIAL
        { 0x0000, 0x0000000000000001ULL },   // denormal  -> SPECIAL
        { 0x0000, 0x0000000000000000ULL }, { 0x0000, 0x0000000000000000ULL },
        { 0x0000, 0x0000000000000000ULL }, { 0x0000, 0x0000000000000000ULL },
      };
      static const uint16_t in_tw[]  = { 0x0000, 0x5555, 0xAAAA, 0xFFFF, 0x0003 };
      static const uint16_t out_tw[] = { 0x55A1, 0x55A1, 0x55A1, 0xFFFF, 0x55A3 };
      int tbad = 0;
      for (unsigned k = 0; k < sizeof in_tw / sizeof in_tw[0]; k++) {
        for (int i = 0; i < 128; i++) wr8((uint16_t)(IMG + i), 0);
        wr32(IMG + 0, 0x037F); wr32(IMG + 4, 0x0000); wr32(IMG + 8, in_tw[k]);
        for (int i = 0; i < 8; i++) {
          wr64((uint16_t)(IMG + 28 + 10 * i), rv[i].sig);
          wr16((uint16_t)(IMG + 28 + 10 * i + 8), rv[i].se);
        }
        FNINIT();
        run({0x66, 0xDD, mrm_disp16(4), (uint8_t)(IMG & 0xFF), (uint8_t)(IMG >> 8)});  // FRSTOR
        const uint16_t OUT = 0x0700;
        for (int i = 0; i < 32; i++) wr8((uint16_t)(OUT + i), 0xAA);
        run({0x66, 0xD9, mrm_disp16(6), (uint8_t)(OUT & 0xFF), (uint8_t)(OUT >> 8)});  // FNSTENV
        if ((uint16_t)rd32(OUT + 8) != out_tw[k]) tbad++;
      }
      check(tbad == 0,
            "FRSTOR re-derives the non-EMPTY tags from the data and honours EMPTY");
    }

    // The 32-bit (108-byte) form has 32-bit environment fields, so the whole
    // of each FIP/FDP field is cleared, not just its low word.
    for (int i = 0; i < 120; i++) wr8((uint16_t)(SAVE + i), 0xFF);
    FNINIT(); push(1.0);
    run({0x66, 0xDD, mrm_disp16(6), (uint8_t)(SAVE & 0xFF), (uint8_t)(SAVE >> 8)});
    check(rd8((uint16_t)(SAVE + 14)) == 0 && rd8((uint16_t)(SAVE + 15)) == 0,
          "FNSAVE 32-bit form zeroes all of the FIP field at +12..+15");

    // The rest of the 32-bit image, and the round trip back through it.  The
    // 32-bit environment is 28 bytes, not 14, so CW/SW/TW sit at +0/+4/+8 and
    // the eight 80-bit registers start at +28 — an offset FRSTOR has to agree
    // with exactly, or a save/restore pair silently shifts every register.
    for (int i = 0; i < 120; i++) wr8((uint16_t)(SAVE + i), 0xFF);
    FNINIT();
    wr16(0x0100, 0x0B7F); opm(0xD9, 5, 0x0100); // CW = round up
    push(1.5); push(-2.25); push(3.0);          // TOP = 5
    opr(0xDD, 0xC1);                            // FFREE ST(1) -> a hole
    uint16_t saved32_sw = sw();
    run({0x66, 0xDD, mrm_disp16(6), (uint8_t)(SAVE & 0xFF), (uint8_t)(SAVE >> 8)});
    check(rd16(SAVE + 0) == 0x0B7F, "FNSAVE32 writes CW at +0");
    check(rd16(SAVE + 4) == saved32_sw, "FNSAVE32 writes SW at +4 (32-bit stride)");
    check(rd16(SAVE + 8) == 0x33FF, "FNSAVE32 writes the physical tag word at +8");
    check(rd80_exp((uint16_t)(SAVE + 28)) == 0x4000 &&
          rd80_mant((uint16_t)(SAVE + 28)) == 0xC000000000000000ULL,
          "FNSAVE32 stores ST(0)=3.0 at +28, where the register area begins");
    check(rd80_exp((uint16_t)(SAVE + 48)) == 0x3FFF &&
          rd80_mant((uint16_t)(SAVE + 48)) == 0xC000000000000000ULL,
          "FNSAVE32 stores ST(2)=1.5 at +48 (registers are 10 bytes apart)");
    check(cw() == 0x037F && sw() == 0 && tg(0) == TAG_EMPTY,
          "FNSAVE32 re-initialises the FPU afterwards");

    FNINIT();
    run({0x66, 0xDD, mrm_disp16(4), (uint8_t)(SAVE & 0xFF), (uint8_t)(SAVE >> 8)});
    check(cw() == 0x0B7F, "FRSTOR32 restores CW from +0");
    check(sw() == saved32_sw, "FRSTOR32 restores SW from +4");
    check(ftop() == 5, "FRSTOR32 restored TOP = 5");
    check(st(0) == 3.0 && st(2) == 1.5, "FRSTOR32 restored the register values from +28");
    check(tg(0) == TAG_VALID && tg(1) == TAG_EMPTY && tg(2) == TAG_VALID,
          "FRSTOR32 restored the tags from +8, hole included");

    // FNSTENV / FLDENV: all seven environment fields, in the layout the
    // operand size and the processor mode select.
    const uint16_t ENV = 0x0300;
    for (int i = 0; i < 32; i++) wr8((uint16_t)(ENV + i), 0xAA);
    FNINIT();
    wr16(0x0100, 0x077F); opm(0xD9, 5, 0x0100);
    push(1.0); push(2.0);                       // TOP = 6
    opr(0xDD, 0xE1);                            // FUCOM ST(1) -> C3=0,C0=0 (2 > 1)
    uint16_t env_sw = sw();
    opm(0xD9, 6, ENV);                          // FNSTENV
    check(rd16(ENV + 0) == 0x077F, "FNSTENV writes CW at +0");
    check(rd16(ENV + 2) == env_sw, "FNSTENV writes SW at +2");
    // TOP is 6 and two registers are live, so FPR6 and FPR7 are valid and the
    // other six are empty: 0x0FFF.
    check(rd16(ENV + 4) == 0x0FFF, "FNSTENV writes the tag word at +4");
    FNINIT();
    check(cw() == 0x037F && ftop() == 0, "FNINIT wiped the environment");
    opm(0xD9, 4, ENV);                          // FLDENV
    check(cw() == 0x077F, "FLDENV restores CW");
    check(sw() == env_sw && ftop() == 6, "FLDENV restores SW and TOP");
    check(tg(0) == TAG_VALID && tg(1) == TAG_VALID && tg(2) == TAG_EMPTY,
          "FLDENV restores the tag word");
    // Which of the four layouts is selected depends on the operand size AND on
    // whether the processor is in protected mode - and virtual-8086 mode has
    // CR0.PE set but uses the REAL-address-mode layout, which is the one case
    // where testing CR0.PE alone gives the wrong answer.  In the real layout
    // the word at +8 carries the 11-bit opcode; in the protected one it is the
    // code selector.
    FNINIT();
    push(1.0);                                  // sets FIP/FCS/FOP
    for (int i = 0; i < 32; i++) wr8((uint16_t)(ENV + i), 0xAA);
    opm(0xD9, 6, ENV);                          // real mode
    uint16_t fop_fld = (uint16_t)(((0xDD & 7) << 8) | mrm_disp16(0));
    check(rd16(ENV + 8) == (uint16_t)((((CS_LIN >> 16) & 0x0F) << 12) | fop_fld),
          "FNSTENV in real mode writes the opcode at +8");
    cpu->cr0 |= emu88::CR0_PE;
    cpu->eflags_hi |= (uint16_t)(emu88::EFLAG_VM >> 16);
    for (int i = 0; i < 32; i++) wr8((uint16_t)(ENV + i), 0xAA);
    opm(0xD9, 6, ENV);
    check(rd16(ENV + 8) == (uint16_t)((((CS_LIN >> 16) & 0x0F) << 12) | fop_fld),
          "V86 mode takes the REAL layout even though CR0.PE is set");
    cpu->eflags_hi = 0;
    for (int i = 0; i < 32; i++) wr8((uint16_t)(ENV + i), 0xAA);
    opm(0xD9, 6, ENV);
    check(rd16(ENV + 8) == CS_SEL,
          "protected mode writes the code selector at +8 instead");

    // The reserved upper halves of the 16-bit fields in the 32-BIT image are
    // written as ONES.  Measured on the host with the destination pre-poisoned
    // with 0x00, 0xAA and 0x5A, identical every time, so they are stored and
    // not left over: FNINIT then FNSTENV32 gives +00=FFFF037F, +04=FFFF0000,
    // +08=FFFFFFFF, +18=FFFF0000.  The three dwords carrying a full 32 bits -
    // FIP, the selector-and-opcode, FDP - have no reserved half and get none.
    for (int i = 0; i < 32; i++) wr8((uint16_t)(ENV + i), 0xAA);
    FNINIT();
    run({0x66, 0xD9, mrm_disp16(6), (uint8_t)(ENV & 0xFF), (uint8_t)(ENV >> 8)});
    check((rd32(ENV + 0) >> 16) == 0xFFFF, "FNSTENV32 fills the reserved half of +0 with ones");
    check((rd32(ENV + 4) >> 16) == 0xFFFF, "...and of +4");
    check((rd32(ENV + 8) >> 16) == 0xFFFF, "...and of +8");
    check((rd32(ENV + 24) >> 16) == 0xFFFF, "...and of the operand selector at +24");
    check((rd32(ENV + 12) >> 16) == 0x0000,
          "...but FIP at +12 is a full 32-bit field and keeps its high half");
    cpu->cr0 &= (uint32_t)~emu88::CR0_PE;
    setup();

    // The 32-bit real-address-mode image is not the 16-bit one widened.  The
    // pointer's high bits go at bits 27:16 of +16 and +24, with twelve bits of
    // room; the 16-bit layout's bits 15:12, with four, is a different picture
    // and is what this used to assert.  A real-mode linear address needs
    // twenty-ONE bits ((0xFFFF << 4) + 0xFFFF is 0x10FFEF), so the old packing
    // truncated as well as misplaced - and nothing reached it until this
    // fixture, because the harness's own CS puts every address under 2^20.
    FNINIT();
    cpu->fpu.fcs = 0xFFFF; cpu->fpu.fip = 0xFFFF;
    cpu->fpu.fds = 0xFFFF; cpu->fpu.fdp = 0xFFFF;
    cpu->fpu.fop = 0x01DD;
    for (int i = 0; i < 32; i++) wr8((uint16_t)(ENV + i), 0xAA);
    run({0x66, 0xD9, mrm_disp16(6), (uint8_t)(ENV & 0xFF), (uint8_t)(ENV >> 8)});
    check((rd32(ENV + 12) & 0xFFFF) == 0xFFEF,
          "FNSTENV32 writes the low word of the 21-bit linear address at +12");
    check(rd32(ENV + 16) == 0x001001DD,
          "FNSTENV32 puts IP[31:16] at bits 27:16 of +16, with the opcode below");
    check(rd32(ENV + 24) == 0x00100000,
          "...and OP[31:16] at bits 27:16 of +24");
    // And back: the round trip has to survive the full twenty-one bits.
    cpu->fpu.fip = 0; cpu->fpu.fdp = 0; cpu->fpu.fcs = 0; cpu->fpu.fds = 0;
    run({0x66, 0xD9, mrm_disp16(4), (uint8_t)(ENV & 0xFF), (uint8_t)(ENV >> 8)});
    check(cpu->fpu.fip == 0x0010FFEF && cpu->fpu.fdp == 0x0010FFEF,
          "FLDENV32 reads all twenty-one bits of both pointers back");
    FNINIT();

    // FNSTENV masks every exception afterwards, so the handler it is about to
    // run cannot be re-entered by its own arithmetic.
    FNINIT();
    wr16(0x0100, 0x0000); opm(0xD9, 5, 0x0100); // all exceptions unmasked
    opm(0xD9, 6, ENV);
    check(cw() == 0x003F, "FNSTENV masks all six exceptions afterwards");
    check(rd16(ENV + 0) == 0x0000, "...but writes the PRE-mask control word");
    FNINIT();
  }

  //=========================================================================
  // 17b. ES and B are a FUNCTION of the other two words, not a latch
  //=========================================================================
  // ES is the OR of the exception flags that are currently unmasked, and B
  // follows it on a 387.  So an instruction that raises nothing at all can
  // still change ES, in either direction, purely by moving SW or CW - and
  // three of them do: FLDCW, FLDENV and FRSTOR load one or both words, and
  // FNSTENV masks all six after storing.
  //
  // Every expected value below was measured on the host x87 before it was
  // written here, and the whole block was run against the unfixed core first:
  // it failed five of these nine, which is the only reason they are worth
  // having.  The four that passed unfixed are kept deliberately - the old
  // code could set ES and never clear it, so the SETTING direction was
  // already right and a test set that only covered it would have proved
  // nothing.
  {
    const uint16_t ENVB = 0x0500, CWSLOT = 0x0110;

    // Setting direction: a masked #Z leaves ES clear, and an FLDCW that
    // unmasks ZM makes ES appear with no arithmetic in between.
    FNINIT();
    wr16(CWSLOT, 0x037F); opm(0xD9, 5, CWSLOT);      // all masked
    push(1.0); push(0.0); opr(0xDE, 0xF9);           // FLD1; FLDZ; FDIVP -> #Z
    check(sw() == 0x3804, "a MASKED #Z leaves SW=3804 - ZE set, ES clear");
    wr16(CWSLOT, 0x037B); opm(0xD9, 5, CWSLOT);      // unmask ZM only
    check(sw() == 0xB884, "FLDCW unmasking ZM raises ES and B out of nothing");

    // Clearing direction.  FNSTENV stores the environment and THEN masks all
    // six, so the live word must come back with ES clear while ZE stays set -
    // and the image it wrote keeps the pre-mask ES.
    opm(0xD9, 6, ENVB);
    check(sw() == 0x3804, "FNSTENV masks all six, so live ES clears; ZE stays");
    check(rd16(ENVB + 2) == 0xB884, "...while the STORED image keeps ES set");

    // FLDENV: the ES arriving in the image is discarded and recomputed from
    // the loaded flags against the loaded mask.  The first four cases are the
    // ones the unfixed core got wrong - it stored the loaded word verbatim.
    struct EnvCase { uint16_t sw_in, cw_in, sw_out; const char *what; };
    static const EnvCase envc[] = {
      { 0xB880, 0x037F, 0x3800, "ES set in the image but no flag set: cleared" },
      { 0xB884, 0x037F, 0x3804, "ES set, ZE set, ZM masked: cleared" },
      { 0x3884, 0x037F, 0x3804, "ES clear, ZE set, ZM masked: stays clear" },
      { 0xB800, 0x037F, 0x3800, "ES set alone with everything masked: cleared" },
      { 0x3804, 0x037B, 0xB884, "ES clear in the image but ZM unmasked: SET" },
    };
    for (const EnvCase &e : envc) {
      FNINIT();
      for (int i = 0; i < 14; i++) wr8((uint16_t)(ENVB + i), 0);
      wr16(ENVB + 0, e.cw_in); wr16(ENVB + 2, e.sw_in); wr16(ENVB + 4, 0xFFFF);
      opm(0xD9, 4, ENVB);                             // FLDENV
      char msg[96];
      std::snprintf(msg, sizeof msg, "FLDENV sw=%04X cw=%04X -> %04X: %s",
                    e.sw_in, e.cw_in, e.sw_out, e.what);
      check(sw() == e.sw_out, msg);
    }
    FNINIT();
  }

  //=========================================================================
  // 18. FBLD / FBSTP (packed BCD)
  //=========================================================================
  {
    const uint16_t BCD = 0x0400;
    FNINIT(); push(12345.0);
    for (int i = 0; i < 10; i++) wr8((uint16_t)(BCD + i), 0xEE);
    opm(0xDF, 6, BCD);                          // FBSTP
    check(rd8(BCD + 0) == 0x45 && rd8(BCD + 1) == 0x23 && rd8(BCD + 2) == 0x01,
          "FBSTP 12345 packs digits little-endian: 45 23 01");
    check(rd8(BCD + 8) == 0x00 && rd8(BCD + 9) == 0x00, "FBSTP 12345: sign byte 0");
    check(ftop() == 0, "FBSTP pops");
    FNINIT(); push(-12345.0);
    opm(0xDF, 6, BCD);
    check(rd8(BCD + 9) == 0x80, "FBSTP -12345 sets the sign byte to 0x80");
    check(rd8(BCD + 0) == 0x45, "FBSTP stores the magnitude");
    FNINIT();
    opm(0xDF, 4, BCD);                          // FBLD
    check(st(0) == -12345.0, "FBLD reads the packed BCD back, sign included");
    FNINIT(); push(987654321.0);
    opm(0xDF, 6, BCD);
    opm(0xDF, 4, BCD);
    check(st(0) == 987654321.0, "FBSTP/FBLD round-trip of 987654321");
  }

  //=========================================================================
  // 19. CR0.EM / CR0.TS gate the escape opcodes (#NM)
  //=========================================================================
  {
    setup();
    FNINIT();
    cpu->cr0 |= emu88::CR0_EM;
    run({0xD9, 0xE8});                          // FLD1
    check(cpu->exception_pending, "FLD1 with CR0.EM set raises an exception (#NM)");
    check(ftop() == 0 && cpu->fpu.tags[7] == TAG_EMPTY,
          "the trapped FLD1 pushed nothing");
    setup();
    FNINIT();
    cpu->cr0 |= emu88::CR0_TS;
    run({0xD9, 0xE8});
    check(cpu->exception_pending, "FLD1 with CR0.TS set raises an exception (#NM)");
    setup();
    FNINIT();
    run({0xD9, 0xE8});
    check(!cpu->exception_pending && st(0) == 1.0,
          "with CR0.EM and CR0.TS clear the same FLD1 executes");
  }

  //=========================================================================
  // 19b. #MF: an unmasked exception is DELIVERED, and not by the instruction
  //      that raised it
  //=========================================================================
  // Everything asserted here was measured on the host x87 first (deferred
  // delivery, the ten no-wait encodings, #MF beating an operand fault, FIP
  // pointing at the raising instruction).
  //
  // Run against the core BEFORE delivery existed, this block failed 6 of its
  // 23 checks, and the other 17 passed VACUOUSLY - they assert that some
  // encoding does NOT report, which is trivially true of a core that never
  // reports anything.  That is worth writing down rather than quoting "23
  // checks" and moving on: a no-wait assertion here is evidence only in a
  // build where the waiting assertions next to it fail without the fix.  The
  // six that discriminate are the two reporting points (a waiting escape and
  // FWAIT), the two encodings the naive no-wait table gets wrong (D9 F8 and
  // FNOP), the operand-fault priority, and the fault's no-side-effects rule.
  //
  // The vector is checked rather than assumed: real-mode do_interrupt loads
  // CS:IP from the IVT, so a distinctive IVT entry per vector says which one
  // was taken.  That matters more here than anywhere else in this file,
  // because #MF is vector 16 == INT 10h and the wrong answer is a plausible
  // one.
  {
    const uint16_t CWSLOT = 0x0120;
    const uint16_t MF_SEG = 0x7A00, MF_OFF = 0x00F0;   // IVT[16]
    const uint16_t GP_SEG = 0x7B00, GP_OFF = 0x00E0;   // IVT[13]
    auto set_ivt = [&](uint8_t vec, uint16_t seg, uint16_t off) {
      mem->store_mem16(vec * 4 + 0, off);
      mem->store_mem16(vec * 4 + 2, seg);
    };
    auto took = [&](uint16_t seg, uint16_t off) {
      return cpu->sregs[emu88::seg_CS] == seg && cpu->ip == off;
    };
    // Raise an unmasked #Z.  1.0/0.0 with ZM clear.
    auto arm = [&]() {
      setup(); FNINIT();
      set_ivt(16, MF_SEG, MF_OFF);
      set_ivt(13, GP_SEG, GP_OFF);
      cpu->cr0 |= emu88::CR0_NE;
      wr16(CWSLOT, 0x037B); opm(0xD9, 5, CWSLOT);      // unmask ZM only
      push(1.0); push(0.0); opr(0xDE, 0xF9);           // FDIVP -> #Z
    };

    arm();
    check(!cpu->exception_pending,
          "#MF: the instruction that RAISES an unmasked exception does not trap");
    check((sw() & 0x0080) != 0, "...it latches ES instead");
    uint32_t raiser_fip = cpu->fpu.fip;
    int top_before = ftop();

    // The next waiting instruction reports it.
    run({0xD9, 0xE8});                                 // FLD1
    check(cpu->exception_pending && took(MF_SEG, MF_OFF),
          "#MF: the NEXT waiting instruction takes vector 16");
    check(cpu->fpu.fip == raiser_fip,
          "#MF: FIP still points at the instruction that raised, not the reporter");
    // #MF is a FAULT: the reporting instruction has not run, so the stack it
    // would have pushed onto is untouched and IRET re-executes it.
    check(ftop() == top_before && cpu->fpu.tags[(top_before + 7) & 7] == TAG_EMPTY,
          "#MF: the reporting FLD1 pushed nothing");

    // WAIT is the other reporting point, and the one that makes every 9B-
    // prefixed control instruction (FSTSW, FCLEX, FSTENV...) behave.
    arm();
    run({0x9B});
    check(cpu->exception_pending && took(MF_SEG, MF_OFF),
          "#MF: FWAIT (9B) reports a pending exception");

    // The ten no-wait encodings must NOT report.  This is the exemption that
    // lets a handler look at the FPU without re-entering itself.
    struct NW { uint8_t esc, modrm; const char *name; };
    static const NW nowait[] = {
      { 0xDF, 0xE0, "FNSTSW AX  DF E0" }, { 0xDB, 0xE0, "FNENI      DB E0" },
      { 0xDB, 0xE1, "FNDISI     DB E1" }, { 0xDB, 0xE2, "FNCLEX     DB E2" },
      { 0xDB, 0xE3, "FNINIT     DB E3" }, { 0xDB, 0xE4, "FNSETPM    DB E4" },
    };
    for (const NW &n : nowait) {
      arm();
      opr(n.esc, n.modrm);
      char msg[96];
      std::snprintf(msg, sizeof msg, "#MF: %s is no-wait and does not report", n.name);
      check(!cpu->exception_pending, msg);
    }
    // The four memory-form no-wait encodings, D9 /6 /7 and DD /6 /7.
    static const NW nowaitm[] = {
      { 0xD9, 6, "FNSTENV  D9 /6" }, { 0xD9, 7, "FNSTCW   D9 /7" },
      { 0xDD, 6, "FNSAVE   DD /6" }, { 0xDD, 7, "FNSTSW m DD /7" },
    };
    for (const NW &n : nowaitm) {
      arm();
      opm(n.esc, n.modrm, 0x0600);
      char msg[96];
      std::snprintf(msg, sizeof msg, "#MF: %s is no-wait and does not report", n.name);
      check(!cpu->exception_pending, msg);
    }

    // ...and the mod==3 forms that SHARE those reg fields are arithmetic, so
    // they do report.  This is the pair the naive table gets wrong.
    arm();
    opr(0xD9, 0xF8);                                   // FPREM, i.e. D9 /7 mod==3
    check(cpu->exception_pending && took(MF_SEG, MF_OFF),
          "#MF: D9 F8 (FPREM) shares /7 with FNSTCW but is WAITING");
    arm();
    opr(0xD9, 0xD0);                                   // FNOP
    check(cpu->exception_pending && took(MF_SEG, MF_OFF),
          "#MF: FNOP is WAITING despite the mnemonic");

    // Clearing the condition stops the reporting, by either documented route.
    arm();
    opr(0xDB, 0xE2);                                   // FNCLEX
    run({0xD9, 0xE8});                                 // FLD1
    check(!cpu->exception_pending && st(0) == 1.0,
          "#MF: FNCLEX clears ES, so the next waiting instruction runs");
    arm();
    opm(0xD9, 6, 0x0600);                              // FNSTENV masks all six
    run({0xD9, 0xE8});
    check(!cpu->exception_pending && st(0) == 1.0,
          "#MF: FNSTENV masks all six, so the next waiting instruction runs");

    // Priority: a pending exception outranks this instruction's own operand
    // fault.  FADD m32 at 0xFFFE crosses the real-mode segment limit, which
    // on its own is a #GP - with #Z pending it must report 16, not 13.
    arm();
    opm(0xD8, 0, 0xFFFE);
    check(cpu->exception_pending && took(MF_SEG, MF_OFF),
          "#MF: a pending exception outranks the reporting instruction's operand fault");
    setup(); FNINIT();
    set_ivt(13, GP_SEG, GP_OFF);
    opm(0xD8, 0, 0xFFFE);
    check(cpu->exception_pending && took(GP_SEG, GP_OFF),
          "...and with nothing pending the same operand fault is taken");

    // With CR0.NE clear there is nothing attached to ERROR# on a bare core,
    // so the instruction runs.  This is what dosiz and any non-PC embedder
    // see, and it is why dos_machine overrides fpu_signal_error.
    setup(); FNINIT();
    set_ivt(16, MF_SEG, MF_OFF);
    cpu->cr0 &= ~(uint32_t)emu88::CR0_NE;
    wr16(CWSLOT, 0x037B); opm(0xD9, 5, CWSLOT);
    push(1.0); push(0.0); opr(0xDE, 0xF9);
    run({0xD9, 0xE8});
    check(!cpu->exception_pending,
          "#MF: with CR0.NE clear the bare core reports nowhere and runs on");
    setup();
  }

  //=========================================================================
  // 20. A longer sequence: the decoder, the stack and the arithmetic together
  //=========================================================================
  {
    // Compute ((3 + 4) * 2 - 5) / 0.5 == 18 the way a compiler would.
    FNINIT();
    wrd(0x0110, 3.0); FLDm64(0x0110);
    wrd(0x0110, 4.0); opm(0xDC, 0, 0x0110);     // FADD m64  -> 7
    wrd(0x0110, 2.0); opm(0xDC, 1, 0x0110);     // FMUL m64  -> 14
    wrd(0x0110, 5.0); opm(0xDC, 4, 0x0110);     // FSUB m64  -> 9
    wrd(0x0110, 0.5); opm(0xDC, 6, 0x0110);     // FDIV m64  -> 18
    check(st(0) == 18.0, "((3+4)*2-5)/0.5 = 18 through five memory-form ops");
    check(ftop() == 7, "only one register is in use at the end");
    wr64(0x0118, 0);
    FSTPm64(0x0118);
    check(rdd(0x0118) == 18.0 && ftop() == 0, "the result stores and the stack empties");

    // Same sum on the register stack, ending with FSTSW AX after a compare.
    FNINIT();
    push(18.0); push(9.0); push(2.0);           // ST0=2 ST1=9 ST2=18
    opr(0xDE, 0xC9);                            // FMULP ST(1),ST(0) -> ST0 = 18
    check(st(0) == 18.0 && ftop() == 6, "FMULP folded 9*2");
    opr(0xDA, 0xE9);                            // FUCOMPP: 18 vs 18
    opr(0xDF, 0xE0);                            // FNSTSW AX
    check((cpu->get_reg16(emu88::reg_AX) & 0x4500) == 0x4000,
          "FUCOMPP + FSTSW AX reports equality in AH");
    check(ftop() == 0, "and the stack is empty again");
  }


  //=========================================================================
  // 21. What the 80-bit register file makes reachable at all
  //
  // Everything in this section was impossible to express while ST(0) was a
  // host double: there was no denormal class, no unsupported class, no
  // rounding to a chosen number of significand bits, and no way to see an
  // overflow or a gradual underflow because the double's own exponent range
  // ran out first.
  //=========================================================================
  {
    // --- Gradual underflow.  2^-16400 is below the smallest normal but above
    // the smallest denormal, so it is representable only as a denormal, and
    // arriving there costs #U and #P.
    FNINIT();
    push80(0x0001, 0x8000000000000000ULL);      // 2^-16382, the smallest normal
    push80(0x0001, 0x8000000000000000ULL);
    wrd(0x0110, ldexp(1.0, -20));
    FNINIT();
    push80(0x0001, 0xC000000000000000ULL);      // 1.5 * 2^-16382
    push(0.5);
    opr(0xDE, 0xC9);                            // FMULP ST(1), ST(0)
    check(st_is(0, 0x0000, 0x6000000000000000ULL),
          "a product below the normal range becomes a denormal");
    check((sw() & SW_UE) == 0 && (sw() & SW_PE) == 0,
          "...with no #U when the denormal is exact");
    FNINIT();
    push80(0x0000, 0x0000000000000003ULL);      // 3 * 2^-16445
    push(0.5);
    opr(0xDE, 0xC9);
    check(st_is(0, 0x0000, 0x0000000000000002ULL),
          "3 * 2^-16445 halved rounds to 2 * 2^-16445 (nearest-even)");
    check((sw() & (SW_UE | SW_PE)) == (SW_UE | SW_PE),
          "...and an INEXACT denormal result raises both #U and #P");
    check((sw() & SW_DE) != 0, "the denormal operand raises #D as well");

    // --- Overflow.  The largest finite value doubled leaves the range.
    FNINIT();
    push80(0x7FFE, 0xFFFFFFFFFFFFFFFFULL);
    push(2.0);
    opr(0xDE, 0xC9);
    check(st_is(0, 0x7FFF, 0x8000000000000000ULL), "an overflow delivers +infinity");
    check((sw() & (SW_OE | SW_PE)) == (SW_OE | SW_PE), "...with #O and #P");
    // Under round-toward-zero the masked response is the largest finite value
    // instead, and under PC=24 that value has only 24 significand bits.
    FNINIT();
    wr16(0x0100, 0x0F7F); opm(0xD9, 5, 0x0100); // RC = truncate, PC = 64
    push80(0x7FFE, 0xFFFFFFFFFFFFFFFFULL);
    push(2.0);
    opr(0xDE, 0xC9);
    check(st_is(0, 0x7FFE, 0xFFFFFFFFFFFFFFFFULL),
          "round-to-zero overflow delivers the largest finite value");
    FNINIT();
    wr16(0x0100, 0x0C3F); opm(0xD9, 5, 0x0100); // RC = truncate, PC = 24
    push80(0x7FFE, 0xFFFFFFFFFFFFFFFFULL);
    push(2.0);
    opr(0xDE, 0xC9);
    check(st_is(0, 0x7FFE, 0xFFFFFF0000000000ULL),
          "...and under PC=24 that is the largest finite 24-bit value");
    FNINIT();

    // --- Rounding control really does reach the arithmetic.  1/3 under each
    // of the four modes; nearest and up agree here, down and truncate agree.
    struct { uint16_t cw; uint64_t sig; const char *name; } rcs[] = {
      { 0x037F, 0xAAAAAAAAAAAAAAABULL, "nearest"  },
      { 0x077F, 0xAAAAAAAAAAAAAAAAULL, "down"     },
      { 0x0B7F, 0xAAAAAAAAAAAAAAABULL, "up"       },
      { 0x0F7F, 0xAAAAAAAAAAAAAAAAULL, "truncate" },
    };
    bool rc_ok = true;
    for (auto &k : rcs) {
      FNINIT();
      wr16(0x0100, k.cw); opm(0xD9, 5, 0x0100);
      push(1.0); push(3.0);
      opr(0xDE, 0xF9);                          // FDIVP ST(1), ST(0) -> 1/3
      if (!st_is(0, 0x3FFD, k.sig)) rc_ok = false;
    }
    check(rc_ok, "all four rounding modes reach FDIV's 64th significand bit");
    FNINIT();

    // --- Narrowing stores round under the guest's RC, not the host's.
    wr16(0x0100, 0x077F); opm(0xD9, 5, 0x0100); // round down
    push(1.0); wrd(0x0110, ldexp(1.0, -60)); opm(0xDC, 0, 0x0110);
    FSTPm64(0x0118);
    check(rd64(0x0118) == 0x3FF0000000000000ULL,
          "FSTP m64real rounds 1+2^-60 DOWN to 1.0 under RC=down");
    FNINIT();
    wr16(0x0100, 0x0B7F); opm(0xD9, 5, 0x0100); // round up
    push(1.0); wrd(0x0110, ldexp(1.0, -60)); opm(0xDC, 0, 0x0110);
    FSTPm64(0x0118);
    check(rd64(0x0118) == 0x3FF0000000000001ULL,
          "...and UP to the next double under RC=up");
    check((sw() & SW_PE) != 0, "a narrowing store that rounds reports #P");
    FNINIT();

    // --- A signalling NaN in a m32real or m64real source DOES raise on the
    // load, unlike the m80real one, because that load is a conversion.
    FNINIT();
    wr32(0x0110, 0x7FA00000u);                  // single-precision SNaN
    opm(0xD9, 0, 0x0110);                       // FLD m32real
    check((sw() & SW_IE) != 0, "FLD m32real of an SNaN raises #IA");
    check(st_is(0, 0x7FFF, 0xE000000000000000ULL), "...and delivers it quieted");
    FNINIT();
    wr64(0x0110, 0x7FF4000000000000ULL);        // double-precision SNaN
    FLDm64(0x0110);
    check((sw() & SW_IE) != 0, "FLD m64real of an SNaN raises #IA too");
    // A denormal single or double raises #D on the load and normalises.
    FNINIT();
    wr32(0x0110, 0x00000001u);                  // the smallest single denormal
    opm(0xD9, 0, 0x0110);
    check((sw() & SW_DE) != 0, "FLD m32real of a denormal raises #D");
    check(st_is(0, 0x3F6A, 0x8000000000000000ULL), "...and normalises it to 2^-149");

    // --- NaN propagation picks the larger significand.
    FNINIT();
    push80(0x7FFF, 0xC000000000000001ULL);      // the smaller payload
    push80(0x7FFF, 0xE000000000000000ULL);      // the larger
    opr(0xDE, 0xC1);                            // FADDP ST(1), ST(0)
    check(st_is(0, 0x7FFF, 0xE000000000000000ULL),
          "with two NaNs the larger significand wins");

    // --- FFREEP (DF C0+i), which GCC emits as a cheap pop and which used to
    // be decoded as nothing at all, leaving the stack one deeper than the
    // compiler believed.
    FNINIT();
    push(1.0); push(2.0);
    opr(0xDF, 0xC0);                            // FFREEP ST(0)
    check(ftop() == 7 && st(0) == 1.0, "FFREEP ST(0) frees and pops");
    check(tg(1) == TAG_EMPTY, "...leaving the vacated slot empty");

    // --- The other four undocumented alias groups, for the same reason: three
    // of the four POP, so decoding them as nothing left the stack one deeper
    // every pass.  All four were measured on the host before being decoded
    // here; the comparison instruction in each case is the documented
    // encoding that does the same thing.
    //   D9 D8-DF  FSTP1 ST(i)   = DD D8-DF
    //   DF C8-CF  FXCH7 ST(i)   = D9 C8-CF
    //   DF D0-D7  FSTP8 ST(i)   = DD D8-DF
    //   DF D8-DF  FSTP9 ST(i)   = DD D8-DF
    FNINIT();
    push(1.5); push(2.5); push(3.5);
    opr(0xD9, 0xDA);                            // FSTP1 ST(2)
    check(st(0) == 2.5 && st(1) == 3.5,
          "D9 D8+i is FSTP ST(i): it stores THEN pops, not a bare pop");
    FNINIT();
    push(1.5); push(2.5); push(3.5);
    opr(0xDF, 0xC9);                            // FXCH7 ST(1)
    check(st(0) == 2.5 && st(1) == 3.5 && ftop() == 5,
          "DF C8+i is FXCH ST(i), and does not pop");
    for (int enc = 0; enc < 2; enc++) {
      FNINIT();
      push(1.5); push(2.5); push(3.5);
      opr(0xDF, (uint8_t)(enc ? 0xD9 : 0xD1));  // FSTP8 / FSTP9 ST(1)
      check(st(0) == 3.5 && st(1) == 1.5 && ftop() == 6,
            enc ? "DF D8+i is FSTP ST(i)" : "DF D0+i is FSTP ST(i)");
    }

    // --- FNSAVE / FRSTOR are lossless for every encoding class now, which is
    // the property that made a save/restore pair safe to use at all.
    {
      const uint16_t S2 = 0x0500;
      struct { uint16_t se; uint64_t sig; } vals[8] = {
        { 0x0000, 0x0000000000000001ULL },       // denormal
        { 0x7FFF, 0x8000000000000000ULL },       // +infinity
        { 0xFFFF, 0xA123456789ABCDEFULL },       // negative SNaN with a payload
        { 0x7FFF, 0xDEADBEEFDEADBEEFULL },       // QNaN with a payload
        { 0x4000, 0x4000000000000000ULL },       // unnormal (unsupported)
        { 0x3FFF, 0xFFFFFFFFFFFFFFFFULL },       // a full 64-bit significand
        { 0x8000, 0x0000000000000000ULL },       // negative zero
        { 0x0001, 0x8000000000000000ULL },       // the smallest normal
      };
      FNINIT();
      for (int i = 7; i >= 0; i--) push80(vals[i].se, vals[i].sig);
      opm(0xDD, 6, S2);                          // FNSAVE
      FNINIT();
      opm(0xDD, 4, S2);                          // FRSTOR
      bool lossless = true;
      for (int i = 0; i < 8; i++)
        if (!st_is(i, vals[i].se, vals[i].sig)) lossless = false;
      check(lossless, "FNSAVE/FRSTOR round-trip all eight encoding classes exactly");
    }
  }


  //=========================================================================
  // 22. A faulting memory operand aborts the whole instruction
  //
  // #GP, #PF and #SS are FAULTS.  The handler returns to the same instruction
  // and it runs again from the start, so the x87 state it re-enters with has
  // to be the state it left.  Nothing enforced that until 2026-08-28: a
  // faulting FLD still pushed, a faulting FSTP still popped, and the retag and
  // the status word went with them, so a guest that page-faulted on an x87
  // operand resumed with a register stack one deeper or one shallower than it
  // had left.
  //=========================================================================
  {
    // Real mode on a 286 or later enforces the 0xFFFF segment limit, so an
    // eight-byte operand at DS:0xFFFE runs off the end and raises #GP.
    const uint16_t OFF_LIMIT = 0xFFFE;
    uint16_t sw_before;
    int top_before;

    setup(); FNINIT();
    push(1.5); push(2.5);                       // TOP = 6, ST(0)=2.5, ST(1)=1.5
    sw_before = sw(); top_before = ftop();
    FLDm64(OFF_LIMIT);
    check(cpu->exception_pending, "FLD m64real past the segment limit faults");
    check(ftop() == top_before, "the faulting FLD pushed nothing");
    check(st(0) == 2.5 && st(1) == 1.5, "...and the stack is untouched");
    check(sw() == sw_before, "...and so is the status word");

    setup(); FNINIT();
    push(1.5); push(2.5);
    sw_before = sw(); top_before = ftop();
    FSTPm64(OFF_LIMIT);
    check(cpu->exception_pending, "FSTP m64real past the limit faults");
    check(ftop() == top_before && st(0) == 2.5, "the faulting FSTP popped nothing");
    check(sw() == sw_before, "...and left no flags behind");

    // The conversion path has its own state to leak: FISTP of 2.5 rounds, and
    // rounding sets #P.  A faulting store must not leave that behind either.
    setup(); FNINIT();
    push(2.5);
    sw_before = sw();
    opm(0xDF, 7, OFF_LIMIT);                    // FISTP m64int
    check(cpu->exception_pending, "FISTP m64int past the limit faults");
    check(sw() == sw_before, "...and leaves no #P behind");
    check(ftop() == 7 && st(0) == 2.5, "...and does not pop");

    // FNSAVE re-initialises the FPU when it succeeds.  When it faults part way
    // through the environment image, it must not.
    setup(); FNINIT();
    push(1.5); push(2.5);
    top_before = ftop();
    opm(0xDD, 6, OFF_LIMIT);
    check(cpu->exception_pending, "FNSAVE past the limit faults");
    check(ftop() == top_before && st(0) == 2.5 && st(1) == 1.5,
          "...and does NOT re-initialise the FPU");
    check(tg(0) == TAG_VALID, "...and does not empty the tags");

    // And when it succeeds it resets the instruction pointers along with
    // everything else, so the FNSTENV after it writes zeros rather than an
    // address pointing back at the FNSAVE.
    setup(); FNINIT();
    push(1.0);
    opm(0xDD, 6, 0x0200);                       // FNSAVE, in range
    for (int i = 0; i < 32; i++) wr8((uint16_t)(0x0300 + i), 0xAA);
    opm(0xD9, 6, 0x0300);                       // FNSTENV
    check(rd16(0x0300 + 6) == 0 && rd16(0x0300 + 8) == 0,
          "a successful FNSAVE resets FIP and the opcode, not just the registers");

    // FBSTP writes ten bytes one at a time.  The first fits at 0xFFFF, the
    // second runs off the end and faults - and the remaining eight must not be
    // written, which is not automatic: check_segment_write LETS AN ACCESS
    // THROUGH once an exception is already pending, so without an explicit
    // guard the rest of the field lands at whatever the offset wrapped to.
    // Where the suppressed writes WOULD have gone is the part worth knowing:
    // a real-mode effective address masks the offset to sixteen bits, so an
    // offset past the limit does not run off the end of the segment - it wraps
    // to the START of it.  Without the guard, bytes 2..9 of the field land on
    // DS:0001..DS:0008, silently overwriting whatever the guest kept there.
    setup(); FNINIT();
    push(12345.0);
    for (int i = 0; i < 16; i++) wr8((uint16_t)i, 0xEE);
    opm(0xDF, 6, 0xFFFF);                       // byte 0 fits at 0xFFFF, byte 1 faults
    check(cpu->exception_pending, "FBSTP past the limit faults");
    check(ftop() == 7 && st(0) == 12345.0, "...and does not pop");
    check(rd8(0xFFFF) == 0x45, "...and the one byte that fitted was written");
    bool unwrapped = true;
    for (int i = 0; i < 16; i++) if (rd8((uint16_t)i) != 0xEE) unwrapped = false;
    check(unwrapped, "...and nothing after the fault wrapped onto the low segment");

    // The environment stores are the same shape and were the one multi-field
    // x87 write with NO guard on them: FNSAVE's register loop and FBSTP's byte
    // loop both carried `&& !fault_abort()` and fpu_store_env's seven fields
    // did not, so the fields after the faulting one wrapped to the start of
    // the segment exactly as the bytes above would have.  FNSAVE is here as
    // well as FNSTENV because its guarded register loop sits behind an
    // unguarded environment.
    setup(); FNINIT();
    for (int i = 0; i < 16; i++) wr8((uint16_t)i, 0xEE);
    opm(0xD9, 6, 0xFFF8);                       // FNSTENV, 14 bytes from 0xFFF8
    check(cpu->exception_pending, "FNSTENV past the limit faults");
    {
      bool clean = true;
      for (int i = 0; i < 16; i++) if (rd8((uint16_t)i) != 0xEE) clean = false;
      check(clean, "...and no environment field wrapped onto the low segment");
    }

    setup(); FNINIT();
    for (int i = 0; i < 16; i++) wr8((uint16_t)i, 0xEE);
    opm(0xDD, 6, 0xFFF8);                       // FNSAVE, environment then registers
    check(cpu->exception_pending, "FNSAVE past the limit faults");
    {
      bool clean = true;
      for (int i = 0; i < 16; i++) if (rd8((uint16_t)i) != 0xEE) clean = false;
      check(clean, "...and neither its environment nor its registers wrapped");
    }

    // The control: the same three instructions at an address that fits.
    setup(); FNINIT();
    push(1.5);
    FSTPm64(0x0140);
    check(!cpu->exception_pending && ftop() == 0,
          "the same FSTP inside the limit pops normally");
    setup();
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
    std::printf("ALL FPU TESTS PASS (%d checks)\n", g_checks);
    return 0;
  }
  std::printf("FPU TESTS FAILED: %d of %d checks failed\n", g_failures, g_checks);
  return 1;
}
