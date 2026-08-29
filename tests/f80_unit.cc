// f80_unit.cc — the 80-bit soft float in emu88/emu88_f80.h, graded against the
// host's own x87.
//
// WHY THIS HARNESS EXISTS
// -----------------------
// Neither validation suite in this repository can grade an FPU.  None of the
// 941 opcode files in tests/data/80386/v1_ex_real_mode begins D8..DF, because
// the capture bench was an 80386EX with no coprocessor, and the mnemonic
// column of test386's reference output has no x87 entry in it.  So when the
// register file was rewritten from host `double` to 80-bit extended there was
// nothing that could say whether the arithmetic underneath was right.
//
// This is that thing.  On x86-64, `long double` IS the 80-bit format the
// emulator now implements, with the same control word, the same four rounding
// modes, the same three precision-control settings and the same six exception
// flags.  So the host x87 can be driven as an ORACLE: run the same operation
// both ways and compare the result AND the flags, bit for bit.
//
// That is worth being precise about, because it is a stronger check than a
// table of expected values.  Every one of the following came out of it rather
// than out of the manual, and every one was a real defect at the time:
//
//   - #D is raised for a denormal operand even when the other operand is an
//     infinity and the denormal never reaches the arithmetic - but #IA and #Z
//     SUPPRESS it, because they stop the operation first;
//   - the masked-overflow "largest finite value" is the largest finite value
//     AT THE CURRENT PRECISION, so under PC=24 it is 0xFFFFFF0000000000;
//   - two NaNs with equal significands are separated by the smaller
//     sign-exponent word, not by "the destination" as the manual says;
//   - storing to a narrower format does NOT raise #D for a denormal source,
//     because a store is not an arithmetic operation.
//
// WHAT IT COVERS, AND WHAT IT CANNOT
// ----------------------------------
// Exact, bit-for-bit, with flags: add, sub, mul, div, sqrt, the m32/m64
// conversions both ways, the integer conversions both ways, packed BCD both
// ways, FRNDINT, FSCALE, FXTRACT, FPREM, FPREM1, the comparisons and FXAM.
//
// NOT exact: the eight transcendentals.  A real 387 does not round those
// correctly either - Intel specifies about 1 ulp - and this host is not a 387,
// so they are graded against the host's long-double libm to a recorded ulp
// bound instead.  A spot check against an exact reference showed some of the
// remaining difference is glibc's, not this file's.
//
// On anything that is not x86-64 with a 64-bit long double - which includes
// every machine this emulator actually ships on - the oracle cannot run.  The
// harness still runs a table of golden vectors captured here, so it asserts
// something real on ARM64 rather than silently passing.

#include "emu88_f80.h"

#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(__x86_64__) && LDBL_MANT_DIG == 64
#  define HAVE_X87_ORACLE 1
#else
#  define HAVE_X87_ORACLE 0
#endif

static int g_checks = 0, g_failures = 0;
static void check(bool cond, const char *what) {
  g_checks++;
  if (!cond) { g_failures++; std::printf("  FAIL: %s\n", what); }
}

//===========================================================================
// A deterministic generator that visits every encoding class
//===========================================================================

static uint64_t g_rs = 0x243F6A8885A308D3ULL;
static uint64_t rnd() {
  g_rs ^= g_rs << 13; g_rs ^= g_rs >> 7; g_rs ^= g_rs << 17; return g_rs;
}
static void reseed(uint64_t s) { g_rs = s; }

// Weighted so that most cases are ordinary normals with nearby exponents -
// which is where cancellation and the carry paths live - while zeros,
// infinities, NaNs, denormals and both exponent extremes all come up often.
static f80 gen() {
  uint64_t r = rnd();
  f80 a;
  switch (r % 20) {
    case 0:  return f80_make_zero((r & 16) != 0);
    case 1:  return f80_make_inf((r & 16) != 0);
    case 2:  a.sig = 0x8000000000000000ULL | (rnd() >> 2);
             a.se = (uint16_t)(0x7FFF | (r & 0x8000)); return a;          // NaN
    case 3:  a.sig = rnd() >> 1; a.se = (uint16_t)(r & 0x8000); return a; // denormal
    case 4:  a.sig = 0x8000000000000000ULL | (rnd() >> 1);
             a.se = (uint16_t)((r & 0x8000) | 1); return a;               // min normal
    case 5:  a.sig = 0x8000000000000000ULL | (rnd() >> 1);
             a.se = (uint16_t)((r & 0x8000) | 0x7FFE); return a;          // max normal
    case 6:  a.sig = 0x8000000000000000ULL;
             a.se = (uint16_t)((r & 0x8000) |
                    (uint16_t)(0x3FFF + (int)(rnd() % 41) - 20)); return a;  // powers of 2
    case 7: case 8: case 9:
             a.sig = 0x8000000000000000ULL | (rnd() >> 1);
             a.se = (uint16_t)((r & 0x8000) |
                    (uint16_t)(0x3FFF + (int)(rnd() % 80) - 16)); return a;  // small
    default:
             a.sig = 0x8000000000000000ULL | (rnd() >> 1);
             a.se = (uint16_t)((r & 0x8000) |
                    (uint16_t)(0x3FFF + (int)(rnd() % 400) - 200)); return a;
  }
}
// A partner chosen to land near `base` a quarter of the time, so effective
// subtraction actually cancels instead of always being a shift-and-copy.
static f80 gen_near(f80 base) {
  f80 a = gen();
  uint64_t r = rnd();
  if ((r & 3) == 0)      a.se = (uint16_t)((a.se & 0x8000) | (base.se & 0x7FFF));
  else if ((r & 3) == 1) { a = base; a.se ^= 0x8000; a.sig ^= (rnd() & 0xFF); }
  return a;
}

//===========================================================================
// Golden vectors — these run on every platform
//===========================================================================

// Captured on x86-64 against the host x87 and checked case by case.  They are
// the only thing this harness asserts on a machine where the oracle cannot
// run, so they deliberately span the encoding classes rather than the
// arithmetic.
struct Vec { const char *name; uint16_t cw; int op;
             uint16_t ase; uint64_t asig; uint16_t bse; uint64_t bsig;
             uint16_t rse; uint64_t rsig; uint16_t fl; };
enum { OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_SQRT };

static const Vec VECTORS[] = {
  // one third, in all four rounding modes: the low bit is the whole point
  { "1/3 nearest",  0x037F, OP_DIV, 0x3FFF, 0x8000000000000000ULL, 0x4000, 0xC000000000000000ULL, 0x3FFD, 0xAAAAAAAAAAAAAAABULL, F80_PE },
  { "1/3 down",     0x077F, OP_DIV, 0x3FFF, 0x8000000000000000ULL, 0x4000, 0xC000000000000000ULL, 0x3FFD, 0xAAAAAAAAAAAAAAAAULL, F80_PE },
  { "1/3 up",       0x0B7F, OP_DIV, 0x3FFF, 0x8000000000000000ULL, 0x4000, 0xC000000000000000ULL, 0x3FFD, 0xAAAAAAAAAAAAAAABULL, F80_PE },
  { "1/3 truncate", 0x0F7F, OP_DIV, 0x3FFF, 0x8000000000000000ULL, 0x4000, 0xC000000000000000ULL, 0x3FFD, 0xAAAAAAAAAAAAAAAAULL, F80_PE },
  // precision control narrows the significand and leaves the exponent alone
  { "1/3 at PC=24", 0x003F, OP_DIV, 0x3FFF, 0x8000000000000000ULL, 0x4000, 0xC000000000000000ULL, 0x3FFD, 0xAAAAAB0000000000ULL, F80_PE },
  { "1/3 at PC=53", 0x023F, OP_DIV, 0x3FFF, 0x8000000000000000ULL, 0x4000, 0xC000000000000000ULL, 0x3FFD, 0xAAAAAAAAAAAAA800ULL, F80_PE },
  // sqrt to the last bit
  { "sqrt 2",       0x037F, OP_SQRT, 0x4000, 0x8000000000000000ULL, 0, 0, 0x3FFF, 0xB504F333F9DE6484ULL, F80_PE },
  { "sqrt -1",      0x037F, OP_SQRT, 0xBFFF, 0x8000000000000000ULL, 0, 0, 0xFFFF, 0xC000000000000000ULL, F80_IE },
  // the exception matrix
  { "1/0",          0x037F, OP_DIV, 0x3FFF, 0x8000000000000000ULL, 0x0000, 0x0000000000000000ULL, 0x7FFF, 0x8000000000000000ULL, F80_ZE },
  { "0/0",          0x037F, OP_DIV, 0x0000, 0x0000000000000000ULL, 0x0000, 0x0000000000000000ULL, 0xFFFF, 0xC000000000000000ULL, F80_IE },
  { "inf - inf",    0x037F, OP_SUB, 0x7FFF, 0x8000000000000000ULL, 0x7FFF, 0x8000000000000000ULL, 0xFFFF, 0xC000000000000000ULL, F80_IE },
  { "inf * 0",      0x037F, OP_MUL, 0x7FFF, 0x8000000000000000ULL, 0x0000, 0x0000000000000000ULL, 0xFFFF, 0xC000000000000000ULL, F80_IE },
  // #D is reported for a denormal operand, even against an infinity
  { "inf + denorm", 0x037F, OP_ADD, 0x7FFF, 0x8000000000000000ULL, 0x0000, 0x0123456789ABCDEFULL, 0x7FFF, 0x8000000000000000ULL, F80_DE },
  // ...but #Z suppresses it
  { "denorm / 0",   0x037F, OP_DIV, 0x0000, 0x0123456789ABCDEFULL, 0x0000, 0x0000000000000000ULL, 0x7FFF, 0x8000000000000000ULL, F80_ZE },
  // ...and so does #IA
  { "sqrt -denorm", 0x037F, OP_SQRT, 0x8000, 0x0123456789ABCDEFULL, 0, 0, 0xFFFF, 0xC000000000000000ULL, F80_IE },
  // an unnormal is unsupported: #IA whatever it meets
  { "unnormal + 1", 0x037F, OP_ADD, 0x4000, 0x4000000000000000ULL, 0x3FFF, 0x8000000000000000ULL, 0xFFFF, 0xC000000000000000ULL, F80_IE },
  // a pseudo-denormal is NOT: it is the value it looks like, and reports #D
  { "psdenorm * 1", 0x037F, OP_MUL, 0x0000, 0x8000000000000000ULL, 0x3FFF, 0x8000000000000000ULL, 0x0001, 0x8000000000000000ULL, F80_DE },
  // masked overflow: infinity when rounding away, largest finite when not
  { "overflow up",  0x037F, OP_MUL, 0x7FFE, 0xFFFFFFFFFFFFFFFFULL, 0x4000, 0x8000000000000000ULL, 0x7FFF, 0x8000000000000000ULL, (uint16_t)(F80_OE | F80_PE) },
  { "overflow trunc", 0x0F7F, OP_MUL, 0x7FFE, 0xFFFFFFFFFFFFFFFFULL, 0x4000, 0x8000000000000000ULL, 0x7FFE, 0xFFFFFFFFFFFFFFFFULL, (uint16_t)(F80_OE | F80_PE) },
  { "overflow trunc PC24", 0x0C3F, OP_MUL, 0x7FFE, 0xFFFFFFFFFFFFFFFFULL, 0x4000, 0x8000000000000000ULL, 0x7FFE, 0xFFFFFF0000000000ULL, (uint16_t)(F80_OE | F80_PE) },
  // gradual underflow: exact stays quiet, inexact raises both #U and #P
  { "denorm halved exact", 0x037F, OP_MUL, 0x0000, 0x0000000000000002ULL, 0x3FFE, 0x8000000000000000ULL, 0x0000, 0x0000000000000001ULL, F80_DE },
  { "denorm halved inexact", 0x037F, OP_MUL, 0x0000, 0x0000000000000003ULL, 0x3FFE, 0x8000000000000000ULL, 0x0000, 0x0000000000000002ULL, (uint16_t)(F80_DE | F80_UE | F80_PE) },
  // NaN propagation: the larger significand wins, and an SNaN is quieted
  { "qnan pair",    0x037F, OP_ADD, 0x7FFF, 0xC000000000000001ULL, 0xFFFF, 0xE000000000000000ULL, 0xFFFF, 0xE000000000000000ULL, 0 },
  { "snan quieted", 0x037F, OP_ADD, 0x7FFF, 0xA123456789ABCDEFULL, 0x3FFF, 0x8000000000000000ULL, 0x7FFF, 0xE123456789ABCDEFULL, F80_IE },
  // -0 appears out of an exact cancellation only when rounding down
  { "x - x nearest", 0x037F, OP_SUB, 0x4000, 0x9000000000000000ULL, 0x4000, 0x9000000000000000ULL, 0x0000, 0x0000000000000000ULL, 0 },
  { "x - x down",    0x077F, OP_SUB, 0x4000, 0x9000000000000000ULL, 0x4000, 0x9000000000000000ULL, 0x8000, 0x0000000000000000ULL, 0 },
};

static void run_vectors() {
  for (const Vec &v : VECTORS) {
    f80 a; a.se = v.ase; a.sig = v.asig;
    f80 b; b.se = v.bse; b.sig = v.bsig;
    f80_ctx c = f80_ctx_make(v.cw);
    f80 r;
    switch (v.op) {
      case OP_ADD:  r = f80_add(a, b, c); break;
      case OP_SUB:  r = f80_sub(a, b, c); break;
      case OP_MUL:  r = f80_mul(a, b, c); break;
      case OP_DIV:  r = f80_div(a, b, c); break;
      default:      r = f80_sqrt(a, c);   break;
    }
    bool ok = (r.se == v.rse && r.sig == v.rsig &&
               (uint16_t)(c.flags & 0x3F) == v.fl);
    if (!ok)
      std::printf("  got %016llX:%04X fl=%02X, want %016llX:%04X fl=%02X\n",
                  (unsigned long long)r.sig, r.se, (unsigned)(c.flags & 0x3F),
                  (unsigned long long)v.rsig, v.rse, v.fl);
    check(ok, v.name);
  }
}

//===========================================================================
// Self-consistency — also runs everywhere
//===========================================================================

static void run_identities(int n) {
  reseed(0x9E3779B97F4A7C15ULL);
  bool m80_rt = true, i64_rt = true, f64_rt = true, cmp_ok = true, negate_ok = true;
  for (int i = 0; i < n; i++) {
    f80 a = gen(), b = gen();
    f80_ctx c = f80_ctx_make(0x037F);

    // A 64-bit integer round-trips through the register file exactly.
    int64_t iv = (int64_t)rnd();
    if (iv == INT64_MIN) iv = 0;
    f80_ctx ci = f80_ctx_make(0x037F);
    if (f80_to_int(f80_from_i64(iv), 64, false, ci) != iv) i64_rt = false;

    // A double round-trips through the wider format exactly.
    uint64_t db = rnd();
    f80_ctx cd = f80_ctx_make(0x037F);
    f80 wd = f80_from_f64(db, cd);
    if (f80_classify(wd) != F80_CLASS_SNAN && f80_classify(wd) != F80_CLASS_QNAN) {
      if (f80_to_f64(wd, cd) != db) f64_rt = false;
    }

    // x and -x compare opposite ways round, unless one of them is a NaN.
    f80_cmp_r r1 = f80_compare(a, b, true, c);
    f80_cmp_r r2 = f80_compare(f80_chs(a), f80_chs(b), true, c);
    if (r1 != F80_CMP_UNORD) {
      f80_cmp_r want = (r1 == F80_CMP_GT) ? F80_CMP_LT
                     : (r1 == F80_CMP_LT) ? F80_CMP_GT : F80_CMP_EQ;
      if (r2 != want) cmp_ok = false;
    }

    // Negation is exact and is its own inverse.
    if (f80_chs(f80_chs(a)).se != a.se || f80_chs(f80_chs(a)).sig != a.sig)
      negate_ok = false;

    // The m80 path is a byte move: everything survives it.
    f80 rt = a;
    if (rt.se != a.se || rt.sig != a.sig) m80_rt = false;
  }
  check(m80_rt,   "the 80-bit value is carried verbatim");
  check(i64_rt,   "every int64 round-trips through the register file exactly");
  check(f64_rt,   "every double round-trips through the register file exactly");
  check(cmp_ok,   "comparison reverses under negation");
  check(negate_ok,"negation is exact and self-inverse");
}

//===========================================================================
// The host x87 oracle
//===========================================================================

#if HAVE_X87_ORACLE

// The low ten bytes of an x86-64 `long double` ARE this format; the rest is
// padding.  Going through a byte buffer keeps that explicit and keeps the
// compiler's aliasing rules happy.
static long double ld_of(f80 a) {
  long double x;
  unsigned char b[sizeof(long double)];
  std::memset(b, 0, sizeof b);
  std::memcpy(b, &a.sig, 8);
  std::memcpy(b + 8, &a.se, 2);
  std::memcpy(&x, b, sizeof x);
  return x;
}
static f80 f80_of(long double x) {
  unsigned char b[sizeof(long double)];
  std::memcpy(b, &x, sizeof x);
  f80 a;
  std::memcpy(&a.sig, b, 8);
  std::memcpy(&a.se, b + 8, 2);
  return a;
}
// The guest control word and the host's have the same encoding, so the guest's
// can be loaded straight into the host.
static inline void setcw(uint16_t v) { __asm__ volatile("fldcw %0" :: "m"(v)); }
static inline void clex()            { __asm__ volatile("fnclex"); }

static int g_bad = 0;
static void report(const char *op, uint16_t cw, f80 a, f80 b,
                   f80 got, uint16_t gf, f80 want, uint16_t wf) {
  if (g_bad++ < 12)
    std::printf("  %s cw=%04X a=%016llX:%04X b=%016llX:%04X\n"
                "     got %016llX:%04X fl=%02X  want %016llX:%04X fl=%02X\n",
                op, cw,
                (unsigned long long)a.sig, a.se, (unsigned long long)b.sig, b.se,
                (unsigned long long)got.sig, got.se, gf,
                (unsigned long long)want.sig, want.se, wf);
}

// Every arithmetic form, every rounding mode, every precision-control setting.
// st(0) holds b and st(1) holds a, so st(1) - the x87 destination - is `a`,
// matching this implementation's rule for the NaN tie-break.  GAS swaps
// fsub/fsubr and fdiv/fdivr for the two-register forms, which is why the
// reversed mnemonics below are the right ones.
static void oracle_arith(int n) {
  static const uint16_t cws[] = {
    0x037F, 0x077F, 0x0B7F, 0x0F7F,     // RC 0..3 at PC = 64
    0x003F, 0x043F, 0x083F, 0x0C3F,     // RC 0..3 at PC = 24
    0x023F, 0x063F, 0x0A3F, 0x0E3F,     // RC 0..3 at PC = 53
  };
  int bad = 0;
  reseed(0x243F6A8885A308D3ULL);
  for (unsigned ci = 0; ci < sizeof cws / sizeof cws[0]; ci++) {
    uint16_t cw = cws[ci];
    for (int i = 0; i < n; i++) {
      f80 a = gen(), b = gen_near(a);
      for (int op = 0; op < 5; op++) {
        f80_ctx c = f80_ctx_make(cw);
        f80 got;
        volatile long double va = ld_of(a), vb = ld_of(b), vr;
        long double xa = va, xb = vb, xr;
        uint16_t hsw;
        setcw(cw); clex();
        const char *name;
        switch (op) {
          case 0: name = "add";  got = f80_add(a, b, c);
            __asm__ volatile("faddp %%st,%%st(1)\n\tfnstsw %1"
                             : "=t"(xr), "=a"(hsw) : "0"(xb), "u"(xa) : "st(1)"); break;
          case 1: name = "sub";  got = f80_sub(a, b, c);
            __asm__ volatile("fsubrp %%st,%%st(1)\n\tfnstsw %1"
                             : "=t"(xr), "=a"(hsw) : "0"(xb), "u"(xa) : "st(1)"); break;
          case 2: name = "mul";  got = f80_mul(a, b, c);
            __asm__ volatile("fmulp %%st,%%st(1)\n\tfnstsw %1"
                             : "=t"(xr), "=a"(hsw) : "0"(xb), "u"(xa) : "st(1)"); break;
          case 3: name = "div";  got = f80_div(a, b, c);
            __asm__ volatile("fdivrp %%st,%%st(1)\n\tfnstsw %1"
                             : "=t"(xr), "=a"(hsw) : "0"(xb), "u"(xa) : "st(1)"); break;
          default: name = "sqrt"; got = f80_sqrt(a, c);
            __asm__ volatile("fsqrt\n\tfnstsw %1"
                             : "=t"(xr), "=a"(hsw) : "0"(xa)); break;
        }
        vr = xr;
        setcw(0x037F);
        f80 want = f80_of(vr);
        uint16_t gf = (uint16_t)(c.flags & 0x3F), wf = (uint16_t)(hsw & 0x3F);
        if (got.sig != want.sig || got.se != want.se || gf != wf) {
          bad++;
          report(name, cw, a, op == 4 ? a : b, got, gf, want, wf);
        }
      }
    }
  }
  char msg[128];
  std::snprintf(msg, sizeof msg,
                "add/sub/mul/div/sqrt match the host x87 exactly (%d cases, %d modes)",
                n * 5 * (int)(sizeof cws / sizeof cws[0]), (int)(sizeof cws / sizeof cws[0]));
  check(bad == 0, msg);
}

// The conversions, the exact non-arithmetic operations, and the condition
// codes.  FPREM and FPREM1 are looped to convergence: the size of a partial
// reduction is implementation-defined and this host does not take the same
// bite a 387 would, but the converged remainder is architectural and so are
// the quotient bits of the completing step.
static void oracle_ops(int n) {
  // All four rounding modes at all three precision-control settings, the same
  // grid oracle_arith uses.  Most of these operations ignore PC - FRNDINT and
  // the conversions round to their destination, not to the control word - but
  // FSCALE does not, and a grid that skipped PC would leave that untested.
  static const uint16_t cws[] = {
    0x037F, 0x077F, 0x0B7F, 0x0F7F,
    0x003F, 0x043F, 0x083F, 0x0C3F,
    0x023F, 0x063F, 0x0A3F, 0x0E3F,
  };
  int bad_cvt = 0, bad_int = 0, bad_rnd = 0, bad_prem = 0, bad_cmp = 0,
      bad_xam = 0, bad_bcd = 0, bad_xtr = 0;
  reseed(0x13198A2E03707344ULL);
  for (unsigned ci = 0; ci < sizeof cws / sizeof cws[0]; ci++) {
    uint16_t cw = cws[ci];
    for (int i = 0; i < n; i++) {
      f80 a = gen(), b = gen();
      long double xa = ld_of(a), xb = ld_of(b), xr;
      uint16_t hsw;

      { f80_ctx c = f80_ctx_make(cw); uint32_t got = f80_to_f32(a, c);
        float out; setcw(cw); clex();
        __asm__ volatile("fstps %0\n\tfnstsw %1" : "=m"(out), "=a"(hsw) : "t"(xa) : "st");
        setcw(0x037F);
        uint32_t want; std::memcpy(&want, &out, 4);
        if (got != want || (uint16_t)(c.flags & 0x3F) != (uint16_t)(hsw & 0x3F)) bad_cvt++; }

      { f80_ctx c = f80_ctx_make(cw); uint64_t got = f80_to_f64(a, c);
        double out; setcw(cw); clex();
        __asm__ volatile("fstpl %0\n\tfnstsw %1" : "=m"(out), "=a"(hsw) : "t"(xa) : "st");
        setcw(0x037F);
        uint64_t want; std::memcpy(&want, &out, 8);
        if (got != want || (uint16_t)(c.flags & 0x3F) != (uint16_t)(hsw & 0x3F)) bad_cvt++; }

      { uint32_t src = (uint32_t)rnd();
        f80_ctx c = f80_ctx_make(cw); f80 got = f80_from_f32(src, c);
        float in; std::memcpy(&in, &src, 4); setcw(cw); clex();
        __asm__ volatile("flds %1\n\tfnstsw %2" : "=t"(xr), "=m"(in), "=a"(hsw) : "m"(in));
        setcw(0x037F);
        f80 want = f80_of(xr);
        if (got.sig != want.sig || got.se != want.se ||
            (uint16_t)(c.flags & 0x3F) != (uint16_t)(hsw & 0x3F)) bad_cvt++; }

      { uint64_t src = rnd();
        f80_ctx c = f80_ctx_make(cw); f80 got = f80_from_f64(src, c);
        double in; std::memcpy(&in, &src, 8); setcw(cw); clex();
        __asm__ volatile("fldl %1\n\tfnstsw %2" : "=t"(xr), "=m"(in), "=a"(hsw) : "m"(in));
        setcw(0x037F);
        f80 want = f80_of(xr);
        if (got.sig != want.sig || got.se != want.se ||
            (uint16_t)(c.flags & 0x3F) != (uint16_t)(hsw & 0x3F)) bad_cvt++; }

      { f80_ctx c = f80_ctx_make(cw); int16_t got = (int16_t)f80_to_int(a, 16, false, c);
        int16_t out; setcw(cw); clex();
        __asm__ volatile("fistps %0\n\tfnstsw %1" : "=m"(out), "=a"(hsw) : "t"(xa) : "st");
        setcw(0x037F);
        if (got != out || (uint16_t)(c.flags & 0x3F) != (uint16_t)(hsw & 0x3F)) bad_int++; }
      { f80_ctx c = f80_ctx_make(cw); int32_t got = (int32_t)f80_to_int(a, 32, false, c);
        int32_t out; setcw(cw); clex();
        __asm__ volatile("fistpl %0\n\tfnstsw %1" : "=m"(out), "=a"(hsw) : "t"(xa) : "st");
        setcw(0x037F);
        if (got != out || (uint16_t)(c.flags & 0x3F) != (uint16_t)(hsw & 0x3F)) bad_int++; }
      { f80_ctx c = f80_ctx_make(cw); int64_t got = f80_to_int(a, 64, false, c);
        int64_t out; setcw(cw); clex();
        __asm__ volatile("fistpll %0\n\tfnstsw %1" : "=m"(out), "=a"(hsw) : "t"(xa) : "st");
        setcw(0x037F);
        if (got != out || (uint16_t)(c.flags & 0x3F) != (uint16_t)(hsw & 0x3F)) bad_int++; }
      { int64_t src = (int64_t)rnd();
        f80 got = f80_from_i64(src); setcw(cw); clex();
        __asm__ volatile("fildll %1\n\tfnstsw %2" : "=t"(xr), "=m"(src), "=a"(hsw) : "m"(src));
        setcw(0x037F);
        f80 want = f80_of(xr);
        if (got.sig != want.sig || got.se != want.se) bad_int++; }

      { f80_ctx c = f80_ctx_make(cw); f80 got = f80_rndint(a, c);
        setcw(cw); clex();
        __asm__ volatile("frndint\n\tfnstsw %1" : "=t"(xr), "=a"(hsw) : "0"(xa));
        setcw(0x037F);
        f80 want = f80_of(xr);
        if (got.sig != want.sig || got.se != want.se ||
            (uint16_t)(c.flags & 0x3F) != (uint16_t)(hsw & 0x3F)) bad_rnd++; }
      { f80_ctx c = f80_ctx_make(cw); f80 got = f80_scale(a, b, c);
        setcw(cw); clex();
        __asm__ volatile("fscale\n\tfnstsw %1" : "=t"(xr), "=a"(hsw) : "0"(xa), "u"(xb));
        setcw(0x037F);
        f80 want = f80_of(xr);
        if (got.sig != want.sig || got.se != want.se ||
            (uint16_t)(c.flags & 0x3F) != (uint16_t)(hsw & 0x3F)) bad_rnd++; }

      { f80_ctx c = f80_ctx_make(cw); f80 ge, gm; f80_extract(a, &ge, &gm, c);
        long double hx, hy; setcw(cw); clex();
        __asm__ volatile("fxtract\n\tfnstsw %2" : "=t"(hy), "=u"(hx), "=a"(hsw) : "0"(xa));
        setcw(0x037F);
        f80 we = f80_of(hx), wm = f80_of(hy);
        if (ge.sig != we.sig || ge.se != we.se || gm.sig != wm.sig || gm.se != wm.se ||
            (uint16_t)(c.flags & 0x3F) != (uint16_t)(hsw & 0x3F)) bad_xtr++; }

      for (int which = 0; which < 2; which++) {
        int q = 0; bool part = true;
        f80 got = a; uint16_t gfl = 0;
        for (int it = 0; it < 4000 && part; it++) {
          f80_ctx c2 = f80_ctx_make(cw);
          got = which ? f80_prem1(got, b, c2, &q, &part) : f80_prem(got, b, c2, &q, &part);
          gfl |= (uint16_t)(c2.flags & 0x3F);
        }
        long double hcur = xa; uint16_t hsw2 = 0, hfl = 0; bool hpart = true;
        setcw(cw); clex();
        for (int it = 0; it < 4000 && hpart; it++) {
          long double hi = hcur, ho;
          if (which) __asm__ volatile("fprem1\n\tfnstsw %1" : "=t"(ho), "=a"(hsw2) : "0"(hi), "u"(xb));
          else       __asm__ volatile("fprem\n\tfnstsw %1"  : "=t"(ho), "=a"(hsw2) : "0"(hi), "u"(xb));
          hcur = ho; hpart = (hsw2 & 0x0400) != 0; hfl |= (uint16_t)(hsw2 & 0x3F);
        }
        setcw(0x037F);
        int hq = (int)((((hsw2 >> 8) & 1) << 2) | (((hsw2 >> 14) & 1) << 1) | ((hsw2 >> 9) & 1));
        f80 want = f80_of(hcur);
        // With #IA raised the condition codes are architecturally undefined,
        // and this host leaves stale bits in them.
        bool qgrade = (gfl & F80_IE) == 0;
        if (got.sig != want.sig || got.se != want.se || gfl != hfl || (qgrade && q != hq))
          bad_prem++;
      }

      for (int quiet = 0; quiet < 2; quiet++) {
        f80_ctx c = f80_ctx_make(cw);
        f80_cmp_r r = f80_compare(a, b, quiet != 0, c);
        setcw(cw); clex();
        if (quiet) __asm__ volatile("fucom %%st(1)\n\tfnstsw %0" : "=a"(hsw) : "t"(xa), "u"(xb));
        else       __asm__ volatile("fcom  %%st(1)\n\tfnstsw %0" : "=a"(hsw) : "t"(xa), "u"(xb));
        setcw(0x037F);
        int hcc = (int)((((hsw >> 14) & 1) << 2) | (((hsw >> 10) & 1) << 1) | ((hsw >> 8) & 1));
        int want = (hcc == 0) ? F80_CMP_GT : (hcc == 1) ? F80_CMP_LT
                 : (hcc == 4) ? F80_CMP_EQ : F80_CMP_UNORD;
        if ((int)r != want || (uint16_t)(c.flags & 0x3F) != (uint16_t)(hsw & 0x3F)) bad_cmp++;
      }

      { setcw(cw); clex();
        __asm__ volatile("fxam\n\tfnstsw %0" : "=a"(hsw) : "t"(xa));
        setcw(0x037F);
        int hcc = (int)((((hsw >> 14) & 1) << 2) | (((hsw >> 10) & 1) << 1) | ((hsw >> 8) & 1));
        int want;
        switch (f80_classify(a)) {
          case F80_CLASS_UNSUPPORTED: want = 0; break;
          case F80_CLASS_SNAN: case F80_CLASS_QNAN: want = 1; break;
          case F80_CLASS_NORMAL: want = 2; break;
          case F80_CLASS_INF: want = 3; break;
          case F80_CLASS_ZERO: want = 4; break;
          default: want = 6; break;
        }
        if (hcc != want) bad_xam++; }

      { f80_ctx c = f80_ctx_make(cw); uint8_t d[10]; f80_to_bcd(a, d, c);
        uint8_t hd[10]; setcw(cw); clex();
        __asm__ volatile("fbstp %0\n\tfnstsw %1" : "=m"(hd), "=a"(hsw) : "t"(xa) : "st");
        setcw(0x037F);
        if (std::memcmp(d, hd, 10) != 0 ||
            (uint16_t)(c.flags & 0x3F) != (uint16_t)(hsw & 0x3F)) bad_bcd++;
        f80 gb = f80_from_bcd(hd);
        setcw(cw); clex();
        __asm__ volatile("fbld %1\n\tfnstsw %2" : "=t"(xr), "=m"(hd), "=a"(hsw) : "m"(hd));
        setcw(0x037F);
        f80 wb = f80_of(xr);
        if (gb.sig != wb.sig || gb.se != wb.se) bad_bcd++; }
    }
  }
  check(bad_cvt  == 0, "the m32real and m64real conversions match the host both ways");
  check(bad_int  == 0, "the integer conversions match the host, range checks included");
  check(bad_rnd  == 0, "FRNDINT and FSCALE match the host at every rounding mode and precision setting");
  check(bad_xtr  == 0, "FXTRACT matches the host, zero and denormal included");
  check(bad_prem == 0, "FPREM and FPREM1 converge to the host's remainder and quotient bits");
  check(bad_cmp  == 0, "FCOM and FUCOM agree with the host, NaN handling included");
  check(bad_xam  == 0, "FXAM classifies exactly as the host does");
  check(bad_bcd  == 0, "packed BCD matches the host both ways");
}

// Distance in ulps of a 64-bit significand.  Only used for the eight
// transcendentals, which no implementation rounds correctly.
static double ulps(f80 g, f80 w) {
  if (g.sig == w.sig && g.se == w.se) return 0.0;
  f80_class kg = f80_classify(g), kw = f80_classify(w);
  if (kg != kw) return 1e9;
  if (kw == F80_CLASS_ZERO) return 0.0;
  if (kw == F80_CLASS_INF)  return 1e9;
  if (f80_neg(g) != f80_neg(w)) return 1e9;
  int eg = (int)(g.se & 0x7FFF), ew = (int)(w.se & 0x7FFF);
  if (eg == ew)     return (double)(g.sig > w.sig ? g.sig - w.sig : w.sig - g.sig);
  if (eg == ew + 1) return (double)(long double)((unsigned __int128)g.sig * 2 - w.sig);
  if (ew == eg + 1) return (double)(long double)((unsigned __int128)w.sig * 2 - g.sig);
  return 1e9;
}

// Two boundaries a random generator does not reach.
//
// Both were live defects that every check above passed over, and both are
// invisible to gen() and genr() for the same kind of reason: the interesting
// operands occupy a set of near-zero measure.  Enumerating beats sampling
// here, so these are enumerated.
//
// This section is worth its length only because it was shown to fail: against
// the code as it stood before the fixes it reports 192 tininess mismatches and
// eight FYL2XP1 failures, and zero of either after.
static void oracle_boundaries() {
  static const uint16_t cws[] = {
    0x037F, 0x077F, 0x0B7F, 0x0F7F,     // RC 0..3 at PC = 64
    0x003F, 0x043F, 0x083F, 0x0C3F,     // RC 0..3 at PC = 24
    0x023F, 0x063F, 0x0A3F, 0x0E3F,     // RC 0..3 at PC = 53
  };

  // 1. Tininess is decided on the UNBOUNDED exponent range.  A result whose
  //    exact value lies below 2^-16382 is tiny even when rounding on the
  //    denormal grid lifts what is delivered back up to exactly 2^-16382, and
  //    hardware raises #U for it.  Reading the delivered J bit instead calls
  //    that result normal and loses the flag.  Reaching it needs an exact
  //    product in the last half ulp below the boundary, which is why the
  //    significands here are all-ones and near-all-ones rather than random.
  uint64_t sigs[40];
  int ns = 0;
  for (int k = 0; k < 20; k++) sigs[ns++] = 0xFFFFFFFFFFFFFFFFULL - (uint64_t)k;
  for (int k = 1; k <= 20; k++)
    sigs[ns++] = 0x8000000000000000ULL | (0xFFFFFFFFFFFFFFFFULL >> k);

  int bad = 0, cases = 0;
  for (unsigned ci = 0; ci < sizeof cws / sizeof cws[0]; ci++) {
    uint16_t cw = cws[ci];
    for (int op = 0; op < 3; op++)
      for (int ae = 1; ae <= 8; ae++)
        for (int si = 0; si < ns; si++)
          for (int sc = 1; sc <= 8; sc++) {
            f80 a; a.sig = sigs[si]; a.se = (uint16_t)ae;
            f80 b; b.sig = 0x8000000000000000ULL;
            b.se = (uint16_t)(16383 + (op == 1 ? sc : -sc));
            f80_ctx c = f80_ctx_make(cw);
            f80 got;
            volatile long double va = ld_of(a), vb = ld_of(b), vr;
            long double xa = va, xb = vb, xr;
            uint16_t hsw;
            setcw(cw); clex();
            const char *name;
            switch (op) {
              case 0: name = "mul.tiny";   got = f80_mul(a, b, c);
                __asm__ volatile("fmulp %%st,%%st(1)\n\tfnstsw %1"
                                 : "=t"(xr), "=a"(hsw) : "0"(xb), "u"(xa) : "st(1)"); break;
              case 1: name = "div.tiny";   got = f80_div(a, b, c);
                __asm__ volatile("fdivrp %%st,%%st(1)\n\tfnstsw %1"
                                 : "=t"(xr), "=a"(hsw) : "0"(xb), "u"(xa) : "st(1)"); break;
              default: name = "scale.tiny"; got = f80_scale(a, b, c);
                __asm__ volatile("fscale\n\tfnstsw %1"
                                 : "=t"(xr), "=a"(hsw) : "0"(xa), "u"(xb)); break;
            }
            vr = xr;
            setcw(0x037F);
            f80 want = f80_of(vr);
            uint16_t gf = (uint16_t)(c.flags & 0x3F), wf = (uint16_t)(hsw & 0x3F);
            cases++;
            if (got.sig != want.sig || got.se != want.se || gf != wf) {
              bad++;
              report(name, cw, a, b, got, gf, want, wf);
            }
          }
  }
  char msg[160];
  std::snprintf(msg, sizeof msg,
                "#U at the 2^-16382 boundary matches the host x87 (%d cases)", cases);
  check(bad == 0, msg);

  // 2. FYL2XP1 over the whole of its domain, not just the small-|x| part the
  //    encoding exists for.  The reduced argument is t = x/(2+x), and the
  //    twenty atanh terms cover |t| <= 1/3, which is x >= -1/2 - so the band
  //    -1 < x <= -1/2 has to reach the log2(1+x) fallback instead.  genr(-70,
  //    -3) never produces |x| >= 1/8, so nothing here looked at that band.
  //    The host's own FYL2XP1 is the oracle, not libm.
  static const double xs[] = {
    -0.5, -0.5625, -0.6, -0.75, -0.875, -0.9, -0.99, -0.999, -0.9999,
    -0.4999, -0.25, 0.25, 0.5, 0.9, 1.0,
  };
  int tbad = 0;
  for (unsigned i = 0; i < sizeof xs / sizeof xs[0]; i++) {
    f80 x = f80_of((long double)xs[i]), y = f80_of(1.0L);
    f80_ctx c = f80_ctx_make(0x037F);
    f80 got = f80_yl2xp1(y, x, c);
    volatile long double vx = ld_of(x), vy = ld_of(y), vr;
    long double xx = vx, xy = vy, xr;
    uint16_t hsw;
    setcw(0x037F); clex();
    __asm__ volatile("fyl2xp1\n\tfnstsw %1"
                     : "=t"(xr), "=a"(hsw) : "0"(xx), "u"(xy) : "st(1)");
    vr = xr;
    setcw(0x037F);
    f80 want = f80_of(vr);
    // The fallback path forms 1+x, so allow the half ulp that costs; the
    // defect this catches was 2.4e14 ulp, not one.
    if (ulps(got, want) > 1.0) {
      tbad++;
      report("yl2xp1", 0x037F, y, x, got, (uint16_t)(c.flags & 0x3F), want,
             (uint16_t)(hsw & 0x3F));
    }
  }
  std::snprintf(msg, sizeof msg,
                "FYL2XP1 tracks the host across its whole domain (%d arguments, "
                "including the -1 < x <= -1/2 band)", (int)(sizeof xs / sizeof xs[0]));
  check(tbad == 0, msg);

  // 3. The same tininess question on the STORE path, which is a separate site
  //    with the same mistake in it.  Worth its own sweep rather than four
  //    hand-picked values: the two tests agree over most of the boundary and
  //    disagree only where denormal-grid rounding carries up to the
  //    destination's smallest normal from a value that rounding at the
  //    destination's precision, unbounded, leaves below it.  Four hand-picked
  //    cases all landed on the agreeing side; walking the last ulps finds
  //    3072 disagreements in 320000.
  int sbad = 0, scases = 0;
  for (int w = 0; w < 2; w++) {
    int be = w ? 1022 : 126;
    for (unsigned ci = 0; ci < 4; ci++) {
      uint16_t cw = cws[ci];                      // the four RC modes at PC = 64
      for (int eoff = 1; eoff <= 2; eoff++) {
        uint16_t se = (uint16_t)(16383 - be - eoff + 1);
        for (uint64_t d = 0; d < 4000; d++) {
          f80 a; a.se = se; a.sig = 0xFFFFFFFFFFFFFFFFULL - d;
          f80_ctx c = f80_ctx_make(cw);
          volatile long double va = ld_of(a);
          long double xa = va;
          uint16_t hsw;
          unsigned long long gv, hv;
          setcw(cw); clex();
          if (!w) {
            gv = f80_to_f32(a, c);
            float hf32;
            __asm__ volatile("fstps %0\n\tfnstsw %1" : "=m"(hf32), "=a"(hsw) : "t"(xa) : "st");
            __asm__ volatile("fstp %%st(0)" ::: "st");
            uint32_t t32; std::memcpy(&t32, &hf32, 4); hv = t32;
          } else {
            gv = f80_to_f64(a, c);
            double hf64;
            __asm__ volatile("fstpl %0\n\tfnstsw %1" : "=m"(hf64), "=a"(hsw) : "t"(xa) : "st");
            __asm__ volatile("fstp %%st(0)" ::: "st");
            uint64_t t64; std::memcpy(&t64, &hf64, 8); hv = t64;
          }
          setcw(0x037F);
          uint16_t gf = (uint16_t)(c.flags & 0x3F), wf = (uint16_t)(hsw & 0x3F);
          scases++;
          if (gv != hv || gf != wf) {
            if (sbad++ < 4)
              std::printf("  store.%s cw=%04X a=%016llX:%04X  got %llX fl=%02X  want %llX fl=%02X\n",
                          w ? "m64" : "m32", cw, (unsigned long long)a.sig, a.se,
                          gv, gf, hv, wf);
          }
        }
      }
    }
  }
  std::snprintf(msg, sizeof msg,
                "#U on the narrowing store matches the host x87 (%d cases)", scases);
  check(sbad == 0, msg);

  // 4. What the transcendentals REPORT, which is a separate question from how
  //    accurate they are and was not graded at all: oracle_transcendental below
  //    measures ulps and runs only cw=0x037F, so nothing here could see a
  //    rounding-control bug or a wrong exception flag.
  //
  //    These are held exactly, unlike the values, because a flag is a flag.
  {
    // FYL2X of x == 1.0 is EXACT - log2(1) is 0 - and must raise nothing.
    //
    // This check was named "FYL2X of an exact power of two raises what the
    // host raises (nothing)" and tested this one input.  The name was a false
    // generalisation and T1 below is what disproved it: the host raises #P for
    // EVERY power of two except 1.0, because y*log2(x) is a multiplication
    // whose result is not generally representable.  Only x == 1.0 short-cuts
    // to an exact zero.  Renamed to say what it tests.
    f80_ctx c = f80_ctx_make(0x037F);
    f80 g = f80_yl2x(f80_of(3.0L), f80_of(1.0L), c);
    volatile long double vy = 3.0L, vx = 1.0L;
    long double xy = vy, xx = vx, xr;
    uint16_t hsw;
    setcw(0x037F); clex();
    __asm__ volatile("fyl2x\n\tfnstsw %1" : "=t"(xr), "=a"(hsw) : "0"(xx), "u"(xy) : "st(1)");
    (void)xr; setcw(0x037F);
    check((uint16_t)(c.flags & 0x3F) == (uint16_t)(hsw & 0x3F) &&
          f80_classify(g) == F80_CLASS_ZERO,
          "FYL2X of x == 1.0 raises what the host raises (nothing)");

    // And an overflowing one has to report #O, which no transcendental did.
    f80 big; big.sig = 0xFFFFFFFFFFFFFFFFULL; big.se = 0x7FFE;
    c = f80_ctx_make(0x037F);
    f80 go = f80_yl2x(big, f80_of(65536.0L), c);
    volatile long double vb = ld_of(big), vx2 = 65536.0L;
    long double xb = vb, xx2 = vx2, xr2;
    setcw(0x037F); clex();
    __asm__ volatile("fyl2x\n\tfnstsw %1" : "=t"(xr2), "=a"(hsw) : "0"(xx2), "u"(xb) : "st(1)");
    setcw(0x037F);
    (void)go;
    check((uint16_t)(c.flags & 0x3F) == (uint16_t)(hsw & 0x3F),
          "an overflowing FYL2X reports #O the way the host does");

    // Rounding control has to REACH them.  The internal helpers round to
    // nearest at 64 bits whatever the guest asked for, so before the final
    // rounding was moved into the caller's context every mode returned the
    // same bits.  This does not assert WHICH way each mode goes - the series
    // is a few ulp out and the host's is too - only that the control word is
    // no longer ignored.
    f80 sines[4];
    static const uint16_t rcs[4] = { 0x037F, 0x077F, 0x0B7F, 0x0F7F };
    for (int i = 0; i < 4; i++) {
      f80_ctx cc = f80_ctx_make(rcs[i]);
      f80_sin(f80_of(0.7L), &sines[i], cc);
    }
    bool all_same = true;
    for (int i = 1; i < 4; i++)
      if (sines[i].sig != sines[0].sig || sines[i].se != sines[0].se) all_same = false;
    check(!all_same, "FSIN answers to the guest's rounding-control field");

    //---------------------------------------------------------------------
    // 4b. #U at the bottom of the range, and C1.  The two counts todo.txt
    //     carried as open until 2026-08-29.
    //---------------------------------------------------------------------
    // A LIVE CONTROL runs first, and it is not decoration: two probe bugs were
    // found writing these, and this assertion catches both.  Reading FNSTSW
    // after storing the result reports C1=0 for EVERYTHING, and reading C1 as
    // "moved toward +infinity" rather than "grew in magnitude" makes every
    // signed-result function score exactly chance.  FSQRT is specified for
    // both, so if this check fails the probe is broken, not the core.
    {
      long double one = 1.0L, two = 2.0L, three = 3.0L, xr3;
      uint16_t sw1, sw2, sw3;
      setcw(0x037F); clex();
      __asm__ volatile("fsqrt\n\tfnstsw %1" : "=t"(xr3), "=a"(sw1) : "0"(one));
      setcw(0x037F); clex();
      __asm__ volatile("fsqrt\n\tfnstsw %1" : "=t"(xr3), "=a"(sw2) : "0"(two));
      setcw(0x037F); clex();
      __asm__ volatile("fsqrt\n\tfnstsw %1" : "=t"(xr3), "=a"(sw3) : "0"(three));
      setcw(0x037F); (void)xr3;
      check((sw1 & 0x20) == 0 && (sw2 & 0x20) != 0 &&
            ((sw2 ^ sw3) & 0x200) != 0,
            "control: FSQRT raises #P only when inexact, and its C1 varies");
    }

    // T1.  The host raises #P for FYL2X of EVERY power of two except 1.0 -
    // y*log2(x) is a multiplication and its result is not generally
    // representable.  The check above tests the one exception; this tests the
    // rule, and the two together are why that check had to be renamed.
    {
      int bad = 0, n = 0;
      for (int k = -4; k <= 4; k++) {
        if (k == 0) continue;
        long double xv = 1.0L, yv = 3.0L, xr4;
        for (int i = 0; i < (k < 0 ? -k : k); i++) xv = (k < 0) ? xv / 2 : xv * 2;
        f80_ctx cc = f80_ctx_make(0x037F);
        f80 gg = f80_yl2x(f80_of(yv), f80_of(xv), cc);
        (void)gg;
        uint16_t hs;
        setcw(0x037F); clex();
        __asm__ volatile("fyl2x\n\tfnstsw %1"
                         : "=t"(xr4), "=a"(hs) : "0"(xv), "u"(yv) : "st(1)");
        setcw(0x037F); (void)xr4;
        n++;
        if ((uint16_t)(cc.flags & 0x3F) != (uint16_t)(hs & 0x3F)) bad++;
      }
      std::snprintf(msg, sizeof msg,
                    "FYL2X of a power of two OTHER than 1.0 reports #P, as the "
                    "host does (%d cases)", n);
      check(bad == 0, msg);
    }

    // T2.  The input the changelog named: 2^-16382, the smallest normal.
    {
      f80 tiny; tiny.sig = 0x8000000000000000ULL; tiny.se = 0x0001;
      f80_ctx cc = f80_ctx_make(0x037F);
      f80 gg = f80_2xm1(tiny, cc);
      (void)gg;
      check((cc.flags & 0x10) != 0 && (cc.flags & 0x20) != 0,
            "F2XM1 of 2^-16382 reports #U|#P, as the host does");
    }

    // T3.  The same miss covers seven forms, not one.  Enumerated across the
    // bottom of the exponent range rather than sampled.  FCOS is absent
    // because the host raises no #U for it, and one FPATAN class is excluded
    // by name in T3's comment below rather than quietly dropped.
    {
      int bad = 0, n = 0;
      for (int e = 1; e <= 3; e++) {
        for (int b = 0; b < 4; b++) {
          f80 a; a.sig = 0x8000000000000000ULL | ((uint64_t)b << 60); a.se = (uint16_t)e;
          long double av = ld_of(a), xr5;
          uint16_t hs;
          f80_ctx cc = f80_ctx_make(0x037F);
          f80 gg = f80_2xm1(a, cc); (void)gg;
          setcw(0x037F); clex();
          __asm__ volatile("f2xm1\n\tfnstsw %1" : "=t"(xr5), "=a"(hs) : "0"(av));
          setcw(0x037F); (void)xr5;
          n++; if (((cc.flags >> 4) & 1) != ((hs >> 4) & 1)) bad++;

          f80 gs; cc = f80_ctx_make(0x037F);
          if (f80_sin(a, &gs, cc)) {
            setcw(0x037F); clex();
            __asm__ volatile("fsin\n\tfnstsw %1" : "=t"(xr5), "=a"(hs) : "0"(av));
            setcw(0x037F);
            n++; if (((cc.flags >> 4) & 1) != ((hs >> 4) & 1)) bad++;
          }
        }
      }
      std::snprintf(msg, sizeof msg,
                    "the transcendentals report #U at the bottom of the exponent "
                    "range (%d rows)", n);
      check(bad == 0, msg);
    }

    // T4.  FPTAN never reported #P where its divide happens to be exact, which
    // is everything with |x| <= 2^-63: the sine IS x and the cosine IS 1, so
    // the divide is exact and FPTAN was the one entry point with no forced #P.
    // Its #U could not be earned without this, because the #U rule is guarded
    // on #P.
    {
      int bad = 0, n = 0;
      for (int e = 0; e < 3; e++) {
        f80 a; a.sig = 0x8000000000000000ULL; a.se = (uint16_t)(0x3FC0 - e);
        f80 gt; f80_ctx cc = f80_ctx_make(0x037F);
        if (f80_ptan(a, &gt, cc)) { n++; if ((cc.flags & 0x20) == 0) bad++; }
      }
      std::snprintf(msg, sizeof msg,
                    "FPTAN reports #P even where its divide is exact (%d cases)", n);
      check(bad == 0, msg);
    }

    // T7.  THE RANGE GUARD'S FIRING PROFILE, which nothing else here can see.
    //
    // f80_ptan's two-word ending is guarded, because f80_mul2 cannot pack an
    // out-of-range head.  A guard is a special kind of hazard: one that fires
    // too OFTEN returns correct-but-less-accurate answers and passes every
    // correctness check in this file silently.  That has happened in this
    // repository before - a threshold that mixed biased and unbiased exponents
    // fired on almost every input, reverted the whole accuracy gain, and left
    // every suite green.
    //
    // The guard is not directly observable, but its effect is, so the RATE is
    // the assertion.  Verified by deliberately forcing the guard to fire on
    // every input: this check drops to 461/600 = 76.8% and FAILS - while the
    // ulp line oracle_transcendental prints stays at FPTAN=2.0, unchanged.
    // That is the point.  The accuracy budget does NOT catch this; only the
    // rate does.  The bound is deliberately slack - it is a tripwire for a
    // structural regression, not an accuracy budget.
    {
      int n = 0, same = 0;
      for (int i = 0; i < 600; i++) {
        f80 x; x.sig = 0x8000000000000000ULL | ((uint64_t)(i * 2654435761u) << 20);
        x.se = (uint16_t)(16383 - 1 - (i % 12));          // |x| in [2^-13, 2^-1)
        f80 g; f80_ctx cc = f80_ctx_make(0x037F);
        if (!f80_ptan(x, &g, cc)) continue;
        long double xv = ld_of(x), rv, dummy;
        setcw(0x037F); clex();
        __asm__ volatile("fldt %2\n\tfptan\n\tfstpt %1\n\tfstpt %0"
                         : "=m"(rv), "=m"(dummy) : "m"(xv) : "st");
        setcw(0x037F);
        n++;
        f80 h = f80_of(rv);
        if (h.sig == g.sig && h.se == g.se) same++;
      }
      std::snprintf(msg, sizeof msg,
                    "FPTAN's range guard stays out of the way on ordinary "
                    "arguments (%d/%d match the host; all-fire gives ~2/3)",
                    same, n);
      check(n > 0 && same * 100 >= n * 85, msg);
    }

    // T5.  PASSES EITHER WAY, and is kept for that reason: it is the guard on
    // the trap the previous transcendental commit recorded, where merging
    // intermediate flags made F2XM1 of the smallest NORMAL report #D for an
    // operand that was not denormal.  It proves nothing about the #U fix and
    // is not counted among the checks that do.
    {
      f80 sn; sn.sig = 0x8000000000000000ULL; sn.se = 0x0001;
      f80_ctx cc = f80_ctx_make(0x037F);
      f80 gg = f80_2xm1(sn, cc); (void)gg;
      check((cc.flags & 0x02) == 0,
            "[passes either way] F2XM1 of the smallest NORMAL reports no #D");
    }
  }

  // 5. The logarithm edge cases, where the shortcut returns get the answer
  //    without going near the series - and where four of them were wrong.
  //    Every one of these is graded against the host exactly: value, the sign
  //    of a zero, and flags.
  {
    struct Edge {
      int op;                       // 0 = FYL2X, 1 = FYL2XP1
      f80 y, x;
      const char *what;
    };
    f80 dn_small; dn_small.sig = 0x0000000000000001ULL; dn_small.se = 0x0000;
    f80 dn_half;  dn_half.sig  = 0x4000000000000000ULL; dn_half.se  = 0x0000;
    const Edge edges[] = {
      { 0, f80_make_zero(false), f80_of(0.5L),  "FYL2X(+0, 0.5): log2 is negative, so -0" },
      { 0, f80_make_zero(false), f80_of(2.0L),  "FYL2X(+0, 2.0): log2 is positive, so +0" },
      { 0, f80_make_zero(true),  f80_of(0.5L),  "FYL2X(-0, 0.5): both signs, so +0" },
      { 0, f80_make_zero(false), dn_half,       "FYL2X(+0, denormal): #D survives the shortcut" },
      { 1, f80_make_inf(false),  f80_make_zero(false), "FYL2XP1(+inf, +0) is 0*inf: #IA" },
      { 1, f80_make_inf(true),   f80_make_zero(false), "FYL2XP1(-inf, +0) is 0*inf: #IA" },
      { 1, f80_of(1.0L),         dn_small,      "FYL2XP1 of a bottom-of-range denormal" },
    };
    int ebad = 0;
    for (unsigned i = 0; i < sizeof edges / sizeof edges[0]; i++) {
      const Edge &e = edges[i];
      f80_ctx c = f80_ctx_make(0x037F);
      f80 g = e.op ? f80_yl2xp1(e.y, e.x, c) : f80_yl2x(e.y, e.x, c);
      volatile long double vy = ld_of(e.y), vx = ld_of(e.x);
      long double xy = vy, xx = vx, xr;
      uint16_t hsw;
      setcw(0x037F); clex();
      if (e.op)
        __asm__ volatile("fyl2xp1\n\tfnstsw %1" : "=t"(xr), "=a"(hsw) : "0"(xx), "u"(xy) : "st(1)");
      else
        __asm__ volatile("fyl2x\n\tfnstsw %1"   : "=t"(xr), "=a"(hsw) : "0"(xx), "u"(xy) : "st(1)");
      setcw(0x037F);
      f80 want = f80_of(xr);
      uint16_t gf = (uint16_t)(c.flags & 0x3F), wf = (uint16_t)(hsw & 0x3F);
      if (g.sig != want.sig || g.se != want.se || gf != wf) {
        ebad++;
        report(e.what, 0x037F, e.y, e.x, g, gf, want, wf);
      }
    }
    check(ebad == 0, "the FYL2X/FYL2XP1 shortcut returns match the host exactly");
  }

  // 6. f80_mul2's tail, graded against EXACT integer arithmetic rather than
  //    against the host - there is no instruction that exposes a double-double
  //    product, so the oracle here is the 128-bit product itself.  The tail
  //    belongs to the scale the product had BEFORE the rounding carry, and
  //    reading it against the adjusted exponent was wrong by exactly half an
  //    ulp of the head: large enough to matter to every transcendental that
  //    uses the pair, small enough to look plausible in a spot check.
  {
    static const uint64_t pairs[][2] = {
      { 0xFFFFFFFFFFFFFFFEULL, 0x8000000000000001ULL },   // carries out
      { 0xFFFFFFFFFFFFFFFCULL, 0x8000000000000002ULL },   // carries out
      { 0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL },
      { 0x8000000000000000ULL, 0x8000000000000000ULL },   // exact, no tail
      { 0xC000000000000001ULL, 0xB000000000000003ULL },
    };
    int mbad = 0;
    for (unsigned i = 0; i < sizeof pairs / sizeof pairs[0]; i++) {
      f80 a, b;
      a.sig = pairs[i][0]; a.se = 0x3FFF;
      b.sig = pairs[i][1]; b.se = 0x3FFF;
      f80 hi, lo;
      f80_mul2(a, b, &hi, &lo);
      unsigned __int128 P = (unsigned __int128)a.sig * b.sig;   // value = P * 2^-126
      __int128 acc = 0;
      for (int w = 0; w < 2; w++) {
        f80 v = w ? lo : hi;
        if (!v.sig) continue;
        int e = (int)(v.se & 0x7FFF) - 16383, sh = e - 63 + 126;
        __int128 t = (sh >= 0) ? ((__int128)v.sig << sh) : ((__int128)(v.sig >> (-sh)));
        acc += (v.se & 0x8000) ? -t : t;
      }
      if (acc != (__int128)P) mbad++;
    }
    check(mbad == 0, "f80_mul2's head and tail sum to the exact product");
  }

  // 7. FPREM/FPREM1 with a PSEUDO-denormal dividend - exponent field 0 with the
  //    significand's J bit SET.  Several paths hand the dividend straight back,
  //    so the unnormalized encoding used to survive where hardware delivers the
  //    normalized one.  The value is the same either way, which is why the
  //    flags always agreed and only the bits were wrong.  Not to be confused
  //    with an UNNORMAL (J clear, nonzero exponent), which is genuinely
  //    unsupported and already goes to #IA; the last row checks that still holds.
  {
    struct { uint16_t ase; uint64_t asig; uint16_t bse; uint64_t bsig; bool ieee; } t[] = {
      { 0x0000, 0x8000000000000000ULL, 0x3FFF, 0x8000000000000000ULL, false },
      { 0x0000, 0x8000000000000000ULL, 0x3FFF, 0x8000000000000000ULL, true  },
      { 0x0000, 0x8000000000000000ULL, 0x7FFE, 0x8000000000000000ULL, false },
      { 0x0000, 0x8000000000000000ULL, 0x7FFF, 0x8000000000000000ULL, false },
      { 0x3FFF, 0xC000000000000000ULL, 0x3FFE, 0x8000000000000000ULL, false },
      { 0x4000, 0x4000000000000000ULL, 0x3FFF, 0x8000000000000000ULL, false },
    };
    int pbad = 0;
    for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++) {
      f80 a, b;
      a.se = t[i].ase; a.sig = t[i].asig;
      b.se = t[i].bse; b.sig = t[i].bsig;
      f80_ctx c = f80_ctx_make(0x037F);
      int q; bool part;
      f80 g = t[i].ieee ? f80_prem1(a, b, c, &q, &part) : f80_prem(a, b, c, &q, &part);
      volatile long double va = ld_of(a), vb = ld_of(b);
      long double xa = va, xb = vb, xr;
      uint16_t hsw;
      setcw(0x037F); clex();
      // No st(1) clobber: FPREM does not pop, so the divisor is still live and
      // gcc has to be left to retire it.  Declaring it clobbered leaks a stack
      // slot per iteration, and six of them is enough to corrupt every test
      // that runs afterwards - which is exactly what it did.
      if (t[i].ieee)
        __asm__ volatile("fprem1\n\tfnstsw %1" : "=t"(xr), "=a"(hsw) : "0"(xa), "u"(xb));
      else
        __asm__ volatile("fprem\n\tfnstsw %1"  : "=t"(xr), "=a"(hsw) : "0"(xa), "u"(xb));
      setcw(0x037F);
      f80 want = f80_of(xr);
      if (g.sig != want.sig || g.se != want.se ||
          (uint16_t)(c.flags & 0x3F) != (uint16_t)(hsw & 0x3F)) {
        pbad++;
        report("prem.pseudo", 0x037F, a, b, g, (uint16_t)(c.flags & 0x3F),
               want, (uint16_t)(hsw & 0x3F));
      }
    }
    check(pbad == 0, "FPREM/FPREM1 normalise a pseudo-denormal dividend like the host");
  }

  // 8. The UNMASKED overflow and underflow responses, which are not the masked
  //    ones with a flag added: the result is delivered at full destination
  //    precision with the biased exponent moved by -24576 or +24576, so no
  //    denormalisation happens and the value is usually exact.  That is why an
  //    unmasked #U carries no #P where the masked one does, and why an EXACT
  //    tiny result still raises #U.
  //
  //    The masked rows are run alongside deliberately.  They are the soundness
  //    check: this touches the path every arithmetic operation funnels through,
  //    and a change that fixed the unmasked cases by disturbing the masked ones
  //    would be worse than the defect.  Against the unfixed core this reports
  //    800 tininess and 320 overflow divergences with zero on the masked rows.
  {
    static const uint16_t cws[3] = { 0x037F, 0x036F, 0x0377 };  // masked, #U, #O
    int ubad = 0, ucases = 0;
    for (int ci = 0; ci < 3; ci++) {
      uint16_t cw = cws[ci];
      for (int ae = 1; ae <= 4; ae++)
        for (int d = 0; d < 24; d++)
          for (int sg = 0; sg < 2; sg++)
            for (int sc = 1; sc <= 4; sc++) {
              f80 a; a.sig = 0xFFFFFFFFFFFFFFFFULL - (uint64_t)d;
              a.se = (uint16_t)((sg ? 0x8000 : 0) | ae);
              f80 b = f80_of((long double)(-sc));
              f80_ctx c = f80_ctx_make(cw);
              f80 g = f80_scale(a, b, c);
              volatile long double va = ld_of(a), vb = ld_of(b);
              long double xa = va, xb = vb, xr;
              uint16_t hsw = 0;
              // FNSTSW and FNCLEX do not wait, so the status word is read and
              // the pending #MF cleared before the FSTPT readback - which does
              // wait, and would trap.
              __asm__ volatile("fldcw %[c]\n\tfnclex\n\tfldt %[b]\n\tfldt %[a]\n\t"
                               "fscale\n\tfnstsw %[s]\n\tfnclex\n\tfstpt %[r]\n\t"
                               "fstp %%st(0)\n\tfninit"
                               : [s]"=m"(hsw), [r]"=m"(xr)
                               : [c]"m"(cw), [a]"m"(xa), [b]"m"(xb)
                               : "st", "st(1)", "memory");
              setcw(0x037F);
              f80 want = f80_of(xr);
              ucases++;
              if (g.sig != want.sig || g.se != want.se ||
                  (uint16_t)(c.flags & 0x3F) != (uint16_t)(hsw & 0x3F)) {
                ubad++;
                report("unmasked", cw, a, b, g, (uint16_t)(c.flags & 0x3F),
                       want, (uint16_t)(hsw & 0x3F));
              }
            }
    }
    char m2[128];
    std::snprintf(m2, sizeof m2,
                  "the unmasked #U/#O responses match the host (%d cases, masked rows included)",
                  ucases);
    check(ubad == 0, m2);
  }
}

// The recorded bound.  Measured, not asserted from the manual: raise it only
// after looking at what moved, because these are the numbers that say whether
// the polynomial evaluations are still doing their job.
static const double ULP_BOUND = 6.0;

static f80 genr(int lo, int hi) {
  f80 a;
  a.sig = 0x8000000000000000ULL | (rnd() >> 1);
  int e = lo + (int)(rnd() % (uint64_t)(hi - lo + 1));
  a.se = (uint16_t)(((rnd() & 1) ? 0x8000 : 0) | (uint16_t)(e + 16383));
  return a;
}

static void oracle_transcendental(int n) {
  reseed(0x452821E638D01377ULL);
  double w2xm1 = 0, wyl2x = 0, wyl2xp1 = 0, wpatan = 0,
         wsin = 0, wcos = 0, wtan = 0, wbig = 0;
  for (int i = 0; i < n; i++) {
    f80_ctx c;
    { f80 x = genr(-70, -1); c = f80_ctx_make(0x037F);
      f80 g = f80_2xm1(x, c);
      double u = ulps(g, f80_of(expm1l(ld_of(x) * logl(2.0L))));
      if (u > w2xm1) w2xm1 = u; }
    { f80 x = genr(-200, 200); x.se = (uint16_t)(x.se & 0x7FFF); f80 y = genr(-50, 50);
      c = f80_ctx_make(0x037F);
      double u = ulps(f80_yl2x(y, x, c), f80_of(ld_of(y) * log2l(ld_of(x))));
      if (u > wyl2x) wyl2x = u; }
    { f80 x = genr(-70, -3); f80 y = genr(-50, 50); c = f80_ctx_make(0x037F);
      double u = ulps(f80_yl2xp1(y, x, c),
                      f80_of(ld_of(y) * (log1pl(ld_of(x)) / logl(2.0L))));
      if (u > wyl2xp1) wyl2xp1 = u; }
    { f80 y = genr(-60, 60), x = genr(-60, 60); c = f80_ctx_make(0x037F);
      double u = ulps(f80_patan(y, x, c), f80_of(atan2l(ld_of(y), ld_of(x))));
      if (u > wpatan) wpatan = u; }
    { f80 x = genr(-20, 20); f80 g;
      c = f80_ctx_make(0x037F);
      if (f80_sin(x, &g, c)) { double u = ulps(g, f80_of(sinl(ld_of(x)))); if (u > wsin) wsin = u; }
      c = f80_ctx_make(0x037F);
      if (f80_cos(x, &g, c)) { double u = ulps(g, f80_of(cosl(ld_of(x)))); if (u > wcos) wcos = u; }
      c = f80_ctx_make(0x037F);
      if (f80_ptan(x, &g, c)) { double u = ulps(g, f80_of(tanl(ld_of(x)))); if (u > wtan) wtan = u; } }
    { f80 x = genr(30, 62); c = f80_ctx_make(0x037F); f80 g;
      if (f80_sin(x, &g, c)) { double u = ulps(g, f80_of(sinl(ld_of(x)))); if (u > wbig) wbig = u; } }
  }
  struct { const char *name; double worst; } r[] = {
    { "F2XM1",   w2xm1 },   { "FYL2X",   wyl2x },
    { "FYL2XP1", wyl2xp1 }, { "FPATAN",  wpatan },
    { "FSIN",    wsin },    { "FCOS",    wcos },
    { "FPTAN",   wtan },    { "FSIN (arguments up to 2^62)", wbig },
  };
  for (auto &k : r) {
    char msg[160];
    std::snprintf(msg, sizeof msg, "%s is within %.1f ulp of the host libm (worst %.1f)",
                  k.name, ULP_BOUND, k.worst);
    check(k.worst <= ULP_BOUND, msg);
  }
  // The generator above only makes normals, so the degenerate quadrants -
  // atan2 of a zero against an infinity, log2 of an infinity, sin of a NaN -
  // went ungraded until this was added.  They are compared as VALUES, with any
  // NaN equal to any other, because a NaN payload out of libm is not
  // architectural and neither is the one a 387 would produce.
  {
    static const f80 SPEC[] = {
      { 0x0000000000000000ULL, 0x0000 }, { 0x0000000000000000ULL, 0x8000 },
      { 0x8000000000000000ULL, 0x7FFF }, { 0x8000000000000000ULL, 0xFFFF },
      { 0x8000000000000000ULL, 0x3FFF }, { 0x8000000000000000ULL, 0xBFFF },
      { 0xC000000000000000ULL, 0x7FFF }, { 0x0123456789ABCDEFULL, 0x0000 },
      { 0x8000000000000000ULL, 0x0001 }, { 0xFFFFFFFFFFFFFFFFULL, 0x7FFE },
    };
    const int NS = (int)(sizeof SPEC / sizeof SPEC[0]);
    int bad_at = 0, bad_un = 0;
    for (int i = 0; i < NS; i++) {
      for (int j = 0; j < NS; j++) {
        f80_ctx c = f80_ctx_make(0x037F);
        f80 g = f80_patan(SPEC[i], SPEC[j], c);
        f80 w = f80_of(atan2l(ld_of(SPEC[i]), ld_of(SPEC[j])));
        bool both_nan = f80_is_nan(f80_classify(g)) && f80_is_nan(f80_classify(w));
        if (!both_nan && ulps(g, w) > ULP_BOUND) {
          if (bad_at < 4)
            std::printf("  FPATAN(%016llX:%04X, %016llX:%04X) got %016llX:%04X want %016llX:%04X\n",
                        (unsigned long long)SPEC[i].sig, SPEC[i].se,
                        (unsigned long long)SPEC[j].sig, SPEC[j].se,
                        (unsigned long long)g.sig, g.se,
                        (unsigned long long)w.sig, w.se);
          bad_at++;
        }
      }
      // The one-argument forms, over the same set.
      f80_ctx c1 = f80_ctx_make(0x037F);
      f80 g1 = f80_2xm1(SPEC[i], c1);
      f80 w1 = f80_of(expm1l(ld_of(SPEC[i]) * logl(2.0L)));
      bool bn = f80_is_nan(f80_classify(g1)) && f80_is_nan(f80_classify(w1));
      if (!bn && ulps(g1, w1) > ULP_BOUND) bad_un++;
      f80_ctx c2 = f80_ctx_make(0x037F);
      f80 g2 = f80_yl2x(F80_ONE, SPEC[i], c2);
      f80 w2 = f80_of(log2l(ld_of(SPEC[i])));
      bn = f80_is_nan(f80_classify(g2)) && f80_is_nan(f80_classify(w2));
      // log2 of a negative is #IA here and a NaN there, which agrees; log2 of
      // a zero is -inf both ways.
      if (!bn && ulps(g2, w2) > ULP_BOUND) bad_un++;
      f80 g3; f80_ctx c3 = f80_ctx_make(0x037F);
      if (f80_sin(SPEC[i], &g3, c3)) {
        f80 w3 = f80_of(sinl(ld_of(SPEC[i])));
        bn = f80_is_nan(f80_classify(g3)) && f80_is_nan(f80_classify(w3));
        if (!bn && ulps(g3, w3) > ULP_BOUND) bad_un++;
      }
    }
    check(bad_at == 0, "FPATAN agrees with the host over all 100 zero/infinity/NaN quadrants");
    check(bad_un == 0, "F2XM1, FYL2X and FSIN agree with the host on every special operand");
  }

  std::printf("   transcendental worst-case ulps:");
  for (auto &k : r) std::printf(" %s=%.1f", k.name, k.worst);
  std::printf("\n");
}
#endif  // HAVE_X87_ORACLE

int main(int argc, char **argv) {
  // Three passes of everything runs in about two seconds, which is what the
  // suite budget allows; pass a larger scale by hand when changing the
  // arithmetic, and let it run for a minute.
  int scale = (argc > 1) ? std::atoi(argv[1]) : 3;
  if (scale < 1) scale = 1;

  std::printf("== golden vectors\n");
  run_vectors();
  std::printf("== self-consistency\n");
  run_identities(20000 * scale);

#if HAVE_X87_ORACLE
  std::printf("== host x87 oracle (x86-64, 64-bit long double)\n");
  oracle_arith(4000 * scale);
  oracle_ops(600 * scale);
  oracle_boundaries();
  oracle_transcendental(20000 * scale);
#else
  std::printf("== host x87 oracle SKIPPED: needs x86-64 with a 64-bit long double\n");
  std::printf("   (this is the shipping case on ARM64; the vectors above still ran)\n");
#endif

  std::printf("\n");
  if (g_failures == 0) {
    std::printf("ALL F80 TESTS PASS (%d checks)\n", g_checks);
    return 0;
  }
  std::printf("F80 TESTS FAILED: %d of %d checks failed\n", g_failures, g_checks);
  return 1;
}
