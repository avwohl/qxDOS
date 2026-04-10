// emu88_fpu.cc — x87 FPU emulation using host double precision
//
// Implements the core x87 instruction set needed for DJGPP applications (DOOM).
// Uses host 'double' (64-bit) for FPU registers — sufficient precision for DOS games.

#include "emu88.h"
#include <cmath>
#include <cstring>
#include <cfloat>
#include <climits>

// Status word bits
static constexpr uint16_t SW_IE  = 0x0001;
static constexpr uint16_t SW_DE  = 0x0002;
static constexpr uint16_t SW_ZE  = 0x0004;
static constexpr uint16_t SW_OE  = 0x0008;
static constexpr uint16_t SW_UE  = 0x0010;
static constexpr uint16_t SW_PE  = 0x0020;
static constexpr uint16_t SW_SF  = 0x0040;
static constexpr uint16_t SW_ES  = 0x0080;
static constexpr uint16_t SW_C0  = 0x0100;
static constexpr uint16_t SW_C1  = 0x0200;
static constexpr uint16_t SW_C2  = 0x0400;
static constexpr uint16_t SW_C3  = 0x4000;
static constexpr uint16_t SW_B   = 0x8000;

// Tag values
static constexpr uint8_t TAG_VALID   = 0;
static constexpr uint8_t TAG_ZERO    = 1;
static constexpr uint8_t TAG_SPECIAL = 2;
static constexpr uint8_t TAG_EMPTY   = 3;

// Helpers
#define FPU_TOP       ((fpu.sw >> 11) & 7)
#define FPU_SET_TOP(t) (fpu.sw = (fpu.sw & ~0x3800) | (((t) & 7) << 11))
#define ST(i)         fpu.regs[(FPU_TOP + (i)) & 7]
#define TAG(i)        fpu.tags[(FPU_TOP + (i)) & 7]

static inline uint8_t compute_tag(double val) {
  if (val == 0.0) return TAG_ZERO;
  if (std::isnan(val) || std::isinf(val)) return TAG_SPECIAL;
  return TAG_VALID;
}

//=============================================================================
// FPU initialization
//=============================================================================

void emu88::fpu_init() {
  fpu.cw = 0x037F;  // all exceptions masked, 64-bit precision, round nearest
  fpu.sw = 0;
  for (int i = 0; i < 8; i++) {
    fpu.regs[i] = 0.0;
    fpu.tags[i] = TAG_EMPTY;
  }
}

//=============================================================================
// Stack operations
//=============================================================================

static inline void fpu_push(emu88::FPUState &fpu, double val) {
  int top = (fpu.sw >> 11) & 7;
  top = (top - 1) & 7;
  fpu.sw = (fpu.sw & ~0x3800) | (top << 11);
  fpu.regs[top] = val;
  fpu.tags[top] = compute_tag(val);
}

static inline double fpu_pop(emu88::FPUState &fpu) {
  int top = (fpu.sw >> 11) & 7;
  double val = fpu.regs[top];
  fpu.tags[top] = TAG_EMPTY;
  top = (top + 1) & 7;
  fpu.sw = (fpu.sw & ~0x3800) | (top << 11);
  return val;
}

//=============================================================================
// Memory access helpers for FPU operand types
//=============================================================================

double emu88::fpu_read_m32real(uint16_t seg, uint32_t off) {
  uint32_t raw = fetch_dword(seg, off);
  float f;
  memcpy(&f, &raw, 4);
  return (double)f;
}

double emu88::fpu_read_m64real(uint16_t seg, uint32_t off) {
  uint32_t lo = fetch_dword(seg, off);
  uint32_t hi = fetch_dword(seg, off + 4);
  uint64_t raw = ((uint64_t)hi << 32) | lo;
  double d;
  memcpy(&d, &raw, 8);
  return d;
}

double emu88::fpu_read_m80real(uint16_t seg, uint32_t off) {
  uint64_t mantissa = 0;
  for (int i = 0; i < 8; i++)
    mantissa |= ((uint64_t)fetch_byte(seg, off + i)) << (i * 8);
  uint16_t exp_sign = fetch_word(seg, off + 8);

  bool sign = (exp_sign >> 15) & 1;
  int exp = exp_sign & 0x7FFF;

  if (exp == 0x7FFF) {
    if ((mantissa & 0x7FFFFFFFFFFFFFFFULL) == 0)
      return sign ? -INFINITY : INFINITY;
    return NAN;
  }
  if (exp == 0 && mantissa == 0)
    return sign ? -0.0 : 0.0;

  // value = mantissa * 2^(exp - 16383 - 63)
  double result = ldexp((double)mantissa, exp - 16383 - 63);
  return sign ? -result : result;
}

void emu88::fpu_write_m32real(uint16_t seg, uint32_t off, double val) {
  float f = (float)val;
  uint32_t raw;
  memcpy(&raw, &f, 4);
  store_dword(seg, off, raw);
}

void emu88::fpu_write_m64real(uint16_t seg, uint32_t off, double val) {
  uint64_t raw;
  memcpy(&raw, &val, 8);
  store_dword(seg, off, (uint32_t)raw);
  store_dword(seg, off + 4, (uint32_t)(raw >> 32));
}

void emu88::fpu_write_m80real(uint16_t seg, uint32_t off, double val) {
  uint16_t exp_sign = 0;
  uint64_t mant80 = 0;

  if (val == 0.0) {
    if (std::signbit(val)) exp_sign = 0x8000;
  } else if (std::isinf(val)) {
    exp_sign = 0x7FFF | (val < 0 ? 0x8000 : 0);
    mant80 = 0x8000000000000000ULL;
  } else if (std::isnan(val)) {
    exp_sign = 0x7FFF;
    mant80 = 0xC000000000000000ULL;
  } else {
    bool sign = val < 0;
    if (sign) val = -val;
    // Extract from double: sign(1) + exp(11, bias 1023) + frac(52)
    uint64_t raw;
    memcpy(&raw, &val, 8);
    int dexp = (raw >> 52) & 0x7FF;
    uint64_t dfrac = raw & ((1ULL << 52) - 1);
    // Rebias: 80-bit exp = double_exp - 1023 + 16383
    int exp80 = dexp - 1023 + 16383;
    if (exp80 < 0) exp80 = 0;
    if (exp80 > 0x7FFE) { exp80 = 0x7FFF; mant80 = 0x8000000000000000ULL; }
    else {
      // Set J bit (bit 63) and shift 52-bit fraction to 62:11
      mant80 = 0x8000000000000000ULL | (dfrac << 11);
    }
    exp_sign = exp80 | (sign ? 0x8000 : 0);
  }

  for (int i = 0; i < 8; i++)
    store_byte(seg, off + i, (uint8_t)(mant80 >> (i * 8)));
  store_word(seg, off + 8, exp_sign);
}

//=============================================================================
// Rounding helper
//=============================================================================

double emu88::fpu_round(double val) {
  int rc = (fpu.cw >> 10) & 3;
  switch (rc) {
    case 0: return rint(val);   // round to nearest (even)
    case 1: return floor(val);  // round down
    case 2: return ceil(val);   // round up
    case 3: return trunc(val);  // truncate
  }
  return val;
}

//=============================================================================
// Comparison helper — sets C3,C2,C0
//=============================================================================

void emu88::fpu_compare(double a, double b) {
  fpu.sw &= ~(SW_C0 | SW_C2 | SW_C3);
  if (std::isnan(a) || std::isnan(b)) {
    fpu.sw |= SW_C0 | SW_C2 | SW_C3;  // unordered
  } else if (a > b) {
    // C3=0, C2=0, C0=0 — already cleared
  } else if (a < b) {
    fpu.sw |= SW_C0;  // C3=0, C2=0, C0=1
  } else {
    fpu.sw |= SW_C3;  // C3=1, C2=0, C0=0 — equal
  }
}

//=============================================================================
// Main FPU instruction dispatcher
//=============================================================================

void emu88::execute_fpu(emu88_uint8 opcode) {
  emu88_uint8 modrm_byte = fetch_ip_byte();
  modrm_result mr = decode_modrm(modrm_byte);

  uint8_t esc = opcode - 0xD8;  // 0-7
  uint8_t reg = (modrm_byte >> 3) & 7;
  uint8_t rm  = modrm_byte & 7;
  bool is_mem = !mr.is_register;

  switch (esc) {

  //=========================================================================
  // D8: FADD/FMUL/FCOM/FCOMP/FSUB/FSUBR/FDIV/FDIVR — m32real or ST(i)
  //=========================================================================
  case 0: {
    double val;
    if (is_mem) {
      val = fpu_read_m32real(mr.seg, mr.offset);
    } else {
      val = ST(rm);
    }
    switch (reg) {
      case 0: ST(0) += val; TAG(0) = compute_tag(ST(0)); break;  // FADD
      case 1: ST(0) *= val; TAG(0) = compute_tag(ST(0)); break;  // FMUL
      case 2: fpu_compare(ST(0), val); break;                     // FCOM
      case 3: fpu_compare(ST(0), val); fpu_pop(fpu); break;       // FCOMP
      case 4: ST(0) -= val; TAG(0) = compute_tag(ST(0)); break;  // FSUB
      case 5: ST(0) = val - ST(0); TAG(0) = compute_tag(ST(0)); break; // FSUBR
      case 6: // FDIV
        if (val == 0.0) { fpu.sw |= SW_ZE; ST(0) = std::copysign(INFINITY, ST(0)); }
        else ST(0) /= val;
        TAG(0) = compute_tag(ST(0));
        break;
      case 7: // FDIVR
        if (ST(0) == 0.0) { fpu.sw |= SW_ZE; ST(0) = std::copysign(INFINITY, val); }
        else ST(0) = val / ST(0);
        TAG(0) = compute_tag(ST(0));
        break;
    }
    break;
  }

  //=========================================================================
  // D9: FLD m32real, FST/FSTP m32real, FLDCW, FSTCW, special register ops
  //=========================================================================
  case 1: {
    if (is_mem) {
      switch (reg) {
        case 0: { // FLD m32real
          double val = fpu_read_m32real(mr.seg, mr.offset);
          fpu_push(fpu, val);
          break;
        }
        case 2: // FST m32real
          fpu_write_m32real(mr.seg, mr.offset, ST(0));
          break;
        case 3: // FSTP m32real
          fpu_write_m32real(mr.seg, mr.offset, ST(0));
          fpu_pop(fpu);
          break;
        case 4: // FLDENV (14/28 bytes) — stub
          fpu.cw = fetch_word(mr.seg, mr.offset);
          fpu.sw = fetch_word(mr.seg, mr.offset + (op_size_32 ? 4 : 2));
          break;
        case 5: // FLDCW m16
          fpu.cw = fetch_word(mr.seg, mr.offset);
          break;
        case 6: // FNSTENV — stub: write cw, sw, tw
          store_word(mr.seg, mr.offset, fpu.cw);
          store_word(mr.seg, mr.offset + (op_size_32 ? 4 : 2), fpu.sw);
          break;
        case 7: // FNSTCW m16
          store_word(mr.seg, mr.offset, fpu.cw);
          break;
        default:
          break;
      }
    } else {
      // Register-register: modrm >= 0xC0
      uint8_t op2 = modrm_byte;
      if (op2 >= 0xC0 && op2 <= 0xC7) {
        // FLD ST(i) — push copy of ST(i)
        double val = ST(rm);
        fpu_push(fpu, val);
      } else if (op2 >= 0xC8 && op2 <= 0xCF) {
        // FXCH ST(i)
        double tmp = ST(0);
        ST(0) = ST(rm);
        ST(rm) = tmp;
        uint8_t ttmp = TAG(0);
        TAG(0) = TAG(rm);
        TAG(rm) = ttmp;
      } else switch (op2) {
        case 0xD0: break;  // FNOP
        case 0xE0: ST(0) = -ST(0); TAG(0) = compute_tag(ST(0)); break;  // FCHS
        case 0xE1: ST(0) = fabs(ST(0)); TAG(0) = compute_tag(ST(0)); break;  // FABS
        case 0xE4: fpu_compare(ST(0), 0.0); break;  // FTST
        case 0xE5: { // FXAM
          fpu.sw &= ~(SW_C0 | SW_C1 | SW_C2 | SW_C3);
          if (std::signbit(ST(0))) fpu.sw |= SW_C1;
          if (TAG(0) == TAG_EMPTY) {
            fpu.sw |= SW_C0 | SW_C3;  // empty
          } else if (std::isnan(ST(0))) {
            fpu.sw |= SW_C0;  // NaN
          } else if (std::isinf(ST(0))) {
            fpu.sw |= SW_C0 | SW_C2;  // infinity
          } else if (ST(0) == 0.0) {
            fpu.sw |= SW_C3;  // zero
          } else {
            fpu.sw |= SW_C2;  // normal
          }
          break;
        }
        case 0xE8: fpu_push(fpu, 1.0); break;                    // FLD1
        case 0xE9: fpu_push(fpu, log2(10.0)); break;             // FLDL2T
        case 0xEA: fpu_push(fpu, 1.0 / log(2.0)); break;        // FLDL2E = log2(e)
        case 0xEB: fpu_push(fpu, M_PI); break;                   // FLDPI
        case 0xEC: fpu_push(fpu, log10(2.0)); break;             // FLDLG2
        case 0xED: fpu_push(fpu, log(2.0)); break;               // FLDLN2
        case 0xEE: fpu_push(fpu, 0.0); break;                    // FLDZ
        case 0xF0: // F2XM1: ST(0) = 2^ST(0) - 1
          ST(0) = pow(2.0, ST(0)) - 1.0;
          TAG(0) = compute_tag(ST(0));
          break;
        case 0xF1: // FYL2X: ST(1) = ST(1) * log2(ST(0)), pop
          ST(1) = ST(1) * log2(ST(0));
          TAG(1) = compute_tag(ST(1));
          fpu_pop(fpu);
          break;
        case 0xF2: // FPTAN: ST(0) = tan(ST(0)), push 1.0
          ST(0) = tan(ST(0));
          TAG(0) = compute_tag(ST(0));
          fpu_push(fpu, 1.0);
          fpu.sw &= ~SW_C2;  // reduction complete
          break;
        case 0xF3: // FPATAN: ST(1) = atan2(ST(1), ST(0)), pop
          ST(1) = atan2(ST(1), ST(0));
          TAG(1) = compute_tag(ST(1));
          fpu_pop(fpu);
          break;
        case 0xF4: { // FXTRACT: extract exponent and significand
          int exp;
          double sig = frexp(ST(0), &exp);
          ST(0) = (double)(exp - 1);  // exponent (unbiased)
          TAG(0) = compute_tag(ST(0));
          fpu_push(fpu, ldexp(sig, 1));  // significand in [1,2)
          break;
        }
        case 0xF5: { // FPREM1 (IEEE remainder)
          double q = round(ST(0) / ST(1));
          ST(0) = ST(0) - q * ST(1);
          TAG(0) = compute_tag(ST(0));
          fpu.sw &= ~SW_C2;  // reduction complete
          break;
        }
        case 0xF6: { // FDECSTP
          int top = FPU_TOP;
          FPU_SET_TOP((top - 1) & 7);
          break;
        }
        case 0xF7: { // FINCSTP
          int top = FPU_TOP;
          FPU_SET_TOP((top + 1) & 7);
          break;
        }
        case 0xF8: { // FPREM (8087-compatible remainder)
          if (ST(1) != 0.0) {
            double q = trunc(ST(0) / ST(1));
            ST(0) = ST(0) - q * ST(1);
            TAG(0) = compute_tag(ST(0));
          }
          fpu.sw &= ~SW_C2;  // reduction complete
          break;
        }
        case 0xF9: // FYL2XP1: ST(1) = ST(1) * log2(ST(0) + 1), pop
          ST(1) = ST(1) * log2(ST(0) + 1.0);
          TAG(1) = compute_tag(ST(1));
          fpu_pop(fpu);
          break;
        case 0xFA: // FSQRT
          ST(0) = sqrt(ST(0));
          TAG(0) = compute_tag(ST(0));
          break;
        case 0xFB: { // FSINCOS: push cos, ST(1) = sin (original ST(0))
          double v = ST(0);
          ST(0) = sin(v);
          TAG(0) = compute_tag(ST(0));
          fpu_push(fpu, cos(v));
          fpu.sw &= ~SW_C2;
          break;
        }
        case 0xFC: // FRNDINT
          ST(0) = fpu_round(ST(0));
          TAG(0) = compute_tag(ST(0));
          break;
        case 0xFD: { // FSCALE: ST(0) = ST(0) * 2^trunc(ST(1))
          double exp = trunc(ST(1));
          ST(0) = ldexp(ST(0), (int)exp);
          TAG(0) = compute_tag(ST(0));
          break;
        }
        case 0xFE: // FSIN
          ST(0) = sin(ST(0));
          TAG(0) = compute_tag(ST(0));
          fpu.sw &= ~SW_C2;
          break;
        case 0xFF: // FCOS
          ST(0) = cos(ST(0));
          TAG(0) = compute_tag(ST(0));
          fpu.sw &= ~SW_C2;
          break;
        default:
          fprintf(stderr, "[FPU] unhandled D9 register op: %02X\n", op2);
          break;
      }
    }
    break;
  }

  //=========================================================================
  // DA: m32int arithmetic / FCMOV (register)
  //=========================================================================
  case 2: {
    if (is_mem) {
      double val = (double)(int32_t)fetch_dword(mr.seg, mr.offset);
      switch (reg) {
        case 0: ST(0) += val; break;  // FIADD
        case 1: ST(0) *= val; break;  // FIMUL
        case 2: fpu_compare(ST(0), val); break;  // FICOM
        case 3: fpu_compare(ST(0), val); fpu_pop(fpu); break;  // FICOMP
        case 4: ST(0) -= val; break;  // FISUB
        case 5: ST(0) = val - ST(0); break;  // FISUBR
        case 6: if (val != 0.0) ST(0) /= val; else { fpu.sw |= SW_ZE; ST(0) = INFINITY; } break;
        case 7: if (ST(0) != 0.0) ST(0) = val / ST(0); else { fpu.sw |= SW_ZE; ST(0) = INFINITY; } break;
      }
      if (reg <= 1 || reg >= 4) TAG(0) = compute_tag(ST(0));
    } else {
      // FCMOV — conditional moves based on EFLAGS
      bool cond = false;
      switch (reg) {
        case 0: cond = get_flag(FLAG_CF); break;  // FCMOVB (CF=1)
        case 1: cond = get_flag(FLAG_ZF); break;  // FCMOVE (ZF=1)
        case 2: cond = get_flag(FLAG_CF) || get_flag(FLAG_ZF); break;  // FCMOVBE
        case 3: cond = get_flag(FLAG_PF); break;  // FCMOVU (PF=1)
        default: break;
      }
      if (cond) {
        ST(0) = ST(rm);
        TAG(0) = TAG(rm);
      }
    }
    break;
  }

  //=========================================================================
  // DB: FILD/FIST/FISTP m32int, FLD/FSTP m80real, FINIT, FCLEX, FCOMI
  //=========================================================================
  case 3: {
    if (is_mem) {
      switch (reg) {
        case 0: { // FILD m32int
          int32_t val = (int32_t)fetch_dword(mr.seg, mr.offset);
          fpu_push(fpu, (double)val);
          break;
        }
        case 1: { // FISTTP m32int (SSE3 — round toward zero and pop)
          int32_t val = (int32_t)trunc(ST(0));
          store_dword(mr.seg, mr.offset, (uint32_t)val);
          fpu_pop(fpu);
          break;
        }
        case 2: { // FIST m32int
          int32_t val = (int32_t)fpu_round(ST(0));
          store_dword(mr.seg, mr.offset, (uint32_t)val);
          break;
        }
        case 3: { // FISTP m32int
          int32_t val = (int32_t)fpu_round(ST(0));
          store_dword(mr.seg, mr.offset, (uint32_t)val);
          fpu_pop(fpu);
          break;
        }
        case 5: { // FLD m80real
          double val = fpu_read_m80real(mr.seg, mr.offset);
          fpu_push(fpu, val);
          break;
        }
        case 7: { // FSTP m80real
          fpu_write_m80real(mr.seg, mr.offset, ST(0));
          fpu_pop(fpu);
          break;
        }
        default:
          fprintf(stderr, "[FPU] unhandled DB mem reg=%d\n", reg);
          break;
      }
    } else {
      uint8_t op2 = modrm_byte;
      if (op2 == 0xE2) {
        // FNCLEX — clear exceptions
        fpu.sw &= ~(SW_IE | SW_DE | SW_ZE | SW_OE | SW_UE | SW_PE | SW_SF | SW_ES | SW_B);
      } else if (op2 == 0xE3) {
        // FNINIT
        fpu_init();
      } else if (op2 >= 0xC0 && op2 <= 0xC7) {
        // FCMOVNB (CF=0)
        if (!get_flag(FLAG_CF)) { ST(0) = ST(rm); TAG(0) = TAG(rm); }
      } else if (op2 >= 0xC8 && op2 <= 0xCF) {
        // FCMOVNE (ZF=0)
        if (!get_flag(FLAG_ZF)) { ST(0) = ST(rm); TAG(0) = TAG(rm); }
      } else if (op2 >= 0xD0 && op2 <= 0xD7) {
        // FCMOVNBE (CF=0 and ZF=0)
        if (!get_flag(FLAG_CF) && !get_flag(FLAG_ZF)) { ST(0) = ST(rm); TAG(0) = TAG(rm); }
      } else if (op2 >= 0xD8 && op2 <= 0xDF) {
        // FCMOVNU (PF=0)
        if (!get_flag(FLAG_PF)) { ST(0) = ST(rm); TAG(0) = TAG(rm); }
      } else if (op2 >= 0xE8 && op2 <= 0xEF) {
        // FUCOMI ST(0), ST(i) — compare, set EFLAGS
        double a = ST(0), b = ST(rm);
        clear_flag(FLAG_CF); clear_flag(FLAG_PF); clear_flag(FLAG_ZF);
        if (std::isnan(a) || std::isnan(b)) {
          set_flag(FLAG_CF); set_flag(FLAG_PF); set_flag(FLAG_ZF);
        } else if (a < b) {
          set_flag(FLAG_CF);
        } else if (a == b) {
          set_flag(FLAG_ZF);
        }
      } else if (op2 >= 0xF0 && op2 <= 0xF7) {
        // FCOMI ST(0), ST(i) — compare, set EFLAGS
        double a = ST(0), b = ST(rm);
        clear_flag(FLAG_CF); clear_flag(FLAG_PF); clear_flag(FLAG_ZF);
        if (std::isnan(a) || std::isnan(b)) {
          set_flag(FLAG_CF); set_flag(FLAG_PF); set_flag(FLAG_ZF);
        } else if (a < b) {
          set_flag(FLAG_CF);
        } else if (a == b) {
          set_flag(FLAG_ZF);
        }
      } else {
        fprintf(stderr, "[FPU] unhandled DB register op: %02X\n", op2);
      }
    }
    break;
  }

  //=========================================================================
  // DC: FADD/FMUL/FCOM/FCOMP/FSUB/FSUBR/FDIV/FDIVR — m64real or ST(i),ST(0)
  //=========================================================================
  case 4: {
    if (is_mem) {
      double val = fpu_read_m64real(mr.seg, mr.offset);
      switch (reg) {
        case 0: ST(0) += val; TAG(0) = compute_tag(ST(0)); break;
        case 1: ST(0) *= val; TAG(0) = compute_tag(ST(0)); break;
        case 2: fpu_compare(ST(0), val); break;
        case 3: fpu_compare(ST(0), val); fpu_pop(fpu); break;
        case 4: ST(0) -= val; TAG(0) = compute_tag(ST(0)); break;
        case 5: ST(0) = val - ST(0); TAG(0) = compute_tag(ST(0)); break;
        case 6:
          if (val == 0.0) { fpu.sw |= SW_ZE; ST(0) = std::copysign(INFINITY, ST(0)); }
          else ST(0) /= val;
          TAG(0) = compute_tag(ST(0));
          break;
        case 7:
          if (ST(0) == 0.0) { fpu.sw |= SW_ZE; ST(0) = std::copysign(INFINITY, val); }
          else ST(0) = val / ST(0);
          TAG(0) = compute_tag(ST(0));
          break;
      }
    } else {
      // Register: destination is ST(i), source is ST(0)
      // Note: FSUB/FSUBR and FDIV/FDIVR are swapped for DC vs D8
      switch (reg) {
        case 0: ST(rm) += ST(0); TAG(rm) = compute_tag(ST(rm)); break;  // FADD
        case 1: ST(rm) *= ST(0); TAG(rm) = compute_tag(ST(rm)); break;  // FMUL
        case 2: fpu_compare(ST(0), ST(rm)); break;  // FCOM (undocumented)
        case 3: fpu_compare(ST(0), ST(rm)); fpu_pop(fpu); break;  // FCOMP (undocumented)
        case 4: // FSUBR ST(i), ST(0) => ST(i) = ST(0) - ST(i)
          ST(rm) = ST(0) - ST(rm); TAG(rm) = compute_tag(ST(rm)); break;
        case 5: // FSUB ST(i), ST(0) => ST(i) = ST(i) - ST(0)
          ST(rm) -= ST(0); TAG(rm) = compute_tag(ST(rm)); break;
        case 6: // FDIVR ST(i), ST(0) => ST(i) = ST(0) / ST(i)
          if (ST(rm) == 0.0) { fpu.sw |= SW_ZE; ST(rm) = std::copysign(INFINITY, ST(0)); }
          else ST(rm) = ST(0) / ST(rm);
          TAG(rm) = compute_tag(ST(rm));
          break;
        case 7: // FDIV ST(i), ST(0) => ST(i) = ST(i) / ST(0)
          if (ST(0) == 0.0) { fpu.sw |= SW_ZE; ST(rm) = std::copysign(INFINITY, ST(rm)); }
          else ST(rm) /= ST(0);
          TAG(rm) = compute_tag(ST(rm));
          break;
      }
    }
    break;
  }

  //=========================================================================
  // DD: FLD/FST/FSTP m64real, FFREE, FUCOM, FUCOMP, FRSTOR, FNSAVE, FNSTSW
  //=========================================================================
  case 5: {
    if (is_mem) {
      switch (reg) {
        case 0: { // FLD m64real
          double val = fpu_read_m64real(mr.seg, mr.offset);
          fpu_push(fpu, val);
          break;
        }
        case 1: { // FISTTP m64int (SSE3)
          int64_t val = (int64_t)trunc(ST(0));
          uint64_t raw = (uint64_t)val;
          store_dword(mr.seg, mr.offset, (uint32_t)raw);
          store_dword(mr.seg, mr.offset + 4, (uint32_t)(raw >> 32));
          fpu_pop(fpu);
          break;
        }
        case 2: // FST m64real
          fpu_write_m64real(mr.seg, mr.offset, ST(0));
          break;
        case 3: // FSTP m64real
          fpu_write_m64real(mr.seg, mr.offset, ST(0));
          fpu_pop(fpu);
          break;
        case 4: { // FRSTOR — restore FPU state (94/108 bytes)
          // Simplified: restore CW, SW, TW, then 8 registers
          uint32_t base = mr.offset;
          int step = op_size_32 ? 4 : 2;
          fpu.cw = fetch_word(mr.seg, base);
          fpu.sw = fetch_word(mr.seg, base + step);
          uint16_t tw = fetch_word(mr.seg, base + step * 2);
          // Skip IP/DP fields, jump to register area
          uint32_t reg_off = base + (op_size_32 ? 28 : 14);
          for (int i = 0; i < 8; i++) {
            fpu.regs[(FPU_TOP + i) & 7] = fpu_read_m80real(mr.seg, reg_off + i * 10);
            fpu.tags[(FPU_TOP + i) & 7] = (tw >> (i * 2)) & 3;
          }
          break;
        }
        case 6: { // FNSAVE — save FPU state (94/108 bytes)
          uint32_t base = mr.offset;
          int step = op_size_32 ? 4 : 2;
          store_word(mr.seg, base, fpu.cw);
          store_word(mr.seg, base + step, fpu.sw);
          // Compute tag word
          uint16_t tw = 0;
          for (int i = 0; i < 8; i++)
            tw |= (fpu.tags[(FPU_TOP + i) & 7] & 3) << (i * 2);
          store_word(mr.seg, base + step * 2, tw);
          // Zero IP/DP fields
          for (int i = 3; i < (op_size_32 ? 7 : 7); i++)
            store_word(mr.seg, base + step * i, 0);
          // Save registers
          uint32_t reg_off = base + (op_size_32 ? 28 : 14);
          for (int i = 0; i < 8; i++)
            fpu_write_m80real(mr.seg, reg_off + i * 10, fpu.regs[(FPU_TOP + i) & 7]);
          // Re-initialize FPU after save
          fpu_init();
          break;
        }
        case 7: // FNSTSW m16
          store_word(mr.seg, mr.offset, fpu.sw);
          break;
        default:
          fprintf(stderr, "[FPU] unhandled DD mem reg=%d\n", reg);
          break;
      }
    } else {
      uint8_t op2 = modrm_byte;
      if (op2 >= 0xC0 && op2 <= 0xC7) {
        // FFREE ST(i)
        TAG(rm) = TAG_EMPTY;
      } else if (op2 >= 0xD0 && op2 <= 0xD7) {
        // FST ST(i)
        ST(rm) = ST(0);
        TAG(rm) = TAG(0);
      } else if (op2 >= 0xD8 && op2 <= 0xDF) {
        // FSTP ST(i)
        ST(rm) = ST(0);
        TAG(rm) = TAG(0);
        fpu_pop(fpu);
      } else if (op2 >= 0xE0 && op2 <= 0xE7) {
        // FUCOM ST(i) — unordered compare
        fpu_compare(ST(0), ST(rm));
      } else if (op2 >= 0xE8 && op2 <= 0xEF) {
        // FUCOMP ST(i)
        fpu_compare(ST(0), ST(rm));
        fpu_pop(fpu);
      } else {
        fprintf(stderr, "[FPU] unhandled DD register op: %02X\n", op2);
      }
    }
    break;
  }

  //=========================================================================
  // DE: m16int arithmetic / FADDP/FMULP/FCOMPP/FSUBP/FSUBRP/FDIVP/FDIVRP
  //=========================================================================
  case 6: {
    if (is_mem) {
      double val = (double)(int16_t)fetch_word(mr.seg, mr.offset);
      switch (reg) {
        case 0: ST(0) += val; break;
        case 1: ST(0) *= val; break;
        case 2: fpu_compare(ST(0), val); break;
        case 3: fpu_compare(ST(0), val); fpu_pop(fpu); break;
        case 4: ST(0) -= val; break;
        case 5: ST(0) = val - ST(0); break;
        case 6: if (val != 0.0) ST(0) /= val; else { fpu.sw |= SW_ZE; ST(0) = INFINITY; } break;
        case 7: if (ST(0) != 0.0) ST(0) = val / ST(0); else { fpu.sw |= SW_ZE; ST(0) = INFINITY; } break;
      }
      if (reg <= 1 || reg >= 4) TAG(0) = compute_tag(ST(0));
    } else {
      // Register: op and pop
      switch (reg) {
        case 0: ST(rm) += ST(0); TAG(rm) = compute_tag(ST(rm)); fpu_pop(fpu); break;  // FADDP
        case 1: ST(rm) *= ST(0); TAG(rm) = compute_tag(ST(rm)); fpu_pop(fpu); break;  // FMULP
        case 2: // FCOMP (undocumented alias)
          fpu_compare(ST(0), ST(rm)); fpu_pop(fpu);
          break;
        case 3: // FCOMPP (only DE D9)
          if (rm == 1) {
            fpu_compare(ST(0), ST(1));
            fpu_pop(fpu); fpu_pop(fpu);
          }
          break;
        case 4: // FSUBRP ST(i), ST(0) => ST(i) = ST(0) - ST(i), pop
          ST(rm) = ST(0) - ST(rm);
          TAG(rm) = compute_tag(ST(rm));
          fpu_pop(fpu);
          break;
        case 5: // FSUBP ST(i), ST(0) => ST(i) = ST(i) - ST(0), pop
          ST(rm) -= ST(0);
          TAG(rm) = compute_tag(ST(rm));
          fpu_pop(fpu);
          break;
        case 6: // FDIVRP ST(i), ST(0) => ST(i) = ST(0) / ST(i), pop
          if (ST(rm) == 0.0) { fpu.sw |= SW_ZE; ST(rm) = std::copysign(INFINITY, ST(0)); }
          else ST(rm) = ST(0) / ST(rm);
          TAG(rm) = compute_tag(ST(rm));
          fpu_pop(fpu);
          break;
        case 7: // FDIVP ST(i), ST(0) => ST(i) = ST(i) / ST(0), pop
          if (ST(0) == 0.0) { fpu.sw |= SW_ZE; ST(rm) = std::copysign(INFINITY, ST(rm)); }
          else ST(rm) /= ST(0);
          TAG(rm) = compute_tag(ST(rm));
          fpu_pop(fpu);
          break;
      }
    }
    break;
  }

  //=========================================================================
  // DF: FILD/FIST/FISTP m16int, FBLD/FBSTP, FILD/FISTP m64int, FNSTSW AX
  //=========================================================================
  case 7: {
    if (is_mem) {
      switch (reg) {
        case 0: { // FILD m16int
          int16_t val = (int16_t)fetch_word(mr.seg, mr.offset);
          fpu_push(fpu, (double)val);
          break;
        }
        case 1: { // FISTTP m16int (SSE3)
          int16_t val = (int16_t)trunc(ST(0));
          store_word(mr.seg, mr.offset, (uint16_t)val);
          fpu_pop(fpu);
          break;
        }
        case 2: { // FIST m16int
          int16_t val = (int16_t)fpu_round(ST(0));
          store_word(mr.seg, mr.offset, (uint16_t)val);
          break;
        }
        case 3: { // FISTP m16int
          int16_t val = (int16_t)fpu_round(ST(0));
          store_word(mr.seg, mr.offset, (uint16_t)val);
          fpu_pop(fpu);
          break;
        }
        case 4: { // FBLD m80bcd — load packed BCD
          uint64_t mantissa = 0;
          for (int i = 0; i < 9; i++) {
            uint8_t b = fetch_byte(mr.seg, mr.offset + i);
            mantissa += ((uint64_t)(b & 0x0F) + (uint64_t)((b >> 4) & 0x0F) * 10) *
                        (uint64_t)pow(100.0, i);
          }
          uint8_t sign_byte = fetch_byte(mr.seg, mr.offset + 9);
          double val = (double)mantissa;
          if (sign_byte & 0x80) val = -val;
          fpu_push(fpu, val);
          break;
        }
        case 5: { // FILD m64int
          uint32_t lo = fetch_dword(mr.seg, mr.offset);
          uint32_t hi = fetch_dword(mr.seg, mr.offset + 4);
          int64_t val = (int64_t)(((uint64_t)hi << 32) | lo);
          fpu_push(fpu, (double)val);
          break;
        }
        case 6: { // FBSTP m80bcd — store packed BCD and pop
          double val = ST(0);
          bool sign = val < 0;
          if (sign) val = -val;
          uint64_t intval = (uint64_t)fpu_round(val);
          for (int i = 0; i < 9; i++) {
            uint8_t lo_nib = intval % 10; intval /= 10;
            uint8_t hi_nib = intval % 10; intval /= 10;
            store_byte(mr.seg, mr.offset + i, (hi_nib << 4) | lo_nib);
          }
          store_byte(mr.seg, mr.offset + 9, sign ? 0x80 : 0x00);
          fpu_pop(fpu);
          break;
        }
        case 7: { // FISTP m64int
          int64_t val = (int64_t)fpu_round(ST(0));
          uint64_t raw = (uint64_t)val;
          store_dword(mr.seg, mr.offset, (uint32_t)raw);
          store_dword(mr.seg, mr.offset + 4, (uint32_t)(raw >> 32));
          fpu_pop(fpu);
          break;
        }
      }
    } else {
      uint8_t op2 = modrm_byte;
      if (op2 == 0xE0) {
        // FNSTSW AX
        regs[reg_AX] = fpu.sw;
      } else if (op2 >= 0xE8 && op2 <= 0xEF) {
        // FUCOMIP ST(0), ST(i) — unordered compare, set EFLAGS, pop
        double a = ST(0), b = ST(rm);
        clear_flag(FLAG_CF); clear_flag(FLAG_PF); clear_flag(FLAG_ZF);
        if (std::isnan(a) || std::isnan(b)) {
          set_flag(FLAG_CF); set_flag(FLAG_PF); set_flag(FLAG_ZF);
        } else if (a < b) {
          set_flag(FLAG_CF);
        } else if (a == b) {
          set_flag(FLAG_ZF);
        }
        fpu_pop(fpu);
      } else if (op2 >= 0xF0 && op2 <= 0xF7) {
        // FCOMIP ST(0), ST(i)
        double a = ST(0), b = ST(rm);
        clear_flag(FLAG_CF); clear_flag(FLAG_PF); clear_flag(FLAG_ZF);
        if (std::isnan(a) || std::isnan(b)) {
          set_flag(FLAG_CF); set_flag(FLAG_PF); set_flag(FLAG_ZF);
        } else if (a < b) {
          set_flag(FLAG_CF);
        } else if (a == b) {
          set_flag(FLAG_ZF);
        }
        fpu_pop(fpu);
      } else {
        fprintf(stderr, "[FPU] unhandled DF register op: %02X\n", op2);
      }
    }
    break;
  }

  default:
    fprintf(stderr, "[FPU] unhandled ESC%d\n", esc);
    break;
  }
}
