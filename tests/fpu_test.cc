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
// emu88.h declares:  struct FPUState { double regs[8]; uint8_t tags[8];
//                                      uint16_t cw; uint16_t sw; }
// The x87 register stack is HOST DOUBLES.  It is not 80-bit extended, and no
// amount of test-writing makes it so.  Every real-387 result that depends on a
// 64-bit mantissa, on the denormal/unsupported tag encodings, or on the
// exception flags the 387 raises is simply not reproducible here.
//
// So there are three kinds of assertion in this file, and they are kept apart
// on purpose:
//
//   check()   — this implementation is right, and a plausible bug flips it.
//   diverge() — this implementation provably differs from a real 387.  The
//               assertion pins THIS implementation's value and the comment
//               above it names the gap.  It documents the hole instead of
//               hiding it; if the value ever changes the test fails and
//               somebody has to re-read the comment.
//   bug()     — a defect that is NOT explained by the double-precision design.
//               The assertion states the CORRECT (real-387) behaviour.  It is
//               expected to fail, is reported as "KNOWN BUG", and is held to a
//               baseline count exactly the way tests/run_suites.sh holds
//               SingleStepTests to SST_BASELINE: if a bug gets fixed the count
//               drops, the harness FAILS, and KNOWN_BUGS_EXPECTED has to be
//               lowered deliberately.  A known bug can therefore never quietly
//               become "the way it works".
//
//               THE LEDGER IS EMPTY: KNOWN_BUGS_EXPECTED is 0.  All nine
//               defects this harness was written to record — the m80real
//               subnormal encode/decode, 0/0 taking the zero-divide path
//               instead of #IA, the two integer divide-by-zero results that
//               lost their sign, the stale C1 out of fpu_compare, FPREM1's
//               half-away-from-zero quotient, the OF/SF/AF that FCOMI left
//               alone, and the half-written 32-bit FNSAVE environment — are
//               fixed, and each assertion stayed put as an ordinary check().
//               The machinery is left here for the next defect.
//
//               One gap this file used to name as UNTESTABLE is closed too.
//               FIST/FISTP/FISTTP of an out-of-range value cast a double
//               straight to int16_t/int32_t/int64_t, which is undefined
//               behaviour rather than the 387's #IA-and-integer-indefinite, so
//               a test there would have been testing the compiler.  All eight
//               store paths go through one range check now and section 8
//               asserts it at both boundaries of all three widths, across the
//               rounding boundary, and for infinities and a NaN.
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
static void diverge(bool cond, const char *what) {
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
static double   st(int i)     { return cpu->fpu.regs[(ftop() + i) & 7]; }
static uint8_t  tg(int i)     { return cpu->fpu.tags[(ftop() + i) & 7]; }
static uint16_t sw()          { return cpu->fpu.sw; }
static uint16_t cw()          { return cpu->fpu.cw; }

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

static const uint16_t SW_IE = 0x0001, SW_ZE = 0x0004, SW_SF = 0x0040;

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
      if (cpu->fpu.regs[i] != 0.0) all_zero = false;
    }
    check(all_empty, "fpu_init: all 8 tags are TAG_EMPTY");
    check(all_zero, "fpu_init: all 8 registers are 0.0");

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
    cpu->fpu.sw |= SW_ZE | SW_IE | SW_SF | 0x0080 /*ES*/ | 0x8000 /*B*/;
    cpu->fpu.sw |= 0x4000;           // C3
    opr(0xDB, 0xE2);                 // FNCLEX
    check((sw() & (SW_ZE | SW_IE | SW_SF | 0x0080 | 0x8000)) == 0,
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
    check(cpu->fpu.regs[0] == 8.0 && cpu->fpu.regs[7] == 1.0,
          "physical regs[0]=8 (last push), regs[7]=1 (first push)");

    // Real 387: a 9th push is a stack overflow — IE|SF set, C1=1, the
    // destination gets the "indefinite" QNaN and the old value survives.
    // Here the push is unconditional: it just overwrites ST(7).
    push(99.0);
    diverge(ftop() == 7, "9th push wraps TOP to 7 (no overflow detection)");
    diverge(st(0) == 99.0, "9th push silently overwrites the oldest register");
    diverge((sw() & (SW_IE | SW_SF)) == 0, "9th push sets neither IE nor SF");
    diverge((sw() & 0x0200) == 0, "9th push leaves C1 clear (no overflow flag)");

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
    // Real 387 tags a denormal TAG_SPECIAL; a subnormal double is just "valid"
    // here because compute_tag() only knows zero/NaN/inf.
    push(5e-324);
    diverge(tg(0) == TAG_VALID, "subnormal is tagged TAG_VALID, not TAG_SPECIAL");
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

    // FLD ST(7) (D9 C7) — the far end of the D9 C0-C7 range.  With a full
    // stack ST(7) is the slot the push lands in, so the source has to be read
    // before TOP moves; both the value and the TOP change are asserted.
    FNINIT();
    for (int i = 1; i <= 8; i++) push((double)i);   // ST0=8 ... ST7=1
    opr(0xD9, 0xC7);                  // FLD ST(7)
    check(ftop() == 7, "FLD ST(7) pushed (TOP 0 -> 7)");
    check(st(0) == 1.0, "FLD ST(7) copied the deepest register to the top");
    check(st(1) == 8.0, "FLD ST(7) left the old ST(0) directly below it");
    check(tg(0) == TAG_VALID, "FLD ST(7) tags the new top valid");

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
  // 5. m80real — the lossy path, tested honestly
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
    diverge(st(0) == 1.0, "FLD m80real 1+2^-53 collapses to exactly 1.0 (53-bit stack)");
    FSTPm80(0x0130);
    diverge(rd80_mant(0x0130) == 0x8000000000000000ULL,
            "storing it back yields 1.0's encoding — the low mantissa bits are gone");

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
    // A real 387 keeps the sign and the 62-bit payload of a NaN.  Here the
    // reader returns the host NAN and the writer emits one fixed pattern.
    FNINIT();
    wr80(0x0120, 0xC123456789ABCDEFULL, 0xFFFF);   // negative NaN, payload set
    FLDm80(0x0120);
    diverge(std::isnan(st(0)) && !std::signbit(st(0)),
            "FLD m80real of a NEGATIVE NaN loses the sign");
    FSTPm80(0x0130);
    diverge(rd80_exp(0x0130) == 0x7FFF && rd80_mant(0x0130) == 0xC000000000000000ULL,
            "FSTP m80real emits one canonical QNaN — payload not preserved");

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
    // Real 387 loads these from an 80-bit ROM.  Here they are the host
    // library's doubles, so only 53 mantissa bits ever reach the guest.
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
    // Pin the gap: pi is short by the 11 mantissa bits a 387 would supply.
    FNINIT();
    opr(0xD9, 0xEB);
    FSTPm80(0x0120);
    diverge(rd80_mant(0x0120) == 0xC90FDAA22168C000ULL,
            "FLDPI stored as m80 has 11 zero low bits (387 ROM ends ...C235)");

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

    // 64-bit integers wider than 53 bits cannot survive the double stack.
    FNINIT(); wr64(0x0110, 9007199254740993ULL);   // 2^53 + 1
    opm(0xDF, 5, 0x0110);
    diverge(st(0) == 9007199254740992.0, "FILD m64int 2^53+1 rounds to 2^53");
    opm(0xDF, 7, 0x0118);
    diverge((int64_t)rd64(0x0118) == 9007199254740992LL,
            "FISTP m64int returns 2^53, not the 2^53+1 that went in");

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
    // Real 387 reports a denormal as C3=1 C2=1 C0=0.  compute_tag() has no
    // denormal class, so FXAM calls a subnormal double "normal".
    FNINIT(); push(5e-324);
    opr(0xD9, 0xE5);
    diverge(cc() == 0x4, "FXAM on a subnormal reports normal (C2), not denormal (C3|C2)");
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
    // Real 387 raises IE and returns the indefinite QNaN for sqrt of a
    // negative; here the host NaN comes back with no flag set at all.
    FNINIT(); push(-4.0);
    opr(0xD9, 0xFA);
    check(std::isnan(st(0)), "FSQRT -4 = NaN");
    diverge((sw() & SW_IE) == 0, "FSQRT of a negative sets no IE");

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
    // Real 387: FXTRACT(0) gives ST(1) = -infinity with ZE.  frexp(0) reports
    // exponent 0, so this returns -1.
    FNINIT(); push(0.0);
    opr(0xD9, 0xF4);
    diverge(st(1) == -1.0 && st(0) == 0.0,
            "FXTRACT 0 gives exponent -1 (387 gives -inf and ZE)");
    diverge((sw() & SW_ZE) == 0, "FXTRACT 0 sets no ZE");
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
    opr(0xD9, 0xF8); check(st(0) == 5.0, "FPREM guards a zero divisor: ST(0) untouched");

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

    // Real 387 does the reduction 64 exponent-bits at a time and sets C2 when
    // it did not finish.  This does the whole thing in one double divide and
    // always reports "complete" — and for a huge ratio the answer is wrong.
    FNINIT(); push(3.0); push(ldexp(1.0, 100));
    opr(0xD9, 0xF8);
    diverge((sw() & 0x0400) == 0, "FPREM always clears C2 (never a partial reduction)");
    diverge(st(0) != 1.0, "FPREM 2^100 mod 3 is not the true remainder 1.0");
    // The quotient bits C0/C3/C1 that a 387 leaves behind are never written.
    FNINIT(); push(4.0); push(13.0);
    cpu->fpu.sw &= (uint16_t)~0x4700;
    opr(0xD9, 0xF8);
    diverge((sw() & 0x4700) == 0, "FPREM writes no quotient bits into C0/C1/C3");
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

    // The precision-control field is ignored: a 24-bit PC setting still gives
    // full double results.  (A real 387 with PC=00 rounds to 24 bits.)
    FNINIT();
    wr16(0x0100, 0x003F);                       // PC = 00 (single), RC = nearest
    opm(0xD9, 5, 0x0100);
    push(1.0); wrd(0x0110, ldexp(1.0, -30)); opm(0xDC, 0, 0x0110);
    diverge(st(0) == 1.0 + ldexp(1.0, -30),
            "precision control is ignored: PC=24-bit still yields a full double");
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

    // Real 387 sets C2 and leaves the operand alone when |x| >= 2^63.  These
    // hand the argument straight to the host libm and always clear C2.
    FNINIT(); push(ldexp(1.0, 70));
    cpu->fpu.sw |= 0x0400;
    opr(0xD9, 0xFE);
    diverge((sw() & 0x0400) == 0, "FSIN of 2^70 clears C2 (no out-of-range check)");
    diverge(st(0) != ldexp(1.0, 70), "FSIN of 2^70 replaced the argument anyway");
    FNINIT(); push(ldexp(1.0, 70));
    cpu->fpu.sw |= 0x0400;
    opr(0xD9, 0xFF);
    diverge((sw() & 0x0400) == 0, "FCOS of 2^70 clears C2 as well");

    // F2XM1 and FYL2XP1 exist on real hardware precisely to keep precision
    // near zero.  Computing them as pow(2,x)-1 and log2(x+1) in double throws
    // that away completely: for x = 2^-60 both return exactly zero, where the
    // true answers are about 6.0e-19 and 8.7e-19.
    const double tiny = ldexp(1.0, -60);
    FNINIT(); push(tiny);
    opr(0xD9, 0xF0);
    diverge(st(0) == 0.0, "F2XM1 2^-60 returns exactly 0 (true value ~6.0e-19)");
    check(std::expm1(tiny * M_LN2) != 0.0, "...and the true value really is non-zero");
    FNINIT(); push(1.0); push(tiny);
    opr(0xD9, 0xF9);
    diverge(st(0) == 0.0, "FYL2XP1 2^-60 returns exactly 0 (true value ~1.25e-18)");

    // FYL2X of zero: the value a 387 produces, but without the ZE flag.
    FNINIT(); push(2.0); push(0.0);
    opr(0xD9, 0xF1);
    check(std::isinf(st(0)) && st(0) < 0, "FYL2X 2*log2(0) = -infinity");
    diverge((sw() & SW_ZE) == 0, "FYL2X of zero sets no ZE");
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
    check(rd16(SAVE + 4) == 0xFFCC, "FNSAVE tag word: valid,empty,valid then five empties");
    bool zeroed = true;
    for (int i = 6; i < 14; i++) if (rd8((uint16_t)(SAVE + i)) != 0) zeroed = false;
    check(zeroed, "FNSAVE zeroes the FIP/FDP area (+6..+13) in the 16-bit form");
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
    // The tag word here is written TOP-relative (ST(0) in bits 1:0).  A real
    // 387 writes it in physical register order, so this image is only
    // self-consistent, not interchangeable with hardware or another emulator.
    diverge(rd16(SAVE + 4) == 0xFFCC, "FNSAVE tag word is TOP-relative, not physical order");

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
    // Same TOP-relative tag word as the 16-bit form (see the note above), at +8.
    diverge(rd16(SAVE + 8) == 0xFFCC, "FNSAVE32 writes the tag word at +8");
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

    // FNSTENV / FLDENV are stubs: control word and status word, nothing else.
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
    diverge(rd16(ENV + 4) == 0xAAAA, "FNSTENV writes no tag word at +4 (stub)");
    FNINIT();
    check(cw() == 0x037F && ftop() == 0, "FNINIT wiped the environment");
    opm(0xD9, 4, ENV);                          // FLDENV
    check(cw() == 0x077F, "FLDENV restores CW");
    check(sw() == env_sw && ftop() == 6, "FLDENV restores SW and TOP");
    diverge(tg(0) == TAG_EMPTY, "FLDENV does not restore the tag word (stub)");
    // Real FNSTENV masks all exceptions in the FPU afterwards; this does not.
    FNINIT();
    wr16(0x0100, 0x0000); opm(0xD9, 5, 0x0100); // all exceptions unmasked
    opm(0xD9, 6, ENV);
    diverge(cw() == 0x0000, "FNSTENV does not mask exceptions afterwards");
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
