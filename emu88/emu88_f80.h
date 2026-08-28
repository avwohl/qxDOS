#ifndef EMU88_F80_H
#define EMU88_F80_H

// emu88_f80.h — 80-bit double extended-precision soft float for the x87.
//
// WHY THIS IS A HEADER AND NOT A .cc
// ----------------------------------
// A second product compiles emu88 out of this working tree.  dosiz's
// src/CMakeLists.txt names exactly six emu88 translation units - emu88.cc,
// emu88_pmode.cc, emu88_fpu.cc, emu88_mem.cc, opl.cc, sound_blaster.cc - and
// nothing there globs.  A new emu88/*.cc would compile here, pass every suite
// here, and fail to LINK there, with no signal in between.  So all of this is
// header-only and reaches the build through emu88_fpu.cc's include of it.
// emu88.h includes it too, because FPUState::regs is now f80 rather than
// double, which puts this file in all six of dosiz's TUs.  Keep it
// self-contained: <cstdint> only, no libm, no host FPU.
//
// WHY NOT HOST long double
// ------------------------
// On x86-64 `long double` IS this format, and using it would be four lines.
// On Apple ARM64 - which is the shipping target - `long double` is a plain
// binary64, and on Linux ARM64 it is binary128.  A register file whose
// precision depends on the host is the bug this file exists to remove, so the
// arithmetic is integer-only and bit-identical on every target.  The host's
// 80-bit long double is still used, but only as a TEST ORACLE, in
// tests/f80_unit.cc, where the platform check is explicit.
//
// THE FORMAT
// ----------
// value = (-1)^sign * significand * 2^(E - 63), significand an explicit-J
// 64-bit integer, E the unbiased exponent.  Bias 16383.
//
//   biased == 0            J == 0, frac == 0   zero
//                          J == 0, frac != 0   denormal
//                          J == 1              pseudo-denormal (same value as
//                                              a denormal; a 387 accepts it and
//                                              raises #D)
//   0 < biased < 0x7FFF    J == 1              normal
//                          J == 0              unnormal      -> UNSUPPORTED
//   biased == 0x7FFF       J == 1, frac == 0   infinity
//                          J == 1, bit62 == 1  QNaN
//                          J == 1, bit62 == 0  SNaN  (frac != 0)
//                          J == 0              pseudo-NaN/-inf -> UNSUPPORTED
//
// An 80387 raises #IA on every UNSUPPORTED encoding.  The 8087/287 gave some
// of them arithmetic meaning; the 387 does not, and neither does this.
//
// THE INTERNAL WORKING FORM
// -------------------------
// Every routine that has to round funnels through f80_round_pack(), which is
// handed a sign, a true binary exponent, and a 128-bit significand normalised
// so that bit 127 is set:
//
//     value = (-1)^sign * sig128 * 2^(exp - 127),   sig128 in [2^127, 2^128)
//
// so `exp` is floor(log2|value|) and the low 64 bits of sig128 are the guard,
// round and sticky material.  Every operand unpacked by f80_unpack() has at
// least 64 trailing zero bits in sig128, which is what makes the alignment
// shift in add/sub exact for every exponent difference up to 64.

#include <cstdint>

//===========================================================================
// The value, the exception flags, and the control/status plumbing
//===========================================================================

struct f80 {
  uint64_t sig;   // explicit-J 64-bit significand
  uint16_t se;    // sign in bit 15, biased 15-bit exponent in bits 14..0
};

// Deliberately the x87 status-word bit positions, so a caller ORs the
// accumulated flags straight into SW without a translation table.
enum {
  F80_IE = 0x0001,   // invalid operation
  F80_DE = 0x0002,   // denormal operand
  F80_ZE = 0x0004,   // zero divide
  F80_OE = 0x0008,   // overflow
  F80_UE = 0x0010,   // underflow
  F80_PE = 0x0020    // precision (inexact)
};

// One of these crosses every call.  `cw` is read for RC and PC; `flags` and
// `c1` are written.  Nothing here touches the guest status word directly -
// emu88_fpu.cc ORs `flags` into SW and places `c1` once the instruction's
// stack effect is known, because a stack fault overrides an arithmetic C1.
struct f80_ctx {
  uint16_t cw;
  uint16_t flags;
  bool     c1;

  // RC, control word bits 11:10.  0 nearest-even, 1 down, 2 up, 3 truncate.
  int rc() const { return (cw >> 10) & 3; }

  // PC, control word bits 9:8, as a significand width.  00 is 24-bit, 10 is
  // 53-bit, 11 is 64-bit.  01 is reserved; a 387 behaves as if it were 64, and
  // so does this.
  int prec() const {
    switch ((cw >> 8) & 3) {
      case 0:  return 24;
      case 2:  return 53;
      default: return 64;
    }
  }
};

static inline f80_ctx f80_ctx_make(uint16_t cw) {
  f80_ctx c;
  c.cw = cw;
  c.flags = 0;
  c.c1 = false;
  return c;
}

enum f80_class {
  F80_CLASS_ZERO,
  F80_CLASS_DENORMAL,
  F80_CLASS_NORMAL,
  F80_CLASS_INF,
  F80_CLASS_QNAN,
  F80_CLASS_SNAN,
  F80_CLASS_UNSUPPORTED
};

enum f80_cmp_r {
  F80_CMP_GT,
  F80_CMP_LT,
  F80_CMP_EQ,
  F80_CMP_UNORD
};

//===========================================================================
// Bit-level accessors and constructors
//===========================================================================

static inline bool     f80_neg(f80 a)    { return (a.se & 0x8000) != 0; }
static inline unsigned f80_biased(f80 a) { return a.se & 0x7FFFu; }

static inline f80 f80_make_zero(bool neg) {
  f80 r; r.sig = 0; r.se = neg ? 0x8000 : 0x0000; return r;
}
static inline f80 f80_make_inf(bool neg) {
  f80 r; r.sig = 0x8000000000000000ULL; r.se = neg ? 0xFFFF : 0x7FFF; return r;
}
// The "real indefinite" QNaN: the masked #IA response for every operation
// whose operands carry no NaN of their own.
static inline f80 f80_indefinite() {
  f80 r; r.sig = 0xC000000000000000ULL; r.se = 0xFFFF; return r;
}
static inline f80 f80_max_finite(bool neg) {
  f80 r; r.sig = 0xFFFFFFFFFFFFFFFFULL; r.se = (uint16_t)((neg ? 0x8000 : 0) | 0x7FFE); return r;
}
static inline f80 f80_min_denormal(bool neg) {
  f80 r; r.sig = 1; r.se = neg ? 0x8000 : 0x0000; return r;
}

static inline f80 f80_chs(f80 a) { a.se = (uint16_t)(a.se ^ 0x8000); return a; }
static inline f80 f80_abs(f80 a) { a.se = (uint16_t)(a.se & 0x7FFF); return a; }

static inline f80_class f80_classify(f80 a) {
  unsigned e = a.se & 0x7FFFu;
  uint64_t s = a.sig;
  if (e == 0) {
    if (s == 0) return F80_CLASS_ZERO;
    // Both the ordinary denormal (J clear) and the pseudo-denormal (J set)
    // land here: they denote the same set of values and a 387 computes with
    // both, raising #D.  Only the encoding differs.
    return F80_CLASS_DENORMAL;
  }
  if (e == 0x7FFF) {
    if (!(s >> 63)) return F80_CLASS_UNSUPPORTED;      // pseudo-NaN, pseudo-inf
    if ((s << 1) == 0) return F80_CLASS_INF;
    return (s & (1ULL << 62)) ? F80_CLASS_QNAN : F80_CLASS_SNAN;
  }
  if (!(s >> 63)) return F80_CLASS_UNSUPPORTED;        // unnormal
  return F80_CLASS_NORMAL;
}

static inline bool f80_is_nan(f80_class k) {
  return k == F80_CLASS_QNAN || k == F80_CLASS_SNAN;
}

//===========================================================================
// Small integer helpers
//===========================================================================

static inline int f80_clz64(uint64_t x) {   // x != 0
#if defined(__GNUC__) || defined(__clang__)
  return __builtin_clzll(x);
#else
  int n = 0;
  while (!(x >> 63)) { x <<= 1; n++; }
  return n;
#endif
}

static inline int f80_clz128(unsigned __int128 x) {   // x != 0
  uint64_t hi = (uint64_t)(x >> 64);
  if (hi) return f80_clz64(hi);
  return 64 + f80_clz64((uint64_t)x);
}

//===========================================================================
// Unpacking
//===========================================================================

struct f80_wide {
  bool     sign;
  int32_t  exp;              // true binary exponent
  unsigned __int128 sig;     // normalised, bit 127 set
};

// Only for ZERO, DENORMAL and NORMAL operands - the caller has already dealt
// with NaNs, infinities and unsupported encodings.  A zero unpacks to sig == 0
// with exp unspecified; every caller special-cases zero before getting here.
static inline f80_wide f80_unpack(f80 a) {
  f80_wide w;
  w.sign = f80_neg(a);
  unsigned e = a.se & 0x7FFFu;
  if (e == 0) {
    if (a.sig == 0) { w.sig = 0; w.exp = 0; return w; }
    // value = sig * 2^(-16382-63) = sig * 2^-16445, so with k the index of the
    // significand's high bit the normalised exponent is k - 16445.  A
    // pseudo-denormal (k == 63) lands on -16382, the smallest normal exponent,
    // which is exactly the value it denotes.
    int k = 63 - f80_clz64(a.sig);
    w.sig = (unsigned __int128)a.sig << (127 - k);
    w.exp = k - 16445;
  } else {
    w.sig = (unsigned __int128)a.sig << 64;
    w.exp = (int32_t)e - 16383;
  }
  return w;
}

//===========================================================================
// The rounding engine
//===========================================================================

// Round a 128-bit significand at `prec` bits after an extra `dshift` right
// shift (which is how the denormal grid is reached), under rounding mode `rc`
// for a value of sign `sign`.
//
// *keep is the rounded integer, `prec - dshift` bits wide before the carry and
// possibly 2^prec after it.  *inexact says whether anything was discarded;
// *inc says whether the magnitude was rounded up, which is what C1 reports.
static inline void f80_round_sig(unsigned __int128 sig, bool sticky, int prec,
                                 int dshift, int rc, bool sign,
                                 unsigned __int128 *keep, bool *inexact,
                                 bool *inc) {
  int drop = 128 - prec + dshift;
  unsigned __int128 k;
  bool roundbit, more;
  if (drop >= 128) {
    k = 0;
    if (drop == 128) {
      roundbit = ((sig >> 127) & 1) != 0;
      more = ((sig << 1) != 0) || sticky;
    } else {
      roundbit = false;
      more = (sig != 0) || sticky;
    }
  } else {
    k = sig >> drop;
    roundbit = drop > 0 && (((sig >> (drop - 1)) & 1) != 0);
    unsigned __int128 low = (drop >= 2)
        ? (sig & ((((unsigned __int128)1) << (drop - 1)) - 1))
        : (unsigned __int128)0;
    more = (low != 0) || sticky;
  }
  bool ie = roundbit || more;
  bool up;
  switch (rc) {
    case 0:  up = roundbit && (more || ((k & 1) != 0)); break;   // nearest even
    case 1:  up = sign && ie;  break;                            // toward -inf
    case 2:  up = !sign && ie; break;                            // toward +inf
    default: up = false;       break;                            // toward zero
  }
  if (up) k += 1;
  *keep = k;
  *inexact = ie;
  *inc = up;
}

// Round and pack.  `sig` must be zero or normalised with bit 127 set.
// `prec` is the significand width the result is rounded to: 64 normally, and
// 24 or 53 when precision control says so.  The EXPONENT range is the full
// 15-bit one whatever `prec` is - that is what makes PC different from
// converting through a narrower format.
static inline f80 f80_round_pack(bool sign, int32_t exp, unsigned __int128 sig,
                                 bool sticky, int prec, f80_ctx &c) {
  if (sig == 0) { c.c1 = false; return f80_make_zero(sign); }

  int rc = c.rc();
  int dshift = (exp < -16382) ? (int)(-16382 - exp) : 0;
  if (dshift > 200) dshift = 200;              // everything below is sticky anyway

  unsigned __int128 keep;
  bool inexact, inc;
  f80_round_sig(sig, sticky, prec, dshift, rc, sign, &keep, &inexact, &inc);

  int32_t E = exp + dshift;

  if (dshift == 0) {
    // The carry out of the precision window is the only way a normal result
    // changes exponent here.
    if ((keep >> prec) != 0) { keep >>= 1; E += 1; }
    if (E > 16383) {
      // Masked overflow.  Round-to-nearest and the mode that rounds away from
      // this result's sign deliver an infinity; the other two deliver the
      // largest finite number, because rounding toward zero (or toward the
      // other infinity) cannot manufacture one.
      bool to_inf;
      switch (rc) {
        case 0:  to_inf = true;  break;
        case 1:  to_inf = sign;  break;
        case 2:  to_inf = !sign; break;
        default: to_inf = false; break;
      }
      c.flags |= F80_OE | F80_PE;
      c.c1 = to_inf;
      if (to_inf) return f80_make_inf(sign);
      // "Largest finite" means largest AT THE CURRENT PRECISION: with PC at
      // 24 bits the low 40 significand bits are not writable, so the masked
      // overflow response is 0xFFFFFF0000000000, not 0xFFFFFFFFFFFFFFFF.
      f80 mx;
      mx.sig = ~(uint64_t)0 << (64 - prec);
      mx.se  = (uint16_t)((sign ? 0x8000 : 0) | 0x7FFE);
      return mx;
    }
    f80 r;
    r.sig = (uint64_t)(keep << (64 - prec));
    r.se  = (uint16_t)((sign ? 0x8000 : 0) | (uint16_t)(E + 16383));
    if (inexact) c.flags |= F80_PE;
    c.c1 = inc;
    return r;
  }

  // Denormal grid.  `keep` cannot carry past 2^prec here, because the extra
  // dshift already cost it that many bits; the one thing it CAN do is reach
  // the smallest normal, which is the tininess-after-rounding rule x86 uses:
  // a result that rounds up to 2^-16382 is not tiny and raises no #U.
  f80 r;
  uint64_t m = (uint64_t)(keep << (64 - prec));
  r.sig = m;
  r.se  = (uint16_t)((sign ? 0x8000 : 0) | ((m >> 63) ? 1u : 0u));
  if (inexact) {
    c.flags |= F80_PE;
    if ((m >> 63) == 0) c.flags |= F80_UE;    // tiny AND inexact
  }
  c.c1 = inc;
  return r;
}

//===========================================================================
// NaN propagation
//===========================================================================

// x87 NaN rules, shared by every two-operand arithmetic form.  Returns true
// when a NaN or unsupported operand decided the result.
//
// `a` is the destination in every x87 form that reaches here, which is what
// settles the tie when both operands are NaNs with equal significands.
static inline bool f80_prop_nan2(f80 a, f80 b, f80_ctx &c, f80 *out) {
  f80_class ka = f80_classify(a), kb = f80_classify(b);
  if (ka == F80_CLASS_UNSUPPORTED || kb == F80_CLASS_UNSUPPORTED) {
    c.flags |= F80_IE;
    *out = f80_indefinite();
    return true;
  }
  bool na = f80_is_nan(ka), nb = f80_is_nan(kb);
  if (!na && !nb) return false;
  if (ka == F80_CLASS_SNAN || kb == F80_CLASS_SNAN) c.flags |= F80_IE;
  f80 r;
  // The larger significand wins.  On an exact tie the hardware takes the one
  // with the smaller sign-exponent word, which for two NaNs means the positive
  // one - not, as the manual's "the destination" would suggest, ST(0).  This
  // was read off the host x87 by tests/f80_unit.cc rather than off the page.
  if (na && nb) {
    if (a.sig > b.sig)      r = a;
    else if (b.sig > a.sig) r = b;
    else                    r = (a.se < b.se) ? a : b;
  }
  else          r = na ? a : b;
  r.sig |= 1ULL << 62;                         // deliver it quiet
  *out = r;
  return true;
}

static inline bool f80_prop_nan1(f80 a, f80_ctx &c, f80 *out) {
  f80_class k = f80_classify(a);
  if (k == F80_CLASS_UNSUPPORTED) {
    c.flags |= F80_IE;
    *out = f80_indefinite();
    return true;
  }
  if (!f80_is_nan(k)) return false;
  if (k == F80_CLASS_SNAN) c.flags |= F80_IE;
  f80 r = a;
  r.sig |= 1ULL << 62;
  *out = r;
  return true;
}

//===========================================================================
// Addition and subtraction
//===========================================================================

// The alignment shift is exact for every exponent difference up to 64, because
// f80_unpack leaves at least 64 trailing zeros in sig128.  Beyond 64 the
// discarded bits become a sticky flag, and that is safe precisely because an
// exponent difference of 65 or more limits cancellation to a single bit: the
// difference cannot collapse into the region the sticky bit stands for.
//
// The sticky bit is applied to a subtraction as a BORROW, not as a jam.  With
// the smaller operand equal to S + d for some 0 < d < 1ulp, the exact result
// is (A - S) - d, which is strictly between (A - S - 1) and (A - S).  So
// "A - S - 1, inexact" is the exactly right thing to hand the rounder, and
// jamming the sticky into the low bit instead would be off by one ulp.
static inline f80 f80_addsub_p(f80 a, f80 b, bool sub, f80_ctx &c, int prec) {
  f80 r;
  if (f80_prop_nan2(a, b, c, &r)) return r;

  f80_class ka = f80_classify(a), kb = f80_classify(b);
  bool sa = f80_neg(a);
  bool sb = f80_neg(b) != sub;

  // #D reports a denormal OPERAND, and it is reported even when the operand
  // never reaches the arithmetic - infinity plus a denormal is an infinity
  // that still sets DE.  What suppresses it is a HIGHER-priority exception:
  // #IA and #Z stop the operation, and hardware then reports only those.  So
  // the flag is held here and raised at each ordinary exit.
  bool de = (ka == F80_CLASS_DENORMAL || kb == F80_CLASS_DENORMAL);

  if (ka == F80_CLASS_INF || kb == F80_CLASS_INF) {
    if (ka == F80_CLASS_INF && kb == F80_CLASS_INF) {
      if (sa != sb) { c.flags |= F80_IE; return f80_indefinite(); }
      if (de) c.flags |= F80_DE;
      return f80_make_inf(sa);
    }
    if (de) c.flags |= F80_DE;
    return (ka == F80_CLASS_INF) ? f80_make_inf(sa) : f80_make_inf(sb);
  }
  if (de) c.flags |= F80_DE;

  if (ka == F80_CLASS_ZERO && kb == F80_CLASS_ZERO) {
    // A sum of two zeros is negative only when both were, except under
    // round-down, where the sign of an exact zero is negative by definition.
    c.c1 = false;
    return f80_make_zero((sa && sb) || (sa != sb && c.rc() == 1));
  }
  // A zero operand still goes through the rounder: with PC at 24 or 53 bits,
  // ST(0) + 0.0 is a rounding operation, not a copy.
  if (ka == F80_CLASS_ZERO) {
    f80_wide w = f80_unpack(b);
    return f80_round_pack(sb, w.exp, w.sig, false, prec, c);
  }
  if (kb == F80_CLASS_ZERO) {
    f80_wide w = f80_unpack(a);
    return f80_round_pack(sa, w.exp, w.sig, false, prec, c);
  }

  f80_wide wa = f80_unpack(a), wb = f80_unpack(b);
  if (wb.exp > wa.exp || (wb.exp == wa.exp && wb.sig > wa.sig)) {
    f80_wide t = wa; wa = wb; wb = t;
    bool ts = sa; sa = sb; sb = ts;
  }

  int32_t d = wa.exp - wb.exp;
  unsigned __int128 bs;
  bool sticky = false;
  if (d == 0) {
    bs = wb.sig;
  } else if (d >= 128) {
    bs = 0;
    sticky = true;                 // wb.sig is normalised, so it is never zero
  } else {
    bs = wb.sig >> d;
    sticky = (wb.sig << (128 - d)) != 0;
  }

  int32_t rexp = wa.exp;
  unsigned __int128 rs;
  if (sa == sb) {
    rs = wa.sig + bs;
    if (rs < wa.sig) {             // carried out of bit 127
      sticky = sticky || ((rs & 1) != 0);
      rs = (rs >> 1) | (((unsigned __int128)1) << 127);
      rexp += 1;
    }
  } else {
    rs = wa.sig - bs - (sticky ? 1u : 0u);
    if (rs == 0) {
      c.c1 = false;
      return f80_make_zero(c.rc() == 1);
    }
    int sh = f80_clz128(rs);
    rs <<= sh;
    rexp -= sh;
  }
  return f80_round_pack(sa, rexp, rs, sticky, prec, c);
}

static inline f80 f80_add(f80 a, f80 b, f80_ctx &c) {
  return f80_addsub_p(a, b, false, c, c.prec());
}
static inline f80 f80_sub(f80 a, f80 b, f80_ctx &c) {
  return f80_addsub_p(a, b, true, c, c.prec());
}

//===========================================================================
// Multiplication
//===========================================================================

static inline f80 f80_mul_p(f80 a, f80 b, f80_ctx &c, int prec) {
  f80 r;
  if (f80_prop_nan2(a, b, c, &r)) return r;

  f80_class ka = f80_classify(a), kb = f80_classify(b);
  bool s = f80_neg(a) != f80_neg(b);
  bool de = (ka == F80_CLASS_DENORMAL || kb == F80_CLASS_DENORMAL);

  if (ka == F80_CLASS_INF || kb == F80_CLASS_INF) {
    if (ka == F80_CLASS_ZERO || kb == F80_CLASS_ZERO) {
      c.flags |= F80_IE;                        // #IA suppresses #D
      return f80_indefinite();
    }
    if (de) c.flags |= F80_DE;
    return f80_make_inf(s);
  }
  if (de) c.flags |= F80_DE;
  if (ka == F80_CLASS_ZERO || kb == F80_CLASS_ZERO) {
    c.c1 = false;
    return f80_make_zero(s);
  }

  f80_wide wa = f80_unpack(a), wb = f80_unpack(b);
  uint64_t ma = (uint64_t)(wa.sig >> 64), mb = (uint64_t)(wb.sig >> 64);
  unsigned __int128 p = (unsigned __int128)ma * mb;   // exact, 126 or 127 bits
  int32_t e;
  if ((p >> 127) != 0) e = wa.exp + wb.exp + 1;
  else                 { p <<= 1; e = wa.exp + wb.exp; }
  return f80_round_pack(s, e, p, false, prec, c);
}

static inline f80 f80_mul(f80 a, f80 b, f80_ctx &c) {
  return f80_mul_p(a, b, c, c.prec());
}

//===========================================================================
// Division
//===========================================================================

static inline f80 f80_div_p(f80 a, f80 b, f80_ctx &c, int prec) {
  f80 r;
  if (f80_prop_nan2(a, b, c, &r)) return r;

  f80_class ka = f80_classify(a), kb = f80_classify(b);
  bool s = f80_neg(a) != f80_neg(b);
  bool de = (ka == F80_CLASS_DENORMAL || kb == F80_CLASS_DENORMAL);

  if (ka == F80_CLASS_INF) {
    if (kb == F80_CLASS_INF) { c.flags |= F80_IE; return f80_indefinite(); }
    if (de) c.flags |= F80_DE;
    return f80_make_inf(s);
  }
  if (kb == F80_CLASS_INF) {
    if (de) c.flags |= F80_DE;
    c.c1 = false;
    return f80_make_zero(s);
  }
  if (kb == F80_CLASS_ZERO) {
    // 0/0 is an invalid operation; x/0 is a zero divide.  They are different
    // exceptions with different masked results and the same shape, which is
    // why they are easy to conflate - and BOTH suppress a denormal numerator's
    // #D, which is how the hardware orders them.
    if (ka == F80_CLASS_ZERO) { c.flags |= F80_IE; return f80_indefinite(); }
    c.flags |= F80_ZE;
    return f80_make_inf(s);
  }
  if (de) c.flags |= F80_DE;
  if (ka == F80_CLASS_ZERO) {
    c.c1 = false;
    return f80_make_zero(s);
  }

  f80_wide wa = f80_unpack(a), wb = f80_unpack(b);
  uint64_t ma = (uint64_t)(wa.sig >> 64), mb = (uint64_t)(wb.sig >> 64);

  // Two 128/64 divisions give 128 quotient bits and an exact remainder, which
  // is all the rounder needs: the top 64 bits are the significand, the next 64
  // are guard and round, and a non-zero final remainder is the sticky bit.
  unsigned __int128 num;
  int32_t e;
  if (ma >= mb) { num = (unsigned __int128)ma << 63; e = wa.exp - wb.exp; }
  else          { num = (unsigned __int128)ma << 64; e = wa.exp - wb.exp - 1; }
  uint64_t q1 = (uint64_t)(num / mb);
  uint64_t r1 = (uint64_t)(num % mb);
  unsigned __int128 num2 = (unsigned __int128)r1 << 64;
  uint64_t q2 = (uint64_t)(num2 / mb);
  uint64_t r2 = (uint64_t)(num2 % mb);
  unsigned __int128 q = ((unsigned __int128)q1 << 64) | q2;
  return f80_round_pack(s, e, q, r2 != 0, prec, c);
}

static inline f80 f80_div(f80 a, f80 b, f80_ctx &c) {
  return f80_div_p(a, b, c, c.prec());
}

//===========================================================================
// Square root
//===========================================================================

// Digit-by-digit, 65 iterations, entirely in integers.  65 is the whole answer
// the rounder needs: 64 significand bits plus the round bit, with everything
// below folded into a sticky.  A Newton iteration seeded from a host double
// would be faster and would put a 53-bit approximation in the middle of a
// 64-bit result, which is the class of shortcut this file exists to remove.
static inline f80 f80_sqrt_p(f80 a, f80_ctx &c, int prec) {
  f80 r;
  if (f80_prop_nan1(a, c, &r)) return r;

  f80_class k = f80_classify(a);
  if (k == F80_CLASS_ZERO) { c.c1 = false; return a; }      // sqrt(-0) is -0
  if (f80_neg(a)) { c.flags |= F80_IE; return f80_indefinite(); }
  if (k == F80_CLASS_INF) return a;
  if (k == F80_CLASS_DENORMAL) c.flags |= F80_DE;

  f80_wide w = f80_unpack(a);

  // N = sig128 * 2^s is a 256-bit radicand, with s chosen so that the halved
  // exponent is an integer and floor(sqrt(N)) lands in [2^127, 2^128).
  int s = (w.exp & 1) ? 128 : 127;
  unsigned __int128 nhi, nlo;
  if (s == 128) { nhi = w.sig; nlo = 0; }
  else          { nhi = w.sig >> 1; nlo = (unsigned __int128)(w.sig & 1) << 127; }

  // 65 steps consume the top 130 bits of N, which is bits 255..126; bit 127
  // and bit 126 are the only two that come out of the low half.
  unsigned __int128 root = 0, rem = 0;
  for (int i = 0; i < 65; i++) {
    unsigned pair = 0;
    for (int h = 1; h >= 0; h--) {
      int j = 254 + h - 2 * i;
      unsigned bit = (j >= 128) ? (unsigned)((nhi >> (j - 128)) & 1)
                                : (unsigned)((nlo >> j) & 1);
      pair = (pair << 1) | bit;
    }
    rem = (rem << 2) | pair;
    unsigned __int128 t = (root << 2) | 1;
    if (rem >= t) { rem -= t; root = (root << 1) | 1; }
    else          { root <<= 1; }
  }
  // Anything left below bit 126 keeps the result inexact even when the 130-bit
  // prefix came out exact.
  bool sticky = (rem != 0)
             || ((nlo & ((((unsigned __int128)1) << 126) - 1)) != 0);

  int32_t erev = 127 + ((w.exp - 127 - s) / 2);
  return f80_round_pack(false, erev, root << 63, sticky, prec, c);
}

static inline f80 f80_sqrt(f80 a, f80_ctx &c) {
  return f80_sqrt_p(a, c, c.prec());
}

//===========================================================================
// Comparison
//===========================================================================

// `quiet` selects the FUCOM family, which lets a QNaN through without raising
// #IA.  The FCOM family raises on any NaN at all.
static inline f80_cmp_r f80_compare(f80 a, f80 b, bool quiet, f80_ctx &c) {
  f80_class ka = f80_classify(a), kb = f80_classify(b);
  if (ka == F80_CLASS_UNSUPPORTED || kb == F80_CLASS_UNSUPPORTED ||
      ka == F80_CLASS_SNAN || kb == F80_CLASS_SNAN) {
    c.flags |= F80_IE;
    return F80_CMP_UNORD;
  }
  if (ka == F80_CLASS_QNAN || kb == F80_CLASS_QNAN) {
    if (!quiet) c.flags |= F80_IE;
    return F80_CMP_UNORD;
  }
  if (ka == F80_CLASS_DENORMAL || kb == F80_CLASS_DENORMAL) c.flags |= F80_DE;

  bool za = (ka == F80_CLASS_ZERO), zb = (kb == F80_CLASS_ZERO);
  if (za && zb) return F80_CMP_EQ;                 // +0 == -0

  bool sa = f80_neg(a), sb = f80_neg(b);
  if (za) return sb ? F80_CMP_GT : F80_CMP_LT;
  if (zb) return sa ? F80_CMP_LT : F80_CMP_GT;
  if (sa != sb) return sa ? F80_CMP_LT : F80_CMP_GT;

  bool agt;
  if (ka == F80_CLASS_INF && kb == F80_CLASS_INF) return F80_CMP_EQ;
  if (ka == F80_CLASS_INF) agt = true;
  else if (kb == F80_CLASS_INF) agt = false;
  else {
    // Unpack rather than compare (se, sig) as a pair: a pseudo-denormal has a
    // biased exponent of 0 and the magnitude of the smallest normal, so the
    // lexicographic shortcut gets it wrong.
    f80_wide wa = f80_unpack(a), wb = f80_unpack(b);
    if (wa.exp == wb.exp && wa.sig == wb.sig) return F80_CMP_EQ;
    agt = (wa.exp > wb.exp) || (wa.exp == wb.exp && wa.sig > wb.sig);
  }
  if (sa) agt = !agt;                              // both negative: order flips
  return agt ? F80_CMP_GT : F80_CMP_LT;
}

//===========================================================================
// Round to integer, scale, extract
//===========================================================================

static inline f80 f80_from_i64(int64_t v) {
  if (v == 0) return f80_make_zero(false);
  bool s = v < 0;
  // Negating INT64_MIN as a signed value is undefined; go through unsigned.
  uint64_t m = s ? (~(uint64_t)v) + 1u : (uint64_t)v;
  int z = f80_clz64(m);
  f80 r;
  r.sig = m << z;
  r.se  = (uint16_t)((s ? 0x8000 : 0) | (uint16_t)((63 - z) + 16383));
  return r;
}
static inline f80 f80_from_i32(int32_t v) { return f80_from_i64((int64_t)v); }
static inline f80 f80_from_i16(int16_t v) { return f80_from_i64((int64_t)v); }

// FRNDINT.  Precision control does not apply: the destination is an integer,
// and rounding to 24 or 53 significand bits on top of that would round twice.
static inline f80 f80_rndint(f80 a, f80_ctx &c) {
  f80 r;
  if (f80_prop_nan1(a, c, &r)) return r;
  f80_class k = f80_classify(a);
  if (k == F80_CLASS_ZERO || k == F80_CLASS_INF) { c.c1 = false; return a; }
  if (k == F80_CLASS_DENORMAL) c.flags |= F80_DE;

  f80_wide w = f80_unpack(a);
  if (w.exp >= 63) { c.c1 = false; return a; }        // ulp is already >= 1
  if (w.exp >= 0) {
    // Rounding to an integer IS rounding to exp+1 significant bits.
    return f80_round_pack(w.sign, w.exp, w.sig, false, (int)(w.exp + 1), c);
  }

  // |value| < 1, so the answer is 0 or 1 and only the mode decides which.
  bool one;
  switch (c.rc()) {
    case 0:  one = (w.exp == -1) && (w.sig > (((unsigned __int128)1) << 127)); break;
    case 1:  one = w.sign;  break;
    case 2:  one = !w.sign; break;
    default: one = false;   break;
  }
  c.flags |= F80_PE;
  c.c1 = one;
  if (!one) return f80_make_zero(w.sign);
  r.sig = 0x8000000000000000ULL;
  r.se  = (uint16_t)((w.sign ? 0x8000 : 0) | 0x3FFF);
  return r;
}

// Truncate toward zero to a signed 64-bit integer, saturating.  Used only by
// FSCALE, where the architectural rule is "truncate ST(1)" and any magnitude
// past the exponent range gives the same answer as the saturated one.
static inline int64_t f80_trunc_i64_sat(f80 a) {
  f80_class k = f80_classify(a);
  if (k == F80_CLASS_ZERO || f80_is_nan(k) || k == F80_CLASS_UNSUPPORTED) return 0;
  bool s = f80_neg(a);
  if (k == F80_CLASS_INF) return s ? INT64_MIN : INT64_MAX;
  f80_wide w = f80_unpack(a);
  if (w.exp < 0) return 0;
  if (w.exp >= 63) return s ? INT64_MIN : INT64_MAX;
  uint64_t m = (uint64_t)(w.sig >> (127 - w.exp));
  return s ? -(int64_t)m : (int64_t)m;
}

// FSCALE: ST(0) * 2^trunc(ST(1)).  The scaling is exact - only the pack can
// round - so this is an exponent adjustment and nothing else.
static inline f80 f80_scale(f80 a, f80 b, f80_ctx &c) {
  f80 r;
  if (f80_prop_nan2(a, b, c, &r)) return r;
  f80_class ka = f80_classify(a), kb = f80_classify(b);

  if (kb == F80_CLASS_INF) {
    bool bneg = f80_neg(b);
    if (ka == F80_CLASS_ZERO && !bneg) { c.flags |= F80_IE; return f80_indefinite(); }
    if (ka == F80_CLASS_INF   &&  bneg) { c.flags |= F80_IE; return f80_indefinite(); }
    if (ka == F80_CLASS_DENORMAL) c.flags |= F80_DE;
    if (ka == F80_CLASS_ZERO || ka == F80_CLASS_INF) { c.c1 = false; return a; }
    c.c1 = false;
    return bneg ? f80_make_zero(f80_neg(a)) : f80_make_inf(f80_neg(a));
  }
  // A denormal in EITHER operand is reported, including when ST(0) is a zero
  // or an infinity that the scaling cannot move.
  if (ka == F80_CLASS_DENORMAL || kb == F80_CLASS_DENORMAL) c.flags |= F80_DE;
  if (ka == F80_CLASS_ZERO || ka == F80_CLASS_INF) { c.c1 = false; return a; }

  int64_t n = f80_trunc_i64_sat(b);
  if (n >  100000) n =  100000;
  if (n < -100000) n = -100000;
  f80_wide w = f80_unpack(a);
  int64_t e = (int64_t)w.exp + n;
  if (e >  100000) e =  100000;
  if (e < -100000) e = -100000;
  // 64 bits, NOT c.prec().  Precision control applies to add, sub, mul, div
  // and sqrt and to nothing else; FSCALE only moves the exponent, and the host
  // x87 leaves all 64 significand bits alone under PC=24 and PC=53.  Rounding
  // here to the control word's precision was wrong and the oracle said so as
  // soon as its FSCALE grid was widened past PC=64.
  return f80_round_pack(w.sign, (int32_t)e, w.sig, false, 64, c);
}

// FXTRACT: ST(0) becomes the unbiased exponent as an integer-valued f80, and
// the significand in [1,2) is pushed above it.  A denormal is normalised
// first, so the exponent reported is the true one, not zero.
static inline void f80_extract(f80 a, f80 *exp_out, f80 *sig_out, f80_ctx &c) {
  f80 r;
  if (f80_prop_nan1(a, c, &r)) { *exp_out = r; *sig_out = r; return; }
  f80_class k = f80_classify(a);
  if (k == F80_CLASS_ZERO) {
    c.flags |= F80_ZE;                   // log(0) is a zero divide, and so is this
    *exp_out = f80_make_inf(true);
    *sig_out = a;
    return;
  }
  if (k == F80_CLASS_INF) {
    *exp_out = f80_make_inf(false);
    *sig_out = a;
    return;
  }
  if (k == F80_CLASS_DENORMAL) c.flags |= F80_DE;
  f80_wide w = f80_unpack(a);
  *exp_out = f80_from_i32(w.exp);
  f80 m;
  m.sig = (uint64_t)(w.sig >> 64);
  m.se  = (uint16_t)((w.sign ? 0x8000 : 0) | 0x3FFF);
  *sig_out = m;
}

//===========================================================================
// FPREM and FPREM1
//===========================================================================

// Both are exact: the remainder of two representable numbers is representable,
// so there is no rounding and precision control does not apply.  The work is
// an integer division of the aligned significands.
//
// `quo` returns the low three quotient bits, which the caller maps to C0, C3
// and C1.  `partial` is the C2 report: when the exponents are 64 or more apart
// a 387 does not run the reduction to completion, it takes a bite out of it and
// asks to be called again, and software is expected to loop on C2.
static inline f80 f80_prem_common(f80 a, f80 b, bool ieee, f80_ctx &c,
                                  int *quo, bool *partial) {
  *quo = 0;
  *partial = false;
  f80 r;
  if (f80_prop_nan2(a, b, c, &r)) return r;
  f80_class ka = f80_classify(a), kb = f80_classify(b);

  if (ka == F80_CLASS_INF || kb == F80_CLASS_ZERO) {
    c.flags |= F80_IE;
    return f80_indefinite();
  }
  if (ka == F80_CLASS_DENORMAL || kb == F80_CLASS_DENORMAL) c.flags |= F80_DE;
  if (kb == F80_CLASS_INF) return a;           // x mod inf is x
  if (ka == F80_CLASS_ZERO) return a;

  f80_wide wa = f80_unpack(a), wb = f80_unpack(b);
  uint64_t ma = (uint64_t)(wa.sig >> 64), mb = (uint64_t)(wb.sig >> 64);
  int32_t d = wa.exp - wb.exp;

  if (d < 0) {
    // |a| < |b|, so the truncating quotient is 0 and FPREM is done.  FPREM1
    // rounds the quotient to nearest even instead, and 0 is even, so it only
    // differs when |a| is strictly more than half of |b| - which, with the
    // exponents one apart, is exactly ma > mb.
    if (!ieee || d < -1 || ma <= mb) return a;
    *quo = 1;
    // |b| - |a| = (2*mb - ma) * 2^(eb-64), and 2*mb - ma is below 2^64 here,
    // so the IEEE remainder is exact and needs no subtraction path.
    uint64_t rm = (uint64_t)((((unsigned __int128)mb) << 1) - ma);
    int rz = f80_clz64(rm);
    return f80_round_pack(!wa.sign, wb.exp - 1 - rz,
                          (unsigned __int128)rm << (64 + rz), false, 64, c);
  }

  // A partial reduction takes 32 bits off the exponent difference and asks to
  // be called again through C2.  The architecture allows anything from 32 to
  // 63 bits per step and leaves the condition codes undefined while C2 is set;
  // 32 is the conservative choice and the quotient bits are reported anyway.
  int32_t step = d;
  if (d >= 64) { step = 32; *partial = true; }

  // A = ma << step fits: step <= 63 and ma < 2^64.
  unsigned __int128 A = (unsigned __int128)ma << step;
  uint64_t q = (uint64_t)(A / mb);
  uint64_t rem = (uint64_t)(A % mb);
  // The remainder's weight: for the complete case a - q*b has b's ulp; for the
  // partial one the reduction stopped `d - step` binary places early.
  int32_t rexp_base = wb.exp + (d - step);

  bool rsign = wa.sign;
  if (ieee && !*partial) {
    // Round the quotient to nearest even, which is the only thing separating
    // FPREM1 from FPREM.
    unsigned __int128 twice = (unsigned __int128)rem * 2;
    if (twice > mb || (twice == mb && (q & 1))) {
      rem = mb - rem;
      q += 1;
      rsign = !rsign;
    }
  }
  *quo = (int)(q & 7u);

  if (rem == 0) { c.c1 = false; return f80_make_zero(rsign); }
  int z = f80_clz64(rem);
  unsigned __int128 rs = (unsigned __int128)rem << (64 + z);
  return f80_round_pack(rsign, rexp_base - z, rs, false, 64, c);
}

static inline f80 f80_prem(f80 a, f80 b, f80_ctx &c, int *quo, bool *partial) {
  return f80_prem_common(a, b, false, c, quo, partial);
}
static inline f80 f80_prem1(f80 a, f80 b, f80_ctx &c, int *quo, bool *partial) {
  return f80_prem_common(a, b, true, c, quo, partial);
}

//===========================================================================
// Conversions to and from the IEEE single and double formats
//===========================================================================

// Widening is exact, so this only has to decode.  The one thing it is NOT
// allowed to do is flush a denormal: a 387 loads it, normalises it into the
// wider exponent range, and reports #D.
static inline f80 f80_from_ieee(uint64_t bits, int mbits, int ebits, f80_ctx &c) {
  int bias = (1 << (ebits - 1)) - 1;
  uint64_t emax = (((uint64_t)1) << ebits) - 1;
  bool s = ((bits >> (ebits + mbits)) & 1) != 0;
  uint64_t e = (bits >> mbits) & emax;
  uint64_t m = bits & ((((uint64_t)1) << mbits) - 1);

  f80 r;
  if (e == 0) {
    if (m == 0) return f80_make_zero(s);
    c.flags |= F80_DE;
    int k = 63 - f80_clz64(m);
    r.sig = m << (63 - k);
    r.se  = (uint16_t)((s ? 0x8000 : 0) | (uint16_t)(k + 1 - bias - mbits + 16383));
    return r;
  }
  if (e == emax) {
    if (m == 0) return f80_make_inf(s);
    if ((m & (((uint64_t)1) << (mbits - 1))) == 0) c.flags |= F80_IE;   // SNaN
    r.sig = (1ULL << 63) | (m << (63 - mbits)) | (1ULL << 62);
    r.se  = s ? 0xFFFF : 0x7FFF;
    return r;
  }
  r.sig = (1ULL << 63) | (m << (63 - mbits));
  r.se  = (uint16_t)((s ? 0x8000 : 0) | (uint16_t)((int)e - bias + 16383));
  return r;
}

// Narrowing rounds, and under the guest's RC rather than the host's.  The old
// implementation stored `(float)val`, which followed whatever rounding mode the
// HOST was in - a control word the guest cannot see and never set.
static inline uint64_t f80_to_ieee(f80 a, int mbits, int ebits, f80_ctx &c) {
  int bias = (1 << (ebits - 1)) - 1;
  uint64_t emax = (((uint64_t)1) << ebits) - 1;
  uint64_t fmask = (((uint64_t)1) << mbits) - 1;
  uint64_t sbit = ((uint64_t)(f80_neg(a) ? 1 : 0)) << (ebits + mbits);

  f80_class k = f80_classify(a);
  if (k == F80_CLASS_UNSUPPORTED) {
    c.flags |= F80_IE;
    return (((uint64_t)1) << (ebits + mbits)) | (emax << mbits)
         | (((uint64_t)1) << (mbits - 1));
  }
  if (f80_is_nan(k)) {
    if (k == F80_CLASS_SNAN) c.flags |= F80_IE;
    uint64_t payload = ((a.sig << 1) >> (64 - mbits)) | (((uint64_t)1) << (mbits - 1));
    return sbit | (emax << mbits) | payload;
  }
  if (k == F80_CLASS_INF)  return sbit | (emax << mbits);
  if (k == F80_CLASS_ZERO) { c.c1 = false; return sbit; }
  // No #D here.  Storing is not an arithmetic operation, and hardware reports
  // only the underflow and precision the narrower destination causes.

  f80_wide w = f80_unpack(a);
  int prec = mbits + 1;
  int dshift = (w.exp < 1 - bias) ? (int)(1 - bias - w.exp) : 0;
  if (dshift > 4000) dshift = 4000;
  unsigned __int128 keep;
  bool inexact, inc;
  f80_round_sig(w.sig, false, prec, dshift, c.rc(), w.sign, &keep, &inexact, &inc);

  if (dshift == 0) {
    int32_t E = w.exp;
    if ((keep >> prec) != 0) { keep >>= 1; E += 1; }
    if (E > bias) {
      bool to_inf;
      switch (c.rc()) {
        case 0:  to_inf = true;    break;
        case 1:  to_inf = w.sign;  break;
        case 2:  to_inf = !w.sign; break;
        default: to_inf = false;   break;
      }
      c.flags |= F80_OE | F80_PE;
      c.c1 = to_inf;
      return to_inf ? (sbit | (emax << mbits))
                    : (sbit | ((emax - 1) << mbits) | fmask);
    }
    if (inexact) c.flags |= F80_PE;
    c.c1 = inc;
    return sbit | (((uint64_t)(E + bias)) << mbits) | ((uint64_t)keep & fmask);
  }

  uint64_t m = (uint64_t)keep;
  bool normal_now = (m >> mbits) != 0;      // rounded all the way up to 2^(1-bias)
  if (inexact) {
    c.flags |= F80_PE;
    if (!normal_now) c.flags |= F80_UE;
  }
  c.c1 = inc;
  return sbit | ((normal_now ? (uint64_t)1 : (uint64_t)0) << mbits) | (m & fmask);
}

static inline f80 f80_from_f32(uint32_t bits, f80_ctx &c) {
  return f80_from_ieee(bits, 23, 8, c);
}
static inline f80 f80_from_f64(uint64_t bits, f80_ctx &c) {
  return f80_from_ieee(bits, 52, 11, c);
}
static inline uint32_t f80_to_f32(f80 a, f80_ctx &c) {
  return (uint32_t)f80_to_ieee(a, 23, 8, c);
}
static inline uint64_t f80_to_f64(f80 a, f80_ctx &c) {
  return f80_to_ieee(a, 52, 11, c);
}

//===========================================================================
// Conversions to and from the binary integer formats
//===========================================================================

// FIST/FISTP round; FISTTP truncates.  Out of range, or a NaN or an infinity,
// is an INVALID OPERATION and delivers the integer indefinite - the most
// negative value of the destination width - not an undefined C++ cast.
static inline int64_t f80_to_int(f80 a, int bits, bool truncate, f80_ctx &c) {
  int64_t indef = (int64_t)(~(uint64_t)0 << (bits - 1));
  f80_class k = f80_classify(a);
  if (k == F80_CLASS_UNSUPPORTED || f80_is_nan(k) || k == F80_CLASS_INF) {
    c.flags |= F80_IE;
    c.c1 = false;
    return indef;
  }
  if (k == F80_CLASS_ZERO) { c.c1 = false; return 0; }

  f80_wide w = f80_unpack(a);
  if (w.exp >= 64) { c.flags |= F80_IE; c.c1 = false; return indef; }

  // Rounding to an integer is rounding to exp+1 significant bits; a negative
  // width is exactly the |value| < 1 case and f80_round_sig handles it.
  unsigned __int128 keep;
  bool inexact, inc;
  f80_round_sig(w.sig, false, (int)(w.exp + 1), 0, truncate ? 3 : c.rc(),
                w.sign, &keep, &inexact, &inc);

  unsigned __int128 limit = ((unsigned __int128)1) << (bits - 1);
  if (w.sign ? (keep > limit) : (keep >= limit)) {
    c.flags |= F80_IE;                    // #IA suppresses #P: one result, one flag
    c.c1 = false;
    return indef;
  }
  if (inexact) c.flags |= F80_PE;
  c.c1 = inc;
  // Negate in UNSIGNED.  The most negative value of the destination width is
  // in range and reachable - FISTP m64int of -2^63 is a perfectly ordinary
  // instruction - and negating it as a signed int64 is undefined.  UBSan
  // caught this; the oracle did not, because the value that came out was
  // right on this compiler.
  uint64_t mag = (uint64_t)keep;
  return (int64_t)(w.sign ? (~mag + 1u) : mag);
}

//===========================================================================
// Packed BCD (FBLD / FBSTP)
//===========================================================================

// 18 decimal digits in bytes 0..8, two per byte low nibble first, with the
// sign in bit 7 of byte 9.  The largest magnitude is 10^18-1, which is under
// 2^60, so the conversion in is exact and needs no rounding at all.
static inline f80 f80_from_bcd(const uint8_t *d) {
  uint64_t v = 0;
  for (int i = 8; i >= 0; i--)
    v = v * 100u + (uint64_t)((d[i] >> 4) & 0x0F) * 10u + (uint64_t)(d[i] & 0x0F);
  f80 r = f80_from_i64((int64_t)v);
  if (d[9] & 0x80) r = f80_chs(r);
  return r;
}

// The packed-decimal indefinite, which is what a masked #IA stores:
// FFFF C000 0000 0000 0000, little-endian across the ten bytes.
static inline void f80_bcd_indefinite(uint8_t *d) {
  for (int i = 0; i < 7; i++) d[i] = 0x00;
  d[7] = 0xC0; d[8] = 0xFF; d[9] = 0xFF;
}

static inline void f80_to_bcd(f80 a, uint8_t *d, f80_ctx &c) {
  f80_class k = f80_classify(a);
  if (k == F80_CLASS_UNSUPPORTED || f80_is_nan(k) || k == F80_CLASS_INF) {
    c.flags |= F80_IE;
    c.c1 = false;
    f80_bcd_indefinite(d);
    return;
  }

  bool sign = f80_neg(a);
  uint64_t v = 0;
  if (k != F80_CLASS_ZERO) {
    f80_wide w = f80_unpack(a);
    if (w.exp >= 64) {
      c.flags |= F80_IE;
      c.c1 = false;
      f80_bcd_indefinite(d);
      return;
    }
    unsigned __int128 keep;
    bool inexact, inc;
    f80_round_sig(w.sig, false, (int)(w.exp + 1), 0, c.rc(), w.sign,
                  &keep, &inexact, &inc);
    if (keep > (unsigned __int128)999999999999999999ULL) {
      c.flags |= F80_IE;
      c.c1 = false;
      f80_bcd_indefinite(d);
      return;
    }
    if (inexact) c.flags |= F80_PE;
    c.c1 = inc;
    v = (uint64_t)keep;
  } else {
    c.c1 = false;
  }
  for (int i = 0; i < 9; i++) {
    uint8_t lo = (uint8_t)(v % 10u); v /= 10u;
    uint8_t hi = (uint8_t)(v % 10u); v /= 10u;
    d[i] = (uint8_t)((hi << 4) | lo);
  }
  d[9] = sign ? 0x80 : 0x00;
}

//===========================================================================
// Constants
//===========================================================================
//
// Every constant in this file was computed by integer arithmetic in
// scripts kept out of the build (Machin's formula for pi, 2*atanh(1/3) for
// ln 2, and the corresponding series for the rest), carried to 800 bits and
// rounded to nearest.  The six FLDxx values that come out of that agree
// bit-for-bit with the ones an 80387's ROM loads, which is the check that the
// generator was right:
//
//   FLD1    3FFF 8000000000000000        FLDLG2  3FFD 9A209A84FBCFF799
//   FLDL2T  4000 D49A784BCD1B8AFE        FLDLN2  3FFE B17217F7D1CF79AC
//   FLDL2E  3FFF B8AA3B295C17F0BC        FLDPI   4000 C90FDAA22168C235
//
// The old implementation pushed log2(10.0), 1.0/log(2.0) and M_PI as host
// doubles, so FLDPI was 0xC90FDAA22168C000 - the low eleven bits gone.

static const f80 F80_ONE  = { 0x8000000000000000ULL, 0x3FFF };   // 1.0
static const f80 F80_L2T  = { 0xD49A784BCD1B8AFEULL, 0x4000 };   // log2(10)
static const f80 F80_L2E  = { 0xB8AA3B295C17F0BCULL, 0x3FFF };   // log2(e)
static const f80 F80_PI   = { 0xC90FDAA22168C235ULL, 0x4000 };   // pi
static const f80 F80_LG2  = { 0x9A209A84FBCFF799ULL, 0x3FFD };   // log10(2)
static const f80 F80_LN2  = { 0xB17217F7D1CF79ACULL, 0x3FFE };   // ln(2)

// The same three constants again, split into a rounded head and the exact
// remainder, which is how the reduction steps below get more than 64 bits out
// of a 64-bit format.
static const f80 F80_LN2_HI  = { 0xB17217F7D1CF79ACULL, 0x3FFE };
static const f80 F80_LN2_LO  = { 0xD871319FF0342543ULL, 0xBFBC };
static const f80 F80_L2E_HI  = { 0xB8AA3B295C17F0BCULL, 0x3FFF };
static const f80 F80_L2E_LO  = { 0x82F0025F2DC582EEULL, 0xBFBE };
static const f80 F80_PIO2_HI = { 0xC90FDAA22168C235ULL, 0x3FFF };
static const f80 F80_PIO2_LO = { 0xECE675D1FC8F8CBBULL, 0xBFBD };

// 2/pi to 192 bits, most significant word first, the first bit being the one
// worth 2^-1.  Used by the trigonometric argument reduction; see f80_trig_reduce.
static const uint64_t F80_2_OVER_PI[3] = {
  0xA2F9836E4E441529ULL, 0xFC2757D1F534DDC0ULL, 0xDB6295993C439041ULL
};

// 1/n! for n = 2..20 -- the tail of exp, used by F2XM1.
static const f80 F80_INV_FACT[19] = {
  { 0x8000000000000000ULL, 0x3FFE },  // 1/2!
  { 0xAAAAAAAAAAAAAAABULL, 0x3FFC },  // 1/3!
  { 0xAAAAAAAAAAAAAAABULL, 0x3FFA },  // 1/4!
  { 0x8888888888888889ULL, 0x3FF8 },  // 1/5!
  { 0xB60B60B60B60B60BULL, 0x3FF5 },  // 1/6!
  { 0xD00D00D00D00D00DULL, 0x3FF2 },  // 1/7!
  { 0xD00D00D00D00D00DULL, 0x3FEF },  // 1/8!
  { 0xB8EF1D2AB6399C7DULL, 0x3FEC },  // 1/9!
  { 0x93F27DBBC4FAE397ULL, 0x3FE9 },  // 1/10!
  { 0xD7322B3FAA271C7FULL, 0x3FE5 },  // 1/11!
  { 0x8F76C77FC6C4BDAAULL, 0x3FE2 },  // 1/12!
  { 0xB092309D43684BE5ULL, 0x3FDE },  // 1/13!
  { 0xC9CBA54603E4E906ULL, 0x3FDA },  // 1/14!
  { 0xD73F9F399DC0F88FULL, 0x3FD6 },  // 1/15!
  { 0xD73F9F399DC0F88FULL, 0x3FD2 },  // 1/16!
  { 0xCA963B81856A5359ULL, 0x3FCE },  // 1/17!
  { 0xB413C31DCBECBBDEULL, 0x3FCA },  // 1/18!
  { 0x97A4DA340A0AB926ULL, 0x3FC6 },  // 1/19!
  { 0xF2A15D201011283DULL, 0x3FC1 },  // 1/20!
};

// 1/(2k+1) for k = 0..19 -- atanh(t)/t, used by FYL2X and FYL2XP1.  Fifteen
// terms cover FYL2X's |t| <= 0.1716; twenty cover |t| <= 1/3, which is what
// FYL2XP1 needs to stay accurate across the whole of |x| <= 1 rather than
// only across the |x| < 1 - sqrt(2)/2 the architecture promises.
static const f80 F80_ATANH_C[20] = {
  { 0x8000000000000000ULL, 0x3FFF },  // 1/1
  { 0xAAAAAAAAAAAAAAABULL, 0x3FFD },  // 1/3
  { 0xCCCCCCCCCCCCCCCDULL, 0x3FFC },  // 1/5
  { 0x9249249249249249ULL, 0x3FFC },  // 1/7
  { 0xE38E38E38E38E38EULL, 0x3FFB },  // 1/9
  { 0xBA2E8BA2E8BA2E8CULL, 0x3FFB },  // 1/11
  { 0x9D89D89D89D89D8AULL, 0x3FFB },  // 1/13
  { 0x8888888888888889ULL, 0x3FFB },  // 1/15
  { 0xF0F0F0F0F0F0F0F1ULL, 0x3FFA },  // 1/17
  { 0xD79435E50D79435EULL, 0x3FFA },  // 1/19
  { 0xC30C30C30C30C30CULL, 0x3FFA },  // 1/21
  { 0xB21642C8590B2164ULL, 0x3FFA },  // 1/23
  { 0xA3D70A3D70A3D70AULL, 0x3FFA },  // 1/25
  { 0x97B425ED097B425FULL, 0x3FFA },  // 1/27
  { 0x8D3DCB08D3DCB08DULL, 0x3FFA },  // 1/29
  { 0x8421084210842108ULL, 0x3FFA },  // 1/31
  { 0xF83E0F83E0F83E10ULL, 0x3FF9 },  // 1/33
  { 0xEA0EA0EA0EA0EA0FULL, 0x3FF9 },  // 1/35
  { 0xDD67C8A60DD67C8AULL, 0x3FF9 },  // 1/37
  { 0xD20D20D20D20D20DULL, 0x3FF9 },  // 1/39
};

// (-1)^k/(2k+1) for k = 0..8 -- atan(t)/t, used by FPATAN.
static const f80 F80_ATAN_C[9] = {
  { 0x8000000000000000ULL, 0x3FFF },  // +1/1
  { 0xAAAAAAAAAAAAAAABULL, 0xBFFD },  // -1/3
  { 0xCCCCCCCCCCCCCCCDULL, 0x3FFC },  // +1/5
  { 0x9249249249249249ULL, 0xBFFC },  // -1/7
  { 0xE38E38E38E38E38EULL, 0x3FFB },  // +1/9
  { 0xBA2E8BA2E8BA2E8CULL, 0xBFFB },  // -1/11
  { 0x9D89D89D89D89D8AULL, 0x3FFB },  // +1/13
  { 0x8888888888888889ULL, 0xBFFB },  // -1/15
  { 0xF0F0F0F0F0F0F0F1ULL, 0x3FFA },  // +1/17
};

// (-1)^k/(2k+1)! for k = 0..10 -- sin(r)/r.
static const f80 F80_SIN_C[11] = {
  { 0x8000000000000000ULL, 0x3FFF },  // +1/1!
  { 0xAAAAAAAAAAAAAAABULL, 0xBFFC },  // -1/3!
  { 0x8888888888888889ULL, 0x3FF8 },  // +1/5!
  { 0xD00D00D00D00D00DULL, 0xBFF2 },  // -1/7!
  { 0xB8EF1D2AB6399C7DULL, 0x3FEC },  // +1/9!
  { 0xD7322B3FAA271C7FULL, 0xBFE5 },  // -1/11!
  { 0xB092309D43684BE5ULL, 0x3FDE },  // +1/13!
  { 0xD73F9F399DC0F88FULL, 0xBFD6 },  // -1/15!
  { 0xCA963B81856A5359ULL, 0x3FCE },  // +1/17!
  { 0x97A4DA340A0AB926ULL, 0xBFC6 },  // -1/19!
  { 0xB8DC77B6E7AB8C5FULL, 0x3FBD },  // +1/21!
};

// (-1)^k/(2k)! for k = 0..11 -- cos(r).
static const f80 F80_COS_C[12] = {
  { 0x8000000000000000ULL, 0x3FFF },  // +1/0!
  { 0x8000000000000000ULL, 0xBFFE },  // -1/2!
  { 0xAAAAAAAAAAAAAAABULL, 0x3FFA },  // +1/4!
  { 0xB60B60B60B60B60BULL, 0xBFF5 },  // -1/6!
  { 0xD00D00D00D00D00DULL, 0x3FEF },  // +1/8!
  { 0x93F27DBBC4FAE397ULL, 0xBFE9 },  // -1/10!
  { 0x8F76C77FC6C4BDAAULL, 0x3FE2 },  // +1/12!
  { 0xC9CBA54603E4E906ULL, 0xBFDA },  // -1/14!
  { 0xD73F9F399DC0F88FULL, 0x3FD2 },  // +1/16!
  { 0xB413C31DCBECBBDEULL, 0xBFCA },  // -1/18!
  { 0xF2A15D201011283DULL, 0x3FC1 },  // +1/20!
  { 0x8671CB6DBFC294A3ULL, 0xBFB9 },  // -1/22!
};

// atan(k/16) for k = 0..16, the FPATAN reduction table.
static const f80 F80_ATAN_TBL[17] = {
  { 0x0000000000000000ULL, 0x0000 },  // atan(0/16) = 0
  { 0xFFAADDB967EF4E37ULL, 0x3FFA },  // atan(1/16)
  { 0xFEADD4D5617B6E33ULL, 0x3FFB },  // atan(2/16)
  { 0xBDCBDA5E72D81134ULL, 0x3FFC },  // atan(3/16)
  { 0xFADBAFC96406EB15ULL, 0x3FFC },  // atan(4/16)
  { 0x9B13B9B83F5E5E6AULL, 0x3FFD },  // atan(5/16)
  { 0xB7B0CA0F26F78474ULL, 0x3FFD },  // atan(6/16)
  { 0xD327761E611FE5B6ULL, 0x3FFD },  // atan(7/16)
  { 0xED63382B0DDA7B45ULL, 0x3FFD },  // atan(8/16)
  { 0x832BF4A6D9867E2AULL, 0x3FFE },  // atan(9/16)
  { 0x8F005D5EF7F59F9BULL, 0x3FFE },  // atan(10/16)
  { 0x9A2F80E671BDDA20ULL, 0x3FFE },  // atan(11/16)
  { 0xA4BC7D1934F70924ULL, 0x3FFE },  // atan(12/16)
  { 0xAEAC4C38B4D8C080ULL, 0x3FFE },  // atan(13/16)
  { 0xB8053E2BC2319E74ULL, 0x3FFE },  // atan(14/16)
  { 0xC0CE85B8AC526641ULL, 0x3FFE },  // atan(15/16)
  { 0xC90FDAA22168C235ULL, 0x3FFE },  // atan(16/16)
};

//===========================================================================
// Transcendentals
//===========================================================================
//
// WHAT ACCURACY THIS AIMS AT, AND WHY
// -----------------------------------
// An 80387's transcendental instructions are not correctly rounded; Intel
// specifies them to within about 1 ulp of the 64-bit significand, and that is
// the target here.  The old implementation called the host's double-precision
// libm - pow(2,x)-1 for F2XM1, log2(x+1) for FYL2XP1 - so it delivered 53
// bits where the instruction exists to deliver 64, and for F2XM1 and FYL2XP1
// it destroyed exactly the precision those two encodings were added to keep.
//
// Everything below is evaluated in this file's own arithmetic, so it is
// bit-identical on x86-64 and ARM64.  The pattern is always the same: reduce
// the argument using a constant carried to 128 bits as a head/tail pair, then
// evaluate a polynomial whose degree was chosen so the truncation error is
// below 2^-70 over the reduced range.  tests/f80_unit.cc measures the result
// against the host's long-double libm and holds it to a recorded ulp bound.

// Internal arithmetic: round to nearest at full 64-bit precision, and throw
// the flags away.  Precision control does not apply to a transcendental's
// intermediate steps, and the caller decides what the instruction reports.
static inline f80 f80x_add(f80 a, f80 b) { f80_ctx t = f80_ctx_make(0x037F); return f80_add(a, b, t); }
static inline f80 f80x_sub(f80 a, f80 b) { f80_ctx t = f80_ctx_make(0x037F); return f80_sub(a, b, t); }
static inline f80 f80x_mul(f80 a, f80 b) { f80_ctx t = f80_ctx_make(0x037F); return f80_mul(a, b, t); }
static inline f80 f80x_div(f80 a, f80 b) { f80_ctx t = f80_ctx_make(0x037F); return f80_div(a, b, t); }

// Build an f80 from a 64-bit integer `m` whose bit 63 has weight 2^exp63.
// `m` need not be normalised; zero is allowed.
//
// The fast path here is exact and is what every ordinary call takes.  The slow
// one is not decoration: without it, a value that lands below the smallest
// normal gets a biased exponent computed as a negative number and truncated
// into a uint16, which produces a pseudo-denormal worth twice the right
// answer.  F2XM1 of 2^-16382 came out at exactly 2x until this branch existed,
// and the oracle only found it once the transcendentals were fed operands from
// the extremes of the exponent range rather than the middle.
static inline f80 f80_from_sig(bool neg, uint64_t m, int32_t exp63) {
  if (m == 0) return f80_make_zero(neg);
  int z = f80_clz64(m);
  int32_t e = exp63 - z;
  if (e >= -16382 && e <= 16383) {
    f80 r;
    r.sig = m << z;
    r.se  = (uint16_t)((neg ? 0x8000 : 0) | (uint16_t)(e + 16383));
    return r;
  }
  f80_ctx t = f80_ctx_make(0x037F);
  return f80_round_pack(neg, e, ((unsigned __int128)(m << z)) << 64, false, 64, t);
}

// a*b as a head and an exact tail: *hi is the rounded product and *lo is
// (a*b - *hi) exactly, which is representable because the product of two
// 64-bit significands is 128 bits and the head takes the top 64 of them.
// Only used on ordinary finite normals, which is all the callers below have.
static inline void f80_mul2(f80 a, f80 b, f80 *hi, f80 *lo) {
  f80_class ka = f80_classify(a), kb = f80_classify(b);
  if (ka != F80_CLASS_NORMAL || kb != F80_CLASS_NORMAL) {
    *hi = f80x_mul(a, b);
    *lo = f80_make_zero(false);
    return;
  }
  bool s = f80_neg(a) != f80_neg(b);
  f80_wide wa = f80_unpack(a), wb = f80_unpack(b);
  uint64_t ma = (uint64_t)(wa.sig >> 64), mb = (uint64_t)(wb.sig >> 64);
  unsigned __int128 p = (unsigned __int128)ma * mb;
  int32_t e;
  if ((p >> 127) != 0) e = wa.exp + wb.exp + 1;
  else                 { p <<= 1; e = wa.exp + wb.exp; }
  // p = the exact significand, bit 127 set, value = p * 2^(e-127).
  uint64_t head = (uint64_t)(p >> 64);
  uint64_t tail = (uint64_t)p;
  bool up = (tail > (1ULL << 63)) || (tail == (1ULL << 63) && (head & 1));
  if (up) {
    head += 1;
    if (head == 0) { head = 1ULL << 63; e += 1; tail = (uint64_t)(tail >> 1); }
  }
  *hi = f80_from_sig(s, head, e);
  // The tail is what the head threw away, signed by the rounding direction.
  if (up) {
    uint64_t back = (uint64_t)0 - tail;              // 2^64 - tail
    *lo = f80_from_sig(!s, back, e - 64);
  } else {
    *lo = f80_from_sig(s, tail, e - 64);
  }
}

// a + b as a head and an exact tail (Knuth's 2Sum).  Exact for any two
// finite f80s, which is what makes the double-length reductions below work.
static inline void f80_add2(f80 a, f80 b, f80 *hi, f80 *lo) {
  f80 s  = f80x_add(a, b);
  f80 bp = f80x_sub(s, a);
  *lo = f80x_add(f80x_sub(a, f80x_sub(s, bp)), f80x_sub(b, bp));
  *hi = s;
}

// n / (dh + dl) to about 128 bits.  The residual n - q*(dh+dl) is formed
// exactly - q*dh through f80_mul2, and n - (that) by Sterbenz, since the two
// are within a factor of two - and divided again for the tail.
static inline void f80_div2(f80 n, f80 dh, f80 dl, f80 *hi, f80 *lo) {
  f80 q = f80x_div(n, dh);
  f80 p, r;
  f80_mul2(q, dh, &p, &r);
  f80 res = f80x_sub(f80x_sub(f80x_sub(n, p), r), f80x_mul(q, dl));
  *hi = q;
  *lo = f80x_div(res, dh);
}

// Horner over a coefficient table, in x^2, with the LEADING term kept out of
// the recurrence.  Every table below starts at +-1 or +-1/2, and folding that
// into the loop would round it together with a correction eighty bits smaller;
// adding it once at the end instead keeps it exact and takes about a ulp off
// every function in this section.
static inline f80 f80_poly_even(const f80 *c, int n, f80 x2) {
  f80 r = c[n - 1];
  for (int i = n - 2; i >= 1; i--) r = f80x_add(f80x_mul(r, x2), c[i]);
  return f80x_add(c[0], f80x_mul(r, x2));
}
// The same shape in x rather than x^2.
static inline f80 f80_poly_odd(const f80 *c, int n, f80 x) {
  f80 r = c[n - 1];
  for (int i = n - 2; i >= 1; i--) r = f80x_add(f80x_mul(r, x), c[i]);
  return f80x_add(c[0], f80x_mul(r, x));
}

//---------------------------------------------------------------------------
// F2XM1: 2^x - 1, for x in [-1, 1]
//---------------------------------------------------------------------------
//
// 2^x - 1 = expm1(u) with u = x*ln2, and expm1(u) = u + u^2*Q(u) with
// Q(u) = sum_{n>=2} u^(n-2)/n!.  Writing it that way keeps the leading term
// exact: an error in Q is scaled by u^2 and cannot move the result by more
// than |u|/2 of an ulp.  u itself is carried as a head/tail pair, because a
// relative error there passes straight through to the answer.
static inline f80 f80_2xm1(f80 x, f80_ctx &c);
static inline f80 f80_2xm1(f80 x, f80_ctx &c) {
  f80 r;
  if (f80_prop_nan1(x, c, &r)) return r;
  f80_class k = f80_classify(x);
  if (k == F80_CLASS_ZERO) { c.c1 = false; return x; }          // 2^0-1 = 0, exact
  if (k == F80_CLASS_INF)  return f80_neg(x) ? f80_chs(F80_ONE) : x;
  if (k == F80_CLASS_DENORMAL) c.flags |= F80_DE;

  f80_wide w = f80_unpack(x);
  if (w.exp >= 0) {
    // |x| >= 1 is outside the architectural domain, where a 387's result is
    // undefined.  Rather than return the garbage a truncated series gives,
    // reduce: 2^x = 2^n * 2^f with f in [-1/2, 1/2] and n an integer, so
    // 2^x - 1 = 2^n * (1 + (2^f - 1)) - 1.  x - n is exact.
    f80_ctx t = f80_ctx_make(0x037F);
    f80 n = f80_rndint(x, t);
    f80 f = f80x_sub(x, n);
    f80_ctx inner = f80_ctx_make(0x037F);
    f80 e = f80_2xm1(f, inner);
    f80 p = f80x_add(F80_ONE, e);
    f80 sc = f80_scale(p, n, c);
    c.flags |= F80_PE;
    return f80_sub(sc, F80_ONE, c);
  }
  // For |x| below 2^-66 the whole series past the first term is under half an
  // ulp of x*ln2, so the answer IS x*ln2 to the last bit.
  f80 uh, ul, t1, t2;
  f80_mul2(x, F80_LN2_HI, &uh, &t1);
  t2 = f80x_mul(x, F80_LN2_LO);
  ul = f80x_add(t1, t2);
  if (w.exp < -66) {
    c.flags |= F80_PE;
    return f80x_add(uh, ul);
  }
  f80 u  = f80x_add(uh, ul);
  f80 q  = f80_poly_odd(F80_INV_FACT, 19, u);
  f80 corr = f80x_mul(f80x_mul(u, u), q);
  c.flags |= F80_PE;
  return f80x_add(uh, f80x_add(ul, corr));
}

//---------------------------------------------------------------------------
// log2, shared by FYL2X and FYL2XP1
//---------------------------------------------------------------------------
//
// log2(m) for m in [sqrt(1/2), sqrt(2)) as 2*atanh(t)*log2(e), t = (m-1)/(m+1).
// |t| <= 0.1716 there, so fifteen odd terms put the truncation error below
// 2^-72 - and the form is relative-accurate as m approaches 1, which is the
// whole reason FYL2XP1 exists as a separate encoding.
// t arrives as a head and a tail, because a relative error there lands on the
// answer one for one - it was the whole of the 3-ulp error this had before the
// argument was carried in double length.
static inline f80 f80_log2_atanh(f80 th, f80 tl) {
  f80 t2 = f80x_mul(th, th);
  // atanh(t)/t - 1, so the leading t stays exactly what it was handed.
  f80 s  = f80x_sub(f80_poly_even(F80_ATANH_C, 20, t2), F80_ONE);
  f80 ah, al;
  f80_mul2(th, s, &ah, &al);                      // the correction term, t*(...)
  // atanh = (th + tl) + th*s
  f80 hi, lo;
  f80_add2(th, ah, &hi, &lo);
  f80 tail = f80x_add(f80x_add(lo, al), tl);
  // times log2(e), again in two pieces, then times two, which is exact.
  f80 ph, pl;
  f80_mul2(hi, F80_L2E_HI, &ph, &pl);
  f80 acc = f80x_add(f80x_add(pl, f80x_mul(hi, F80_L2E_LO)),
                     f80x_mul(tail, F80_L2E_HI));
  f80 r = f80x_add(ph, acc);
  return f80x_add(r, r);
}

static inline f80 f80_yl2x(f80 y, f80 x, f80_ctx &c) {
  f80 r;
  if (f80_prop_nan2(y, x, c, &r)) return r;
  f80_class kx = f80_classify(x), ky = f80_classify(y);
  if (f80_neg(x) && kx != F80_CLASS_ZERO) { c.flags |= F80_IE; return f80_indefinite(); }
  if (kx == F80_CLASS_ZERO) {
    // log2(0) is -inf, so the answer is an infinity signed by y - unless y is
    // zero too, which is 0 * inf and invalid.
    if (ky == F80_CLASS_ZERO) { c.flags |= F80_IE; return f80_indefinite(); }
    c.flags |= F80_ZE;
    return f80_make_inf(!f80_neg(y));
  }
  if (kx == F80_CLASS_INF) {
    if (ky == F80_CLASS_ZERO) { c.flags |= F80_IE; return f80_indefinite(); }
    return f80_make_inf(f80_neg(y));
  }
  if (ky == F80_CLASS_ZERO) { c.c1 = false; return f80_make_zero(f80_neg(y)); }
  if (kx == F80_CLASS_DENORMAL || ky == F80_CLASS_DENORMAL) c.flags |= F80_DE;

  f80_wide w = f80_unpack(x);
  int32_t e = w.exp;
  f80 m;
  m.sig = (uint64_t)(w.sig >> 64);
  m.se  = 0x3FFF;                                  // m in [1,2)
  // Move to [sqrt(1/2), sqrt(2)) so the series argument stays small.
  if (m.sig > 0xB504F333F9DE6484ULL) { m.se = 0x3FFE; e += 1; }
  // m - 1 is exact by Sterbenz for every m in [1/2, 2); m + 1 is not, so it is
  // carried as a pair and the division with it.
  f80 dh, dl, th, tl;
  f80_add2(m, F80_ONE, &dh, &dl);
  f80_div2(f80x_sub(m, F80_ONE), dh, dl, &th, &tl);
  f80 l = f80_log2_atanh(th, tl);
  f80 total = f80x_add(f80_from_i32(e), l);
  c.flags |= F80_PE;
  return f80x_mul(y, total);
}

static inline f80 f80_yl2xp1(f80 y, f80 x, f80_ctx &c) {
  f80 r;
  if (f80_prop_nan2(y, x, c, &r)) return r;
  f80_class kx = f80_classify(x), ky = f80_classify(y);
  if (kx == F80_CLASS_ZERO) { c.c1 = false; return f80_make_zero(f80_neg(x) != f80_neg(y)); }
  if (kx == F80_CLASS_INF) {
    if (f80_neg(x)) { c.flags |= F80_IE; return f80_indefinite(); }
    if (ky == F80_CLASS_ZERO) { c.flags |= F80_IE; return f80_indefinite(); }
    return f80_make_inf(f80_neg(y));
  }
  if (ky == F80_CLASS_ZERO) { c.c1 = false; return f80_make_zero(f80_neg(x) != f80_neg(y)); }
  if (kx == F80_CLASS_DENORMAL || ky == F80_CLASS_DENORMAL) c.flags |= F80_DE;

  // Past |x| = 1 the reduced argument leaves the series' range, and the
  // architecture calls the result undefined there anyway.  Falling back on
  // log2(1+x) costs the half-ulp that forming 1+x rounds away - which is
  // exactly the precision this encoding exists to avoid, and exactly why the
  // fallback is confined to arguments the encoding was never for.
  if ((int)(x.se & 0x7FFF) - 16383 >= 0)
    return f80_yl2x(y, f80x_add(F80_ONE, x), c);

  // t = x/(2+x) gives atanh(t) = ln(1+x)/2 with no cancellation at all, which
  // is what keeps small x relative-accurate.
  f80 two = F80_ONE; two.se = (uint16_t)(two.se + 1);
  f80 dh, dl, th, tl;
  f80_add2(x, two, &dh, &dl);
  f80_div2(x, dh, dl, &th, &tl);
  f80 l = f80_log2_atanh(th, tl);
  c.flags |= F80_PE;
  return f80x_mul(y, l);
}

//---------------------------------------------------------------------------
// FPATAN: atan2(y, x)
//---------------------------------------------------------------------------

// atan(z) for z in [0,1], via a sixteenth table: with z0 = k/16 the residual
// t = (z-z0)/(1+z*z0) has |t| <= 1/32, where nine odd terms are below 2^-70.
static inline f80 f80_atan_unit(f80 z) {
  f80 sixteen = f80_from_i32(16);
  f80 zs = f80x_mul(z, sixteen);
  f80_ctx rt = f80_ctx_make(0x037F);
  int k = (int)f80_to_int(f80_rndint(zs, rt), 32, false, rt);
  if (k < 0)  k = 0;
  if (k > 16) k = 16;
  f80 z0 = f80x_div(f80_from_i32(k), sixteen);
  f80 t  = f80x_div(f80x_sub(z, z0), f80x_add(F80_ONE, f80x_mul(z, z0)));
  f80 t2 = f80x_mul(t, t);
  f80 s  = f80_poly_even(F80_ATAN_C, 9, t2);
  return f80x_add(F80_ATAN_TBL[k], f80x_mul(t, s));
}

static inline f80 f80_patan(f80 y, f80 x, f80_ctx &c) {
  f80 r;
  if (f80_prop_nan2(y, x, c, &r)) return r;
  f80_class kx = f80_classify(x), ky = f80_classify(y);
  bool sy = f80_neg(y), sx = f80_neg(x);
  if (kx == F80_CLASS_DENORMAL || ky == F80_CLASS_DENORMAL) c.flags |= F80_DE;

  f80 pi = F80_PI;
  f80 pio2 = F80_PIO2_HI;
  f80 pio4 = pio2; pio4.se = (uint16_t)(pio4.se - 1);

  // The eight degenerate quadrants, which are all exact multiples of pi/4.
  if (kx == F80_CLASS_INF && ky == F80_CLASS_INF) {
    f80 v = sx ? f80x_add(pio2, pio4) : pio4;      // 3pi/4 or pi/4
    c.flags |= F80_PE;
    return sy ? f80_chs(v) : v;
  }
  if (ky == F80_CLASS_ZERO) {
    if (!sx && kx != F80_CLASS_ZERO) { c.c1 = false; return f80_make_zero(sy); }
    if (sx) { c.flags |= F80_PE; return sy ? f80_chs(pi) : pi; }
    // x is a zero too: atan2(+-0, +-0) is still defined by the signs.
    if (f80_neg(x)) { c.flags |= F80_PE; return sy ? f80_chs(pi) : pi; }
    c.c1 = false;
    return f80_make_zero(sy);
  }
  if (kx == F80_CLASS_ZERO || ky == F80_CLASS_INF) {
    if (kx == F80_CLASS_INF) {                     // |y| finite, |x| infinite
      if (sx) { c.flags |= F80_PE; return sy ? f80_chs(pi) : pi; }
      c.c1 = false;
      return f80_make_zero(sy);
    }
    c.flags |= F80_PE;
    return sy ? f80_chs(pio2) : pio2;
  }
  if (kx == F80_CLASS_INF) {
    if (sx) { c.flags |= F80_PE; return sy ? f80_chs(pi) : pi; }
    c.c1 = false;
    return f80_make_zero(sy);
  }

  f80 ay = f80_abs(y), ax = f80_abs(x);
  f80 a;
  if (f80_compare(ay, ax, true, c) != F80_CMP_GT) {
    a = f80_atan_unit(f80x_div(ay, ax));
  } else {
    a = f80x_sub(pio2, f80_atan_unit(f80x_div(ax, ay)));
  }
  if (sx) a = f80x_sub(pi, a);
  c.flags |= F80_PE;
  return sy ? f80_chs(a) : a;
}

//---------------------------------------------------------------------------
// Trigonometric argument reduction
//---------------------------------------------------------------------------
//
// r = x - n*(pi/2) with n the nearest integer to x*(2/pi), computed the only
// way that stays accurate for a large x: multiply the significand by a window
// of the binary expansion of 2/pi and keep the fraction.  Rounding x*(2/pi)
// in 64-bit arithmetic instead would leave nothing of r for arguments near
// 2^63, which is the well-known way this goes wrong.
//
// Reduction is only reached for |x| >= pi/4; below that n is 0 and r is x.
// With the exponent of x at most 62, the 192 table bits used here put the
// error in x*(2/pi) below 2^-129, so r is good to about 2^-128 absolute.
struct f80_trig_r {
  f80 hi, lo;      // r = hi + lo, |r| <= pi/4
  int quadrant;    // n mod 4
};

static inline void f80_trig_reduce(f80 x, f80_trig_r *out) {
  f80_wide w = f80_unpack(x);
  if (w.exp < -1) {                       // |x| < 1 <= pi/2, so certainly < pi/4
    out->hi = f80_abs(x);
    out->lo = f80_make_zero(false);
    out->quadrant = 0;
    return;
  }
  uint64_t m = (uint64_t)(w.sig >> 64);
  // |x| = m * 2^s.  The early return above bounds w.exp below at -1, and the
  // caller's range check bounds it above at 62, so s is in [-64, -1] - which
  // is what makes every shift below in range: `point` lands in [193, 256],
  // `hb` in [1, 64] and `sh` in [0, 63].  Widening either bound without
  // revisiting those three is how this stops being true.
  int32_t s = w.exp - 63;

  // 256-bit product of the 64-bit significand with the 192-bit window.
  unsigned __int128 p2 = (unsigned __int128)m * F80_2_OVER_PI[2];
  unsigned __int128 p1 = (unsigned __int128)m * F80_2_OVER_PI[1];
  unsigned __int128 p0 = (unsigned __int128)m * F80_2_OVER_PI[0];
  uint64_t v[4];
  unsigned __int128 t;
  v[0] = (uint64_t)p2;
  t = (unsigned __int128)(uint64_t)(p2 >> 64) + (uint64_t)p1;
  v[1] = (uint64_t)t;
  t = (unsigned __int128)(uint64_t)(p1 >> 64) + (uint64_t)p0 + (uint64_t)(t >> 64);
  v[2] = (uint64_t)t;
  v[3] = (uint64_t)(p0 >> 64) + (uint64_t)(t >> 64);

  // The binary point sits `point` bits up from the bottom of v.
  int point = 192 - s;                    // 193..256
  int hb = point - 192;                   // 1..64, the point's bit within v[3]

  // Round to nearest by adding a half before the split.  The carry out of the
  // top word is real - for |x| just under 1 the product's high word is already
  // above 2^63 - and it is the low bit of n, so it has to be kept.
  uint64_t v4 = 0;
  {
    uint64_t half_hi = ((uint64_t)1) << (hb - 1);
    uint64_t before = v[3];
    v[3] += half_hi;
    if (v[3] < before) v4 = 1;
  }
  uint64_t n = (v4 << (64 - hb)) | ((hb >= 64) ? 0 : (v[3] >> hb));
  out->quadrant = (int)(n & 3u);

  // Fraction: v mod 2^point, then re-centred to [-1/2, 1/2).
  uint64_t topmask = (hb >= 64) ? ~(uint64_t)0 : ((((uint64_t)1) << hb) - 1);
  uint64_t f[4] = { v[0], v[1], v[2], (uint64_t)(v[3] & topmask) };
  // Shift left so the binary point lands exactly at bit 256.
  int sh = 256 - point;                    // 0..63
  if (sh) {
    f[3] = (f[3] << sh) | (f[2] >> (64 - sh));
    f[2] = (f[2] << sh) | (f[1] >> (64 - sh));
    f[1] = (f[1] << sh) | (f[0] >> (64 - sh));
    f[0] = f[0] << sh;
  }
  // Subtract 1/2, which is now bit 255 exactly.
  bool neg;
  uint64_t g[4];
  if (f[3] >= (1ULL << 63)) {
    neg = false;
    g[3] = f[3] - (1ULL << 63); g[2] = f[2]; g[1] = f[1]; g[0] = f[0];
  } else {
    neg = true;
    // 2^255 - f, borrow-propagated
    unsigned __int128 bor = 0;
    uint64_t a0 = 0, a1 = 0, a2 = 0, a3 = (1ULL << 63);
    unsigned __int128 d;
    d = (unsigned __int128)a0 - f[0];              g[0] = (uint64_t)d; bor = (d >> 64) & 1;
    d = (unsigned __int128)a1 - f[1] - (uint64_t)bor; g[1] = (uint64_t)d; bor = (d >> 64) & 1;
    d = (unsigned __int128)a2 - f[2] - (uint64_t)bor; g[2] = (uint64_t)d; bor = (d >> 64) & 1;
    d = (unsigned __int128)a3 - f[3] - (uint64_t)bor; g[3] = (uint64_t)d;
    // n is one larger in magnitude when the fraction went negative... no: the
    // rounding already chose n, and a negative residual is exactly what
    // round-to-nearest produces.  The quadrant stands.
  }

  if ((g[0] | g[1] | g[2] | g[3]) == 0) {
    out->hi = f80_make_zero(false);
    out->lo = f80_make_zero(false);
    return;
  }
  // Normalise so the leading bit is bit 255, then take 128 bits of it.
  int lead = 0;
  while (g[3] == 0) {                      // shift whole words
    g[3] = g[2]; g[2] = g[1]; g[1] = g[0]; g[0] = 0;
    lead += 64;
  }
  int z = f80_clz64(g[3]);
  if (z) {
    g[3] = (g[3] << z) | (g[2] >> (64 - z));
    g[2] = (g[2] << z) | (g[1] >> (64 - z));
    g[1] = (g[1] << z) | (g[0] >> (64 - z));
    g[0] = g[0] << z;
  }
  lead += z;
  // value = (g as a 256-bit fraction of 2^256) * 2^-lead
  f80 ghi = f80_from_sig(neg, g[3], -1 - lead);
  f80 glo = f80_from_sig(neg, g[2], -65 - lead);

  // r = g * (pi/2), carried in two pieces.
  f80 h, l;
  f80_mul2(ghi, F80_PIO2_HI, &h, &l);
  f80 corr = f80x_add(l, f80x_add(f80x_mul(ghi, F80_PIO2_LO),
                                  f80x_mul(glo, F80_PIO2_HI)));
  out->hi = h;
  out->lo = corr;
}

static inline f80 f80_sin_red(f80 rh, f80 rl) {
  f80 r  = f80x_add(rh, rl);
  f80 r2 = f80x_mul(r, r);
  f80 s  = f80_poly_even(F80_SIN_C, 11, r2);      // sin(r)/r
  return f80x_mul(r, s);
}
static inline f80 f80_cos_red(f80 rh, f80 rl) {
  f80 r  = f80x_add(rh, rl);
  f80 r2 = f80x_mul(r, r);
  return f80_poly_even(F80_COS_C, 12, r2);
}

// True when the argument is out of range and the instruction must report C2
// and leave ST(0) alone, which is what a 387 does past 2^63.
static inline bool f80_trig_out_of_range(f80 x) {
  f80_class k = f80_classify(x);
  if (k == F80_CLASS_ZERO || k == F80_CLASS_DENORMAL) return false;
  if (k != F80_CLASS_NORMAL) return false;
  return (int)(x.se & 0x7FFF) - 16383 >= 63;
}

// Each returns false when the argument was out of range, in which case the
// caller sets C2 and leaves the stack alone.
static inline bool f80_sincos(f80 x, f80 *sin_out, f80 *cos_out, f80_ctx &c) {
  f80 r;
  if (f80_prop_nan1(x, c, &r)) { if (sin_out) *sin_out = r; if (cos_out) *cos_out = r; return true; }
  f80_class k = f80_classify(x);
  if (k == F80_CLASS_INF) {
    c.flags |= F80_IE;
    r = f80_indefinite();
    if (sin_out) *sin_out = r;
    if (cos_out) *cos_out = r;
    return true;
  }
  if (f80_trig_out_of_range(x)) return false;
  if (k == F80_CLASS_ZERO) {
    if (sin_out) *sin_out = x;                     // sin(-0) = -0, exact
    if (cos_out) *cos_out = F80_ONE;
    c.c1 = false;
    return true;
  }
  if (k == F80_CLASS_DENORMAL) c.flags |= F80_DE;

  f80_trig_r t;
  f80_trig_reduce(x, &t);
  f80 sr = f80_sin_red(t.hi, t.lo);
  f80 cr = f80_cos_red(t.hi, t.lo);
  bool neg = f80_neg(x);
  f80 sv, cv;
  switch (t.quadrant) {
    case 0:  sv = sr;            cv = cr;            break;
    case 1:  sv = cr;            cv = f80_chs(sr);   break;
    case 2:  sv = f80_chs(sr);   cv = f80_chs(cr);   break;
    default: sv = f80_chs(cr);   cv = sr;            break;
  }
  if (neg) sv = f80_chs(sv);                       // sin is odd, cos is even
  c.flags |= F80_PE;
  if (sin_out) *sin_out = sv;
  if (cos_out) *cos_out = cv;
  return true;
}

static inline bool f80_sin(f80 x, f80 *out, f80_ctx &c)  { return f80_sincos(x, out, 0, c); }
static inline bool f80_cos(f80 x, f80 *out, f80_ctx &c)  { return f80_sincos(x, 0, out, c); }

static inline bool f80_ptan(f80 x, f80 *out, f80_ctx &c) {
  f80 s, co;
  if (!f80_sincos(x, &s, &co, c)) return false;
  *out = f80x_div(s, co);
  return true;
}

#endif  // EMU88_F80_H
