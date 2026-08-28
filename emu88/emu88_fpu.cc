// emu88_fpu.cc — x87 FPU: the instruction decode and the register file's
// bookkeeping.  The arithmetic itself is in emu88_f80.h.
//
// WHAT CHANGED, AND WHY IT WAS WORTH CHANGING
// -------------------------------------------
// This file used to hold the register stack in host `double`.  That is 53
// significand bits where an 80387 has 64, so a whole class of results was not
// reproducible: FLD m80real of 1+2^-53 collapsed to 1.0, there was no denormal
// class, no stack-overflow detection, precision control did nothing, FLDENV
// and FNSTENV were control-word stubs, and F2XM1 was pow(2,x)-1 - which throws
// away exactly the precision that encoding exists to keep.  tests/fpu_test.cc
// pinned thirty-one of those divergences rather than leaving them implicit.
//
// The register file is f80 now (emu88/emu88_f80.h), and every arithmetic path
// goes through it.  What that buys, beyond the obvious 11 bits:
//
//   - the m80real load and store are byte moves, so NaN payloads, signalling
//     NaNs, denormals and the unsupported encodings survive a round trip;
//   - precision control works, because rounding to 24 or 53 significand bits
//     inside a 15-bit exponent range is something only a soft float can do;
//   - the exception flags are real.  DE, OE, UE and PE were never set at all
//     before; they are set now, in the order hardware sets them, and ES and B
//     follow the mask.
//
// HOW THIS WAS VALIDATED
// ----------------------
// tests/f80_unit.cc runs the arithmetic against the HOST's x87 on x86-64,
// where `long double` is this exact format, and compares the result AND the
// exception flags bit for bit.  That is where the surprises came from: that
// #IA and #Z suppress #D but an infinity operand does not, that a masked
// overflow under PC=24 delivers 0xFFFFFF0000000000 rather than all ones, and
// that two NaNs with equal significands are broken by the smaller
// sign-exponent word rather than by "the destination".  None of those are in
// the manual in those words.

#include "emu88.h"

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
// The six exception bits as one mask.  Written out rather than as 0x003F so
// the six named constants above are used rather than being decoration - clang
// warns about an unused one and g++ does not, and tests/build.sh prefers
// clang when it is installed.
static constexpr uint16_t SW_EXC = SW_IE | SW_DE | SW_ZE | SW_OE | SW_UE | SW_PE;

// Tag values
static constexpr uint8_t TAG_VALID   = 0;
static constexpr uint8_t TAG_ZERO    = 1;
static constexpr uint8_t TAG_SPECIAL = 2;
static constexpr uint8_t TAG_EMPTY   = 3;

#define FPU_TOP        ((fpu.sw >> 11) & 7)
#define FPU_SET_TOP(t) (fpu.sw = (uint16_t)((fpu.sw & ~0x3800) | (((t) & 7) << 11)))

// Stack-fault kinds.  A stack fault owns C1 outright: it reports OVERFLOW with
// C1 set and UNDERFLOW with C1 clear, and that meaning displaces the
// "result was rounded up" one for the instruction that faulted.
enum { SF_NONE = 0, SF_UNDER = 1, SF_OVER = 2 };

//=============================================================================
// Tagging
//=============================================================================

// The architectural two-bit tag.  SPECIAL covers everything that is neither a
// zero nor an ordinary normal - denormals, infinities, NaNs, and the
// unsupported encodings - which is what a 387 puts there and what FSTENV and
// FSAVE write out.
static inline uint8_t fpu_tag_of(f80 v) {
  switch (f80_classify(v)) {
    case F80_CLASS_ZERO:   return TAG_ZERO;
    case F80_CLASS_NORMAL: return TAG_VALID;
    default:               return TAG_SPECIAL;
  }
}

//=============================================================================
// FPU initialization
//=============================================================================

void emu88::fpu_init() {
  fpu.cw = 0x037F;  // all exceptions masked, 64-bit precision, round nearest
  fpu.sw = 0;
  for (int i = 0; i < 8; i++) fpu.tags[i] = TAG_EMPTY;
  fpu.fip = 0; fpu.fcs = 0; fpu.fop = 0; fpu.fdp = 0; fpu.fds = 0;
}

// RESET and power-up, which are NOT FNINIT: the SDM gives the data registers
// as +0.0 after either, and unchanged after FNINIT.  Keeping the clear here
// rather than in fpu_init() is the whole difference.
void emu88::fpu_power_on() {
  for (int i = 0; i < 8; i++) fpu.regs[i] = f80_make_zero(false);
  fpu_init();
}

//=============================================================================
// Register access, with the stack faults hardware raises
//=============================================================================

// Reading a register tagged EMPTY is a stack UNDERFLOW: #IS, which is reported
// through IE with SF set, C1 clear, and the indefinite delivered in place of
// the operand.  Nothing checked this before, so a desynchronised stack simply
// carried on with whatever stale value was in the slot.
static inline f80 fpu_get(emu88::FPUState &fpu, int i, f80_ctx &c, int &sf) {
  int p = (((fpu.sw >> 11) & 7) + i) & 7;
  if (fpu.tags[p] == TAG_EMPTY) {
    c.flags |= F80_IE;
    if (sf == SF_NONE) sf = SF_UNDER;
    return f80_indefinite();
  }
  return fpu.regs[p];
}

static inline void fpu_put(emu88::FPUState &fpu, int i, f80 v) {
  int p = (((fpu.sw >> 11) & 7) + i) & 7;
  fpu.regs[p] = v;
  fpu.tags[p] = fpu_tag_of(v);
}

// Pushing onto a register that is not EMPTY is a stack OVERFLOW: #IS with C1
// SET, TOP still decrements, and the destination receives the indefinite - not
// the value that was being pushed.
static inline void fpu_push(emu88::FPUState &fpu, f80 v, f80_ctx &c, int &sf) {
  int top = ((((fpu.sw >> 11) & 7)) - 1) & 7;
  if (fpu.tags[top] != TAG_EMPTY) {
    c.flags |= F80_IE;
    // The FIRST fault owns C1, the way fpu_get already latches an underflow.
    // An instruction that reads an empty register AND then pushes onto a full
    // stack - FLD ST(0) with the stack full, FXTRACT, FPTAN - raises both, and
    // the host reports the underflow: SW=3841 with C1 clear, where overwriting
    // sf here gave 3A41 with C1 set.
    if (sf == SF_NONE) sf = SF_OVER;
    v = f80_indefinite();
  }
  fpu.sw = (uint16_t)((fpu.sw & ~0x3800) | (top << 11));
  fpu.regs[top] = v;
  fpu.tags[top] = fpu_tag_of(v);
}

static inline void fpu_pop(emu88::FPUState &fpu) {
  int top = (fpu.sw >> 11) & 7;
  fpu.tags[top] = TAG_EMPTY;
  fpu.sw = (uint16_t)((fpu.sw & ~0x3800) | ((((top + 1) & 7)) << 11));
}

// The masked #IS response for the TWO-RESULT instructions.  FXTRACT, FPTAN and
// FSINCOS each write one result and then push the other, so an overflowing
// push left the first result standing beside the indefinite instead of being
// replaced by one; and the transcendental had already run, so its inexactness
// was still in c.flags and reached the status word as a PE the instruction
// never earned.  Hardware gives BOTH destinations the indefinite and reports
// IE|SF and the C1 direction bit alone.  Measured on the host, on a full stack
// and an empty one alike: SW=3A41 with C1 set for the overflow and 3841 with
// C1 clear for the underflow, PE clear in both.
static inline void fpu_two_result_fault(emu88::FPUState &fpu, f80_ctx &c, int sf) {
  if (sf == SF_NONE) return;
  fpu_put(fpu, 0, f80_indefinite());
  fpu_put(fpu, 1, f80_indefinite());
  c.flags &= (uint16_t)~(F80_DE | F80_ZE | F80_OE | F80_UE | F80_PE);
  c.c1 = false;
}

//=============================================================================
// Memory access helpers for FPU operand types
//=============================================================================

f80 emu88::fpu_read_m32real(uint16_t seg, uint32_t off, f80_ctx &c, bool quiet_snan) {
  return f80_from_f32(fetch_dword(seg, off), c, quiet_snan);
}

f80 emu88::fpu_read_m64real(uint16_t seg, uint32_t off, f80_ctx &c, bool quiet_snan) {
  uint32_t lo = fetch_dword(seg, off);
  uint32_t hi = fetch_dword(seg, off + 4);
  return f80_from_f64(((uint64_t)hi << 32) | lo, c, quiet_snan);
}

// A ten-byte move.  The old version rebuilt a double out of the encoding with
// ldexp() and lost the sign and payload of a NaN on the way; there is nothing
// to rebuild now.
f80 emu88::fpu_read_m80real(uint16_t seg, uint32_t off) {
  f80 r;
  uint64_t m = 0;
  for (int i = 0; i < 8; i++)
    m |= ((uint64_t)fetch_byte(seg, off + i)) << (i * 8);
  r.sig = m;
  r.se  = fetch_word(seg, off + 8);
  return r;
}

void emu88::fpu_write_m32real(uint16_t seg, uint32_t off, f80 v, f80_ctx &c) {
  store_dword(seg, off, f80_to_f32(v, c));
}

void emu88::fpu_write_m64real(uint16_t seg, uint32_t off, f80 v, f80_ctx &c) {
  uint64_t raw = f80_to_f64(v, c);
  store_dword(seg, off, (uint32_t)raw);
  store_dword(seg, off + 4, (uint32_t)(raw >> 32));
}

void emu88::fpu_write_m80real(uint16_t seg, uint32_t off, f80 v) {
  for (int i = 0; i < 8; i++)
    store_byte(seg, off + i, (uint8_t)(v.sig >> (i * 8)));
  store_word(seg, off + 8, v.se);
}

//=============================================================================
// Unhandled-opcode report
//=============================================================================

// 073605d printed one of these from every branch that decoded nothing.  The
// fprintf was deleted and the `else { }` was left behind, so an x87 encoding
// this file does not implement became a silent no-op - the worst way to fail,
// because the program carries on with a stale ST(0).  Restored with a cap: an
// unhandled opcode inside a game's inner loop would otherwise write to stderr
// faster than the loop runs.
static void fpu_unhandled(const char *escape, uint8_t op2) {
  static int reported = 0;
  if (reported >= 16) return;
  reported++;
  emu88_fatal("[FPU] unhandled %s register op: %02X%s", escape, op2,
              reported == 16 ? "  (further reports suppressed)" : "");
}

//=============================================================================
// Condition codes
//=============================================================================

static inline void fpu_set_cc(emu88::FPUState &fpu, f80_cmp_r r) {
  fpu.sw &= (uint16_t)~(SW_C0 | SW_C2 | SW_C3);
  switch (r) {
    case F80_CMP_GT:    break;                                  // 0 0 0
    case F80_CMP_LT:    fpu.sw |= SW_C0; break;                 // 0 0 1
    case F80_CMP_EQ:    fpu.sw |= SW_C3; break;                 // 1 0 0
    default:            fpu.sw |= SW_C0 | SW_C2 | SW_C3; break; // unordered
  }
}

// The FCOMI family reports into EFLAGS instead, and clears OF/SF/AF while it
// is there.
void emu88::fpu_cmp_eflags(f80_cmp_r r) {
  clear_flag(FLAG_CF); clear_flag(FLAG_PF); clear_flag(FLAG_ZF);
  clear_flag(FLAG_OF); clear_flag(FLAG_SF); clear_flag(FLAG_AF);
  switch (r) {
    case F80_CMP_GT: break;
    case F80_CMP_LT: set_flag(FLAG_CF); break;
    case F80_CMP_EQ: set_flag(FLAG_ZF); break;
    default: set_flag(FLAG_CF); set_flag(FLAG_PF); set_flag(FLAG_ZF); break;
  }
}

//=============================================================================
// The environment and full-state images
//=============================================================================

// The tag word FSTENV and FSAVE write is in PHYSICAL register order - bit pair
// i describes FPR i, not ST(i).  This file used to write it TOP-relative,
// which a 387 does not, and FRSTOR read it back the same wrong way so the
// round trip inside the emulator looked right.
static inline uint16_t fpu_tag_word(const emu88::FPUState &fpu) {
  uint16_t tw = 0;
  for (int i = 0; i < 8; i++) tw |= (uint16_t)((fpu.tags[i] & 3) << (i * 2));
  return tw;
}

// The environment is seven fields, and which seven bytes they occupy depends
// on BOTH the operand size and whether the processor is in protected mode.
// All four layouts are below; the old code wrote two of the seven and read
// two, which is why tests/fpu_test.cc had FNSTENV pinned as a stub.
void emu88::fpu_store_env(uint16_t seg, uint32_t base, bool op32) {
  uint16_t tw = fpu_tag_word(fpu);
  // Every field below is written through one of these two, and they exist for
  // the reason the FNSAVE register-area loop and the FBSTP loop already carry
  // `&& !fault_abort()`: check_segment_write deliberately lets an access
  // through once an exception is already pending.  An unguarded run of stores
  // therefore keeps going PAST the fault with the offset wrapped to 16 bits,
  // landing at the start of the segment - memory the instruction never named.
  // execute_fpu restores the FPU state on a fault but cannot un-write guest
  // memory.  FNSTENV [FFF8] in real mode was writing four bytes to DS:0002.
  auto sw16 = [&](uint32_t off, uint16_t v) {
    if (fault_abort()) return;
    store_word(seg, off, v);
  };
  auto sw32 = [&](uint32_t off, uint32_t v) {
    if (fault_abort()) return;
    store_dword(seg, off, v);
  };
  // Virtual-8086 mode has CR0.PE set and uses the REAL-address-mode layout,
  // which is the one place `protected_mode()` alone gives the wrong answer.
  bool prot = protected_mode() && !v86_mode();
  if (!op32) {
    sw16(base + 0,  fpu.cw);
    sw16(base + 2,  fpu.sw);
    sw16(base + 4,  tw);
    if (prot) {
      sw16(base + 6,  (uint16_t)fpu.fip);
      sw16(base + 8,  fpu.fcs);
      sw16(base + 10, (uint16_t)fpu.fdp);
      sw16(base + 12, fpu.fds);
    } else {
      uint32_t ilin = ((uint32_t)fpu.fcs << 4) + fpu.fip;
      uint32_t dlin = ((uint32_t)fpu.fds << 4) + fpu.fdp;
      sw16(base + 6,  (uint16_t)ilin);
      sw16(base + 8,  (uint16_t)((((ilin >> 16) & 0x0F) << 12) | (fpu.fop & 0x07FF)));
      sw16(base + 10, (uint16_t)dlin);
      sw16(base + 12, (uint16_t)(((dlin >> 16) & 0x0F) << 12));
    }
    return;
  }
  // The reserved upper halves of the 16-bit fields are written as ONES, not
  // zeroes.  Measured on the host with the destination pre-poisoned three ways
  // (0x00, 0xAA, 0x5A) and identical every time, so they are actively stored
  // rather than left over: FNINIT then FNSTENV32 gives +00=FFFF037F,
  // +04=FFFF0000, +08=FFFFFFFF, +18=FFFF0000.  The three dwords that carry a
  // full 32 bits - FIP at +0C, the selector-and-opcode at +10, FDP at +14 -
  // have no reserved half and get none.
  //
  // CW, SW and TW sit at +0/+4/+8 in BOTH 32-bit layouts, so the ones go in for
  // real-address mode too; only the protected form could actually be measured,
  // because this host cannot leave protected mode.  The operand selector at
  // +18 is protected-mode-only and is handled inside that branch; the
  // real-address branch below packs its pointers differently and is left as
  // it is rather than guessed at.
  sw32(base + 0, 0xFFFF0000u | fpu.cw);
  sw32(base + 4, 0xFFFF0000u | fpu.sw);
  sw32(base + 8, 0xFFFF0000u | tw);
  if (prot) {
    sw32(base + 12, fpu.fip);
    sw32(base + 16, (uint32_t)fpu.fcs | ((uint32_t)(fpu.fop & 0x07FF) << 16));
    sw32(base + 20, fpu.fdp);
    sw32(base + 24, 0xFFFF0000u | fpu.fds);
  } else {
    // The 32-bit real-address-mode image is NOT the 16-bit one widened.  SDM
    // Vol.1, "Real Mode x87 FPU State Image in Memory, 32-Bit Format":
    //
    //   +0Ch  bits 15:0  = IP 15:0,      bits 31:16 reserved
    //   +10h  bits 27:16 = IP 31:16,     bits 10:0  = opcode
    //   +14h  bits 15:0  = OP 15:0,      bits 31:16 reserved
    //   +18h  bits 27:16 = OP 31:16,     bits 15:0  reserved
    //
    // The pointer's high bits live in the UPPER half of the dword, with twelve
    // bits of room - not at bits 12:15 with four, which is the SIXTEEN-bit
    // layout and is what this branch used to copy.  A real-mode linear address
    // needs twenty-one bits at the top ((0xFFFF << 4) + 0xFFFF is 0x10FFEF),
    // so the old packing truncated as well as misplaced.
    uint32_t ilin = ((uint32_t)fpu.fcs << 4) + fpu.fip;
    uint32_t dlin = ((uint32_t)fpu.fds << 4) + fpu.fdp;
    sw32(base + 12, ilin & 0xFFFF);
    sw32(base + 16, (ilin & 0xFFFF0000) | (fpu.fop & 0x07FF));
    sw32(base + 20, dlin & 0xFFFF);
    sw32(base + 24, dlin & 0xFFFF0000);
  }
}

void emu88::fpu_load_env(uint16_t seg, uint32_t base, bool op32) {
  bool prot = protected_mode() && !v86_mode();
  uint16_t tw;
  if (!op32) {
    fpu.cw = fetch_word(seg, base + 0);
    fpu.sw = fetch_word(seg, base + 2);
    tw     = fetch_word(seg, base + 4);
    if (prot) {
      fpu.fip = fetch_word(seg, base + 6);
      fpu.fcs = fetch_word(seg, base + 8);
      fpu.fdp = fetch_word(seg, base + 10);
      fpu.fds = fetch_word(seg, base + 12);
      fpu.fop = 0;
    } else {
      uint16_t w8 = fetch_word(seg, base + 8);
      uint16_t wc = fetch_word(seg, base + 12);
      fpu.fip = (uint32_t)fetch_word(seg, base + 6) | ((uint32_t)(w8 >> 12) << 16);
      fpu.fop = (uint16_t)(w8 & 0x07FF);
      fpu.fdp = (uint32_t)fetch_word(seg, base + 10) | ((uint32_t)(wc >> 12) << 16);
      fpu.fcs = 0; fpu.fds = 0;
    }
  } else {
    fpu.cw = (uint16_t)fetch_dword(seg, base + 0);
    fpu.sw = (uint16_t)fetch_dword(seg, base + 4);
    tw     = (uint16_t)fetch_dword(seg, base + 8);
    if (prot) {
      fpu.fip = fetch_dword(seg, base + 12);
      uint32_t d16 = fetch_dword(seg, base + 16);
      fpu.fcs = (uint16_t)d16;
      fpu.fop = (uint16_t)((d16 >> 16) & 0x07FF);
      fpu.fdp = fetch_dword(seg, base + 20);
      fpu.fds = (uint16_t)fetch_dword(seg, base + 24);
    } else {
      uint32_t d12 = fetch_dword(seg, base + 12);
      uint32_t d16 = fetch_dword(seg, base + 16);
      uint32_t d20 = fetch_dword(seg, base + 20);
      uint32_t d24 = fetch_dword(seg, base + 24);
      // The same upper half on the way back in - see the store side for the
      // layout.  Reading bits 12:15 here is what made the image round-trip
      // self-consistently while matching no real 387.
      fpu.fip = (d12 & 0xFFFF) | (d16 & 0xFFFF0000);
      fpu.fop = (uint16_t)(d16 & 0x07FF);
      fpu.fdp = (d20 & 0xFFFF) | (d24 & 0xFFFF0000);
      fpu.fcs = 0; fpu.fds = 0;
    }
  }
  for (int i = 0; i < 8; i++) fpu.tags[i] = (uint8_t)((tw >> (i * 2)) & 3);
}

//=============================================================================
// Main FPU instruction dispatcher
//=============================================================================

void emu88::execute_fpu(emu88_uint8 opcode) {
  emu88_uint8 modrm_byte = fetch_ip_byte();
  if (fault_abort()) return;              // the modrm byte's own fetch faulted
  modrm_result mr = decode_modrm(modrm_byte);
  if (fault_abort()) return;              // the address computation faulted

  uint8_t esc = opcode - 0xD8;  // 0-7
  uint8_t reg = (modrm_byte >> 3) & 7;
  uint8_t rm  = modrm_byte & 7;
  bool is_mem = !mr.is_register;

  // A memory operand that faults ABORTS the instruction.  #GP, #PF and #SS are
  // faults, not traps: the handler returns to the same instruction and it runs
  // again from the start, so the x87 state it sees has to be the state it
  // started with.  Nothing enforced that before - a faulting FLD still pushed,
  // a faulting FSTP still popped, and the retag and the status word went with
  // them - so a guest that page-faulted on an x87 operand resumed with a
  // register stack one deeper or one shallower than it had left.
  //
  // The snapshot is taken only for the memory forms.  A register form cannot
  // fault past the two checks above, and the copy is 152 bytes that the hot
  // instructions - FADD ST,ST(i), FMUL, FXCH, the transcendentals - should not
  // have to pay for.
  FPUState saved;
  if (is_mem) saved = fpu;

  f80_ctx c = f80_ctx_make(fpu.cw);
  int  sf = SF_NONE;
  bool c1_own = false;          // the instruction placed C1 itself
  bool track  = true;           // the control forms do not move FIP/FDP
  bool reinit = false;          // FNINIT and FNSAVE leave the FPU reset

  // Shorthands.  ST(i) reads go through fpu_get so an empty register raises
  // the stack underflow instead of handing back a stale value.
  #define RD(i)     fpu_get(fpu, (i), c, sf)
  #define WR(i, v)  fpu_put(fpu, (i), (v))
  // WRR is WR for an ARITHMETIC RESULT, and it exists because the masked #IS
  // response is the indefinite, unconditionally.  fpu_get substitutes the
  // indefinite for an empty operand, but the f80 primitive then runs its
  // ordinary two-NaN tie-break on it, so any QNaN in the live register with a
  // significand above C000000000000000 outranked the substitute and landed in
  // the destination.  Measured on the host: FADD ST,ST(1) with ST(0) empty and
  // ST(1) = 7FFF:FFFFFFFFFFFFFFFF gives FFFF:C000000000000000, not the QNaN.
  //
  // The value is evaluated into a temporary FIRST.  Several call sites read
  // their operands inside the argument - WRR(1, f80_yl2x(RD(1), RD(0), c)) -
  // so sf is not yet set when the argument is written, and testing it before
  // the argument runs would never fire.  It is deliberately NOT applied to
  // FXCH or to FST ST(i): the host exchanges WITH the substitute rather than
  // flooding both registers, and a blanket rule here would destroy the live
  // operand.
  #define WRR(i, v) do { f80 rslt_ = (v); \
                         fpu_put(fpu, (i), sf != SF_NONE ? f80_indefinite() : rslt_); \
                       } while (0)
  #define TAGP(i)   fpu.tags[(FPU_TOP + (i)) & 7]

  switch (esc) {

  //=========================================================================
  // D8: FADD/FMUL/FCOM/FCOMP/FSUB/FSUBR/FDIV/FDIVR — m32real or ST(i)
  //=========================================================================
  case 0: {
    f80 val = is_mem ? fpu_read_m32real(mr.seg, mr.offset, c, false) : RD(rm);
    f80 st0 = RD(0);
    switch (reg) {
      case 0: WRR(0, f80_add(st0, val, c)); break;                 // FADD
      case 1: WRR(0, f80_mul(st0, val, c)); break;                 // FMUL
      case 2: fpu_set_cc(fpu, f80_compare(st0, val, false, c)); break;  // FCOM
      case 3: fpu_set_cc(fpu, f80_compare(st0, val, false, c)); fpu_pop(fpu); break;
      case 4: WRR(0, f80_sub(st0, val, c)); break;                 // FSUB
      case 5: WRR(0, f80_sub(val, st0, c)); break;                 // FSUBR
      case 6: WRR(0, f80_div(st0, val, c)); break;                 // FDIV
      case 7: WRR(0, f80_div(val, st0, c)); break;                 // FDIVR
    }
    break;
  }

  //=========================================================================
  // D9: FLD m32real, FST/FSTP m32real, FLDCW, FSTCW, special register ops
  //=========================================================================
  case 1: {
    if (is_mem) {
      switch (reg) {
        case 0: fpu_push(fpu, fpu_read_m32real(mr.seg, mr.offset, c), c, sf); break;
        case 2: fpu_write_m32real(mr.seg, mr.offset, RD(0), c); break;   // FST
        case 3: fpu_write_m32real(mr.seg, mr.offset, RD(0), c);          // FSTP
                fpu_pop(fpu); break;
        case 4: fpu_load_env(mr.seg, mr.offset, op_size_32);             // FLDENV
                track = false; c1_own = true; break;
        case 5: fpu.cw = fetch_word(mr.seg, mr.offset);                  // FLDCW
                track = false; c1_own = true; break;
        case 6: fpu_store_env(mr.seg, mr.offset, op_size_32);            // FNSTENV
                // A 387 masks every exception after writing the environment,
                // so the handler it is about to run cannot be re-entered.
                fpu.cw |= 0x003F;
                track = false; c1_own = true; break;
        case 7: store_word(mr.seg, mr.offset, fpu.cw);                   // FNSTCW
                track = false; c1_own = true; break;
        default: fpu_unhandled("D9", modrm_byte); break;
      }
    } else {
      uint8_t op2 = modrm_byte;
      if (op2 >= 0xC0 && op2 <= 0xC7) {                    // FLD ST(i)
        f80 v = RD(rm);
        fpu_push(fpu, v, c, sf);
      } else if (op2 >= 0xC8 && op2 <= 0xCF) {             // FXCH ST(i)
        f80 a = RD(0), b = RD(rm);
        WR(0, b); WR(rm, a);
      } else switch (op2) {
        case 0xD0: break;                                  // FNOP
        // FCHS and FABS are the only two instructions here that touch the
        // sign bit directly instead of going through an f80_* routine, which
        // makes them the only two that can deform the #IS substitute: reading
        // an empty ST(0) delivers the indefinite, FFFF:C000000000000000, and
        // flipping or clearing ITS sign leaves 7FFF:C000000000000000 - an
        // ordinary positive QNaN where hardware leaves the indefinite.  The
        // masked #IS response is the indefinite delivered as it is.
        case 0xE0: { f80 v = RD(0);                        // FCHS
          WR(0, sf == SF_NONE ? f80_chs(v) : v); break; }
        case 0xE1: { f80 v = RD(0);                        // FABS
          WR(0, sf == SF_NONE ? f80_abs(v) : v); break; }
        case 0xE4:                                         // FTST
          fpu_set_cc(fpu, f80_compare(RD(0), f80_make_zero(false), false, c));
          break;
        case 0xE5: {                                       // FXAM
          fpu.sw &= (uint16_t)~(SW_C0 | SW_C1 | SW_C2 | SW_C3);
          f80 v = fpu.regs[FPU_TOP];                       // no underflow check
          if (f80_neg(v)) fpu.sw |= SW_C1;
          if (TAGP(0) == TAG_EMPTY) fpu.sw |= SW_C0 | SW_C3;
          else switch (f80_classify(v)) {
            case F80_CLASS_UNSUPPORTED: break;                          // 0 0 0
            case F80_CLASS_SNAN:
            case F80_CLASS_QNAN:     fpu.sw |= SW_C0; break;            // 0 0 1
            case F80_CLASS_NORMAL:   fpu.sw |= SW_C2; break;            // 0 1 0
            case F80_CLASS_INF:      fpu.sw |= SW_C0 | SW_C2; break;    // 0 1 1
            case F80_CLASS_ZERO:     fpu.sw |= SW_C3; break;            // 1 0 0
            default:                 fpu.sw |= SW_C2 | SW_C3; break;    // denormal
          }
          c1_own = true;
          break;
        }
        case 0xE8: fpu_push(fpu, F80_ONE, c, sf); break;   // FLD1
        case 0xE9: fpu_push(fpu, F80_L2T, c, sf); break;   // FLDL2T
        case 0xEA: fpu_push(fpu, F80_L2E, c, sf); break;   // FLDL2E
        case 0xEB: fpu_push(fpu, F80_PI,  c, sf); break;   // FLDPI
        case 0xEC: fpu_push(fpu, F80_LG2, c, sf); break;   // FLDLG2
        case 0xED: fpu_push(fpu, F80_LN2, c, sf); break;   // FLDLN2
        case 0xEE: fpu_push(fpu, f80_make_zero(false), c, sf); break;   // FLDZ
        case 0xF0: WRR(0, f80_2xm1(RD(0), c)); break;       // F2XM1
        case 0xF1:                                         // FYL2X
          WRR(1, f80_yl2x(RD(1), RD(0), c));
          fpu_pop(fpu);
          break;
        case 0xF2: {                                       // FPTAN
          f80 t;
          if (!f80_ptan(RD(0), &t, c)) { fpu.sw |= SW_C2; c1_own = true; break; }
          WR(0, t);
          fpu_push(fpu, F80_ONE, c, sf);
          fpu_two_result_fault(fpu, c, sf);
          fpu.sw &= (uint16_t)~SW_C2;
          break;
        }
        case 0xF3:                                         // FPATAN
          WRR(1, f80_patan(RD(1), RD(0), c));
          fpu_pop(fpu);
          break;
        case 0xF4: {                                       // FXTRACT
          f80 e, m;
          f80_extract(RD(0), &e, &m, c);
          WR(0, e);
          fpu_push(fpu, m, c, sf);
          fpu_two_result_fault(fpu, c, sf);
          break;
        }
        case 0xF5: case 0xF8: {                            // FPREM1 / FPREM
          int q = 0; bool part = false;
          f80 r = (op2 == 0xF5) ? f80_prem1(RD(0), RD(1), c, &q, &part)
                                : f80_prem (RD(0), RD(1), c, &q, &part);
          WR(0, r);
          fpu.sw &= (uint16_t)~(SW_C0 | SW_C1 | SW_C2 | SW_C3);
          if (part) fpu.sw |= SW_C2;
          // C0, C3 and C1 carry quotient bits 2, 1 and 0.
          if (q & 4) fpu.sw |= SW_C0;
          if (q & 2) fpu.sw |= SW_C3;
          if (q & 1) fpu.sw |= SW_C1;
          c1_own = true;
          break;
        }
        case 0xF6: FPU_SET_TOP(FPU_TOP - 1); break;                  // FDECSTP
        case 0xF7: FPU_SET_TOP(FPU_TOP + 1); break;                  // FINCSTP
        case 0xF9:                                         // FYL2XP1
          WRR(1, f80_yl2xp1(RD(1), RD(0), c));
          fpu_pop(fpu);
          break;
        case 0xFA: WRR(0, f80_sqrt(RD(0), c)); break;       // FSQRT
        case 0xFB: {                                       // FSINCOS
          f80 s, co;
          if (!f80_sincos(RD(0), &s, &co, c)) { fpu.sw |= SW_C2; c1_own = true; break; }
          WR(0, s);
          fpu_push(fpu, co, c, sf);
          fpu_two_result_fault(fpu, c, sf);
          fpu.sw &= (uint16_t)~SW_C2;
          break;
        }
        case 0xFC: WRR(0, f80_rndint(RD(0), c)); break;     // FRNDINT
        case 0xFD: WRR(0, f80_scale(RD(0), RD(1), c)); break;  // FSCALE
        case 0xFE: case 0xFF: {                            // FSIN / FCOS
          f80 v;
          bool ok = (op2 == 0xFE) ? f80_sin(RD(0), &v, c) : f80_cos(RD(0), &v, c);
          if (!ok) { fpu.sw |= SW_C2; c1_own = true; break; }
          WR(0, v);
          fpu.sw &= (uint16_t)~SW_C2;
          break;
        }
        default:
          // D9 D8-DF is FSTP1 ST(i): the undocumented second encoding of
          // FSTP ST(i), and a POPPING form, so leaving it unhandled left the
          // stack one deeper on every pass - the FFREEP failure mode exactly.
          // Checked against the host: with 1.5, 2.5, 3.5 pushed, D9 DA stores
          // ST(0) into ST(2) and pops, leaving ST(0)=2.5 and ST(1)=3.5, which
          // is FSTP and not a bare pop.
          if (op2 >= 0xD8 && op2 <= 0xDF) {
            WR(rm, RD(0));
            fpu_pop(fpu);
            break;
          }
          fpu_unhandled("D9", op2);
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
      f80 val = f80_from_i32((int32_t)fetch_dword(mr.seg, mr.offset));
      f80 st0 = RD(0);
      switch (reg) {
        case 0: WRR(0, f80_add(st0, val, c)); break;
        case 1: WRR(0, f80_mul(st0, val, c)); break;
        case 2: fpu_set_cc(fpu, f80_compare(st0, val, false, c)); break;
        case 3: fpu_set_cc(fpu, f80_compare(st0, val, false, c)); fpu_pop(fpu); break;
        case 4: WRR(0, f80_sub(st0, val, c)); break;
        case 5: WRR(0, f80_sub(val, st0, c)); break;
        case 6: WRR(0, f80_div(st0, val, c)); break;
        case 7: WRR(0, f80_div(val, st0, c)); break;
      }
    } else if (modrm_byte == 0xE9) {
      // FUCOMPP (DA E9) — the only register-form DA opcode that is not an
      // FCMOV.  GCC emits it for long-double relational operators; without it
      // the comparison is a no-op and the two pops are skipped, desyncing the
      // stack.
      fpu_set_cc(fpu, f80_compare(RD(0), RD(1), true, c));
      fpu_pop(fpu);
      fpu_pop(fpu);
    } else {
      bool cond = false;
      switch (reg) {
        case 0: cond = get_flag(FLAG_CF); break;                        // FCMOVB
        case 1: cond = get_flag(FLAG_ZF); break;                        // FCMOVE
        case 2: cond = get_flag(FLAG_CF) || get_flag(FLAG_ZF); break;   // FCMOVBE
        case 3: cond = get_flag(FLAG_PF); break;                        // FCMOVU
        default: fpu_unhandled("DA", modrm_byte); break;
      }
      // Both operands are read whatever the condition says, so an empty
      // register is a stack underflow on the paths that move nothing too -
      // and the masked #IS response is the indefinite in ST(0), whichever
      // operand was empty and whatever the condition decided.  Measured on the
      // host: with ST(2) freed, FCMOVB ST(2) leaves FFFF:C000000000000000 at
      // CF=0 and at CF=1 alike, and the same with ST(0) freed instead.
      f80 src = RD(rm), dst = RD(0);
      WR(0, sf != SF_NONE ? f80_indefinite() : (cond ? src : dst));
    }
    break;
  }

  //=========================================================================
  // DB: FILD/FIST/FISTP m32int, FLD/FSTP m80real, FINIT, FCLEX, FCOMI
  //=========================================================================
  case 3: {
    if (is_mem) {
      switch (reg) {
        case 0:                                                     // FILD m32int
          fpu_push(fpu, f80_from_i32((int32_t)fetch_dword(mr.seg, mr.offset)), c, sf);
          break;
        case 1:                                                     // FISTTP m32int
          store_dword(mr.seg, mr.offset, (uint32_t)(int32_t)f80_to_int(RD(0), 32, true, c));
          fpu_pop(fpu);
          break;
        case 2:                                                     // FIST m32int
          store_dword(mr.seg, mr.offset, (uint32_t)(int32_t)f80_to_int(RD(0), 32, false, c));
          break;
        case 3:                                                     // FISTP m32int
          store_dword(mr.seg, mr.offset, (uint32_t)(int32_t)f80_to_int(RD(0), 32, false, c));
          fpu_pop(fpu);
          break;
        case 5:                                                     // FLD m80real
          fpu_push(fpu, fpu_read_m80real(mr.seg, mr.offset), c, sf);
          break;
        case 7:                                                     // FSTP m80real
          fpu_write_m80real(mr.seg, mr.offset, RD(0));
          fpu_pop(fpu);
          break;
        default:
          fpu_unhandled("DB", modrm_byte);
          break;
      }
    } else {
      uint8_t op2 = modrm_byte;
      if (op2 == 0xE0 || op2 == 0xE1 || op2 == 0xE4) {
        // FNENI, FNDISI, FSETPM — 287 control instructions that a 387 and
        // everything after it decode as no-ops.  They used to be reported as
        // unhandled, which is noisier than the truth.
        track = false;
        c1_own = true;
      } else if (op2 == 0xE2) {                                     // FNCLEX
        fpu.sw &= (uint16_t)~(SW_EXC | SW_SF | SW_ES | SW_B);
        track = false; c1_own = true;
      } else if (op2 == 0xE3) {                                     // FNINIT
        fpu_init();
        reinit = true;                                              // nothing to commit
      } else if (op2 >= 0xC0 && op2 <= 0xDF) {
        bool cond;
        if      (op2 <= 0xC7) cond = !get_flag(FLAG_CF);                          // FCMOVNB
        else if (op2 <= 0xCF) cond = !get_flag(FLAG_ZF);                          // FCMOVNE
        else if (op2 <= 0xD7) cond = !get_flag(FLAG_CF) && !get_flag(FLAG_ZF);    // FCMOVNBE
        else                  cond = !get_flag(FLAG_PF);                          // FCMOVNU
        f80 src = RD(rm), dst = RD(0);      // see the DA note: #IS owns ST(0)
        WR(0, sf != SF_NONE ? f80_indefinite() : (cond ? src : dst));
      } else if (op2 >= 0xE8 && op2 <= 0xEF) {                      // FUCOMI
        // C1 is cleared, not left alone: it reports the stack-fault direction
        // and nothing else here, which the commit below does for free.
        fpu_cmp_eflags(f80_compare(RD(0), RD(rm), true, c));
      } else if (op2 >= 0xF0 && op2 <= 0xF7) {                      // FCOMI
        fpu_cmp_eflags(f80_compare(RD(0), RD(rm), false, c));
      } else {
        fpu_unhandled("DB", op2);
      }
    }
    break;
  }

  //=========================================================================
  // DC: FADD/FMUL/FCOM/FCOMP/FSUB/FSUBR/FDIV/FDIVR — m64real or ST(i),ST(0)
  //=========================================================================
  case 4: {
    if (is_mem) {
      f80 val = fpu_read_m64real(mr.seg, mr.offset, c, false);   // an operand
      f80 st0 = RD(0);
      switch (reg) {
        case 0: WRR(0, f80_add(st0, val, c)); break;
        case 1: WRR(0, f80_mul(st0, val, c)); break;
        case 2: fpu_set_cc(fpu, f80_compare(st0, val, false, c)); break;
        case 3: fpu_set_cc(fpu, f80_compare(st0, val, false, c)); fpu_pop(fpu); break;
        case 4: WRR(0, f80_sub(st0, val, c)); break;
        case 5: WRR(0, f80_sub(val, st0, c)); break;
        case 6: WRR(0, f80_div(st0, val, c)); break;
        case 7: WRR(0, f80_div(val, st0, c)); break;
      }
    } else {
      // Destination is ST(i), source ST(0).  FSUB/FSUBR and FDIV/FDIVR carry
      // the opposite reg encodings here from the ones they carry in D8.
      f80 sti = RD(rm), st0 = RD(0);
      switch (reg) {
        case 0: WRR(rm, f80_add(sti, st0, c)); break;                    // FADD
        case 1: WRR(rm, f80_mul(sti, st0, c)); break;                    // FMUL
        case 2: fpu_set_cc(fpu, f80_compare(st0, sti, false, c)); break;
        case 3: fpu_set_cc(fpu, f80_compare(st0, sti, false, c)); fpu_pop(fpu); break;
        case 4: WRR(rm, f80_sub(st0, sti, c)); break;                    // FSUBR
        case 5: WRR(rm, f80_sub(sti, st0, c)); break;                    // FSUB
        case 6: WRR(rm, f80_div(st0, sti, c)); break;                    // FDIVR
        case 7: WRR(rm, f80_div(sti, st0, c)); break;                    // FDIV
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
        case 0:                                                    // FLD m64real
          fpu_push(fpu, fpu_read_m64real(mr.seg, mr.offset, c), c, sf);
          break;
        case 1: {                                                  // FISTTP m64int
          uint64_t raw = (uint64_t)f80_to_int(RD(0), 64, true, c);
          store_dword(mr.seg, mr.offset, (uint32_t)raw);
          store_dword(mr.seg, mr.offset + 4, (uint32_t)(raw >> 32));
          fpu_pop(fpu);
          break;
        }
        case 2: fpu_write_m64real(mr.seg, mr.offset, RD(0), c); break;
        case 3: fpu_write_m64real(mr.seg, mr.offset, RD(0), c); fpu_pop(fpu); break;
        case 4: {                                                  // FRSTOR
          fpu_load_env(mr.seg, mr.offset, op_size_32);
          uint32_t roff = mr.offset + (op_size_32 ? 28u : 14u);
          // The register area is ST(0)-first; the tag word is not.  The tags
          // are whatever fpu_load_env just put there, in physical order, and
          // must NOT be recomputed from the values - a saved EMPTY slot holds
          // an arbitrary bit pattern and re-tagging it would resurrect it.
          for (int i = 0; i < 8 && !fault_abort(); i++)
            fpu.regs[(FPU_TOP + i) & 7] = fpu_read_m80real(mr.seg, roff + i * 10);
          track = false; c1_own = true;
          break;
        }
        case 6: {                                                  // FNSAVE
          fpu_store_env(mr.seg, mr.offset, op_size_32);
          uint32_t roff = mr.offset + (op_size_32 ? 28u : 14u);
          // Stop at the first fault rather than writing the remaining eighty
          // bytes: check_segment_write lets an access through once an
          // exception is already pending, so without this the rest of the
          // image lands wherever the address wrapped to.
          for (int i = 0; i < 8 && !fault_abort(); i++)
            fpu_write_m80real(mr.seg, roff + i * 10, fpu.regs[(FPU_TOP + i) & 7]);
          fpu_init();
          track = false;                                            // a control form
          reinit = true;                                            // state is reset
          break;
        }
        case 7: store_word(mr.seg, mr.offset, fpu.sw);             // FNSTSW m16
                track = false; c1_own = true; break;
        default: fpu_unhandled("DD", modrm_byte); break;
      }
    } else {
      uint8_t op2 = modrm_byte;
      if (op2 >= 0xC0 && op2 <= 0xC7) {                            // FFREE ST(i)
        TAGP(rm) = TAG_EMPTY;
      } else if (op2 >= 0xC8 && op2 <= 0xCF) {                     // FXCH (alias)
        f80 a = RD(0), b = RD(rm);
        WR(0, b); WR(rm, a);
      } else if (op2 >= 0xD0 && op2 <= 0xD7) {                     // FST ST(i)
        WR(rm, RD(0));
      } else if (op2 >= 0xD8 && op2 <= 0xDF) {                     // FSTP ST(i)
        WR(rm, RD(0));
        fpu_pop(fpu);
      } else if (op2 >= 0xE0 && op2 <= 0xE7) {                     // FUCOM ST(i)
        fpu_set_cc(fpu, f80_compare(RD(0), RD(rm), true, c));
      } else if (op2 >= 0xE8 && op2 <= 0xEF) {                     // FUCOMP ST(i)
        fpu_set_cc(fpu, f80_compare(RD(0), RD(rm), true, c));
        fpu_pop(fpu);
      } else {
        fpu_unhandled("DD", op2);
      }
    }
    break;
  }

  //=========================================================================
  // DE: m16int arithmetic / FADDP/FMULP/FCOMPP/FSUBP/FSUBRP/FDIVP/FDIVRP
  //=========================================================================
  case 6: {
    if (is_mem) {
      f80 val = f80_from_i16((int16_t)fetch_word(mr.seg, mr.offset));
      f80 st0 = RD(0);
      switch (reg) {
        case 0: WRR(0, f80_add(st0, val, c)); break;
        case 1: WRR(0, f80_mul(st0, val, c)); break;
        case 2: fpu_set_cc(fpu, f80_compare(st0, val, false, c)); break;
        case 3: fpu_set_cc(fpu, f80_compare(st0, val, false, c)); fpu_pop(fpu); break;
        case 4: WRR(0, f80_sub(st0, val, c)); break;
        case 5: WRR(0, f80_sub(val, st0, c)); break;
        case 6: WRR(0, f80_div(st0, val, c)); break;
        case 7: WRR(0, f80_div(val, st0, c)); break;
      }
    } else {
      f80 sti = RD(rm), st0 = RD(0);
      switch (reg) {
        case 0: WRR(rm, f80_add(sti, st0, c)); fpu_pop(fpu); break;      // FADDP
        case 1: WRR(rm, f80_mul(sti, st0, c)); fpu_pop(fpu); break;      // FMULP
        case 2: fpu_set_cc(fpu, f80_compare(st0, sti, false, c));       // FCOMP alias
                fpu_pop(fpu); break;
        case 3:                                                          // FCOMPP
          if (rm == 1) {
            fpu_set_cc(fpu, f80_compare(st0, sti, false, c));
            fpu_pop(fpu); fpu_pop(fpu);
          } else {
            // DE D8 and DE DA..DF are not FCOMPP and are not anything else.
            // They used to fall through as silent no-ops.
            fpu_unhandled("DE", modrm_byte);
          }
          break;
        case 4: WRR(rm, f80_sub(st0, sti, c)); fpu_pop(fpu); break;      // FSUBRP
        case 5: WRR(rm, f80_sub(sti, st0, c)); fpu_pop(fpu); break;      // FSUBP
        case 6: WRR(rm, f80_div(st0, sti, c)); fpu_pop(fpu); break;      // FDIVRP
        case 7: WRR(rm, f80_div(sti, st0, c)); fpu_pop(fpu); break;      // FDIVP
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
        case 0:                                                    // FILD m16int
          fpu_push(fpu, f80_from_i16((int16_t)fetch_word(mr.seg, mr.offset)), c, sf);
          break;
        case 1:                                                    // FISTTP m16int
          store_word(mr.seg, mr.offset, (uint16_t)(int16_t)f80_to_int(RD(0), 16, true, c));
          fpu_pop(fpu);
          break;
        case 2:                                                    // FIST m16int
          store_word(mr.seg, mr.offset, (uint16_t)(int16_t)f80_to_int(RD(0), 16, false, c));
          break;
        case 3:                                                    // FISTP m16int
          store_word(mr.seg, mr.offset, (uint16_t)(int16_t)f80_to_int(RD(0), 16, false, c));
          fpu_pop(fpu);
          break;
        case 4: {                                                  // FBLD m80bcd
          uint8_t d[10];
          for (int i = 0; i < 10; i++) d[i] = fetch_byte(mr.seg, mr.offset + i);
          if (fault_abort()) break;
          fpu_push(fpu, f80_from_bcd(d), c, sf);
          break;
        }
        case 5: {                                                  // FILD m64int
          uint32_t lo = fetch_dword(mr.seg, mr.offset);
          uint32_t hi = fetch_dword(mr.seg, mr.offset + 4);
          fpu_push(fpu, f80_from_i64((int64_t)(((uint64_t)hi << 32) | lo)), c, sf);
          break;
        }
        case 6: {                                                  // FBSTP m80bcd
          uint8_t d[10];
          f80_to_bcd(RD(0), d, c);
          for (int i = 0; i < 10 && !fault_abort(); i++)
            store_byte(mr.seg, mr.offset + i, d[i]);
          fpu_pop(fpu);
          break;
        }
        case 7: {                                                  // FISTP m64int
          uint64_t raw = (uint64_t)f80_to_int(RD(0), 64, false, c);
          store_dword(mr.seg, mr.offset, (uint32_t)raw);
          store_dword(mr.seg, mr.offset + 4, (uint32_t)(raw >> 32));
          fpu_pop(fpu);
          break;
        }
      }
    } else {
      uint8_t op2 = modrm_byte;
      if (op2 == 0xE0) {                                           // FNSTSW AX
        // Through set_reg16, which carries the same fault_abort() guard every
        // other integer-register write in this core has; the direct
        // `regs[reg_AX] = ...` this replaced was the one FPU site that wrote a
        // general-purpose register without it.
        set_reg16(reg_AX, fpu.sw);
        track = false; c1_own = true;
      } else if (op2 >= 0xC0 && op2 <= 0xC7) {
        // FFREEP ST(i): free, then pop.  Undocumented, and emitted by GCC and
        // DJGPP as a one-byte-cheaper way to discard ST(0); it used to be
        // reported as unhandled and did nothing, which left the stack one
        // deeper than the compiler believed.
        TAGP(rm) = TAG_EMPTY;
        fpu_pop(fpu);
      } else if (op2 >= 0xC8 && op2 <= 0xCF) {
        // FXCH7 ST(i): the third encoding of FXCH, after D9 C8-CF and
        // DD C8-CF.  Verified on the host - DF C9 exchanges ST(0) and ST(1)
        // and does not pop.
        f80 a = RD(0), b = RD(rm);
        WR(0, b); WR(rm, a);
      } else if (op2 >= 0xD0 && op2 <= 0xDF) {
        // FSTP8 (D0-D7) and FSTP9 (D8-DF): two more FSTP ST(i) aliases, both
        // popping.  Verified on the host - DF D1 and DF D9 both leave exactly
        // what the documented DD D9 leaves.
        WR(rm, RD(0));
        fpu_pop(fpu);
      } else if (op2 >= 0xE8 && op2 <= 0xEF) {                     // FUCOMIP
        fpu_cmp_eflags(f80_compare(RD(0), RD(rm), true, c));
        fpu_pop(fpu);
      } else if (op2 >= 0xF0 && op2 <= 0xF7) {                     // FCOMIP
        fpu_cmp_eflags(f80_compare(RD(0), RD(rm), false, c));
        fpu_pop(fpu);
      } else {
        fpu_unhandled("DF", op2);
      }
    }
    break;
  }

  default:
    break;
  }

  //=========================================================================
  // Commit: the exception flags, C1, and the last-instruction pointers
  //=========================================================================

  // Nothing above this line is allowed to stand if a memory operand faulted.
  // What this does NOT undo is the memory the instruction had already written
  // before the faulting access - a multi-dword store that faults halfway
  // leaves its first half behind, here as on the integer side of this core.
  if (is_mem && fault_abort()) { fpu = saved; return; }
  if (reinit) return;

  if (track) {
    fpu.fip = insn_ip;
    fpu.fcs = sregs[seg_CS];
    fpu.fop = (uint16_t)(((opcode & 7) << 8) | modrm_byte);
    if (is_mem) { fpu.fdp = mr.offset; fpu.fds = mr.seg; }
  }

  // A stack fault owns C1 whatever else the instruction wanted to put there:
  // set means overflow, clear means underflow.  Otherwise C1 is the
  // "result was rounded up" report, and it is CLEARED when nothing rounded -
  // which is most instructions, most of the time.
  if (sf != SF_NONE) {
    fpu.sw |= SW_SF;
    fpu.sw &= (uint16_t)~SW_C1;
    if (sf == SF_OVER) fpu.sw |= SW_C1;
  } else if (!c1_own) {
    fpu.sw &= (uint16_t)~SW_C1;
    if (c.c1) fpu.sw |= SW_C1;
  }

  // #D is the lowest-priority report here and a higher-priority one displaces
  // it.  The f80 primitives already do this within a single call, but two
  // paths reach this point with a stale DE beside a higher exception: the
  // memory-operand helpers raise DE into this same context BEFORE the
  // arithmetic decides to raise #IA or #Z, and a stack fault discards the
  // arithmetic's result entirely while its DE stays behind.  Measured on the
  // host: FLD m32 of a denormal onto a FULL stack reports IE alone, and FADD
  // m32 of a denormal with ST(0) empty reports IE alone, where the same FADD
  // against a live ST(0) reports DE|PE.
  {
    uint16_t f = (uint16_t)(c.flags & SW_EXC);
    if ((f & (SW_IE | SW_ZE)) || sf != SF_NONE) f &= (uint16_t)~SW_DE;
    fpu.sw |= f;
  }

  // ES latches when a raised exception is NOT masked, and B follows ES on a
  // 387.  Neither was ever set before.  Note what this still does not do:
  // there is no #MF delivery and no FERR pin here, so an unmasked exception
  // is visible to a program that polls FNSTSW and to nothing else.
  if ((fpu.sw & SW_EXC) & (uint16_t)~(fpu.cw & 0x003F)) fpu.sw |= SW_ES | SW_B;

  #undef RD
  #undef WR
  #undef WRR
  #undef TAGP
}
