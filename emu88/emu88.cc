#include "emu88.h"
#include <cstdio>
#include <cstdarg>
#include <cstring>

static emu88_trace dummy_trace;

void emu88_fatal(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fprintf(stderr, "\n");
}

//=============================================================================
// Constructor and initialization
//=============================================================================

emu88::emu88(emu88_mem *memory)
  : mem(memory),
    trace(&dummy_trace),
    debug(false),
    cycles(0),
    int_pending(false),
    int_vector(0),
    halted(false),
    in_exception(false),
    seg_override(-1),
    rep_prefix(REP_NONE),
    op_size_32(false),
    addr_size_32(false),
    gdtr_base(0), gdtr_limit(0),
    idtr_base(0), idtr_limit(0x3FF),
    cr0(0), cr2(0), cr3(0), cr4(0),
    ldtr(0), tr(0), cpl(0) {
  memset(regs, 0, sizeof(regs));
  memset(regs_hi, 0, sizeof(regs_hi));
  memset(sregs, 0, sizeof(sregs));
  memset(dr, 0, sizeof(dr));
  ip = 0;
  flags = 0x0002;  // bit 1 is always 1 on 8088
  eflags_hi = 0;
  memset(&ldtr_cache, 0, sizeof(ldtr_cache));
  memset(&tr_cache, 0, sizeof(tr_cache));
  init_seg_caches();
  setup_parity();
}

void emu88::setup_parity(void) {
  for (int i = 0; i < 256; i++) {
    int bits = 0;
    int v = i;
    while (v) {
      bits += v & 1;
      v >>= 1;
    }
    parity_table[i] = (bits & 1) ? 0 : 1;  // even parity = 1
  }
}

void emu88::reset(void) {
  memset(regs, 0, sizeof(regs));
  memset(regs_hi, 0, sizeof(regs_hi));
  memset(sregs, 0, sizeof(sregs));
  sregs[seg_CS] = 0xFFFF;  // 8088 starts at FFFF:0000
  ip = 0x0000;
  insn_ip = 0;
  flags = 0x0002;
  eflags_hi = 0;
  halted = false;
  int_pending = false;
  in_exception = false;
  in_double_fault = false;
  exception_pending = false;
  dpmi_exc_dispatched = false;
  exc_dispatch_trace = false;
  unreal_mode = false;
  gp_trace_count = 0;
  rm_trace_count = 0;
  dpmi_trace_func = 0;
  cycles = 0;
  op_size_32 = false;
  addr_size_32 = false;
  lock_ud = true;  // 386 behavior by default
  cpu_type = CPU_386;
  gdtr_base = 0; gdtr_limit = 0;
  idtr_base = 0; idtr_limit = 0x3FF;
  cr0 = 0; cr2 = 0; cr3 = 0; cr4 = 0;
  memset(dr, 0, sizeof(dr));
  ldtr = 0; tr = 0; cpl = 0;
  fpu_init();
  memset(&ldtr_cache, 0, sizeof(ldtr_cache));
  memset(&tr_cache, 0, sizeof(tr_cache));
  init_seg_caches();
}

//=============================================================================
// I/O ports - virtual, override in subclass
//=============================================================================

void emu88::port_out(emu88_uint16 port, emu88_uint8 value) {
  (void)port;
  (void)value;
}

emu88_uint8 emu88::port_in(emu88_uint16 port) {
  (void)port;
  return 0xFF;
}

void emu88::port_out16(emu88_uint16 port, emu88_uint16 value) {
  port_out(port, value & 0xFF);
  port_out(port + 1, (value >> 8) & 0xFF);
}

emu88_uint16 emu88::port_in16(emu88_uint16 port) {
  return port_in(port) | (emu88_uint16(port_in(port + 1)) << 8);
}

//=============================================================================
// Interrupt support
//=============================================================================

void emu88::do_interrupt(emu88_uint8 vector) {
  if (protected_mode()) {
    do_interrupt_pm(vector, false, 0);
    return;
  }
  // Real mode: IVT-based interrupt dispatch
  push_word(flags);
  push_word(sregs[seg_CS]);
  push_word(ip & 0xFFFF);
  clear_flag(FLAG_IF);
  clear_flag(FLAG_TF);
  emu88_uint32 vec_addr = emu88_uint32(vector) * 4;
  ip = mem->fetch_mem16(vec_addr);
  load_segment_real(seg_CS, mem->fetch_mem16(vec_addr + 2));
}

void emu88::request_int(emu88_uint8 vector) {
  int_pending = true;
  int_vector = vector;
}

bool emu88::check_interrupts(void) {
  if (int_pending && get_flag(FLAG_IF)) {
    int_pending = false;
    halted = false;
    do_interrupt(int_vector);
    return true;
  }
  return false;
}

void emu88::halt_cpu(void) {
  halted = true;
}

void emu88::unimplemented_opcode(emu88_uint8 opcode) {
  emu88_fatal("Unimplemented opcode 0x%02X at %04X:%04X", opcode, sregs[seg_CS], ip - 1);
  halted = true;
}

//=============================================================================
// Register access: 8-bit
//=============================================================================

emu88_uint8 emu88::get_reg8(emu88_uint8 r) const {
  // 0=AL, 1=CL, 2=DL, 3=BL, 4=AH, 5=CH, 6=DH, 7=BH
  if (r < 4) {
    return regs[r] & 0xFF;
  }
  return (regs[r - 4] >> 8) & 0xFF;
}

void emu88::set_reg8(emu88_uint8 r, emu88_uint8 val) {
  if (r < 4) {
    regs[r] = (regs[r] & 0xFF00) | val;
  } else {
    regs[r - 4] = (regs[r - 4] & 0x00FF) | (emu88_uint16(val) << 8);
  }
}

//=============================================================================
// Memory access (segment-aware)
//=============================================================================

emu88_uint16 emu88::default_segment(void) const {
  if (seg_override >= 0)
    return sregs[seg_override];
  return sregs[seg_DS];
}

bool emu88::check_segment_read(emu88_uint16 seg, emu88_uint32 off, emu88_uint8 width) {
  if (exception_pending) return true;
  if (!protected_mode() || v86_mode()) {
    // 286 real mode: fault on word/dword access crossing 64KB boundary
    // String ops wrap around instead of faulting on real 286 hardware
    if (!lock_ud && width > 1) {
      emu88_uint32 last_byte = (off & 0xFFFF) + (emu88_uint32)(width - 1);
      if (last_byte > 0xFFFF) {
        ip = insn_ip;
        raise_exception(13, 0);
        return false;
      }
    }
    return true;
  }
  for (int i = 0; i < 6; i++) {
    if (sregs[i] != seg) continue;
    if (!seg_cache[i].valid) {
      raise_exception(i == seg_SS ? 12 : 13, 0);
      return false;
    }
    if (!check_segment_limit(i, off, width)) {
      raise_exception(i == seg_SS ? 12 : 13, 0);
      return false;
    }
    return true;
  }
  return true;
}

bool emu88::check_segment_write(emu88_uint16 seg, emu88_uint32 off, emu88_uint8 width) {
  if (exception_pending) return true;
  if (!protected_mode() || v86_mode()) {
    if (!lock_ud && width > 1) {
      emu88_uint32 last_byte = (off & 0xFFFF) + (emu88_uint32)(width - 1);
      if (last_byte > 0xFFFF) {
        ip = insn_ip;
        raise_exception(13, 0);
        return false;
      }
    }
    return true;
  }
  for (int i = 0; i < 6; i++) {
    if (sregs[i] != seg) continue;
    if (!seg_cache[i].valid) {
      raise_exception(i == seg_SS ? 12 : 13, 0);
      return false;
    }
    emu88_uint8 type = seg_cache[i].access & 0x0F;
    // Code segment or read-only data segment → not writable
    if ((type & 0x08) || !(type & 0x02)) {
      raise_exception(i == seg_SS ? 12 : 13, 0);
      return false;
    }
    if (!check_segment_limit(i, off, width)) {
      raise_exception(i == seg_SS ? 12 : 13, 0);
      return false;
    }
    return true;
  }
  return true;
}

emu88_uint8 emu88::fetch_byte(emu88_uint16 seg, emu88_uint32 off) {
  if (!check_segment_read(seg, off, 1)) return 0;
  emu88_uint32 linear = effective_address(seg, off);
  emu88_uint32 phys = paging_enabled() ? translate_linear(linear, false) : mem->mask_addr(linear);
  return mem->fetch_mem(phys);
}

void emu88::store_byte(emu88_uint16 seg, emu88_uint32 off, emu88_uint8 val) {
  if (!check_segment_write(seg, off, 1)) return;
  emu88_uint32 linear = effective_address(seg, off);
  emu88_uint32 phys = paging_enabled() ? translate_linear(linear, true) : mem->mask_addr(linear);
  mem->store_mem(phys, val);
}

emu88_uint16 emu88::fetch_word(emu88_uint16 seg, emu88_uint32 off) {
  if (!check_segment_read(seg, off, 2)) return 0;
  emu88_uint32 linear = effective_address(seg, off);
  emu88_uint32 phys = paging_enabled() ? translate_linear(linear, false) : mem->mask_addr(linear);
  return mem->fetch_mem16(phys);
}

void emu88::store_word(emu88_uint16 seg, emu88_uint32 off, emu88_uint16 val) {
  if (!check_segment_write(seg, off, 2)) return;
  emu88_uint32 linear = effective_address(seg, off);
  emu88_uint32 phys = paging_enabled() ? translate_linear(linear, true) : mem->mask_addr(linear);
  mem->store_mem16(phys, val);
}

emu88_uint32 emu88::fetch_dword(emu88_uint16 seg, emu88_uint32 off) {
  if (!check_segment_read(seg, off, 4)) return 0;
  emu88_uint32 linear = effective_address(seg, off);
  emu88_uint32 phys = paging_enabled() ? translate_linear(linear, false) : mem->mask_addr(linear);
  return mem->fetch_mem32(phys);
}

void emu88::store_dword(emu88_uint16 seg, emu88_uint32 off, emu88_uint32 val) {
  if (!check_segment_write(seg, off, 4)) return;
  emu88_uint32 linear = effective_address(seg, off);
  emu88_uint32 phys = paging_enabled() ? translate_linear(linear, true) : mem->mask_addr(linear);
  mem->store_mem32(phys, val);
}

//=============================================================================
// Instruction stream
//=============================================================================

emu88_uint8 emu88::fetch_ip_byte(void) {
  emu88_uint8 val = fetch_byte(sregs[seg_CS], ip);
  ip++;
  if (!protected_mode()) ip &= 0xFFFF;
  return val;
}

emu88_uint16 emu88::fetch_ip_word(void) {
  emu88_uint8 lo = fetch_ip_byte();
  emu88_uint8 hi = fetch_ip_byte();
  return EMU88_MK16(lo, hi);
}

emu88_uint32 emu88::fetch_ip_dword(void) {
  emu88_uint16 lo = fetch_ip_word();
  emu88_uint16 hi = fetch_ip_word();
  return EMU88_MK32(lo, hi);
}

//=============================================================================
// Stack operations
//=============================================================================

void emu88::push_word(emu88_uint16 val) {
  if (stack_32()) {
    emu88_uint32 esp = get_esp() - 2;
    set_esp(esp);
    store_word(sregs[seg_SS], esp, val);
  } else {
    regs[reg_SP] -= 2;
    store_word(sregs[seg_SS], regs[reg_SP], val);
  }
}

emu88_uint16 emu88::pop_word(void) {
  if (stack_32()) {
    emu88_uint32 esp = get_esp();
    emu88_uint16 val = fetch_word(sregs[seg_SS], esp);
    if (!exception_pending) set_esp(esp + 2);
    return val;
  } else {
    emu88_uint16 val = fetch_word(sregs[seg_SS], regs[reg_SP]);
    if (!exception_pending) regs[reg_SP] += 2;
    return val;
  }
}

void emu88::push_dword(emu88_uint32 val) {
  if (stack_32()) {
    emu88_uint32 esp = get_esp() - 4;
    set_esp(esp);
    store_dword(sregs[seg_SS], esp, val);
  } else {
    regs[reg_SP] -= 4;
#ifdef PUSH_DEBUG
    if (cpl == 3) {
      emu88_uint32 lin = effective_address(sregs[seg_SS], regs[reg_SP]);
      fprintf(stderr, "[PUSH32] cpl=%d SS=%04X SP=%04X linear=%08X paging=%d cr0=%08X\n",
              cpl, sregs[seg_SS], regs[reg_SP], lin, paging_enabled(), cr0);
    }
#endif
    store_dword(sregs[seg_SS], regs[reg_SP], val);
  }
}

emu88_uint32 emu88::pop_dword(void) {
  if (stack_32()) {
    emu88_uint32 esp = get_esp();
    emu88_uint32 val = fetch_dword(sregs[seg_SS], esp);
    if (!exception_pending) set_esp(esp + 4);
    return val;
  } else {
    emu88_uint32 val = fetch_dword(sregs[seg_SS], regs[reg_SP]);
    if (!exception_pending) regs[reg_SP] += 4;
    return val;
  }
}

//=============================================================================
// ModR/M decoding
//=============================================================================

emu88::modrm_result emu88::decode_modrm(emu88_uint8 modrm) {
  if (addr_size_32)
    return decode_modrm_32(modrm);

  modrm_result mr;
  mr.mod_field = (modrm >> 6) & 3;
  mr.reg_field = (modrm >> 3) & 7;
  mr.rm_field = modrm & 7;
  mr.is_register = (mr.mod_field == 3);
  mr.seg = 0;
  mr.offset = 0;

  if (mr.is_register) {
    return mr;
  }

  // Default segment depends on r/m and mod
  emu88_uint16 base_seg = sregs[seg_DS];
  emu88_uint16 off = 0;

  switch (mr.rm_field) {
  case 0: off = regs[reg_BX] + regs[reg_SI]; break;
  case 1: off = regs[reg_BX] + regs[reg_DI]; break;
  case 2: off = regs[reg_BP] + regs[reg_SI]; base_seg = sregs[seg_SS]; break;
  case 3: off = regs[reg_BP] + regs[reg_DI]; base_seg = sregs[seg_SS]; break;
  case 4: off = regs[reg_SI]; break;
  case 5: off = regs[reg_DI]; break;
  case 6:
    if (mr.mod_field == 0) {
      // direct address
      off = fetch_ip_word();
    } else {
      off = regs[reg_BP];
      base_seg = sregs[seg_SS];
    }
    break;
  case 7: off = regs[reg_BX]; break;
  }

  if (mr.mod_field == 1) {
    emu88_int8 disp8 = (emu88_int8)fetch_ip_byte();
    off += disp8;
  } else if (mr.mod_field == 2) {
    emu88_uint16 disp16 = fetch_ip_word();
    off += disp16;
  }

  // Apply segment override if active
  mr.seg = (seg_override >= 0) ? sregs[seg_override] : base_seg;
  mr.offset = off;
  return mr;
}

emu88_uint8 emu88::get_rm8(const modrm_result &mr) {
  if (mr.is_register)
    return get_reg8(mr.rm_field);
  return fetch_byte(mr.seg, mr.offset);
}

void emu88::set_rm8(const modrm_result &mr, emu88_uint8 val) {
  if (mr.is_register)
    set_reg8(mr.rm_field, val);
  else
    store_byte(mr.seg, mr.offset, val);
}

emu88_uint16 emu88::get_rm16(const modrm_result &mr) {
  if (mr.is_register)
    return regs[mr.rm_field];
  return fetch_word(mr.seg, mr.offset);
}

void emu88::set_rm16(const modrm_result &mr, emu88_uint16 val) {
  if (mr.is_register)
    regs[mr.rm_field] = val;
  else
    store_word(mr.seg, mr.offset, val);
}

emu88_uint32 emu88::get_rm32(const modrm_result &mr) {
  if (mr.is_register)
    return get_reg32(mr.rm_field);
  return fetch_dword(mr.seg, mr.offset);
}

void emu88::set_rm32(const modrm_result &mr, emu88_uint32 val) {
  if (mr.is_register)
    set_reg32(mr.rm_field, val);
  else
    store_dword(mr.seg, mr.offset, val);
}

//=============================================================================
// 32-bit addressing mode decoding (386+, for 0x67 prefix)
//=============================================================================

emu88::modrm_result emu88::decode_modrm_32(emu88_uint8 modrm) {
  modrm_result mr;
  mr.mod_field = (modrm >> 6) & 3;
  mr.reg_field = (modrm >> 3) & 7;
  mr.rm_field = modrm & 7;
  mr.is_register = (mr.mod_field == 3);
  mr.seg = 0;
  mr.offset = 0;

  if (mr.is_register)
    return mr;

  emu88_uint32 off = 0;
  emu88_uint16 base_seg = sregs[seg_DS];

  if (mr.rm_field == 4) {
    // SIB byte follows
    emu88_uint8 sib = fetch_ip_byte();
    emu88_uint8 scale = (sib >> 6) & 3;
    emu88_uint8 index = (sib >> 3) & 7;
    emu88_uint8 base = sib & 7;

    if (base == 5 && mr.mod_field == 0) {
      off = fetch_ip_dword();
    } else {
      off = get_reg32(base);
      if (base == 4 || base == 5) base_seg = sregs[seg_SS];
    }
    if (index != 4) {
      off += get_reg32(index) << scale;
    }
  } else if (mr.rm_field == 5 && mr.mod_field == 0) {
    off = fetch_ip_dword();
  } else {
    off = get_reg32(mr.rm_field);
    if (mr.rm_field == 4 || mr.rm_field == 5)
      base_seg = sregs[seg_SS];
  }

  if (mr.mod_field == 1) {
    off += (emu88_int32)(emu88_int8)fetch_ip_byte();
  } else if (mr.mod_field == 2) {
    off += fetch_ip_dword();
  }

  mr.seg = (seg_override >= 0) ? sregs[seg_override] : base_seg;
  mr.offset = protected_mode() ? off : (off & 0xFFFF);
  return mr;
}

//=============================================================================
// Flags computation
//=============================================================================

void emu88::set_flags_zsp8(emu88_uint8 val) {
  set_flag_val(FLAG_ZF, val == 0);
  set_flag_val(FLAG_SF, (val & 0x80) != 0);
  set_flag_val(FLAG_PF, parity_table[val]);
}

void emu88::set_flags_zsp16(emu88_uint16 val) {
  set_flag_val(FLAG_ZF, val == 0);
  set_flag_val(FLAG_SF, (val & 0x8000) != 0);
  set_flag_val(FLAG_PF, parity_table[val & 0xFF]);
}

void emu88::set_flags_add8(emu88_uint8 a, emu88_uint8 b, emu88_uint8 carry) {
  emu88_uint16 result = emu88_uint16(a) + emu88_uint16(b) + carry;
  emu88_uint8 r8 = result & 0xFF;
  set_flags_zsp8(r8);
  set_flag_val(FLAG_CF, result > 0xFF);
  set_flag_val(FLAG_AF, ((a ^ b ^ r8) & 0x10) != 0);
  set_flag_val(FLAG_OF, ((a ^ r8) & (b ^ r8) & 0x80) != 0);
}

void emu88::set_flags_add16(emu88_uint16 a, emu88_uint16 b, emu88_uint16 carry) {
  emu88_uint32 result = emu88_uint32(a) + emu88_uint32(b) + carry;
  emu88_uint16 r16 = result & 0xFFFF;
  set_flags_zsp16(r16);
  set_flag_val(FLAG_CF, result > 0xFFFF);
  set_flag_val(FLAG_AF, ((a ^ b ^ r16) & 0x10) != 0);
  set_flag_val(FLAG_OF, ((a ^ r16) & (b ^ r16) & 0x8000) != 0);
}

void emu88::set_flags_sub8(emu88_uint8 a, emu88_uint8 b, emu88_uint8 borrow) {
  emu88_uint16 result = emu88_uint16(a) - emu88_uint16(b) - borrow;
  emu88_uint8 r8 = result & 0xFF;
  set_flags_zsp8(r8);
  set_flag_val(FLAG_CF, result > 0xFF);
  set_flag_val(FLAG_AF, ((a ^ b ^ r8) & 0x10) != 0);
  set_flag_val(FLAG_OF, ((a ^ b) & (a ^ r8) & 0x80) != 0);
}

void emu88::set_flags_sub16(emu88_uint16 a, emu88_uint16 b, emu88_uint16 borrow) {
  emu88_uint32 result = emu88_uint32(a) - emu88_uint32(b) - borrow;
  emu88_uint16 r16 = result & 0xFFFF;
  set_flags_zsp16(r16);
  set_flag_val(FLAG_CF, result > 0xFFFF);
  set_flag_val(FLAG_AF, ((a ^ b ^ r16) & 0x10) != 0);
  set_flag_val(FLAG_OF, ((a ^ b) & (a ^ r16) & 0x8000) != 0);
}

void emu88::set_flags_logic8(emu88_uint8 result) {
  set_flags_zsp8(result);
  clear_flag(FLAG_CF);
  clear_flag(FLAG_OF);
  clear_flag(FLAG_AF);
}

void emu88::set_flags_logic16(emu88_uint16 result) {
  set_flags_zsp16(result);
  clear_flag(FLAG_CF);
  clear_flag(FLAG_OF);
  clear_flag(FLAG_AF);
}

//--- 32-bit flag computation (386+) ---

void emu88::set_flags_zsp32(emu88_uint32 val) {
  set_flag_val(FLAG_ZF, val == 0);
  set_flag_val(FLAG_SF, (val & 0x80000000) != 0);
  set_flag_val(FLAG_PF, parity_table[val & 0xFF]);
}

void emu88::set_flags_add32(emu88_uint32 a, emu88_uint32 b, emu88_uint32 carry) {
  emu88_uint64 result = emu88_uint64(a) + emu88_uint64(b) + carry;
  emu88_uint32 r32 = (emu88_uint32)result;
  set_flags_zsp32(r32);
  set_flag_val(FLAG_CF, result > 0xFFFFFFFF);
  set_flag_val(FLAG_AF, ((a ^ b ^ r32) & 0x10) != 0);
  set_flag_val(FLAG_OF, ((a ^ r32) & (b ^ r32) & 0x80000000) != 0);
}

void emu88::set_flags_sub32(emu88_uint32 a, emu88_uint32 b, emu88_uint32 borrow) {
  emu88_uint64 result = emu88_uint64(a) - emu88_uint64(b) - borrow;
  emu88_uint32 r32 = (emu88_uint32)result;
  set_flags_zsp32(r32);
  set_flag_val(FLAG_CF, result > 0xFFFFFFFF);
  set_flag_val(FLAG_AF, ((a ^ b ^ r32) & 0x10) != 0);
  set_flag_val(FLAG_OF, ((a ^ b) & (a ^ r32) & 0x80000000) != 0);
}

void emu88::set_flags_logic32(emu88_uint32 result) {
  set_flags_zsp32(result);
  clear_flag(FLAG_CF);
  clear_flag(FLAG_OF);
  clear_flag(FLAG_AF);
}

//=============================================================================
// ALU helpers
//=============================================================================

emu88_uint8 emu88::alu_add8(emu88_uint8 a, emu88_uint8 b, emu88_uint8 carry) {
  set_flags_add8(a, b, carry);
  return (a + b + carry) & 0xFF;
}

emu88_uint16 emu88::alu_add16(emu88_uint16 a, emu88_uint16 b, emu88_uint16 carry) {
  set_flags_add16(a, b, carry);
  return (a + b + carry) & 0xFFFF;
}

emu88_uint8 emu88::alu_sub8(emu88_uint8 a, emu88_uint8 b, emu88_uint8 borrow) {
  set_flags_sub8(a, b, borrow);
  return (a - b - borrow) & 0xFF;
}

emu88_uint16 emu88::alu_sub16(emu88_uint16 a, emu88_uint16 b, emu88_uint16 borrow) {
  set_flags_sub16(a, b, borrow);
  return (a - b - borrow) & 0xFFFF;
}

emu88_uint8 emu88::alu_inc8(emu88_uint8 val) {
  emu88_uint8 result = val + 1;
  set_flags_zsp8(result);
  set_flag_val(FLAG_AF, (val & 0x0F) == 0x0F);
  set_flag_val(FLAG_OF, val == 0x7F);
  // CF not affected by INC
  return result;
}

emu88_uint8 emu88::alu_dec8(emu88_uint8 val) {
  emu88_uint8 result = val - 1;
  set_flags_zsp8(result);
  set_flag_val(FLAG_AF, (val & 0x0F) == 0x00);
  set_flag_val(FLAG_OF, val == 0x80);
  // CF not affected by DEC
  return result;
}

emu88_uint16 emu88::alu_inc16(emu88_uint16 val) {
  return val + 1;
  // INC r16 does not affect any flags on 8088
}

emu88_uint16 emu88::alu_dec16(emu88_uint16 val) {
  return val - 1;
  // DEC r16 does not affect any flags on 8088
}

//--- 32-bit ALU helpers (386+) ---

emu88_uint32 emu88::alu_add32(emu88_uint32 a, emu88_uint32 b, emu88_uint32 carry) {
  set_flags_add32(a, b, carry);
  return (emu88_uint32)(emu88_uint64(a) + b + carry);
}

emu88_uint32 emu88::alu_sub32(emu88_uint32 a, emu88_uint32 b, emu88_uint32 borrow) {
  set_flags_sub32(a, b, borrow);
  return (emu88_uint32)(emu88_uint64(a) - b - borrow);
}

emu88_uint32 emu88::alu_inc32(emu88_uint32 val) {
  emu88_uint32 result = val + 1;
  set_flags_zsp32(result);
  set_flag_val(FLAG_AF, (val & 0x0F) == 0x0F);
  set_flag_val(FLAG_OF, val == 0x7FFFFFFF);
  return result;
}

emu88_uint32 emu88::alu_dec32(emu88_uint32 val) {
  emu88_uint32 result = val - 1;
  set_flags_zsp32(result);
  set_flag_val(FLAG_AF, (val & 0x0F) == 0x00);
  set_flag_val(FLAG_OF, val == 0x80000000);
  return result;
}

//=============================================================================
// ALU dispatch (op: ADD=0, OR=1, ADC=2, SBB=3, AND=4, SUB=5, XOR=6, CMP=7)
//=============================================================================

emu88_uint8 emu88::do_alu8(emu88_uint8 op, emu88_uint8 a, emu88_uint8 b) {
  switch (op) {
  case 0: return alu_add8(a, b, 0);
  case 1: { emu88_uint8 r = a | b; set_flags_logic8(r); return r; }
  case 2: return alu_add8(a, b, get_flag(FLAG_CF) ? 1 : 0);
  case 3: return alu_sub8(a, b, get_flag(FLAG_CF) ? 1 : 0);
  case 4: { emu88_uint8 r = a & b; set_flags_logic8(r); return r; }
  case 5: return alu_sub8(a, b, 0);
  case 6: { emu88_uint8 r = a ^ b; set_flags_logic8(r); return r; }
  case 7: alu_sub8(a, b, 0); return a;  // CMP: flags only
  }
  return a;
}

emu88_uint16 emu88::do_alu16(emu88_uint8 op, emu88_uint16 a, emu88_uint16 b) {
  switch (op) {
  case 0: return alu_add16(a, b, 0);
  case 1: { emu88_uint16 r = a | b; set_flags_logic16(r); return r; }
  case 2: return alu_add16(a, b, get_flag(FLAG_CF) ? 1 : 0);
  case 3: return alu_sub16(a, b, get_flag(FLAG_CF) ? 1 : 0);
  case 4: { emu88_uint16 r = a & b; set_flags_logic16(r); return r; }
  case 5: return alu_sub16(a, b, 0);
  case 6: { emu88_uint16 r = a ^ b; set_flags_logic16(r); return r; }
  case 7: alu_sub16(a, b, 0); return a;
  }
  return a;
}

emu88_uint32 emu88::do_alu32(emu88_uint8 op, emu88_uint32 a, emu88_uint32 b) {
  switch (op) {
  case 0: return alu_add32(a, b, 0);
  case 1: { emu88_uint32 r = a | b; set_flags_logic32(r); return r; }
  case 2: return alu_add32(a, b, get_flag(FLAG_CF) ? 1 : 0);
  case 3: return alu_sub32(a, b, get_flag(FLAG_CF) ? 1 : 0);
  case 4: { emu88_uint32 r = a & b; set_flags_logic32(r); return r; }
  case 5: return alu_sub32(a, b, 0);
  case 6: { emu88_uint32 r = a ^ b; set_flags_logic32(r); return r; }
  case 7: alu_sub32(a, b, 0); return a;
  }
  return a;
}

//=============================================================================
// Shift/rotate operations
//=============================================================================

emu88_uint8 emu88::do_shift8(emu88_uint8 op, emu88_uint8 val, emu88_uint8 count) {
  if (cpu_type >= CPU_286) count &= 31;  // 286+ masks shift count to 5 bits
  if (count == 0) return val;
  emu88_uint8 result = val;
  emu88_uint8 cf;
  emu88_uint8 prev;
  for (emu88_uint8 i = 0; i < count; i++) {
    prev = result;
    switch (op) {
    case 0: // ROL
      cf = (result >> 7) & 1;
      result = (result << 1) | cf;
      set_flag_val(FLAG_CF, cf);
      break;
    case 1: // ROR
      cf = result & 1;
      result = (result >> 1) | (cf << 7);
      set_flag_val(FLAG_CF, cf);
      break;
    case 2: // RCL
      cf = (result >> 7) & 1;
      result = (result << 1) | (get_flag(FLAG_CF) ? 1 : 0);
      set_flag_val(FLAG_CF, cf);
      break;
    case 3: // RCR
      cf = result & 1;
      result = (result >> 1) | (get_flag(FLAG_CF) ? 0x80 : 0);
      set_flag_val(FLAG_CF, cf);
      break;
    case 4: // SHL/SAL
      cf = (result >> 7) & 1;
      result <<= 1;
      set_flag_val(FLAG_CF, cf);
      break;
    case 5: // SHR
      cf = result & 1;
      result >>= 1;
      set_flag_val(FLAG_CF, cf);
      break;
    case 6: // SETMO on 8088 (set minus one), SHL alias on 286+
      if (cpu_type == CPU_8088) {
        cf = 0;  // SETMO: CF always 0
        result = 0xFF;
      } else {
        cf = (result >> 7) & 1;
        result <<= 1;
      }
      set_flag_val(FLAG_CF, cf);
      break;
    case 7: // SAR
      cf = result & 1;
      result = (result >> 1) | (result & 0x80);
      set_flag_val(FLAG_CF, cf);
      break;
    }
  }
  if (count == 1 || !lock_ud) {
    // Single-bit: all CPUs compute OF. Multi-bit: 286 computes, 386 leaves undefined.
    switch (op) {
    case 0: case 2: case 4: // left shifts/rotates
      set_flag_val(FLAG_OF, ((result >> 7) & 1) != get_flag(FLAG_CF));
      break;
    case 6: // SHL alias (286+), SETMO (8088)
      if (cpu_type == CPU_8088)
        clear_flag(FLAG_OF);  // SETMO: OF always 0
      else
        set_flag_val(FLAG_OF, ((result >> 7) & 1) != get_flag(FLAG_CF));
      break;
    case 1: case 3: // right rotates
      set_flag_val(FLAG_OF, ((result >> 7) ^ ((result >> 6) & 1)) != 0);
      break;
    case 5: // SHR: OF = MSB of value before last shift
      set_flag_val(FLAG_OF, (prev >> 7) & 1);
      break;
    case 7: // SAR: OF always 0 (sign never changes)
      clear_flag(FLAG_OF);
      break;
    }
  }
  if (op >= 4) {
    set_flags_zsp8(result);
    if (cpu_type == CPU_8088 && op >= 5) {
      // 8088: SHR/SETMO/SAR clear AF; SHL keeps AF from set_flags
      clear_flag(FLAG_AF);
    } else if (!lock_ud) {
      // 286: SHL sets AF from result bit 4; SHR/SAR always set AF
      if (op == 4 || op == 6) set_flag_val(FLAG_AF, result & 0x10);
      else set_flag(FLAG_AF);
    }
  }
  return result;
}

emu88_uint16 emu88::do_shift16(emu88_uint8 op, emu88_uint16 val, emu88_uint8 count) {
  if (cpu_type >= CPU_286) count &= 31;  // 286+ masks shift count to 5 bits
  if (count == 0) return val;
  emu88_uint16 result = val;
  emu88_uint8 cf;
  emu88_uint16 prev;
  for (emu88_uint8 i = 0; i < count; i++) {
    prev = result;
    switch (op) {
    case 0: // ROL
      cf = (result >> 15) & 1;
      result = (result << 1) | cf;
      set_flag_val(FLAG_CF, cf);
      break;
    case 1: // ROR
      cf = result & 1;
      result = (result >> 1) | (emu88_uint16(cf) << 15);
      set_flag_val(FLAG_CF, cf);
      break;
    case 2: // RCL
      cf = (result >> 15) & 1;
      result = (result << 1) | (get_flag(FLAG_CF) ? 1 : 0);
      set_flag_val(FLAG_CF, cf);
      break;
    case 3: // RCR
      cf = result & 1;
      result = (result >> 1) | (get_flag(FLAG_CF) ? 0x8000 : 0);
      set_flag_val(FLAG_CF, cf);
      break;
    case 4: // SHL
      cf = (result >> 15) & 1;
      result <<= 1;
      set_flag_val(FLAG_CF, cf);
      break;
    case 5: // SHR
      cf = result & 1;
      result >>= 1;
      set_flag_val(FLAG_CF, cf);
      break;
    case 6: // SETMO on 8088, SHL alias on 286+
      if (cpu_type == CPU_8088) {
        cf = 0;
        result = 0xFFFF;
      } else {
        cf = (result >> 15) & 1;
        result <<= 1;
      }
      set_flag_val(FLAG_CF, cf);
      break;
    case 7: // SAR
      cf = result & 1;
      result = (result >> 1) | (result & 0x8000);
      set_flag_val(FLAG_CF, cf);
      break;
    }
  }
  if (count == 1 || !lock_ud) {
    switch (op) {
    case 0: case 2: case 4:
      set_flag_val(FLAG_OF, ((result >> 15) & 1) != get_flag(FLAG_CF));
      break;
    case 6:
      if (cpu_type == CPU_8088)
        clear_flag(FLAG_OF);
      else
        set_flag_val(FLAG_OF, ((result >> 15) & 1) != get_flag(FLAG_CF));
      break;
    case 1: case 3:
      set_flag_val(FLAG_OF, ((result >> 15) ^ ((result >> 14) & 1)) != 0);
      break;
    case 5:
      set_flag_val(FLAG_OF, (prev >> 15) & 1);
      break;
    case 7:
      clear_flag(FLAG_OF);
      break;
    }
  }
  if (op >= 4) {
    set_flags_zsp16(result);
    if (cpu_type == CPU_8088 && op >= 5) {
      clear_flag(FLAG_AF);
    } else if (!lock_ud) {
      if (op == 4 || op == 6) set_flag_val(FLAG_AF, result & 0x10);
      else set_flag(FLAG_AF);
    }
  }
  return result;
}

emu88_uint32 emu88::do_shift32(emu88_uint8 op, emu88_uint32 val, emu88_uint8 count) {
  count &= 31;  // 386 masks shift count to 5 bits
  if (count == 0) return val;
  emu88_uint32 result = val;
  emu88_uint8 cf;
  emu88_uint32 prev;
  for (emu88_uint8 i = 0; i < count; i++) {
    prev = result;
    switch (op) {
    case 0: // ROL
      cf = (result >> 31) & 1;
      result = (result << 1) | cf;
      set_flag_val(FLAG_CF, cf);
      break;
    case 1: // ROR
      cf = result & 1;
      result = (result >> 1) | (emu88_uint32(cf) << 31);
      set_flag_val(FLAG_CF, cf);
      break;
    case 2: // RCL
      cf = (result >> 31) & 1;
      result = (result << 1) | (get_flag(FLAG_CF) ? 1 : 0);
      set_flag_val(FLAG_CF, cf);
      break;
    case 3: // RCR
      cf = result & 1;
      result = (result >> 1) | (get_flag(FLAG_CF) ? 0x80000000u : 0);
      set_flag_val(FLAG_CF, cf);
      break;
    case 4: case 6: // SHL
      cf = (result >> 31) & 1;
      result <<= 1;
      set_flag_val(FLAG_CF, cf);
      break;
    case 5: // SHR
      cf = result & 1;
      result >>= 1;
      set_flag_val(FLAG_CF, cf);
      break;
    case 7: // SAR
      cf = result & 1;
      result = (result >> 1) | (result & 0x80000000u);
      set_flag_val(FLAG_CF, cf);
      break;
    }
  }
  if (count == 1 || !lock_ud) {
    switch (op) {
    case 0: case 2: case 4: case 6:
      set_flag_val(FLAG_OF, ((result >> 31) & 1) != (emu88_uint32)get_flag(FLAG_CF));
      break;
    case 1: case 3:
      set_flag_val(FLAG_OF, ((result >> 31) ^ ((result >> 30) & 1)) != 0);
      break;
    case 5:
      set_flag_val(FLAG_OF, (prev >> 31) & 1);
      break;
    case 7:
      clear_flag(FLAG_OF);
      break;
    }
  }
  if (op >= 4) {
    set_flags_zsp32(result);
    if (!lock_ud) {
      if (op == 4 || op == 6) set_flag_val(FLAG_AF, result & 0x10);
      else set_flag(FLAG_AF);
    }
  }
  return result;
}

//=============================================================================
// Group instruction helpers
//=============================================================================

void emu88::execute_grp1_rm8(const modrm_result &mr, emu88_uint8 imm) {
  emu88_uint8 op = mr.reg_field;
  emu88_uint8 val = get_rm8(mr);
  emu88_uint8 result = do_alu8(op, val, imm);
  if (op != 7) // CMP doesn't store result
    set_rm8(mr, result);
}

void emu88::execute_grp1_rm16(const modrm_result &mr, emu88_uint16 imm) {
  emu88_uint8 op = mr.reg_field;
  emu88_uint16 val = get_rm16(mr);
  if (exception_pending) return;
  emu88_uint16 result = do_alu16(op, val, imm);
  if (op != 7)
    set_rm16(mr, result);
}

void emu88::execute_grp2_rm8(const modrm_result &mr, emu88_uint8 count) {
  emu88_uint8 val = get_rm8(mr);
  emu88_uint8 result = do_shift8(mr.reg_field, val, count);
  set_rm8(mr, result);
}

void emu88::execute_grp2_rm16(const modrm_result &mr, emu88_uint8 count) {
  emu88_uint16 val = get_rm16(mr);
  if (exception_pending) return;
  emu88_uint16 result = do_shift16(mr.reg_field, val, count);
  set_rm16(mr, result);
}

void emu88::execute_grp3_rm8(emu88_uint8 modrm_byte) {
  modrm_result mr = decode_modrm(modrm_byte);
  emu88_uint8 val = get_rm8(mr);
  switch (mr.reg_field) {
  case 0: case 1: { // TEST r/m8, imm8
    emu88_uint8 imm = fetch_ip_byte();
    if (!lock_ud && (ip - insn_ip) > 10) { ip = insn_ip; raise_exception(13, 0); return; }
    set_flags_logic8(val & imm);
    break;
  }
  case 2: // NOT r/m8
    set_rm8(mr, ~val);
    break;
  case 3: { // NEG r/m8
    set_flags_sub8(0, val, 0);
    set_rm8(mr, (~val) + 1);
    set_flag_val(FLAG_CF, val != 0);
    break;
  }
  case 4: { // MUL r/m8 (70-77 cycles)
    emu88_uint16 result = emu88_uint16(regs[reg_AX] & 0xFF) * val;
    regs[reg_AX] = result;
    bool of_cf = (result & 0xFF00) != 0;
    set_flag_val(FLAG_CF, of_cf);
    set_flag_val(FLAG_OF, of_cf);
    if (cpu_type == CPU_8088) {
      set_flags_zsp8((result >> 8) & 0xFF);
      clear_flag(FLAG_AF);
    } else if (!lock_ud) {
      set_flags_zsp8((result >> 8) & 0xFF);
      set_flag(FLAG_AF);
    }
    cycles += 54;
    break;
  }
  case 5: { // IMUL r/m8 (80-98 cycles)
    emu88_int16 result = emu88_int16(emu88_int8(regs[reg_AX] & 0xFF)) * emu88_int8(val);
    regs[reg_AX] = emu88_uint16(result);
    bool of_cf = (result < -128 || result > 127);
    set_flag_val(FLAG_CF, of_cf);
    set_flag_val(FLAG_OF, of_cf);
    if (cpu_type == CPU_8088) {
      // 8088 IMULCOF microcode: ZERO->tmpB, LRCY tmpC, ADC tmpA, F
      // Flags come from: AH + sign_bit(AL)  (the sign-extension check)
      {
        emu88_uint8 ah = (emu88_uint16(result) >> 8) & 0xFF;
        emu88_uint8 al_sign = (emu88_uint16(result) >> 7) & 1;
        emu88_uint8 imulcof = (ah + al_sign) & 0xFF;
        set_flags_zsp8(imulcof);
        set_flag_val(FLAG_AF, (ah & 0xF) + al_sign > 0xF);
      }
    } else if (!lock_ud) {
      set_flags_zsp8((emu88_uint16(result) >> 8) & 0xFF);
      set_flag(FLAG_AF);
    }
    cycles += 64;
    break;
  }
  case 6: { // DIV r/m8 (80-90 cycles)
    if (val == 0) {
      ip = insn_ip; do_interrupt(0);
      return;
    }
    emu88_uint16 dividend = regs[reg_AX];
    emu88_uint16 quotient = dividend / val;
    if (quotient > 0xFF) {
      ip = insn_ip; do_interrupt(0);
      return;
    }
    emu88_uint8 remainder = dividend % val;
    set_reg8(reg_AL, quotient & 0xFF);
    set_reg8(reg_AH, remainder);
    if (!lock_ud) {
      set_flags_zsp8(remainder);
      if (cpu_type == CPU_8088) clear_flag(FLAG_AF);
      else set_flag(FLAG_AF);
    }
    cycles += 64;
    break;
  }
  case 7: { // IDIV r/m8 (101-112 cycles)
    if (val == 0) {
      ip = insn_ip; do_interrupt(0);
      return;
    }
    emu88_int16 dividend = emu88_int16(regs[reg_AX]);
    emu88_int16 divisor = emu88_int8(val);
    emu88_int16 quotient = dividend / divisor;
    if (quotient > 127 || quotient < -128) {
      ip = insn_ip; do_interrupt(0);
      return;
    }
    emu88_int8 remainder = dividend % divisor;
    set_reg8(reg_AL, emu88_uint8(quotient));
    set_reg8(reg_AH, emu88_uint8(remainder));
    if (!lock_ud) {
      set_flags_zsp8(emu88_uint8(remainder));
      if (cpu_type == CPU_8088) clear_flag(FLAG_AF);
      else set_flag(FLAG_AF);
    }
    cycles += 85;
    break;
  }
  }
}

void emu88::execute_grp3_rm16(emu88_uint8 modrm_byte) {
  modrm_result mr = decode_modrm(modrm_byte);
  emu88_uint16 val = get_rm16(mr);
  if (exception_pending) return;
  switch (mr.reg_field) {
  case 0: case 1: { // TEST r/m16, imm16
    emu88_uint16 imm = fetch_ip_word();
    if (!lock_ud && (ip - insn_ip) > 10) { ip = insn_ip; raise_exception(13, 0); return; }
    set_flags_logic16(val & imm);
    break;
  }
  case 2: // NOT r/m16
    set_rm16(mr, ~val);
    break;
  case 3: { // NEG r/m16
    set_flags_sub16(0, val, 0);
    set_rm16(mr, (~val) + 1);
    set_flag_val(FLAG_CF, val != 0);
    break;
  }
  case 4: { // MUL r/m16 (118-133 cycles)
    emu88_uint32 result = emu88_uint32(regs[reg_AX]) * val;
    regs[reg_AX] = result & 0xFFFF;
    regs[reg_DX] = (result >> 16) & 0xFFFF;
    bool of_cf = regs[reg_DX] != 0;
    set_flag_val(FLAG_CF, of_cf);
    set_flag_val(FLAG_OF, of_cf);
    if (cpu_type == CPU_8088) {
      set_flags_zsp16(regs[reg_DX]);
      clear_flag(FLAG_AF);
    } else if (!lock_ud) {
      set_flags_zsp16(regs[reg_DX]);
      set_flag(FLAG_AF);
    }
    cycles += 105;
    break;
  }
  case 5: { // IMUL r/m16 (128-154 cycles)
    emu88_int32 result = emu88_int32(emu88_int16(regs[reg_AX])) * emu88_int16(val);
    regs[reg_AX] = emu88_uint16(result);
    regs[reg_DX] = emu88_uint16(result >> 16);
    bool of_cf = (result < -32768 || result > 32767);
    set_flag_val(FLAG_CF, of_cf);
    set_flag_val(FLAG_OF, of_cf);
    if (cpu_type == CPU_8088) {
      // 8088 IMULCOF microcode: flags from DX + sign_bit(AX)
      emu88_uint16 ax_sign = (regs[reg_AX] >> 15) & 1;
      emu88_uint16 imulcof = regs[reg_DX] + ax_sign;
      set_flags_zsp16(imulcof);
      set_flag_val(FLAG_AF, (regs[reg_DX] & 0xF) + ax_sign > 0xF);
    } else if (!lock_ud) {
      set_flags_zsp16(regs[reg_DX]);
      set_flag(FLAG_AF);
    }
    cycles += 118;
    break;
  }
  case 6: { // DIV r/m16 (144-162 cycles)
    if (val == 0) {
      ip = insn_ip; do_interrupt(0);
      return;
    }
    emu88_uint32 dividend = (emu88_uint32(regs[reg_DX]) << 16) | regs[reg_AX];
    emu88_uint32 quotient = dividend / val;
    if (quotient > 0xFFFF) {
      ip = insn_ip; do_interrupt(0);
      return;
    }
    emu88_uint16 remainder = dividend % val;
    regs[reg_AX] = quotient & 0xFFFF;
    regs[reg_DX] = remainder;
    if (!lock_ud) {
      set_flags_zsp16(remainder);
      set_flag(FLAG_AF);
    }
    cycles += 130;
    break;
  }
  case 7: { // IDIV r/m16 (165-184 cycles)
    if (val == 0) {
      ip = insn_ip; do_interrupt(0);
      return;
    }
    emu88_int32 dividend = emu88_int32((emu88_uint32(regs[reg_DX]) << 16) | regs[reg_AX]);
    emu88_int32 divisor = emu88_int16(val);
    emu88_int32 quotient = dividend / divisor;
    if (quotient > 32767 || quotient < -32768) {
      ip = insn_ip; do_interrupt(0);
      return;
    }
    emu88_int16 remainder = dividend % divisor;
    regs[reg_AX] = emu88_uint16(quotient);
    regs[reg_DX] = emu88_uint16(remainder);
    if (!lock_ud) {
      set_flags_zsp16(emu88_uint16(remainder));
      set_flag(FLAG_AF);
    }
    cycles += 150;
    break;
  }
  }
}

void emu88::execute_grp4_rm8(emu88_uint8 modrm_byte) {
  modrm_result mr = decode_modrm(modrm_byte);
  emu88_uint8 val = get_rm8(mr);
  switch (mr.reg_field) {
  case 0: // INC r/m8
    set_rm8(mr, alu_inc8(val));
    break;
  case 1: // DEC r/m8
    set_rm8(mr, alu_dec8(val));
    break;
  default:
    do_interrupt(6);  // #UD - Invalid Opcode
    break;
  }
}

void emu88::execute_grp5_rm16(emu88_uint8 modrm_byte) {
  modrm_result mr = decode_modrm(modrm_byte);
  switch (mr.reg_field) {
  case 0: { // INC r/m16
    emu88_uint16 val = get_rm16(mr);
    if (exception_pending) break;
    emu88_uint16 result = val + 1;
    set_flags_zsp16(result);
    set_flag_val(FLAG_AF, (val & 0x0F) == 0x0F);
    set_flag_val(FLAG_OF, val == 0x7FFF);
    set_rm16(mr, result);
    break;
  }
  case 1: { // DEC r/m16
    emu88_uint16 val = get_rm16(mr);
    if (exception_pending) break;
    emu88_uint16 result = val - 1;
    set_flags_zsp16(result);
    set_flag_val(FLAG_AF, (val & 0x0F) == 0x00);
    set_flag_val(FLAG_OF, val == 0x8000);
    set_rm16(mr, result);
    break;
  }
  case 2: { // CALL r/m16 (near indirect)
    emu88_uint16 target = get_rm16(mr);
    if (exception_pending) break;
    push_word(ip);
    ip = target;
    break;
  }
  case 3: { // CALL m16:16 (far indirect)
    if (mr.is_register) { raise_exception_no_error(6); break; }
    emu88_uint16 off = fetch_word(mr.seg, mr.offset);
    if (exception_pending) break;
    emu88_uint16 seg = fetch_word(mr.seg, mr.offset + 2);
    if (exception_pending) break;
    far_call_or_jmp(seg, off, true);
    break;
  }
  case 4: { // JMP r/m16 (near indirect)
    emu88_uint16 target = get_rm16(mr);
    if (exception_pending) break;
    ip = target;
    break;
  }
  case 5: { // JMP m16:16 (far indirect)
    if (mr.is_register) { raise_exception_no_error(6); break; }
    emu88_uint16 off = fetch_word(mr.seg, mr.offset);
    if (exception_pending) break;
    emu88_uint16 seg = fetch_word(mr.seg, mr.offset + 2);
    if (exception_pending) break;
    far_call_or_jmp(seg, off, false);
    break;
  }
  case 6: // PUSH r/m16
  case 7: { // PUSH r/m16 (undocumented alias, 8088)
    emu88_uint16 val = get_rm16(mr);
    if (exception_pending) break;
    if (cpu_type == CPU_8088 && mr.is_register && mr.rm_field == reg_SP) {
      // 8088: PUSH SP via GRP5 pushes SP-2
      regs[reg_SP] -= 2;
      store_word(sregs[seg_SS], regs[reg_SP], regs[reg_SP]);
    } else {
      push_word(val);
    }
    break;
  }
  default:
    unimplemented_opcode(0xFF);
    break;
  }
}

//=============================================================================
// 32-bit group instruction helpers (386+)
//=============================================================================

void emu88::execute_grp1_rm32(const modrm_result &mr, emu88_uint32 imm) {
  emu88_uint8 op = mr.reg_field;
  emu88_uint32 val = get_rm32(mr);
  if (exception_pending) return;
  emu88_uint32 result = do_alu32(op, val, imm);
  if (op != 7) set_rm32(mr, result);
}

void emu88::execute_grp2_rm32(const modrm_result &mr, emu88_uint8 count) {
  emu88_uint32 val = get_rm32(mr);
  if (exception_pending) return;
  emu88_uint32 result = do_shift32(mr.reg_field, val, count);
  set_rm32(mr, result);
}

void emu88::execute_grp3_rm32(emu88_uint8 modrm_byte) {
  modrm_result mr = decode_modrm(modrm_byte);
  emu88_uint32 val = get_rm32(mr);
  if (exception_pending) return;
  switch (mr.reg_field) {
  case 0: case 1: { // TEST r/m32, imm32
    emu88_uint32 imm = fetch_ip_dword();
    set_flags_logic32(val & imm);
    break;
  }
  case 2: // NOT r/m32
    set_rm32(mr, ~val);
    break;
  case 3: { // NEG r/m32
    set_flags_sub32(0, val, 0);
    set_rm32(mr, (~val) + 1);
    set_flag_val(FLAG_CF, val != 0);
    break;
  }
  case 4: { // MUL r/m32 — EDX:EAX = EAX * r/m32
    emu88_uint64 result = emu88_uint64(get_reg32(reg_AX)) * val;
    set_reg32(reg_AX, (emu88_uint32)result);
    set_reg32(reg_DX, (emu88_uint32)(result >> 32));
    bool of_cf = get_reg32(reg_DX) != 0;
    set_flag_val(FLAG_CF, of_cf);
    set_flag_val(FLAG_OF, of_cf);
    break;
  }
  case 5: { // IMUL r/m32
    emu88_int64 result = emu88_int64(emu88_int32(get_reg32(reg_AX))) * emu88_int32(val);
    set_reg32(reg_AX, (emu88_uint32)result);
    set_reg32(reg_DX, (emu88_uint32)(result >> 32));
    bool of_cf = (result < -(emu88_int64)0x80000000LL || result > 0x7FFFFFFFLL);
    set_flag_val(FLAG_CF, of_cf);
    set_flag_val(FLAG_OF, of_cf);
    break;
  }
  case 6: { // DIV r/m32
    if (val == 0) { ip = insn_ip; do_interrupt(0); return; }
    emu88_uint64 dividend = (emu88_uint64(get_reg32(reg_DX)) << 32) | get_reg32(reg_AX);
    emu88_uint64 quotient = dividend / val;
    if (quotient > 0xFFFFFFFF) { ip = insn_ip; do_interrupt(0); return; }
    emu88_uint32 remainder = (emu88_uint32)(dividend % val);
    set_reg32(reg_AX, (emu88_uint32)quotient);
    set_reg32(reg_DX, remainder);
    break;
  }
  case 7: { // IDIV r/m32
    if (val == 0) { ip = insn_ip; do_interrupt(0); return; }
    emu88_int64 dividend = (emu88_int64)((emu88_uint64(get_reg32(reg_DX)) << 32) | get_reg32(reg_AX));
    emu88_int64 divisor = emu88_int32(val);
    emu88_int64 quotient = dividend / divisor;
    if (quotient > 0x7FFFFFFFLL || quotient < -(emu88_int64)0x80000000LL) { ip = insn_ip; do_interrupt(0); return; }
    emu88_int32 remainder = (emu88_int32)(dividend % divisor);
    set_reg32(reg_AX, (emu88_uint32)quotient);
    set_reg32(reg_DX, (emu88_uint32)remainder);
    break;
  }
  }
}

void emu88::execute_grp5_rm32(emu88_uint8 modrm_byte) {
  modrm_result mr = decode_modrm(modrm_byte);
  switch (mr.reg_field) {
  case 0: { // INC r/m32
    emu88_uint32 val = get_rm32(mr);
    if (exception_pending) break;
    set_rm32(mr, alu_inc32(val));
    break;
  }
  case 1: { // DEC r/m32
    emu88_uint32 val = get_rm32(mr);
    if (exception_pending) break;
    set_rm32(mr, alu_dec32(val));
    break;
  }
  case 2: { // CALL r/m32 (near indirect)
    emu88_uint32 target = get_rm32(mr);
    if (exception_pending) break;
    push_dword(ip);
    ip = protected_mode() ? target : (target & 0xFFFF);
    break;
  }
  case 3: { // CALL m16:32 (far indirect)
    emu88_uint32 off = fetch_dword(mr.seg, mr.offset);
    if (exception_pending) break;
    emu88_uint16 seg = fetch_word(mr.seg, mr.offset + 4);
    if (exception_pending) break;
    far_call_or_jmp(seg, off, true);
    break;
  }
  case 4: { // JMP r/m32 (near indirect)
    emu88_uint32 target = get_rm32(mr);
    if (exception_pending) break;
    ip = protected_mode() ? target : (target & 0xFFFF);
    break;
  }
  case 5: { // JMP m16:32 (far indirect)
    emu88_uint32 off = fetch_dword(mr.seg, mr.offset);
    if (exception_pending) break;
    emu88_uint16 seg = fetch_word(mr.seg, mr.offset + 4);
    if (exception_pending) break;
    far_call_or_jmp(seg, off, false);
    break;
  }
  case 6: { // PUSH r/m32
    emu88_uint32 val = get_rm32(mr);
    if (exception_pending) break;
    push_dword(val);
    break;
  }
  default:
    execute_grp5_rm16(modrm_byte);  // fall back to 16-bit
    break;
  }
}

//=============================================================================
// String operations
//=============================================================================

emu88_uint16 emu88::string_src_seg(void) const {
  if (seg_override >= 0)
    return sregs[seg_override];
  return sregs[seg_DS];
}

void emu88::execute_string_op(emu88_uint8 opcode) {
  emu88_int32 dir = get_flag(FLAG_DF) ? -1 : 1;
  bool a32 = addr_size_32;

  // Helpers for SI/DI/CX access respecting address size
  auto get_si = [&]() -> emu88_uint32 { return a32 ? get_reg32(reg_SI) : regs[reg_SI]; };
  auto get_di = [&]() -> emu88_uint32 { return a32 ? get_reg32(reg_DI) : regs[reg_DI]; };
  auto add_si = [&](emu88_int32 n) { if (a32) set_reg32(reg_SI, get_reg32(reg_SI) + n); else regs[reg_SI] += n; };
  auto add_di = [&](emu88_int32 n) { if (a32) set_reg32(reg_DI, get_reg32(reg_DI) + n); else regs[reg_DI] += n; };
  auto get_cx = [&]() -> emu88_uint32 { return a32 ? get_reg32(reg_CX) : regs[reg_CX]; };
  auto dec_cx = [&]() { if (a32) set_reg32(reg_CX, get_reg32(reg_CX) - 1); else regs[reg_CX]--; };

  // 286 real mode: word/dword accesses wrap at segment boundary instead of faulting.
  // The instruction completes with wrapping, then #GP(0) fires post-execution.
  bool str_boundary_crossed = false;
  bool str_boundary_dst = false;  // true if dest operand crossed (needs extra REP iter)
  auto str_fetch_word = [&](emu88_uint16 seg, emu88_uint32 off) -> emu88_uint16 {
    if (!lock_ud && (off & 0xFFFF) == 0xFFFF) {
      str_boundary_crossed = true;
      emu88_uint8 lo = fetch_byte(seg, 0xFFFF);
      emu88_uint8 hi = fetch_byte(seg, 0);
      return lo | ((emu88_uint16)hi << 8);
    }
    return fetch_word(seg, off);
  };
  auto str_store_word = [&](emu88_uint16 seg, emu88_uint32 off, emu88_uint16 val) {
    if (!lock_ud && (off & 0xFFFF) == 0xFFFF) {
      str_boundary_crossed = true;
      store_byte(seg, 0xFFFF, val & 0xFF);
      store_byte(seg, 0, (val >> 8) & 0xFF);
      return;
    }
    store_word(seg, off, val);
  };
  auto str_fetch_dword = [&](emu88_uint16 seg, emu88_uint32 off) -> emu88_uint32 {
    emu88_uint32 masked = off & 0xFFFF;
    if (!lock_ud && masked + 3 > 0xFFFF) {
      str_boundary_crossed = true;
      emu88_uint32 result = 0;
      for (int i = 0; i < 4; i++)
        result |= (emu88_uint32)fetch_byte(seg, (masked + i) & 0xFFFF) << (i * 8);
      return result;
    }
    return fetch_dword(seg, off);
  };
  auto str_store_dword = [&](emu88_uint16 seg, emu88_uint32 off, emu88_uint32 val) {
    emu88_uint32 masked = off & 0xFFFF;
    if (!lock_ud && masked + 3 > 0xFFFF) {
      str_boundary_crossed = true;
      for (int i = 0; i < 4; i++)
        store_byte(seg, (masked + i) & 0xFFFF, (val >> (i * 8)) & 0xFF);
      return;
    }
    store_dword(seg, off, val);
  };

  auto do_one = [&]() {
    switch (opcode) {
    case 0x6C: { // INSB (80186+)
      store_byte(sregs[seg_ES], get_di(), port_in(regs[reg_DX]));
      if (exception_pending) break;
      add_di(dir);
      break;
    }
    case 0x6D: { // INSW / INSD (80186+)
      str_boundary_crossed = false;
      if (op_size_32) {
        emu88_uint32 val = port_in16(regs[reg_DX]) | (emu88_uint32(port_in16(regs[reg_DX])) << 16);
        str_store_dword(sregs[seg_ES], get_di(), val);
        if (exception_pending) break;
        add_di(dir * 4);
      } else {
        emu88_uint16 val = port_in(regs[reg_DX]) | ((emu88_uint16)port_in(regs[reg_DX]) << 8);
        str_store_word(sregs[seg_ES], get_di(), val);
        if (exception_pending) break;
        add_di(dir * 2);
      }
      if (str_boundary_crossed) str_boundary_dst = true;
      break;
    }
    case 0x6E: { // OUTSB (80186+)
      emu88_uint8 val = fetch_byte(string_src_seg(), get_si());
      if (exception_pending) break;
      port_out(regs[reg_DX], val);
      add_si(dir);
      break;
    }
    case 0x6F: { // OUTSW / OUTSD (80186+)
      if (op_size_32) {
        str_boundary_crossed = false;
        emu88_uint32 val = str_fetch_dword(string_src_seg(), get_si());
        if (exception_pending) break;
        port_out16(regs[reg_DX], val & 0xFFFF);
        port_out16(regs[reg_DX], (val >> 16) & 0xFFFF);
        add_si(dir * 4);
      } else {
        str_boundary_crossed = false;
        emu88_uint16 val = str_fetch_word(string_src_seg(), get_si());
        if (exception_pending) break;
        port_out(regs[reg_DX], val & 0xFF);
        port_out(regs[reg_DX], (val >> 8) & 0xFF);
        add_si(dir * 2);
      }
      // str_boundary_crossed left set for caller to handle
      break;
    }
    case 0xA4: { // MOVSB
      emu88_uint8 val = fetch_byte(string_src_seg(), get_si());
      if (exception_pending) break;
      store_byte(sregs[seg_ES], get_di(), val);
      if (exception_pending) break;
      add_si(dir);
      add_di(dir);
      break;
    }
    case 0xA5: { // MOVSW / MOVSD
      if (op_size_32) {
        str_boundary_crossed = false;
        emu88_uint32 val = str_fetch_dword(string_src_seg(), get_si());
        if (exception_pending) break;
        bool src_crossed = str_boundary_crossed;
        add_si(dir * 4);
        if (src_crossed) { str_boundary_crossed = true; break; }
        str_boundary_crossed = false;
        str_store_dword(sregs[seg_ES], get_di(), val);
        if (exception_pending) break;
        add_di(dir * 4);
        // str_boundary_crossed left set if dest crossed
      } else {
        str_boundary_crossed = false;
        emu88_uint16 val = str_fetch_word(string_src_seg(), get_si());
        if (exception_pending) break;
        bool src_crossed = str_boundary_crossed;
        add_si(dir * 2);
        if (src_crossed) { str_boundary_crossed = true; str_boundary_dst = false; break; }
        str_boundary_crossed = false;
        str_store_word(sregs[seg_ES], get_di(), val);
        if (exception_pending) break;
        add_di(dir * 2);
        if (str_boundary_crossed) str_boundary_dst = true;
      }
      break;
    }
    case 0xA6: { // CMPSB
      emu88_uint8 src = fetch_byte(string_src_seg(), get_si());
      if (exception_pending) break;
      emu88_uint8 dst = fetch_byte(sregs[seg_ES], get_di());
      if (exception_pending) break;
      alu_sub8(src, dst, 0);
      add_si(dir);
      add_di(dir);
      break;
    }
    case 0xA7: { // CMPSW / CMPSD
      if (op_size_32) {
        str_boundary_crossed = false;
        emu88_uint32 src = str_fetch_dword(string_src_seg(), get_si());
        if (exception_pending) break;
        bool src_crossed = str_boundary_crossed;
        str_boundary_crossed = false;
        emu88_uint32 dst = str_fetch_dword(sregs[seg_ES], get_di());
        if (exception_pending) break;
        bool dst_crossed = str_boundary_crossed;
        if (dst_crossed) {
          add_di(dir * 4);
          ip = insn_ip; raise_exception(13, 0);
        } else if (src_crossed) {
          add_si(dir * 4); add_di(dir * 4);
          ip = insn_ip; raise_exception(13, 0);
        } else {
          alu_sub32(src, dst, 0);
          add_si(dir * 4); add_di(dir * 4);
        }
      } else {
        str_boundary_crossed = false;
        emu88_uint16 src = str_fetch_word(string_src_seg(), get_si());
        if (exception_pending) break;
        bool src_crossed = str_boundary_crossed;
        str_boundary_crossed = false;
        emu88_uint16 dst = str_fetch_word(sregs[seg_ES], get_di());
        if (exception_pending) break;
        bool dst_crossed = str_boundary_crossed;
        if (dst_crossed) {
          add_di(dir * 2);
          str_boundary_crossed = true;
          str_boundary_dst = true;
        } else if (src_crossed) {
          add_si(dir * 2); add_di(dir * 2);
          str_boundary_crossed = true;
          str_boundary_dst = false;
        } else {
          alu_sub16(src, dst, 0);
          add_si(dir * 2); add_di(dir * 2);
        }
      }
      break;
    }
    case 0xAA: { // STOSB
      store_byte(sregs[seg_ES], get_di(), regs[reg_AX] & 0xFF);
      if (exception_pending) break;
      add_di(dir);
      break;
    }
    case 0xAB: { // STOSW / STOSD
      str_boundary_crossed = false;
      if (op_size_32) {
        str_store_dword(sregs[seg_ES], get_di(), get_reg32(reg_AX));
        if (exception_pending) break;
        add_di(dir * 4);
      } else {
        str_store_word(sregs[seg_ES], get_di(), regs[reg_AX]);
        if (exception_pending) break;
        add_di(dir * 2);
      }
      if (str_boundary_crossed) str_boundary_dst = true;
      break;
    }
    case 0xAC: { // LODSB
      emu88_uint8 val = fetch_byte(string_src_seg(), get_si());
      if (exception_pending) break;
      set_reg8(reg_AL, val);
      add_si(dir);
      break;
    }
    case 0xAD: { // LODSW / LODSD
      if (op_size_32) {
        str_boundary_crossed = false;
        emu88_uint32 val = str_fetch_dword(string_src_seg(), get_si());
        if (exception_pending) break;
        if (!str_boundary_crossed) set_reg32(reg_AX, val);
        add_si(dir * 4);
      } else {
        str_boundary_crossed = false;
        emu88_uint16 val = str_fetch_word(string_src_seg(), get_si());
        if (exception_pending) break;
        if (!str_boundary_crossed) regs[reg_AX] = val;
        add_si(dir * 2);
      }
      // str_boundary_crossed left set for caller to handle
      break;
    }
    case 0xAE: { // SCASB
      emu88_uint8 val = fetch_byte(sregs[seg_ES], get_di());
      if (exception_pending) break;
      alu_sub8(regs[reg_AX] & 0xFF, val, 0);
      add_di(dir);
      break;
    }
    case 0xAF: { // SCASW / SCASD
      if (op_size_32) {
        str_boundary_crossed = false;
        emu88_uint32 val = str_fetch_dword(sregs[seg_ES], get_di());
        if (exception_pending) break;
        if (!str_boundary_crossed) alu_sub32(get_reg32(reg_AX), val, 0);
        add_di(dir * 4);
      } else {
        str_boundary_crossed = false;
        emu88_uint16 val = str_fetch_word(sregs[seg_ES], get_di());
        if (exception_pending) break;
        if (!str_boundary_crossed) alu_sub16(regs[reg_AX], val, 0);
        else str_boundary_dst = true;
        add_di(dir * 2);
      }
      break;
    }
    }
  };

  if (rep_prefix == REP_NONE) {
    do_one();
    // 286 boundary crossing: fire exception for non-REP
    if (!lock_ud && str_boundary_crossed && !exception_pending) {
      ip = insn_ip;
      raise_exception(13, 0);
    }
  } else {
    while (get_cx() != 0) {
      str_boundary_dst = false;
      do_one();
      if (exception_pending) break;
      // 286 boundary crossing in REP
      if (!lock_ud && str_boundary_crossed) {
        bool is_compare = (opcode == 0xA6 || opcode == 0xA7 || opcode == 0xAE || opcode == 0xAF);
        if (is_compare) {
          dec_cx();
          cycles += 17;
          ip = insn_ip;
          raise_exception(13, 0);
          break;
        }
        dec_cx();
        cycles += 17;
        if (str_boundary_dst) {
          // Dest crossing: extra CX decrement without running another iteration
          dec_cx();
          cycles += 17;
        }
        ip = insn_ip;
        raise_exception(13, 0);
        break;
      }
      dec_cx();
      cycles += 17;
      if (opcode == 0xA6 || opcode == 0xA7 || opcode == 0xAE || opcode == 0xAF) {
        if (rep_prefix == REP_REPZ && !get_flag(FLAG_ZF))
          break;
        if (rep_prefix == REP_REPNZ && get_flag(FLAG_ZF))
          break;
      }
    }
  }
}

//=============================================================================
// Debug
//=============================================================================

void emu88::debug_dump_regs(const char *label) {
  (void)label;
}

//=============================================================================
// Main instruction execution
//=============================================================================

// Approximate 8088 cycle counts per opcode.
// Memory operands cost more but this uses averages.
// Variable-cost instructions (MUL, DIV, REP string) are adjusted in handlers.
static const uint8_t base_cycles[256] = {
  // 0x00-0x07: ALU r/m,r (16), PUSH ES (14)
  16, 16, 10, 10,  4,  4, 14, 12,
  // 0x08-0x0F: OR r/m,r (16), PUSH CS (14), 0x0F prefix (4)
  16, 16, 10, 10,  4,  4, 14,  4,
  // 0x10-0x17: ADC r/m,r (16), PUSH SS (14)
  16, 16, 10, 10,  4,  4, 14, 12,
  // 0x18-0x1F: SBB r/m,r (16), POP DS (12)
  16, 16, 10, 10,  4,  4, 14, 12,
  // 0x20-0x27: AND r/m,r (16), DAA (4)
  16, 16, 10, 10,  4,  4,  2,  4,
  // 0x28-0x2F: SUB r/m,r (16), DAS (4)
  16, 16, 10, 10,  4,  4,  2,  4,
  // 0x30-0x37: XOR r/m,r (16), AAA (8)
  16, 16, 10, 10,  4,  4,  2,  8,
  // 0x38-0x3F: CMP r/m,r (16), AAS (8)
  16, 16, 10, 10,  4,  4,  2,  8,
  // 0x40-0x47: INC r16 (2)
   2,  2,  2,  2,  2,  2,  2,  2,
  // 0x48-0x4F: DEC r16 (2)
   2,  2,  2,  2,  2,  2,  2,  2,
  // 0x50-0x57: PUSH r16 (11)
  11, 11, 11, 11, 11, 11, 11, 11,
  // 0x58-0x5F: POP r16 (12)
  12, 12, 12, 12, 12, 12, 12, 12,
  // 0x60-0x67: 186+ PUSHA/POPA/BOUND/ARPL/FS/GS/OpSz/AdSz
   4,  4,  4,  4,  4,  4,  4,  4,
  // 0x68-0x6F: 186+ PUSH imm/IMUL/PUSH/IMUL/INS/OUTS
   4,  4,  4,  4,  4,  4,  4,  4,
  // 0x70-0x7F: Jcc short (taken=16, not-taken=4, use average 10)
  10, 10, 10, 10, 10, 10, 10, 10,
  10, 10, 10, 10, 10, 10, 10, 10,
  // 0x80-0x83: GRP1 r/m, imm (17 mem, 4 reg, avg 10)
  10, 10, 10, 10,
  // 0x84-0x87: TEST/XCHG r/m,r (13 avg)
  13, 13, 17, 17,
  // 0x88-0x8B: MOV r/m,r or r,r/m (10 mem, 2 reg, avg 6)
  10, 10,  8,  8,
  // 0x8C-0x8F: MOV r/m,sreg / LEA / MOV sreg,r/m / POP r/m
  10,  2, 10, 17,
  // 0x90-0x97: NOP (3), XCHG AX,r (3)
   3,  3,  3,  3,  3,  3,  3,  3,
  // 0x98-0x9F: CBW(2) CWD(5) CALL far(28) WAIT(4) PUSHF(10) POPF(8) SAHF(4) LAHF(4)
   2,  5, 28,  4, 10,  8,  4,  4,
  // 0xA0-0xA3: MOV AL/AX,[addr] / MOV [addr],AL/AX (10)
  10, 10, 10, 10,
  // 0xA4-0xA7: MOVS/CMPS (18 per iteration, REP adjusted separately)
  18, 18, 22, 22,
  // 0xA8-0xAB: TEST AL/AX,imm (4), STOS (11)
   4,  4, 11, 11,
  // 0xAC-0xAF: LODS (12), SCAS (15)
  12, 12, 15, 15,
  // 0xB0-0xB7: MOV r8, imm8 (4)
   4,  4,  4,  4,  4,  4,  4,  4,
  // 0xB8-0xBF: MOV r16, imm16 (4)
   4,  4,  4,  4,  4,  4,  4,  4,
  // 0xC0-0xC3: GRP2 r/m,imm8(186+)/RET/RET
   8, 12, 16, 16,
  // 0xC4-0xC7: LES(16) LDS(16) MOV r/m,imm(10)
  16, 16, 10, 10,
  // 0xC8-0xCF: 186+/186+/RETF(26)/RETF(25)/INT3(52)/INT(51)/INTO/IRET(32)
   4,  4, 26, 25, 52, 51, 53, 32,
  // 0xD0-0xD3: GRP2 r/m,1 (8 avg) / GRP2 r/m,CL (12 avg)
   8,  8, 12, 12,
  // 0xD4-0xD7: AAM(83) AAD(60) SALC(4) XLAT(11)
  83, 60,  4, 11,
  // 0xD8-0xDF: ESC/FPU (2, no FPU)
   2,  2,  2,  2,  2,  2,  2,  2,
  // 0xE0-0xE3: LOOPNZ(19) LOOPZ(18) LOOP(17) JCXZ(18)
  19, 18, 17, 18,
  // 0xE4-0xE7: IN(10) IN(10) OUT(10) OUT(10)
  10, 10, 10, 10,
  // 0xE8-0xEB: CALL near(19) JMP near(15) JMP far(15) JMP short(15)
  19, 15, 15, 15,
  // 0xEC-0xEF: IN DX(8) IN DX(8) OUT DX(8) OUT DX(8)
   8,  8,  8,  8,
  // 0xF0-0xF3: LOCK(2) undef(4) REPNZ(2) REPZ(2)
   2,  4,  2,  2,
  // 0xF4-0xF7: HLT(2) CMC(2) GRP3 r/m8(varies) GRP3 r/m16(varies)
   2,  2, 16, 20,
  // 0xF8-0xFF: CLC(2) STC(2) CLI(2) STI(2) CLD(2) STD(2) GRP4(varies) GRP5(varies)
   2,  2,  2,  2,  2,  2, 15, 15
};

void emu88::execute(void) {
  seg_override = -1;
  rep_prefix = REP_NONE;

  // Default operand/address size depends on code segment D/B bit
  // In V86 mode, always default to 16-bit (like real mode)
  bool default_32 = v86_mode() ? false : code_32();
  op_size_32 = default_32;
  addr_size_32 = default_32;

  // IVT[21h] write trap: log CS:IP of the instruction that modified IVT[21h]
  if (mem->ivt21_trap) {
    mem->ivt21_trap = false;
    uint32_t code_base = seg_cache[seg_CS].base;
    fprintf(stderr, "[IVT21-TRAP] Next insn at %04X:%08X (PM=%d) DS=%04X(base=%08X) ES=%04X(base=%08X)\n",
            sregs[seg_CS], ip, (cr0 & CR0_PE) ? 1 : 0,
            sregs[seg_DS], seg_cache[seg_DS].base,
            sregs[seg_ES], seg_cache[seg_ES].base);
    fprintf(stderr, "  Code bytes around (prev insn -> current): ");
    for (int i = -16; i < 8; i++) {
      if (i == 0) fprintf(stderr, "| ");
      fprintf(stderr, "%02X ", mem->fetch_mem(code_base + ip + i));
    }
    fprintf(stderr, "\n  EDI=%08X ESI=%08X ECX=%08X EAX=%08X\n",
            get_reg32(reg_DI), get_reg32(reg_SI), get_reg32(reg_CX), get_reg32(reg_AX));
  }

  // Save instruction start IP for fault-type exceptions (before prefixes)
  insn_ip = ip;
  exception_pending = false;
  dpmi_exc_dispatched = false;

  // TEMP: trace movedata caller to find source of invalid selector 0x534F
  if ((cr0 & CR0_PE) && sregs[seg_CS] == 0x002C) {
    uint32_t eip = ip;
    // First movedata call: MOV [ESP], EAX at 035086 (sets selector arg)
    if (eip == 0x035086) {
      fprintf(stderr, "[MOVEDATA-TRACE] 1st call: src_sel EAX=0x%08X EDX=0x%08X [EDX+26]_addr=0x%08X\n",
              get_reg32(reg_AX), get_reg32(reg_DX),
              seg_cache[seg_DS].base + get_reg32(reg_DX) + 0x26);
    }
    // After first movedata returns, check local var at [EBP-1A]
    if (eip == 0x03508E) {
      uint32_t ebp = get_reg32(reg_BP);
      uint32_t local_addr = seg_cache[seg_DS].base + ebp - 0x1A;
      uint16_t val = mem->fetch_mem16(local_addr);
      fprintf(stderr, "[MOVEDATA-TRACE] After 1st call: [EBP-1A] = 0x%04X (phys=0x%08X) EBP=0x%08X\n",
              val, local_addr, ebp);
    }
    // Second movedata setup: MOVSX EAX, [EBP-1A] at 0350B0
    if (eip == 0x0350B0) {
      uint32_t ebp = get_reg32(reg_BP);
      uint32_t local_addr = seg_cache[seg_DS].base + ebp - 0x1A;
      uint16_t val = mem->fetch_mem16(local_addr);
      fprintf(stderr, "[MOVEDATA-TRACE] 2nd call src_sel: [EBP-1A] = 0x%04X (will be used as selector!)\n", val);
    }
  }

  // Debug: trace ESP after DPMI exception dispatch
  if (exc_dispatch_trace) {
    exc_dispatch_trace = false;
    fprintf(stderr, "[EXC-TRACE] First insn after dispatch: %04X:%08X SP=%04X ESP=%08X SS=%04X SS.base=%08X SS.B=%d\n",
            sregs[seg_CS], ip, regs[reg_SP], get_esp(), sregs[seg_SS],
            seg_cache[seg_SS].base, (seg_cache[seg_SS].flags >> 2) & 1);
  }

  // Debug: trace DOS4GW handler stubs AND common handler
  if (sregs[seg_CS] == 0x000C && ((ip >= 0x6AB7 && ip <= 0x6B2B) || (ip >= 0x6D00 && ip <= 0x6D70))) {
    static int handler_trace = 0;
    if (handler_trace < 300) {
      handler_trace++;
      uint32_t lin = seg_cache[seg_CS].base + ip;
      fprintf(stderr, "[HTR] %04X:%04X SP=%04X ESP=%08X SS=%04X op:", sregs[seg_CS], (uint16_t)ip, regs[reg_SP], get_esp(), sregs[seg_SS]);
      for (int i = 0; i < 6; i++) fprintf(stderr, " %02X", mem->fetch_mem(lin + i));
      fprintf(stderr, "\n");
    }
  }

  // Debug: detect CPL != CS.RPL (should never happen on real x86)
  if (protected_mode() && !v86_mode() && cpl != (sregs[seg_CS] & 3)) {
    static int cpl_desync = 0;
    if (cpl_desync < 5) {
      cpl_desync++;
      fprintf(stderr, "[CPL-DESYNC] cpl=%d CS=%04X(RPL=%d) at %04X:%08X\n",
              cpl, sregs[seg_CS], sregs[seg_CS] & 3, sregs[seg_CS], ip);
    }
  }

  // INT 2Fh AX=1687h post-return trace: log registers after DPMI detection handler returns
  if (int2f_1687_trace_pending && sregs[seg_CS] == int2f_trace_ret_cs && ip == int2f_trace_ret_ip) {
    int2f_1687_trace_pending = false;
    fprintf(stderr, "[DPMI-DETECT-RET] INT 2Fh AX=1687h handler returned:\n");
    fprintf(stderr, "[DPMI-DETECT-RET]   AX=%04X BX=%04X CX=%04X DX=%04X SI=%04X DI=%04X\n",
            regs[reg_AX], regs[reg_BX], regs[reg_CX], regs[reg_DX],
            regs[reg_SI], regs[reg_DI]);
    fprintf(stderr, "[DPMI-DETECT-RET]   ES=%04X DS=%04X SS=%04X CS=%04X IP=%04X\n",
            sregs[seg_ES], sregs[seg_DS], sregs[seg_SS], sregs[seg_CS], ip);
    fprintf(stderr, "[DPMI-DETECT-RET]   AX=0 means DPMI available; ES:DI = DPMI entry point\n");
    fprintf(stderr, "[DPMI-DETECT-RET]   DPMI entry = %04X:%04X, version=%d.%d, flags=%04X\n",
            sregs[seg_ES], regs[reg_DI],
            regs[reg_DX] >> 8, regs[reg_DX] & 0xFF,
            regs[reg_BX]);
    fprintf(stderr, "[DPMI-DETECT-RET]   Processor type=%02X, PM switch buf size=%04X paras\n",
            regs[reg_CX] & 0xFF, regs[reg_SI]);
  }

  // DPMI post-call trace: check if we've returned from a traced INT 31h call
  if (dpmi_trace_func && sregs[seg_CS] == dpmi_trace_ret_cs && ip == dpmi_trace_ret_eip) {
    uint16_t traced_func = dpmi_trace_func;
    dpmi_trace_func = 0;  // clear before logging
    bool cf = (flags & FLAG_CF) != 0;
    if (traced_func == 0x0302) {
      fprintf(stderr, "[DPMI-RET] 0302h (file read) returned, CF=%d\n", cf);
      // Dump read buffer and data segment state
      if (sregs[seg_DS] == 0x00A7) {
        uint32_t b = seg_cache[seg_DS].base;
        fprintf(stderr, "[POST-READ] [002E]=%02X [0098]=%08X [009C]=%08X\n",
                mem->fetch_mem(b + 0x2E),
                mem->fetch_mem32(b + 0x98), mem->fetch_mem32(b + 0x9C));
        fprintf(stderr, "[POST-READ] Buf[1394..13D3]:");
        for (int i = 0; i < 64; i++) {
          if (i % 16 == 0) fprintf(stderr, "\n  +%04X:", 0x1394 + i);
          fprintf(stderr, " %02X", mem->fetch_mem(b + 0x1394 + i));
        }
        fprintf(stderr, "\n");
      }
      // Trace post-read code execution (disabled for now)
      // gp_trace_count = 2000;
    } else if (traced_func == 0x0501) {
      fprintf(stderr, "[DPMI-RET] 0501h: CF=%d BX:CX=%04X:%04X (linear=%08X) SI:DI=%04X:%04X\n",
              cf, regs[reg_BX], regs[reg_CX],
              ((uint32_t)regs[reg_BX] << 16) | regs[reg_CX],
              regs[reg_SI], regs[reg_DI]);
      // Dump DS=00A7 data segment state at 0501h return
      if (sregs[seg_DS] == 0x00A7) {
        uint32_t b = seg_cache[seg_DS].base;
        fprintf(stderr, "[0501-STATE] base=%08X [002E]=%02X [002F]=%02X\n",
                b, mem->fetch_mem(b + 0x2E), mem->fetch_mem(b + 0x2F));
        fprintf(stderr, "[0501-STATE] [0098]=%08X [009C]=%08X\n",
                mem->fetch_mem32(b + 0x98), mem->fetch_mem32(b + 0x9C));
        fprintf(stderr, "[0501-STATE] Read buf [1394..13A3]:");
        for (int i = 0; i < 16; i++)
          fprintf(stderr, " %02X", mem->fetch_mem(b + 0x1394 + i));
        fprintf(stderr, "\n");
      }
      // Trace disabled for now
      // gp_trace_count = 5000;
    } else if (traced_func == 0x0500) {
      fprintf(stderr, "[DPMI-RET] 0500h: CF=%d  Free memory info buffer:\n", cf);
      uint32_t buf = dpmi_trace_es_base + dpmi_trace_edi;
      for (int i = 0; i < 48; i += 4) {
        uint32_t val = read_linear32(buf + i);
        fprintf(stderr, "  [%02X] = %08X", i, val);
        if (i == 0x00) fprintf(stderr, " (largest free block bytes)");
        if (i == 0x04) fprintf(stderr, " (max unlocked pages)");
        if (i == 0x08) fprintf(stderr, " (max locked pages)");
        if (i == 0x0C) fprintf(stderr, " (linear addr space pages)");
        if (i == 0x10) fprintf(stderr, " (total unlocked pages)");
        if (i == 0x14) fprintf(stderr, " (total free pages)");
        if (i == 0x18) fprintf(stderr, " (total physical pages)");
        if (i == 0x1C) fprintf(stderr, " (free linear addr pages)");
        if (i == 0x20) fprintf(stderr, " (paging file pages)");
        fprintf(stderr, "\n");
      }
    }
  }

  // Breakpoints for [002E] write code paths in selector 0087
  if (cpl == 3 && sregs[seg_CS] == 0x0087) {
    uint32_t eip = ip;
    if (eip == 0x4512 || eip == 0x4553 || eip == 0x650B || eip == 0x774C) {
      fprintf(stderr, "[BRKPT] Hit 0087:%04X! EAX=%08X ESI=%08X DS=%04X ESP=%08X\n",
              eip, get_reg32(reg_AX), get_reg32(reg_SI), sregs[seg_DS], get_esp());
    }
    // Trace the function entry at 0087:44E4 (mode init caller) and 0087:6500 (area near write)
    if (eip >= 0x44E0 && eip <= 0x4560) {
      static int mode_trace = 0;
      if (mode_trace < 100) {
        mode_trace++;
        fprintf(stderr, "[MODE-TRACE] 0087:%04X EAX=%08X ESI=%08X DS=%04X\n",
                eip, get_reg32(reg_AX), get_reg32(reg_SI), sregs[seg_DS]);
      }
    }
  }

  // Real-mode instruction trace (for DOOM.EXE initialization debugging)
  if (rm_trace_count > 0 && !protected_mode()) {
    rm_trace_count--;
    uint32_t lin = ((uint32_t)sregs[seg_CS] << 4) + ip;
    fprintf(stderr, "[RM-INSN] %04X:%04X (lin=%05X): ",
            sregs[seg_CS], ip, lin);
    for (int i = 0; i < 8; i++)
      fprintf(stderr, "%02X ", mem->fetch_mem(lin + i));
    fprintf(stderr, " AX=%04X BX=%04X CX=%04X DX=%04X DS=%04X ES=%04X\n",
            regs[reg_AX], regs[reg_BX], regs[reg_CX], regs[reg_DX],
            sregs[seg_DS], sregs[seg_ES]);
  }

  // GP handler instruction trace — only trace CPL=3 (user code), skip CPL=0 (CWSDPMI kernel)
  if (gp_trace_count > 0 && cpl == 3) {
    gp_trace_count--;
    uint32_t lin = protected_mode() ? (seg_cache[seg_CS].base + ip)
                                     : ((uint32_t)sregs[seg_CS] << 4) + ip;
    uint32_t phys = lin;
    if (paging_enabled()) {
      uint32_t pde_i = (lin >> 22) & 0x3FF;
      uint32_t pte_i = (lin >> 12) & 0x3FF;
      uint32_t pde = mem->fetch_mem32((cr3 & 0xFFFFF000) + pde_i * 4);
      if (pde & 1) {
        uint32_t pte = mem->fetch_mem32((pde & 0xFFFFF000) + pte_i * 4);
        if (pte & 1) phys = (pte & 0xFFFFF000) | (lin & 0xFFF);
      }
    }
    fprintf(stderr, "[INSN] %04X:%08X (lin=%08X) CPL=%d: ",
            sregs[seg_CS], ip, lin, cpl);
    for (int i = 0; i < 8; i++)
      fprintf(stderr, "%02X ", mem->fetch_mem(phys + i));
    fprintf(stderr, " ESP=%08X EAX=%08X DS=%04X\n", get_esp(), get_reg32(reg_AX), sregs[seg_DS]);
  }

  // Targeted trace: DOS4GW code around crash area (CS=000C, 7500-75B0)
  if (sregs[seg_CS] == 0x000C && ip >= 0x7540 && ip <= 0x75B0) {
    static int d4gw_trace = 0;
    // Always log the RETF at 75A0 and 10 instructions before it
    bool force_log = (ip == 0x75A0);
    if (d4gw_trace < 200 || force_log) {
      if (!force_log) d4gw_trace++;
      uint32_t lin = seg_cache[seg_CS].base + ip;
      uint32_t ss_base = seg_cache[seg_SS].base;
      uint32_t esp = get_esp();
      fprintf(stderr, "[D4GW] %04X:%04X ESP=%08X SS=%04X(base=%08X) AX=%04X BX=%04X CX=%04X DX=%04X SI=%04X DI=%04X",
              sregs[seg_CS], ip, esp, sregs[seg_SS], ss_base,
              regs[reg_AX], regs[reg_BX], regs[reg_CX], regs[reg_DX],
              regs[reg_SI], regs[reg_DI]);
      fprintf(stderr, " bytes:");
      for (int i = 0; i < 8; i++)
        fprintf(stderr, " %02X", mem->fetch_mem(lin + i));
      fprintf(stderr, " [SS:SP]=%04X %04X %04X %04X\n",
              mem->fetch_mem16(ss_base + (esp & 0xFFFF)),
              mem->fetch_mem16(ss_base + ((esp + 2) & 0xFFFF)),
              mem->fetch_mem16(ss_base + ((esp + 4) & 0xFFFF)),
              mem->fetch_mem16(ss_base + ((esp + 6) & 0xFFFF)));
    }
  }

  // Handle prefix bytes
  bool lock_prefix = false;
  bool prefix_done = false;
  int prefix_count = 0;
  while (!prefix_done) {
    emu88_uint8 prefix = fetch_byte(sregs[seg_CS], ip);
    switch (prefix) {
    case 0x26: seg_override = seg_ES; ip++; prefix_count++; break;
    case 0x2E: seg_override = seg_CS; ip++; prefix_count++; break;
    case 0x36: seg_override = seg_SS; ip++; prefix_count++; break;
    case 0x3E: seg_override = seg_DS; ip++; prefix_count++; break;
    case 0x64:
      if (cpu_type >= CPU_386) { seg_override = seg_FS; ip++; prefix_count++; }
      else prefix_done = true;
      break;
    case 0x65:
      if (cpu_type >= CPU_386) { seg_override = seg_GS; ip++; prefix_count++; }
      else prefix_done = true;
      break;
    case 0x66:
      if (cpu_type >= CPU_386) { op_size_32 = !op_size_32; ip++; prefix_count++; }
      else prefix_done = true;
      break;
    case 0x67:
      if (cpu_type >= CPU_386) { addr_size_32 = !addr_size_32; ip++; prefix_count++; }
      else prefix_done = true;
      break;
    case 0xF0: lock_prefix = true; ip++; prefix_count++; break;  // LOCK prefix
    case 0xF2: rep_prefix = REP_REPNZ; ip++; prefix_count++; break;
    case 0xF3: rep_prefix = REP_REPZ; ip++; prefix_count++; break;
    default: prefix_done = true; break;
    }
    if (!prefix_done && !protected_mode()) ip &= 0xFFFF;
  }

  // Save prefix count for 286 instruction length check
  emu88_uint32 ip_after_prefixes = ip;

  emu88_uint8 opcode = fetch_ip_byte();
  cycles += base_cycles[opcode];

  // Validate LOCK prefix: only valid with specific memory-destination opcodes
  if (lock_prefix) {
    bool lock_valid = false;
    switch (opcode) {
    // ALU r/m, reg (ADD/OR/ADC/SBB/AND/SUB/XOR only, not CMP)
    case 0x00: case 0x01: case 0x08: case 0x09:
    case 0x10: case 0x11: case 0x18: case 0x19:
    case 0x20: case 0x21: case 0x28: case 0x29:
    case 0x30: case 0x31:
    // GRP1 r/m, imm (ADD/OR/ADC/SBB/AND/SUB/XOR)
    case 0x80: case 0x81: case 0x82: case 0x83:
    // XCHG r/m, reg
    case 0x86: case 0x87:
    // GRP3 (NOT, NEG)
    case 0xF6: case 0xF7:
    // GRP4/5 (INC, DEC)
    case 0xFE: case 0xFF:
      lock_valid = true;
      break;
    case 0x0F: // 2-byte opcodes handled separately
      lock_valid = true; // Will validate on actual 0F opcode if needed
      break;
    }
    if (!lock_valid && lock_ud && cpu_type >= CPU_286) {
      raise_exception_no_error(6);  // #UD (286+ only)
      return;
    }
  }

  // 8088: remap undefined opcodes to their hardware aliases
  if (cpu_type == CPU_8088) {
    if (opcode >= 0x60 && opcode <= 0x6F)
      opcode = (opcode & 0x0F) | 0x70;  // 0x60-0x6F → Jcc (aliases for 0x70-0x7F)
    else if (opcode == 0xC0) opcode = 0xC2;  // RET near imm16
    else if (opcode == 0xC1) opcode = 0xC3;  // RET near
    else if (opcode == 0xC8) opcode = 0xCA;  // RET far imm16
    else if (opcode == 0xC9) opcode = 0xCB;  // RET far
  }

  switch (opcode) {
  //--- ALU: op r/m8, r8 ---
  case 0x00: case 0x08: case 0x10: case 0x18:
  case 0x20: case 0x28: case 0x30: case 0x38: {
    emu88_uint8 op = (opcode >> 3) & 7;
    emu88_uint8 modrm = fetch_ip_byte();
    modrm_result mr = decode_modrm(modrm);
    emu88_uint8 val = get_rm8(mr);
    emu88_uint8 reg = get_reg8(mr.reg_field);
    emu88_uint8 result = do_alu8(op, val, reg);
    if (op != 7) set_rm8(mr, result);
    break;
  }

  //--- ALU: op r/m16, r16 ---
  case 0x01: case 0x09: case 0x11: case 0x19:
  case 0x21: case 0x29: case 0x31: case 0x39: {
    emu88_uint8 op = (opcode >> 3) & 7;
    emu88_uint8 modrm = fetch_ip_byte();
    modrm_result mr = decode_modrm(modrm);
    if (op_size_32) {
      emu88_uint32 val = get_rm32(mr);
      if (exception_pending) break;
      emu88_uint32 reg = get_reg32(mr.reg_field);
      emu88_uint32 result = do_alu32(op, val, reg);
      if (op != 7) set_rm32(mr, result);
    } else {
      emu88_uint16 val = get_rm16(mr);
      if (exception_pending) break;
      emu88_uint16 reg = regs[mr.reg_field];
      emu88_uint16 result = do_alu16(op, val, reg);
      if (op != 7) set_rm16(mr, result);
    }
    break;
  }

  //--- ALU: op r8, r/m8 ---
  case 0x02: case 0x0A: case 0x12: case 0x1A:
  case 0x22: case 0x2A: case 0x32: case 0x3A: {
    emu88_uint8 op = (opcode >> 3) & 7;
    emu88_uint8 modrm = fetch_ip_byte();
    modrm_result mr = decode_modrm(modrm);
    emu88_uint8 reg = get_reg8(mr.reg_field);
    emu88_uint8 val = get_rm8(mr);
    if (exception_pending) break;
    emu88_uint8 result = do_alu8(op, reg, val);
    if (op != 7) set_reg8(mr.reg_field, result);
    break;
  }

  //--- ALU: op r16, r/m16 ---
  case 0x03: case 0x0B: case 0x13: case 0x1B:
  case 0x23: case 0x2B: case 0x33: case 0x3B: {
    emu88_uint8 op = (opcode >> 3) & 7;
    emu88_uint8 modrm = fetch_ip_byte();
    modrm_result mr = decode_modrm(modrm);
    if (op_size_32) {
      emu88_uint32 reg = get_reg32(mr.reg_field);
      emu88_uint32 val = get_rm32(mr);
      if (exception_pending) break;
      emu88_uint32 result = do_alu32(op, reg, val);
      if (op != 7) set_reg32(mr.reg_field, result);
    } else {
      emu88_uint16 reg = regs[mr.reg_field];
      emu88_uint16 val = get_rm16(mr);
      if (exception_pending) break;
      emu88_uint16 result = do_alu16(op, reg, val);
      if (op != 7) regs[mr.reg_field] = result;
    }
    break;
  }

  //--- ALU: op AL, imm8 ---
  case 0x04: case 0x0C: case 0x14: case 0x1C:
  case 0x24: case 0x2C: case 0x34: case 0x3C: {
    emu88_uint8 op = (opcode >> 3) & 7;
    emu88_uint8 imm = fetch_ip_byte();
    emu88_uint8 al = get_reg8(reg_AL);
    emu88_uint8 result = do_alu8(op, al, imm);
    if (op != 7) set_reg8(reg_AL, result);
    break;
  }

  //--- ALU: op AX, imm16 ---
  case 0x05: case 0x0D: case 0x15: case 0x1D:
  case 0x25: case 0x2D: case 0x35: case 0x3D: {
    emu88_uint8 op = (opcode >> 3) & 7;
    if (op_size_32) {
      emu88_uint32 imm = fetch_ip_dword();
      emu88_uint32 result = do_alu32(op, get_reg32(reg_AX), imm);
      if (op != 7) set_reg32(reg_AX, result);
    } else {
      emu88_uint16 imm = fetch_ip_word();
      emu88_uint16 result = do_alu16(op, regs[reg_AX], imm);
      if (op != 7) regs[reg_AX] = result;
    }
    break;
  }

  //--- PUSH segment ---
  case 0x06: if (op_size_32) push_dword((emu88_uint32)sregs[seg_ES]); else push_word(sregs[seg_ES]); break;
  case 0x0E: if (op_size_32) push_dword((emu88_uint32)sregs[seg_CS]); else push_word(sregs[seg_CS]); break;
  case 0x16: if (op_size_32) push_dword((emu88_uint32)sregs[seg_SS]); else push_word(sregs[seg_SS]); break;
  case 0x1E: if (op_size_32) push_dword((emu88_uint32)sregs[seg_DS]); else push_word(sregs[seg_DS]); break;

  //--- POP segment ---
  case 0x07: { emu88_uint16 v = op_size_32 ? (emu88_uint16)pop_dword() : pop_word(); if (!exception_pending) load_segment(seg_ES, v); break; }
  case 0x17: { emu88_uint16 v = op_size_32 ? (emu88_uint16)pop_dword() : pop_word(); if (!exception_pending) load_segment(seg_SS, v); break; }
  case 0x1F: { emu88_uint16 v = op_size_32 ? (emu88_uint16)pop_dword() : pop_word(); if (!exception_pending) load_segment(seg_DS, v); break; }
  // 0x0F: POP CS is not valid on 8088 (undefined behavior)

  //--- DAA ---
  case 0x27: {
    emu88_uint8 al = get_reg8(reg_AL);
    emu88_uint8 old_al = al;
    bool old_cf = get_flag(FLAG_CF);
    bool step1 = false, step2 = false;
    clear_flag(FLAG_CF);
    if ((al & 0x0F) > 9 || get_flag(FLAG_AF)) {
      al += 6;
      set_flag_val(FLAG_CF, old_cf || (al < old_al));
      set_flag(FLAG_AF);
      step1 = true;
    } else {
      clear_flag(FLAG_AF);
    }
    emu88_uint8 al_step1 = al;  // AL after step1, before step2
    if (old_al > 0x99 || old_cf) {
      al += 0x60;
      set_flag(FLAG_CF);
      step2 = true;
    }
    set_reg8(reg_AL, al);
    set_flags_zsp8(al);
    if (cpu_type == CPU_8088) {
      // 8088: OF = signed overflow from either internal ADD (OR'd)
      bool of1 = step1 && (old_al >= 0x7A && old_al <= 0x7F);
      bool of2 = step2 && (al_step1 >= 0x20 && al_step1 <= 0x7F);
      set_flag_val(FLAG_OF, of1 || of2);
    } else if (!lock_ud) clear_flag(FLAG_OF);
    break;
  }

  //--- DAS ---
  case 0x2F: {
    emu88_uint8 al = get_reg8(reg_AL);
    emu88_uint8 old_al = al;
    bool old_cf = get_flag(FLAG_CF);
    bool step1 = false, step2 = false;
    clear_flag(FLAG_CF);
    if ((al & 0x0F) > 9 || get_flag(FLAG_AF)) {
      al -= 6;
      // 8088: CF not set from borrow in step1 (only step2 sets CF)
      if (cpu_type == CPU_8088)
        set_flag_val(FLAG_CF, old_cf);
      else
        set_flag_val(FLAG_CF, old_cf || (old_al < 6));
      set_flag(FLAG_AF);
      step1 = true;
    } else {
      clear_flag(FLAG_AF);
    }
    emu88_uint8 al_step1 = al;  // AL after step1, before step2
    if (old_al > 0x99 || old_cf) {
      al -= 0x60;
      set_flag(FLAG_CF);
      step2 = true;
    }
    set_reg8(reg_AL, al);
    set_flags_zsp8(al);
    if (cpu_type == CPU_8088) {
      // 8088: OF = signed overflow from either internal SUB (OR'd)
      bool of1 = step1 && (old_al >= 0x80 && old_al <= 0x85);
      bool of2 = step2 && (al_step1 >= 0x80 && al_step1 <= 0xDF);
      set_flag_val(FLAG_OF, of1 || of2);
    } else if (!lock_ud) clear_flag(FLAG_OF);
    break;
  }

  //--- AAA ---
  case 0x37: {
    emu88_uint8 al = get_reg8(reg_AL);
    if ((al & 0x0F) > 9 || get_flag(FLAG_AF)) {
      if (cpu_type == CPU_8088) {
        // 8088: separate AL+6 and AH+1 (no carry from AL to AH)
        emu88_uint8 new_al = al + 6;
        set_flags_zsp8(new_al);
        set_flag_val(FLAG_OF, al >= 0x7A && al <= 0x7F);  // signed overflow of al+6
        set_reg8(reg_AL, new_al & 0x0F);
        set_reg8(reg_AH, get_reg8(reg_AH) + 1);
      } else {
        emu88_uint16 ax = regs[reg_AX] + 0x106;
        regs[reg_AX] = (ax & 0xFF0F);
        if (!lock_ud) {
          set_flags_zsp8(ax & 0xFF);
          clear_flag(FLAG_OF);
        }
      }
      set_flag(FLAG_AF);
      set_flag(FLAG_CF);
    } else {
      set_reg8(reg_AL, al & 0x0F);
      clear_flag(FLAG_AF);
      clear_flag(FLAG_CF);
      if (cpu_type == CPU_8088) {
        set_flags_zsp8(al);  // 8088: ZSP from original AL
        clear_flag(FLAG_OF);
      } else if (!lock_ud) { set_flags_zsp8(al & 0x0F); clear_flag(FLAG_OF); }
    }
    break;
  }

  //--- AAS ---
  case 0x3F: {
    emu88_uint8 al = get_reg8(reg_AL);
    if ((al & 0x0F) > 9 || get_flag(FLAG_AF)) {
      if (cpu_type == CPU_8088) {
        // 8088: separate AL-6 and AH-1 (no borrow from AL to AH)
        emu88_uint8 new_al = al - 6;
        set_flags_zsp8(new_al);
        set_flag_val(FLAG_OF, al >= 0x80 && al <= 0x85);  // signed overflow of al-6
        set_reg8(reg_AL, new_al & 0x0F);
        set_reg8(reg_AH, get_reg8(reg_AH) - 1);
      } else {
        emu88_uint16 ax = regs[reg_AX] - 0x106;
        regs[reg_AX] = (ax & 0xFF0F);
        if (!lock_ud) {
          set_flags_zsp8(ax & 0xFF);
          clear_flag(FLAG_OF);
        }
      }
      set_flag(FLAG_AF);
      set_flag(FLAG_CF);
    } else {
      set_reg8(reg_AL, al & 0x0F);
      clear_flag(FLAG_AF);
      clear_flag(FLAG_CF);
      if (cpu_type == CPU_8088) {
        set_flags_zsp8(al);  // 8088: ZSP from original AL
        clear_flag(FLAG_OF);
      } else if (!lock_ud) { set_flags_zsp8(al & 0x0F); clear_flag(FLAG_OF); }
    }
    break;
  }

  //--- INC r16 (0x40-0x47) ---
  case 0x40: case 0x41: case 0x42: case 0x43:
  case 0x44: case 0x45: case 0x46: case 0x47: {
    emu88_uint8 r = opcode & 7;
    if (op_size_32) {
      emu88_uint32 val = get_reg32(r);
      emu88_uint32 result = alu_inc32(val);
      set_reg32(r, result);
    } else {
      emu88_uint16 val = regs[r];
      emu88_uint16 result = val + 1;
      set_flags_zsp16(result);
      set_flag_val(FLAG_AF, (val & 0x0F) == 0x0F);
      set_flag_val(FLAG_OF, val == 0x7FFF);
      regs[r] = result;
    }
    break;
  }

  //--- DEC r16 (0x48-0x4F) ---
  case 0x48: case 0x49: case 0x4A: case 0x4B:
  case 0x4C: case 0x4D: case 0x4E: case 0x4F: {
    emu88_uint8 r = opcode & 7;
    if (op_size_32) {
      emu88_uint32 val = get_reg32(r);
      emu88_uint32 result = alu_dec32(val);
      set_reg32(r, result);
    } else {
      emu88_uint16 val = regs[r];
      emu88_uint16 result = val - 1;
      set_flags_zsp16(result);
      set_flag_val(FLAG_AF, (val & 0x0F) == 0x00);
      set_flag_val(FLAG_OF, val == 0x8000);
      regs[r] = result;
    }
    break;
  }

  //--- PUSH r16 (0x50-0x57) ---
  case 0x50: case 0x51: case 0x52: case 0x53:
  case 0x54: case 0x55: case 0x56: case 0x57:
    if (op_size_32) push_dword(get_reg32(opcode & 7));
    else if (opcode == 0x54 && cpu_type == CPU_8088) {
      // 8088 PUSH SP pushes the already-decremented value (SP-2)
      regs[reg_SP] -= 2;
      store_word(sregs[seg_SS], regs[reg_SP], regs[reg_SP]);
    }
    else push_word(regs[opcode & 7]);
    break;

  //--- POP r16 (0x58-0x5F) ---
  case 0x58: case 0x59: case 0x5A: case 0x5B:
  case 0x5C: case 0x5D: case 0x5E: case 0x5F:
    if (op_size_32) set_reg32(opcode & 7, pop_dword());
    else regs[opcode & 7] = pop_word();
    break;

  //--- Conditional jumps (0x70-0x7F) ---
  case 0x70: { // JO
    emu88_int8 disp = (emu88_int8)fetch_ip_byte();
    if (get_flag(FLAG_OF)) ip += disp;
    break;
  }
  case 0x71: { // JNO
    emu88_int8 disp = (emu88_int8)fetch_ip_byte();
    if (!get_flag(FLAG_OF)) ip += disp;
    break;
  }
  case 0x72: { // JB/JNAE/JC
    emu88_int8 disp = (emu88_int8)fetch_ip_byte();
    if (get_flag(FLAG_CF)) ip += disp;
    break;
  }
  case 0x73: { // JNB/JAE/JNC
    emu88_int8 disp = (emu88_int8)fetch_ip_byte();
    if (!get_flag(FLAG_CF)) ip += disp;
    break;
  }
  case 0x74: { // JE/JZ
    emu88_int8 disp = (emu88_int8)fetch_ip_byte();
    if (get_flag(FLAG_ZF)) ip += disp;
    break;
  }
  case 0x75: { // JNE/JNZ
    emu88_int8 disp = (emu88_int8)fetch_ip_byte();
    if (!get_flag(FLAG_ZF)) ip += disp;
    break;
  }
  case 0x76: { // JBE/JNA
    emu88_int8 disp = (emu88_int8)fetch_ip_byte();
    if (get_flag(FLAG_CF) || get_flag(FLAG_ZF)) ip += disp;
    break;
  }
  case 0x77: { // JNBE/JA
    emu88_int8 disp = (emu88_int8)fetch_ip_byte();
    if (!get_flag(FLAG_CF) && !get_flag(FLAG_ZF)) ip += disp;
    break;
  }
  case 0x78: { // JS
    emu88_int8 disp = (emu88_int8)fetch_ip_byte();
    if (get_flag(FLAG_SF)) ip += disp;
    break;
  }
  case 0x79: { // JNS
    emu88_int8 disp = (emu88_int8)fetch_ip_byte();
    if (!get_flag(FLAG_SF)) ip += disp;
    break;
  }
  case 0x7A: { // JP/JPE
    emu88_int8 disp = (emu88_int8)fetch_ip_byte();
    if (get_flag(FLAG_PF)) ip += disp;
    break;
  }
  case 0x7B: { // JNP/JPO
    emu88_int8 disp = (emu88_int8)fetch_ip_byte();
    if (!get_flag(FLAG_PF)) ip += disp;
    break;
  }
  case 0x7C: { // JL/JNGE
    emu88_int8 disp = (emu88_int8)fetch_ip_byte();
    if (get_flag(FLAG_SF) != get_flag(FLAG_OF)) ip += disp;
    break;
  }
  case 0x7D: { // JNL/JGE
    emu88_int8 disp = (emu88_int8)fetch_ip_byte();
    if (get_flag(FLAG_SF) == get_flag(FLAG_OF)) ip += disp;
    break;
  }
  case 0x7E: { // JLE/JNG
    emu88_int8 disp = (emu88_int8)fetch_ip_byte();
    if (get_flag(FLAG_ZF) || (get_flag(FLAG_SF) != get_flag(FLAG_OF))) ip += disp;
    break;
  }
  case 0x7F: { // JNLE/JG
    emu88_int8 disp = (emu88_int8)fetch_ip_byte();
    if (!get_flag(FLAG_ZF) && (get_flag(FLAG_SF) == get_flag(FLAG_OF))) ip += disp;
    break;
  }

  //--- GRP1: ALU r/m8, imm8 ---
  case 0x80: {
    emu88_uint8 modrm = fetch_ip_byte();
    modrm_result mr = decode_modrm(modrm);
    execute_grp1_rm8(mr, fetch_ip_byte());
    break;
  }

  //--- GRP1: ALU r/m16, imm16 ---
  case 0x81: {
    emu88_uint8 modrm = fetch_ip_byte();
    modrm_result mr = decode_modrm(modrm);
    if (op_size_32) {
      emu88_uint32 imm = fetch_ip_dword();
      if (!lock_ud && (ip - insn_ip) > 10) { ip = insn_ip; raise_exception(13, 0); break; }
      execute_grp1_rm32(mr, imm);
    } else {
      emu88_uint16 imm = fetch_ip_word();
      if (!lock_ud && (ip - insn_ip) > 10) { ip = insn_ip; raise_exception(13, 0); break; }
      execute_grp1_rm16(mr, imm);
    }
    break;
  }

  //--- GRP1: ALU r/m8, imm8 (duplicate of 0x80) ---
  case 0x82: {
    emu88_uint8 modrm = fetch_ip_byte();
    modrm_result mr = decode_modrm(modrm);
    execute_grp1_rm8(mr, fetch_ip_byte());
    break;
  }

  //--- GRP1: ALU r/m16, sign-extended imm8 ---
  case 0x83: {
    emu88_uint8 modrm = fetch_ip_byte();
    modrm_result mr = decode_modrm(modrm);
    emu88_int8 imm8 = (emu88_int8)fetch_ip_byte();
    if (op_size_32) execute_grp1_rm32(mr, emu88_uint32(emu88_int32(imm8)));
    else execute_grp1_rm16(mr, emu88_uint16(emu88_int16(imm8)));
    break;
  }

  //--- TEST r/m8, r8 ---
  case 0x84: {
    emu88_uint8 modrm = fetch_ip_byte();
    modrm_result mr = decode_modrm(modrm);
    set_flags_logic8(get_rm8(mr) & get_reg8(mr.reg_field));
    break;
  }

  //--- TEST r/m16, r16 ---
  case 0x85: {
    emu88_uint8 modrm = fetch_ip_byte();
    modrm_result mr = decode_modrm(modrm);
    if (op_size_32) {
      emu88_uint32 val = get_rm32(mr);
      if (exception_pending) break;
      set_flags_logic32(val & get_reg32(mr.reg_field));
    } else {
      emu88_uint16 val = get_rm16(mr);
      if (exception_pending) break;
      set_flags_logic16(val & regs[mr.reg_field]);
    }
    break;
  }

  //--- XCHG r/m8, r8 ---
  case 0x86: {
    emu88_uint8 modrm = fetch_ip_byte();
    modrm_result mr = decode_modrm(modrm);
    emu88_uint8 a = get_rm8(mr);
    emu88_uint8 b = get_reg8(mr.reg_field);
    set_rm8(mr, b);
    set_reg8(mr.reg_field, a);
    break;
  }

  //--- XCHG r/m16, r16 ---
  case 0x87: {
    emu88_uint8 modrm = fetch_ip_byte();
    modrm_result mr = decode_modrm(modrm);
    if (op_size_32) {
      emu88_uint32 a = get_rm32(mr);
      if (exception_pending) break;
      emu88_uint32 b = get_reg32(mr.reg_field);
      set_rm32(mr, b);
      set_reg32(mr.reg_field, a);
    } else {
      emu88_uint16 a = get_rm16(mr);
      if (exception_pending) break;
      emu88_uint16 b = regs[mr.reg_field];
      set_rm16(mr, b);
      regs[mr.reg_field] = a;
    }
    break;
  }

  //--- MOV r/m8, r8 ---
  case 0x88: {
    emu88_uint8 modrm = fetch_ip_byte();
    modrm_result mr = decode_modrm(modrm);
    set_rm8(mr, get_reg8(mr.reg_field));
    break;
  }

  //--- MOV r/m16, r16 ---
  case 0x89: {
    emu88_uint8 modrm = fetch_ip_byte();
    modrm_result mr = decode_modrm(modrm);
    if (op_size_32) set_rm32(mr, get_reg32(mr.reg_field));
    else set_rm16(mr, regs[mr.reg_field]);
    break;
  }

  //--- MOV r8, r/m8 ---
  case 0x8A: {
    emu88_uint8 modrm = fetch_ip_byte();
    modrm_result mr = decode_modrm(modrm);
    set_reg8(mr.reg_field, get_rm8(mr));
    break;
  }

  //--- MOV r16, r/m16 ---
  case 0x8B: {
    emu88_uint8 modrm = fetch_ip_byte();
    modrm_result mr = decode_modrm(modrm);
    if (op_size_32) {
      emu88_uint32 val = get_rm32(mr);
      if (exception_pending) break;
      set_reg32(mr.reg_field, val);
    } else {
      emu88_uint16 val = get_rm16(mr);
      if (exception_pending) break;
      regs[mr.reg_field] = val;
    }
    break;
  }

  //--- MOV r/m16, sreg (with 66h prefix: zero-extends to 32-bit) ---
  case 0x8C: {
    emu88_uint8 modrm = fetch_ip_byte();
    modrm_result mr = decode_modrm(modrm);
    int sreg_idx = mr.reg_field & 7;
    if (cpu_type == CPU_8088) sreg_idx &= 3;  // 8088: only ES/CS/SS/DS
    else if (!lock_ud && sreg_idx >= 4) { raise_exception_no_error(6); break; }
    if (sreg_idx >= 6) sreg_idx = 0;
    if (op_size_32 && mr.is_register) {
      set_reg32(mr.rm_field, (emu88_uint32)sregs[sreg_idx]);
    } else {
      set_rm16(mr, sregs[sreg_idx]);
    }
    break;
  }

  //--- LEA r16, m ---
  case 0x8D: {
    emu88_uint8 modrm = fetch_ip_byte();
    modrm_result mr = decode_modrm(modrm);
    if (mr.is_register) { raise_exception_no_error(6); break; }
    if (op_size_32) set_reg32(mr.reg_field, mr.offset);
    else regs[mr.reg_field] = mr.offset;
    break;
  }

  //--- MOV sreg, r/m16 ---
  case 0x8E: {
    emu88_uint8 modrm = fetch_ip_byte();
    modrm_result mr = decode_modrm(modrm);
    int sreg_idx = mr.reg_field & 7;
    if (cpu_type == CPU_8088) {
      sreg_idx &= 3;  // 8088: only ES/CS/SS/DS, MOV CS is valid
    } else {
      if (!lock_ud && sreg_idx >= 4) { raise_exception_no_error(6); break; }
      if (sreg_idx == seg_CS) {
        raise_exception_no_error(6);  // MOV CS invalid on 286+
        break;
      }
    }
    if (sreg_idx >= 6) sreg_idx = 0;
    { emu88_uint16 val = get_rm16(mr);
      if (exception_pending) break;
      load_segment(sreg_idx, val);
    }
    break;
  }

  //--- POP r/m16 ---
  case 0x8F: {
    emu88_uint8 modrm = fetch_ip_byte();
    modrm_result mr = decode_modrm(modrm);
    if (mr.reg_field != 0) { raise_exception_no_error(6); break; }
    if (op_size_32) set_rm32(mr, pop_dword());
    else set_rm16(mr, pop_word());
    break;
  }

  //--- NOP (XCHG AX, AX) ---
  case 0x90:
    break;

  //--- XCHG AX, r16 (0x91-0x97) ---
  case 0x91: case 0x92: case 0x93:
  case 0x94: case 0x95: case 0x96: case 0x97: {
    emu88_uint8 r = opcode & 7;
    if (op_size_32) {
      emu88_uint32 tmp = get_reg32(reg_AX);
      set_reg32(reg_AX, get_reg32(r));
      set_reg32(r, tmp);
    } else {
      emu88_uint16 tmp = regs[reg_AX];
      regs[reg_AX] = regs[r];
      regs[r] = tmp;
    }
    break;
  }

  //--- CBW / CWDE ---
  case 0x98:
    if (op_size_32) // CWDE: sign-extend AX to EAX
      set_reg32(reg_AX, emu88_uint32(emu88_int32(emu88_int16(regs[reg_AX]))));
    else // CBW: sign-extend AL to AX
      regs[reg_AX] = emu88_uint16(emu88_int16(emu88_int8(regs[reg_AX] & 0xFF)));
    break;

  //--- CWD / CDQ ---
  case 0x99:
    if (op_size_32) // CDQ: sign-extend EAX to EDX:EAX
      set_reg32(reg_DX, (get_reg32(reg_AX) & 0x80000000) ? 0xFFFFFFFF : 0x00000000);
    else // CWD: sign-extend AX to DX:AX
      regs[reg_DX] = (regs[reg_AX] & 0x8000) ? 0xFFFF : 0x0000;
    break;

  //--- CALL far ptr16:16 / ptr16:32 ---
  case 0x9A: {
    if (op_size_32) {
      emu88_uint32 off = fetch_ip_dword();
      emu88_uint16 seg = fetch_ip_word();
      if (!lock_ud && (ip - insn_ip) > 10) { ip = insn_ip; raise_exception(13, 0); break; }
      far_call_or_jmp(seg, off, true);
    } else {
      emu88_uint16 off = fetch_ip_word();
      emu88_uint16 seg = fetch_ip_word();
      if (!lock_ud && (ip - insn_ip) > 10) { ip = insn_ip; raise_exception(13, 0); break; }
      far_call_or_jmp(seg, off, true);
    }
    break;
  }

  //--- WAIT ---
  case 0x9B:
    // If CR0.TS and CR0.MP are both set, raise #NM
    if ((cr0 & CR0_TS) && (cr0 & CR0_MP)) {
      raise_exception_no_error(7);
      break;
    }
    break;  // no FPU, just continue

  //--- PUSHF / PUSHFD ---
  case 0x9C:
    // In V86 mode, IOPL must be 3 or #GP(0)
    if (v86_mode() && get_iopl() < 3) {
      raise_exception(13, 0);
      break;
    }
    if (op_size_32) push_dword((get_eflags() & ~(emu88_uint32)EFLAG_VM) | 0x0002);
    else if (cpu_type == CPU_8088) push_word(flags | 0xF002);  // 8088: bits 12-15 set
    else push_word((flags & 0x7FFF) | 0x0002);
    break;

  //--- POPF / POPFD ---
  case 0x9D:
    // In V86 mode, IOPL must be 3 or #GP(0)
    if (v86_mode() && get_iopl() < 3) {
      raise_exception(13, 0);
      break;
    }
    if (op_size_32) {
      emu88_uint32 new_eflags = pop_dword();
      if (cpl > 0) {
        // CPL > 0: cannot change IOPL; CPL > IOPL: cannot change IF
        emu88_uint32 mask = 0x003FFFFF & ~(emu88_uint32)EFLAG_VM;
        if (cpl > 0) mask &= ~(emu88_uint32)EFLAG_IOPL_MASK;
        if (cpl > get_iopl()) mask &= ~(emu88_uint32)FLAG_IF;
        emu88_uint32 preserved = get_eflags() & ~mask;
        set_eflags((preserved | (new_eflags & mask)) | 0x0002);
      } else {
        set_eflags((new_eflags & (0x003FFFFF & ~(emu88_uint32)EFLAG_VM)) | 0x0002);
      }
    } else {
      emu88_uint16 new_flags = pop_word();
      if (cpl > 0) {
        emu88_uint16 mask = 0x7FD7;
        if (cpl > 0) mask &= ~(emu88_uint16)EFLAG_IOPL_MASK;
        if (cpl > get_iopl()) mask &= ~(emu88_uint16)FLAG_IF;
        flags = ((flags & ~mask) | (new_flags & mask)) | 0x0002;
      } else {
        uint8_t old_iopl = (flags >> 12) & 3;
        flags = (new_flags & 0x7FD7) | 0x0002;
        uint8_t new_iopl = (flags >> 12) & 3;
        if (new_iopl != old_iopl) {
          static int pf_log = 0;
          if (pf_log < 2) { pf_log++; fprintf(stderr, "[IOPL-POPF] %d→%d at %04X:%08X\n", old_iopl, new_iopl, sregs[seg_CS], insn_ip); }
        }
      }
    }
    break;

  //--- SAHF ---
  case 0x9E:
    flags = (flags & 0xFF00) | get_reg8(reg_AH);
    break;

  //--- LAHF ---
  case 0x9F:
    set_reg8(reg_AH, flags & 0xFF);
    break;

  //--- MOV AL, [moffs] ---
  case 0xA0: {
    emu88_uint32 addr = addr_size_32 ? fetch_ip_dword() : fetch_ip_word();
    set_reg8(reg_AL, fetch_byte(default_segment(), addr));
    break;
  }

  //--- MOV AX/EAX, [moffs] ---
  case 0xA1: {
    emu88_uint32 addr = addr_size_32 ? fetch_ip_dword() : fetch_ip_word();
    if (op_size_32) set_reg32(reg_AX, fetch_dword(default_segment(), addr));
    else regs[reg_AX] = fetch_word(default_segment(), addr);
    break;
  }

  //--- MOV [moffs], AL ---
  case 0xA2: {
    emu88_uint32 addr = addr_size_32 ? fetch_ip_dword() : fetch_ip_word();
    store_byte(default_segment(), addr, get_reg8(reg_AL));
    break;
  }

  //--- MOV [moffs], AX/EAX ---
  case 0xA3: {
    emu88_uint32 addr = addr_size_32 ? fetch_ip_dword() : fetch_ip_word();
    if (op_size_32) store_dword(default_segment(), addr, get_reg32(reg_AX));
    else store_word(default_segment(), addr, regs[reg_AX]);
    break;
  }

  //--- String operations ---
  case 0x6C: case 0x6D: case 0x6E: case 0x6F:  // INS/OUTS (80186+)
  case 0xA4: case 0xA5: case 0xA6: case 0xA7:
  case 0xAA: case 0xAB: case 0xAC: case 0xAD:
  case 0xAE: case 0xAF:
    execute_string_op(opcode);
    break;

  //--- TEST AL, imm8 ---
  case 0xA8: {
    emu88_uint8 imm = fetch_ip_byte();
    set_flags_logic8(get_reg8(reg_AL) & imm);
    break;
  }

  //--- TEST AX, imm16 ---
  case 0xA9: {
    if (op_size_32) {
      emu88_uint32 imm = fetch_ip_dword();
      set_flags_logic32(get_reg32(reg_AX) & imm);
    } else {
      emu88_uint16 imm = fetch_ip_word();
      set_flags_logic16(regs[reg_AX] & imm);
    }
    break;
  }

  //--- MOV r8, imm8 (0xB0-0xB7) ---
  case 0xB0: case 0xB1: case 0xB2: case 0xB3:
  case 0xB4: case 0xB5: case 0xB6: case 0xB7:
    set_reg8(opcode & 7, fetch_ip_byte());
    break;

  //--- MOV r16, imm16 (0xB8-0xBF) ---
  case 0xB8: case 0xB9: case 0xBA: case 0xBB:
  case 0xBC: case 0xBD: case 0xBE: case 0xBF:
    if (op_size_32) set_reg32(opcode & 7, fetch_ip_dword());
    else regs[opcode & 7] = fetch_ip_word();
    break;

  //--- GRP2: shift/rotate r/m8, imm8 (80186+, treated as 1 on 8088) ---
  case 0xC0: {
    emu88_uint8 modrm = fetch_ip_byte();
    modrm_result mr = decode_modrm(modrm);
    emu88_uint8 count = fetch_ip_byte();
    execute_grp2_rm8(mr, count);
    break;
  }

  //--- GRP2: shift/rotate r/m16, imm8 (80186+, treated as 1 on 8088) ---
  case 0xC1: {
    emu88_uint8 modrm = fetch_ip_byte();
    modrm_result mr = decode_modrm(modrm);
    emu88_uint8 count = fetch_ip_byte();
    if (op_size_32) execute_grp2_rm32(mr, count);
    else execute_grp2_rm16(mr, count);
    break;
  }

  //--- RET near imm16 ---
  case 0xC2: {
    emu88_uint16 pop_count = fetch_ip_word();
    if (op_size_32) {
      emu88_uint32 new_ip = pop_dword();
      if (exception_pending) break;
      ip = new_ip;
    } else {
      emu88_uint16 new_ip = pop_word();
      if (exception_pending) break;
      ip = new_ip;
    }
    if (stack_32()) set_esp(get_esp() + pop_count);
    else regs[reg_SP] += pop_count;
    break;
  }

  //--- RET near ---
  case 0xC3:
    if (op_size_32) {
      emu88_uint32 new_ip = pop_dword();
      if (exception_pending) break;
      ip = new_ip;
    } else {
      emu88_uint16 new_ip = pop_word();
      if (exception_pending) break;
      ip = new_ip;
    }
    break;

  //--- LES r16, m16:16 / LES r32, m16:32 ---
  case 0xC4: {
    emu88_uint8 modrm = fetch_ip_byte();
    modrm_result mr = decode_modrm(modrm);
    if (mr.is_register) { raise_exception_no_error(6); break; }
    if (op_size_32) {
      emu88_uint32 off_val = fetch_dword(mr.seg, mr.offset);
      if (exception_pending) break;
      emu88_uint16 seg_val = fetch_word(mr.seg, mr.offset + 4);
      if (exception_pending) break;
      set_reg32(mr.reg_field, off_val);
      load_segment(seg_ES, seg_val);
    } else {
      emu88_uint16 off_val = fetch_word(mr.seg, mr.offset);
      if (exception_pending) break;
      emu88_uint16 seg_val = fetch_word(mr.seg, mr.offset + 2);
      if (exception_pending) break;
      regs[mr.reg_field] = off_val;
      load_segment(seg_ES, seg_val);
    }
    break;
  }

  //--- LDS r16, m16:16 / LDS r32, m16:32 ---
  case 0xC5: {
    emu88_uint8 modrm = fetch_ip_byte();
    modrm_result mr = decode_modrm(modrm);
    if (mr.is_register) { raise_exception_no_error(6); break; }
    if (op_size_32) {
      emu88_uint32 off_val = fetch_dword(mr.seg, mr.offset);
      if (exception_pending) break;
      emu88_uint16 seg_val = fetch_word(mr.seg, mr.offset + 4);
      if (exception_pending) break;
      set_reg32(mr.reg_field, off_val);
      load_segment(seg_DS, seg_val);
    } else {
      emu88_uint16 off_val = fetch_word(mr.seg, mr.offset);
      if (exception_pending) break;
      emu88_uint16 seg_val = fetch_word(mr.seg, mr.offset + 2);
      if (exception_pending) break;
      regs[mr.reg_field] = off_val;
      load_segment(seg_DS, seg_val);
    }
    break;
  }

  //--- MOV r/m8, imm8 ---
  case 0xC6: {
    emu88_uint8 modrm = fetch_ip_byte();
    modrm_result mr = decode_modrm(modrm);
    if (mr.reg_field != 0 && cpu_type >= CPU_286) { raise_exception_no_error(6); break; }
    emu88_uint8 imm = fetch_ip_byte();
    if (!lock_ud && (ip - insn_ip) > 10) { ip = insn_ip; raise_exception(13, 0); break; }
    set_rm8(mr, imm);
    break;
  }

  //--- MOV r/m16, imm16 ---
  case 0xC7: {
    emu88_uint8 modrm = fetch_ip_byte();
    modrm_result mr = decode_modrm(modrm);
    if (mr.reg_field != 0 && cpu_type >= CPU_286) { raise_exception_no_error(6); break; }
    if (op_size_32) {
      emu88_uint32 imm = fetch_ip_dword();
      if (!lock_ud && (ip - insn_ip) > 10) { ip = insn_ip; raise_exception(13, 0); break; }
      set_rm32(mr, imm);
    } else {
      emu88_uint16 imm = fetch_ip_word();
      if (!lock_ud && (ip - insn_ip) > 10) { ip = insn_ip; raise_exception(13, 0); break; }
      set_rm16(mr, imm);
    }
    break;
  }

  //--- RETF imm16 ---
  case 0xCA: {
    // Save ESP for fault rollback (x86 RETF is restartable)
    uint32_t saved_esp_retf = get_esp();
    emu88_uint16 pop_count = fetch_ip_word();
    if (op_size_32) {
      emu88_uint32 new_eip = pop_dword();
      if (exception_pending) break;
      emu88_uint16 new_cs = pop_dword() & 0xFFFF;
      if (exception_pending) break;
      emu88_uint8 ret_cpl = new_cs & 3;
      if (protected_mode() && !v86_mode() && ret_cpl > cpl) {
        // Inter-privilege return: skip pop_count bytes, then pop ESP and SS
        if (stack_32()) set_esp(get_esp() + pop_count);
        else regs[reg_SP] += pop_count;
        emu88_uint32 new_esp = pop_dword();
        if (exception_pending) { if (!dpmi_exc_dispatched) set_esp(saved_esp_retf); break; }
        emu88_uint16 new_ss = pop_dword() & 0xFFFF;
        if (exception_pending) { if (!dpmi_exc_dispatched) set_esp(saved_esp_retf); break; }
        load_segment(seg_CS, new_cs, ret_cpl);
        if (exception_pending) { if (!dpmi_exc_dispatched) set_esp(saved_esp_retf); break; }
        load_segment(seg_SS, new_ss, ret_cpl);
        if (exception_pending) { if (!dpmi_exc_dispatched) { sregs[seg_CS] = (sregs[seg_CS] & 0xFFFC) | cpl; set_esp(saved_esp_retf); } break; }
        cpl = ret_cpl;
        ip = new_eip;
        set_esp(new_esp + pop_count);
        invalidate_segments_for_cpl();
      } else if (protected_mode() && !v86_mode() && ret_cpl < cpl) {
        set_esp(saved_esp_retf);
        raise_exception(13, new_cs & 0xFFFC);
      } else {
        load_segment(seg_CS, new_cs);
        if (exception_pending) { if (!dpmi_exc_dispatched) set_esp(saved_esp_retf); break; }
        ip = new_eip;
        if (stack_32()) set_esp(get_esp() + pop_count);
        else regs[reg_SP] += pop_count;
      }
    } else {
      emu88_uint16 new_ip = pop_word();
      if (exception_pending) break;
      emu88_uint16 new_cs = pop_word();
      if (exception_pending) break;
      emu88_uint8 ret_cpl = new_cs & 3;
      if (protected_mode() && !v86_mode() && ret_cpl > cpl) {
        if (stack_32()) set_esp(get_esp() + pop_count);
        else regs[reg_SP] += pop_count;
        emu88_uint16 new_sp = pop_word();
        if (exception_pending) { if (!dpmi_exc_dispatched) set_esp(saved_esp_retf); break; }
        emu88_uint16 new_ss = pop_word();
        if (exception_pending) { if (!dpmi_exc_dispatched) set_esp(saved_esp_retf); break; }
        load_segment(seg_CS, new_cs, ret_cpl);
        if (exception_pending) { if (!dpmi_exc_dispatched) set_esp(saved_esp_retf); break; }
        load_segment(seg_SS, new_ss, ret_cpl);
        if (exception_pending) { if (!dpmi_exc_dispatched) { sregs[seg_CS] = (sregs[seg_CS] & 0xFFFC) | cpl; set_esp(saved_esp_retf); } break; }
        cpl = ret_cpl;
        ip = new_ip;
        regs[reg_SP] = new_sp + pop_count;
        invalidate_segments_for_cpl();
      } else if (protected_mode() && !v86_mode() && ret_cpl < cpl) {
        set_esp(saved_esp_retf);
        raise_exception(13, new_cs & 0xFFFC);
      } else {
        load_segment(seg_CS, new_cs);
        if (exception_pending) { if (!dpmi_exc_dispatched) set_esp(saved_esp_retf); break; }
        ip = new_ip;
        if (stack_32()) set_esp(get_esp() + pop_count);
        else regs[reg_SP] += pop_count;
      }
    }
    break;
  }

  //--- RETF ---
  case 0xCB: {
    // Save ESP for fault rollback (x86 RETF is restartable)
    uint32_t saved_esp_retf = get_esp();
    // Debug: trace RETF at the DOS4GW common handler exit
    if (insn_ip == 0x6B2B && sregs[seg_CS] == 0x000C) {
      static int retf_trace = 0;
      if (retf_trace < 3) {
        retf_trace++;
        fprintf(stderr, "[RETF-TRACE] at 000C:6B2B op32=%d stack32=%d SP=%04X ESP=%08X SS=%04X SS_base=%08X\n",
                op_size_32, stack_32(), regs[reg_SP], get_esp(), sregs[seg_SS],
                seg_cache[seg_SS].base);
      }
    }
    if (op_size_32) {
      emu88_uint32 new_eip = pop_dword();
      if (exception_pending) break;
      emu88_uint16 new_cs = pop_dword() & 0xFFFF;
      if (exception_pending) break;
      emu88_uint8 ret_cpl = new_cs & 3;
      if (protected_mode() && !v86_mode() && ret_cpl > cpl) {
        emu88_uint32 new_esp = pop_dword();
        if (exception_pending) { if (!dpmi_exc_dispatched) set_esp(saved_esp_retf); break; }
        emu88_uint16 new_ss = pop_dword() & 0xFFFF;
        if (exception_pending) { if (!dpmi_exc_dispatched) set_esp(saved_esp_retf); break; }
        // Use cpl_override=ret_cpl so privilege checks use the target ring
        // CPL is NOT changed until all checks pass (fault = no state change)
        load_segment(seg_CS, new_cs, ret_cpl);
        if (exception_pending) { if (!dpmi_exc_dispatched) set_esp(saved_esp_retf); break; }
        load_segment(seg_SS, new_ss, ret_cpl);
        if (exception_pending) { if (!dpmi_exc_dispatched) { sregs[seg_CS] = (sregs[seg_CS] & 0xFFFC) | cpl; set_esp(saved_esp_retf); } break; }
        // All checks passed — commit privilege change
        cpl = ret_cpl;
        ip = new_eip;
        set_esp(new_esp);
        invalidate_segments_for_cpl();
      } else if (protected_mode() && !v86_mode() && ret_cpl < cpl) {
        set_esp(saved_esp_retf);
        raise_exception(13, new_cs & 0xFFFC);
      } else {
        load_segment(seg_CS, new_cs);
        if (exception_pending) { if (!dpmi_exc_dispatched) set_esp(saved_esp_retf); break; }
        ip = new_eip;
      }
    } else {
      emu88_uint16 new_ip = pop_word();
      if (exception_pending) break;
      emu88_uint16 new_cs = pop_word();
      if (exception_pending) break;
      emu88_uint8 ret_cpl = new_cs & 3;
      if (protected_mode() && !v86_mode() && ret_cpl > cpl) {
        emu88_uint16 new_sp = pop_word();
        if (exception_pending) { if (!dpmi_exc_dispatched) set_esp(saved_esp_retf); break; }
        emu88_uint16 new_ss = pop_word();
        if (exception_pending) { if (!dpmi_exc_dispatched) set_esp(saved_esp_retf); break; }
        load_segment(seg_CS, new_cs, ret_cpl);
        if (exception_pending) { if (!dpmi_exc_dispatched) set_esp(saved_esp_retf); break; }
        load_segment(seg_SS, new_ss, ret_cpl);
        if (exception_pending) { if (!dpmi_exc_dispatched) { sregs[seg_CS] = (sregs[seg_CS] & 0xFFFC) | cpl; set_esp(saved_esp_retf); } break; }
        cpl = ret_cpl;
        ip = new_ip;
        regs[reg_SP] = new_sp;
        invalidate_segments_for_cpl();
      } else if (protected_mode() && !v86_mode() && ret_cpl < cpl) {
        set_esp(saved_esp_retf);
        raise_exception(13, new_cs & 0xFFFC);
      } else {
        load_segment(seg_CS, new_cs);
        if (exception_pending) { if (!dpmi_exc_dispatched) set_esp(saved_esp_retf); break; }
        ip = new_ip;
      }
    }
    break;
  }

  //--- INT 3 ---
  case 0xCC:
    // In V86 mode with IOPL < 3, #GP(0) for monitor to handle
    if (v86_mode() && get_iopl() < 3) {
      raise_exception(13, 0);
      break;
    }
    if (protected_mode()) do_interrupt_pm(3, false, 0, true);
    else do_interrupt(3);
    break;

  //--- INT imm8 ---
  case 0xCD: {
    emu88_uint8 vec = fetch_ip_byte();
    // In V86 mode with IOPL < 3, #GP(0) for monitor to handle
    if (v86_mode() && get_iopl() < 3) {
      // Trace V86 INT 21h calls (file I/O during DOS4GW init)
      if (vec == 0x21) {
        static int v86_int21_count = 0;
        if (v86_int21_count < 50) {
          v86_int21_count++;
          fprintf(stderr, "[V86-INT21] AH=%02X BX=%04X CX=%04X DX=%04X DS=%04X CS:IP=%04X:%04X\n",
                  get_reg8(reg_AH), regs[reg_BX], regs[reg_CX], regs[reg_DX],
                  sregs[seg_DS], sregs[seg_CS], insn_ip);
        }
      }
      raise_exception(13, 0);
      break;
    }
    if (protected_mode()) {
      do_interrupt_pm(vec, false, 0, true);
    }
    else do_interrupt(vec);
    break;
  }

  //--- INTO ---
  case 0xCE:
    if (get_flag(FLAG_OF)) {
      if (v86_mode() && get_iopl() < 3) {
        raise_exception(13, 0);
        break;
      }
      if (protected_mode()) do_interrupt_pm(4, false, 0, true);
      else do_interrupt(4);
    }
    break;

  //--- IRET / IRETD ---
  case 0xCF:
    if (v86_mode()) {
      // V86 mode IRET: IOPL must be 3 or #GP(0)
      if (get_iopl() < 3) {
        raise_exception(13, 0);
        break;
      }
      // V86 IRET acts like real mode
      if (op_size_32) {
        ip = pop_dword();
        load_segment_real(seg_CS, pop_dword() & 0xFFFF);
        // Don't change IOPL or VM in V86 mode
        emu88_uint32 new_eflags = pop_dword();
        emu88_uint32 preserved = get_eflags() & (EFLAG_IOPL_MASK | EFLAG_VM);
        set_eflags(((new_eflags & ~(EFLAG_IOPL_MASK | EFLAG_VM)) | preserved) | 0x0002);
      } else {
        ip = pop_word();
        load_segment_real(seg_CS, pop_word());
        emu88_uint16 new_flags = pop_word();
        emu88_uint16 preserved = flags & (emu88_uint16)EFLAG_IOPL_MASK;
        flags = ((new_flags & ~(emu88_uint16)EFLAG_IOPL_MASK) | preserved) | 0x0002;
      }
    } else if (protected_mode()) {
      // Check NT flag — if set, do task switch back via backlink
      if (flags & EFLAG_NT) {
        emu88_uint16 backlink = read_linear16(tr_cache.base);
        task_switch(backlink, false, true);
        break;
      }

      if (op_size_32) {
        emu88_uint32 new_eip = pop_dword();
        emu88_uint32 new_cs = pop_dword() & 0xFFFF;
        emu88_uint32 new_eflags = pop_dword();

        // Check for return to V86 mode (VM bit set and we're in ring 0)
        if ((new_eflags & EFLAG_VM) && cpl == 0) {
          // Return to V86 mode: pop ESP, SS, ES, DS, FS, GS
          emu88_uint32 new_esp = pop_dword();
          emu88_uint16 new_ss = pop_dword() & 0xFFFF;
          emu88_uint16 new_es = pop_dword() & 0xFFFF;
          emu88_uint16 new_ds = pop_dword() & 0xFFFF;
          emu88_uint16 new_fs = pop_dword() & 0xFFFF;
          emu88_uint16 new_gs = pop_dword() & 0xFFFF;

          // Set EFLAGS with VM bit
          set_eflags((new_eflags & 0x003FFFFF) | 0x0002);

          // Load segment registers as real mode values
          sregs[seg_CS] = new_cs;
          seg_cache[seg_CS].base = (emu88_uint32)new_cs << 4;
          seg_cache[seg_CS].limit = 0xFFFF;
          seg_cache[seg_CS].access = 0x9B;
          seg_cache[seg_CS].flags = 0;
          seg_cache[seg_CS].valid = true;

          ip = new_eip;

          load_segment_real(seg_SS, new_ss);
          set_esp(new_esp);
          load_segment_real(seg_ES, new_es);
          load_segment_real(seg_DS, new_ds);
          load_segment_real(seg_FS, new_fs);
          load_segment_real(seg_GS, new_gs);
          cpl = 3;  // V86 mode is always ring 3
        } else {
          // Normal protected mode IRET
          emu88_uint8 ret_cpl = new_cs & 3;
          if (ret_cpl > cpl) {
            // Outer privilege: pop ESP and SS
            emu88_uint32 new_esp = pop_dword();
            emu88_uint16 new_ss = pop_dword() & 0xFFFF;
            // Trace IRET from ring 0 to ring 3 (GP handler return)
            {
              static int iret_trace = 0;
              if (cpl == 0 && ret_cpl == 3 && iret_trace < 5) {
                iret_trace++;
                fprintf(stderr, "[IRET-TRACE] #%d: ring %d→%d CS:EIP=%04X:%08X EFLAGS=%08X SS:ESP=%04X:%08X\n",
                        iret_trace, cpl, ret_cpl, (uint16_t)new_cs, new_eip, new_eflags,
                        new_ss, new_esp);
              }
            }
            // Use cpl_override to check with target privilege level
            load_segment(seg_CS, new_cs, ret_cpl);
            if (exception_pending) break;
            load_segment(seg_SS, new_ss, ret_cpl);
            if (exception_pending) { sregs[seg_CS] = (sregs[seg_CS] & 0xFFFC) | cpl; break; }
            // All checks passed — commit
            set_eflags((new_eflags & 0x003FFFFF) | 0x0002);
            cpl = ret_cpl;
            ip = new_eip;
            set_esp(new_esp);
            invalidate_segments_for_cpl();
          } else {
            // Same privilege IRETD: CPL>0 cannot change IOPL; CPL>IOPL cannot change IF
            if (cpl > 0) {
              emu88_uint32 mask = 0x003FFFFF & ~(emu88_uint32)EFLAG_IOPL_MASK;
              if (cpl > get_iopl()) mask &= ~(emu88_uint32)FLAG_IF;
              emu88_uint32 preserved = get_eflags() & ~mask;
              set_eflags((preserved | (new_eflags & mask)) | 0x0002);
            } else {
              set_eflags((new_eflags & 0x003FFFFF) | 0x0002);
            }
            load_segment(seg_CS, new_cs);
            ip = new_eip;
          }
        }
      } else {
        emu88_uint16 new_ip = pop_word();
        emu88_uint16 new_cs = pop_word();
        emu88_uint16 new_flags = pop_word();
        emu88_uint8 ret_cpl = new_cs & 3;
        if (ret_cpl > cpl) {
          emu88_uint16 new_sp = pop_word();
          emu88_uint16 new_ss = pop_word();
          // Use cpl_override to check with target privilege level
          load_segment(seg_CS, new_cs, ret_cpl);
          if (exception_pending) break;
          load_segment(seg_SS, new_ss, ret_cpl);
          if (exception_pending) { sregs[seg_CS] = (sregs[seg_CS] & 0xFFFC) | cpl; break; }
          // All checks passed — commit
          { uint8_t oi = (flags >> 12) & 3; flags = (new_flags & 0x7FD7) | 0x0002; uint8_t ni = (flags >> 12) & 3;
            if (ni != oi) { static int il = 0; if (il < 10) { il++; fprintf(stderr, "[IOPL-IRET16-PC] %d→%d flags=%04X at %04X:%08X\n", oi, ni, flags, sregs[seg_CS], insn_ip); } } }
          cpl = ret_cpl;
          ip = new_ip;
          regs[reg_SP] = new_sp;
          invalidate_segments_for_cpl();
        } else {
          // Same privilege IRET16: CPL>0 cannot change IOPL; CPL>IOPL cannot change IF
          if (cpl > 0) {
            emu88_uint16 mask = 0x7FD7;
            mask &= ~(emu88_uint16)EFLAG_IOPL_MASK;
            if (cpl > get_iopl()) mask &= ~(emu88_uint16)FLAG_IF;
            flags = ((flags & ~mask) | (new_flags & mask)) | 0x0002;
          } else {
            flags = (new_flags & 0x7FD7) | 0x0002;
          }
          load_segment(seg_CS, new_cs);
          ip = new_ip;
        }
      }
    } else {
      // Real mode IRET
      if (op_size_32) {
        ip = pop_dword();
        load_segment_real(seg_CS, pop_dword() & 0xFFFF);
        set_eflags((pop_dword() & 0x003FFFFF) | 0x0002);
      } else {
        ip = pop_word();
        load_segment_real(seg_CS, pop_word());
        { uint8_t oi = (flags >> 12) & 3; flags = (pop_word() & 0x7FD7) | 0x0002; uint8_t ni = (flags >> 12) & 3;
          if (ni != oi) { static int il = 0; if (il < 10) { il++; fprintf(stderr, "[IOPL-IRET16-RM] %d→%d at %04X:%04X\n", oi, ni, sregs[seg_CS], ip); } } }
      }
    }
    break;

  //--- GRP2: shift/rotate r/m8, 1 ---
  case 0xD0: {
    emu88_uint8 modrm = fetch_ip_byte();
    modrm_result mr = decode_modrm(modrm);
    execute_grp2_rm8(mr, 1);
    break;
  }

  //--- GRP2: shift/rotate r/m16, 1 ---
  case 0xD1: {
    emu88_uint8 modrm = fetch_ip_byte();
    modrm_result mr = decode_modrm(modrm);
    if (op_size_32) execute_grp2_rm32(mr, 1);
    else execute_grp2_rm16(mr, 1);
    break;
  }

  //--- GRP2: shift/rotate r/m8, CL ---
  case 0xD2: {
    emu88_uint8 modrm = fetch_ip_byte();
    modrm_result mr = decode_modrm(modrm);
    execute_grp2_rm8(mr, get_reg8(reg_CL));
    break;
  }

  //--- GRP2: shift/rotate r/m16, CL ---
  case 0xD3: {
    emu88_uint8 modrm = fetch_ip_byte();
    modrm_result mr = decode_modrm(modrm);
    if (op_size_32) execute_grp2_rm32(mr, get_reg8(reg_CL));
    else execute_grp2_rm16(mr, get_reg8(reg_CL));
    break;
  }

  //--- AAM ---
  case 0xD4: {
    emu88_uint8 base = fetch_ip_byte();  // usually 0x0A
    if (base == 0) {
      // 286: AAM 0 modifies flags before firing #DE
      clear_flag(FLAG_CF); clear_flag(FLAG_OF);
      clear_flag(FLAG_SF); clear_flag(FLAG_ZF); clear_flag(FLAG_AF);
      set_flag_val(FLAG_PF, parity_table[get_reg8(reg_AL)]);
      ip = insn_ip;
      do_interrupt(0);
      break;
    }
    emu88_uint8 al = get_reg8(reg_AL);
    set_reg8(reg_AH, al / base);
    set_reg8(reg_AL, al % base);
    set_flags_zsp8(get_reg8(reg_AL));
    if (!lock_ud) {
      clear_flag(FLAG_CF);
      clear_flag(FLAG_OF);
      clear_flag(FLAG_AF);
    }
    break;
  }

  //--- AAD ---
  case 0xD5: {
    emu88_uint8 base = fetch_ip_byte();  // usually 0x0A
    emu88_uint8 al = get_reg8(reg_AL);
    emu88_uint8 ah = get_reg8(reg_AH);
    emu88_uint8 product = (ah * base) & 0xFF;
    emu88_uint8 result = (product + al) & 0xFF;
    set_reg8(reg_AL, result);
    set_reg8(reg_AH, 0);
    if (cpu_type == CPU_8088) {
      // 8088: AAD sets ZSP, CF, AF, OF from internal addition overflow
      bool cf = (emu88_uint16(product) + al) > 0xFF;
      set_flags_zsp8(result);
      set_flag_val(FLAG_CF, cf);
      set_flag_val(FLAG_OF, ((product ^ result) & (al ^ result) & 0x80) != 0);
      set_flag_val(FLAG_AF, ((product ^ al ^ result) & 0x10) != 0);
    } else if (!lock_ud) {
      bool cf = (emu88_uint16(product) + al) > 0xFF;
      set_flags_zsp8(result);
      set_flag_val(FLAG_CF, cf);
      set_flag_val(FLAG_OF, cf);
      set_flag_val(FLAG_AF, ((product ^ al ^ result) & 0x10) != 0);
    } else {
      set_flags_zsp8(result);
    }
    break;
  }

  //--- XLAT ---
  case 0xD7: {
    emu88_uint16 addr = regs[reg_BX] + get_reg8(reg_AL);
    set_reg8(reg_AL, fetch_byte(default_segment(), addr));
    break;
  }

  //--- ESC (FPU escape, 0xD8-0xDF) ---
  case 0xD8: case 0xD9: case 0xDA: case 0xDB:
  case 0xDC: case 0xDD: case 0xDE: case 0xDF: {
    // If CR0.EM or CR0.TS is set, raise #NM (device not available)
    // CR0.TS is set by task switch; CWSDPMI uses this for lazy FPU context switching
    if (cr0 & (CR0_EM | CR0_TS)) {
      raise_exception_no_error(7);
      break;
    }
    execute_fpu(opcode);
    break;
  }

  //--- LOOPNZ/LOOPNE ---
  case 0xE0: {
    emu88_int8 disp = (emu88_int8)fetch_ip_byte();
    if (addr_size_32) {
      set_reg32(reg_CX, get_reg32(reg_CX) - 1);
      if (get_reg32(reg_CX) != 0 && !get_flag(FLAG_ZF)) ip += disp;
    } else {
      regs[reg_CX]--;
      if (regs[reg_CX] != 0 && !get_flag(FLAG_ZF)) ip += disp;
    }
    break;
  }

  //--- LOOPZ/LOOPE ---
  case 0xE1: {
    emu88_int8 disp = (emu88_int8)fetch_ip_byte();
    if (addr_size_32) {
      set_reg32(reg_CX, get_reg32(reg_CX) - 1);
      if (get_reg32(reg_CX) != 0 && get_flag(FLAG_ZF)) ip += disp;
    } else {
      regs[reg_CX]--;
      if (regs[reg_CX] != 0 && get_flag(FLAG_ZF)) ip += disp;
    }
    break;
  }

  //--- LOOP ---
  case 0xE2: {
    emu88_int8 disp = (emu88_int8)fetch_ip_byte();
    if (addr_size_32) {
      set_reg32(reg_CX, get_reg32(reg_CX) - 1);
      if (get_reg32(reg_CX) != 0) ip += disp;
    } else {
      regs[reg_CX]--;
      if (regs[reg_CX] != 0) ip += disp;
    }
    break;
  }

  //--- JCXZ / JECXZ ---
  case 0xE3: {
    emu88_int8 disp = (emu88_int8)fetch_ip_byte();
    if (addr_size_32) {
      if (get_reg32(reg_CX) == 0) ip += disp;
    } else {
      if (regs[reg_CX] == 0) ip += disp;
    }
    break;
  }

  //--- IN AL, imm8 ---
  case 0xE4: {
    emu88_uint8 port = fetch_ip_byte();
    if (protected_mode() && !check_io_permission(port, 1)) {
      raise_exception(13, 0);
      break;
    }
    set_reg8(reg_AL, port_in(port));
    break;
  }

  //--- IN AX, imm8 ---
  case 0xE5: {
    emu88_uint8 port = fetch_ip_byte();
    emu88_uint8 width = op_size_32 ? 4 : 2;
    if (protected_mode() && !check_io_permission(port, width)) {
      raise_exception(13, 0);
      break;
    }
    if (op_size_32) set_reg32(reg_AX, port_in16(port) | (emu88_uint32(port_in16(port + 2)) << 16));
    else regs[reg_AX] = port_in16(port);
    break;
  }

  //--- OUT imm8, AL ---
  case 0xE6: {
    emu88_uint8 port = fetch_ip_byte();
    if (protected_mode() && !check_io_permission(port, 1)) {
      raise_exception(13, 0);
      break;
    }
    port_out(port, get_reg8(reg_AL));
    break;
  }

  //--- OUT imm8, AX ---
  case 0xE7: {
    emu88_uint8 port = fetch_ip_byte();
    emu88_uint8 width = op_size_32 ? 4 : 2;
    if (protected_mode() && !check_io_permission(port, width)) {
      raise_exception(13, 0);
      break;
    }
    if (op_size_32) {
      emu88_uint32 val = get_reg32(reg_AX);
      port_out16(port, val & 0xFFFF);
      port_out16(port + 2, (val >> 16) & 0xFFFF);
    } else {
      port_out16(port, regs[reg_AX]);
    }
    break;
  }

  //--- CALL near rel16/rel32 ---
  case 0xE8: {
    if (op_size_32) {
      emu88_int32 disp = (emu88_int32)fetch_ip_dword();
      push_dword(ip);
      ip += disp;
    } else {
      emu88_int16 disp = (emu88_int16)fetch_ip_word();
      push_word(ip & 0xFFFF);
      ip += disp;
    }
    break;
  }

  //--- JMP near rel16/rel32 ---
  case 0xE9: {
    if (op_size_32) {
      emu88_int32 disp = (emu88_int32)fetch_ip_dword();
      ip += disp;
    } else {
      emu88_int16 disp = (emu88_int16)fetch_ip_word();
      ip += disp;
    }
    break;
  }

  //--- JMP far ptr16:16 / ptr16:32 ---
  case 0xEA: {
    if (op_size_32) {
      emu88_uint32 off = fetch_ip_dword();
      emu88_uint16 seg = fetch_ip_word();
      if (!lock_ud && (ip - insn_ip) > 10) { ip = insn_ip; raise_exception(13, 0); break; }
      far_call_or_jmp(seg, off, false);
    } else {
      emu88_uint16 off = fetch_ip_word();
      emu88_uint16 seg = fetch_ip_word();
      if (!lock_ud && (ip - insn_ip) > 10) { ip = insn_ip; raise_exception(13, 0); break; }
      far_call_or_jmp(seg, off, false);
    }
    break;
  }

  //--- JMP short rel8 ---
  case 0xEB: {
    emu88_int8 disp = (emu88_int8)fetch_ip_byte();
    ip += disp;
    break;
  }

  //--- IN AL, DX ---
  case 0xEC: {
    emu88_uint16 port = regs[reg_DX];
    if (protected_mode() && !check_io_permission(port, 1)) {
      raise_exception(13, 0);
      break;
    }
    set_reg8(reg_AL, port_in(port));
    break;
  }

  //--- IN AX, DX ---
  case 0xED: {
    emu88_uint16 port = regs[reg_DX];
    emu88_uint8 width = op_size_32 ? 4 : 2;
    if (protected_mode() && !check_io_permission(port, width)) {
      raise_exception(13, 0);
      break;
    }
    if (op_size_32) {
      set_reg32(reg_AX, port_in16(port) | (emu88_uint32(port_in16(port + 2)) << 16));
    } else {
      regs[reg_AX] = port_in16(port);
    }
    break;
  }

  //--- OUT DX, AL ---
  case 0xEE: {
    emu88_uint16 port = regs[reg_DX];
    if (protected_mode() && !check_io_permission(port, 1)) {
      raise_exception(13, 0);
      break;
    }
    port_out(port, get_reg8(reg_AL));
    break;
  }

  //--- OUT DX, AX ---
  case 0xEF: {
    emu88_uint16 port = regs[reg_DX];
    emu88_uint8 width = op_size_32 ? 4 : 2;
    if (protected_mode() && !check_io_permission(port, width)) {
      raise_exception(13, 0);
      break;
    }
    if (op_size_32) {
      emu88_uint32 val = get_reg32(reg_AX);
      port_out16(port, val & 0xFFFF);
      port_out16(port + 2, (val >> 16) & 0xFFFF);
    } else {
      port_out16(port, regs[reg_AX]);
    }
    break;
  }

  //--- HLT ---
  case 0xF4:
    if (protected_mode() && cpl != 0) { raise_exception(13, 0); break; }
    halt_cpu();
    break;

  //--- CMC ---
  case 0xF5:
    set_flag_val(FLAG_CF, !get_flag(FLAG_CF));
    break;

  //--- GRP3: r/m8 ---
  case 0xF6: {
    emu88_uint8 modrm = fetch_ip_byte();
    execute_grp3_rm8(modrm);
    break;
  }

  //--- GRP3: r/m16 ---
  case 0xF7: {
    emu88_uint8 modrm = fetch_ip_byte();
    if (op_size_32) execute_grp3_rm32(modrm);
    else execute_grp3_rm16(modrm);
    break;
  }

  //--- CLC ---
  case 0xF8: clear_flag(FLAG_CF); break;
  //--- STC ---
  case 0xF9: set_flag(FLAG_CF); break;
  //--- CLI ---
  case 0xFA:
    // In V86 mode: IOPL must be 3 or #GP(0)
    // In protected mode: CPL must be <= IOPL or #GP(0)
    if (v86_mode() && get_iopl() < 3) { raise_exception(13, 0); break; }
    if (protected_mode() && !v86_mode() && cpl > get_iopl()) { raise_exception(13, 0); break; }
    clear_flag(FLAG_IF);
    break;
  //--- STI ---
  case 0xFB:
    if (v86_mode() && get_iopl() < 3) { raise_exception(13, 0); break; }
    if (protected_mode() && !v86_mode() && cpl > get_iopl()) { raise_exception(13, 0); break; }
    set_flag(FLAG_IF);
    break;
  //--- CLD ---
  case 0xFC: clear_flag(FLAG_DF); break;
  //--- STD ---
  case 0xFD: set_flag(FLAG_DF); break;

  //--- GRP4: INC/DEC r/m8 ---
  case 0xFE: {
    emu88_uint8 modrm = fetch_ip_byte();
    execute_grp4_rm8(modrm);
    break;
  }

  //--- GRP5: misc r/m16 ---
  case 0xFF: {
    emu88_uint8 modrm = fetch_ip_byte();
    if (op_size_32) execute_grp5_rm32(modrm);
    else execute_grp5_rm16(modrm);
    break;
  }

  //--- 80186+ instructions ---

  //--- PUSHA / PUSHAD (80186+) ---
  case 0x60: {
    if (op_size_32) {
      emu88_uint32 tmp_esp = get_reg32(reg_SP);
      push_dword(get_reg32(reg_AX));
      push_dword(get_reg32(reg_CX));
      push_dword(get_reg32(reg_DX));
      push_dword(get_reg32(reg_BX));
      push_dword(tmp_esp);
      push_dword(get_reg32(reg_BP));
      push_dword(get_reg32(reg_SI));
      push_dword(get_reg32(reg_DI));
    } else {
      // 286: pre-check if any push would cross segment boundary
      if (!lock_ud) {
        for (int k = 1; k <= 8; k++) {
          emu88_uint16 addr = regs[reg_SP] - 2 * k;
          if (addr == 0xFFFF) {
            ip = insn_ip;
            raise_exception(13, 0);
            goto pusha_done;
          }
        }
      }
      {
        emu88_uint16 tmp_sp = regs[reg_SP];
        push_word(regs[reg_AX]);
        push_word(regs[reg_CX]);
        push_word(regs[reg_DX]);
        push_word(regs[reg_BX]);
        push_word(tmp_sp);
        push_word(regs[reg_BP]);
        push_word(regs[reg_SI]);
        push_word(regs[reg_DI]);
      }
    }
    pusha_done:
    break;
  }

  //--- POPA / POPAD (80186+) ---
  case 0x61: {
    if (op_size_32) {
      emu88_uint32 v;
      v = pop_dword(); if (exception_pending) break; set_reg32(reg_DI, v);
      v = pop_dword(); if (exception_pending) break; set_reg32(reg_SI, v);
      v = pop_dword(); if (exception_pending) break; set_reg32(reg_BP, v);
      pop_dword(); if (exception_pending) break;
      v = pop_dword(); if (exception_pending) break; set_reg32(reg_BX, v);
      v = pop_dword(); if (exception_pending) break; set_reg32(reg_DX, v);
      v = pop_dword(); if (exception_pending) break; set_reg32(reg_CX, v);
      v = pop_dword(); set_reg32(reg_AX, v);
    } else {
      // 286: pre-check if any pop would cross segment boundary
      if (!lock_ud) {
        for (int k = 0; k < 8; k++) {
          emu88_uint16 addr = regs[reg_SP] + 2 * k;
          if (addr == 0xFFFF) {
            ip = insn_ip;
            raise_exception(13, 0);
            goto popa_done;
          }
        }
      }
      {
        emu88_uint16 v;
        v = pop_word(); if (exception_pending) break; regs[reg_DI] = v;
        v = pop_word(); if (exception_pending) break; regs[reg_SI] = v;
        v = pop_word(); if (exception_pending) break; regs[reg_BP] = v;
        pop_word(); if (exception_pending) break;
        v = pop_word(); if (exception_pending) break; regs[reg_BX] = v;
        v = pop_word(); if (exception_pending) break; regs[reg_DX] = v;
        v = pop_word(); if (exception_pending) break; regs[reg_CX] = v;
        v = pop_word(); regs[reg_AX] = v;
      }
    }
    popa_done:
    break;
  }

  //--- BOUND (80186+) ---
  case 0x62: {
    emu88_uint8 modrm = fetch_ip_byte();
    modrm_result mr = addr_size_32 ? decode_modrm_32(modrm) : decode_modrm(modrm);
    if (mr.is_register) { raise_exception_no_error(6); break; }  // #UD for register operand
    if (op_size_32) {
      emu88_int32 idx = (emu88_int32)get_reg32(mr.reg_field);
      emu88_int32 lo = (emu88_int32)fetch_dword(mr.seg, mr.offset);
      if (exception_pending) break;
      emu88_int32 hi = (emu88_int32)fetch_dword(mr.seg, mr.offset + 4);
      if (exception_pending) break;
      if (idx < lo || idx > hi) raise_exception_no_error(5);
    } else {
      emu88_int16 idx = (emu88_int16)regs[mr.reg_field];
      emu88_int16 lo = (emu88_int16)fetch_word(mr.seg, mr.offset);
      if (exception_pending) break;
      emu88_int16 hi = (emu88_int16)fetch_word(mr.seg, mr.offset + 2);
      if (exception_pending) break;
      if (idx < lo || idx > hi) raise_exception_no_error(5);
    }
    break;
  }

  //--- ARPL r/m16, r16 (286+ protected mode) ---
  case 0x63: {
    if (protected_mode()) {
      emu88_uint8 modrm = fetch_ip_byte();
      modrm_result mr = decode_modrm(modrm);
      emu88_uint16 dst = get_rm16(mr);
      emu88_uint16 src = regs[mr.reg_field];
      if ((dst & 3) < (src & 3)) {
        dst = (dst & 0xFFFC) | (src & 3);
        set_rm16(mr, dst);
        set_flag(FLAG_ZF);
      } else {
        clear_flag(FLAG_ZF);
      }
    } else {
      // In real mode, 0x63 is undefined — skip modrm
      emu88_uint8 modrm = fetch_ip_byte();
      (void)modrm;
    }
    break;
  }

  //--- PUSH imm16 (80186+) ---
  case 0x68:
    if (op_size_32) push_dword(fetch_ip_dword());
    else push_word(fetch_ip_word());
    break;

  //--- IMUL r16, r/m16, imm16 (80186+) ---
  case 0x69: {
    emu88_uint8 modrm = fetch_ip_byte();
    modrm_result mr = decode_modrm(modrm);
    if (op_size_32) {
      emu88_int32 src = (emu88_int32)get_rm32(mr);
      if (exception_pending) break;
      emu88_int32 imm = (emu88_int32)fetch_ip_dword();
      if (!lock_ud && (ip - insn_ip) > 10) { ip = insn_ip; raise_exception(13, 0); break; }
      emu88_int64 result = (emu88_int64)src * (emu88_int64)imm;
      set_reg32(mr.reg_field, (emu88_uint32)result);
      set_flag_val(FLAG_CF, result != (emu88_int32)result);
      set_flag_val(FLAG_OF, result != (emu88_int32)result);
      if (!lock_ud) {
        set_flags_zsp32((emu88_uint32)(result >> 32));
        set_flag(FLAG_AF);
      }
    } else {
      emu88_int16 src = (emu88_int16)get_rm16(mr);
      if (exception_pending) break;
      emu88_int16 imm = (emu88_int16)fetch_ip_word();
      if (!lock_ud && (ip - insn_ip) > 10) { ip = insn_ip; raise_exception(13, 0); break; }
      emu88_int32 result = (emu88_int32)src * (emu88_int32)imm;
      regs[mr.reg_field] = (emu88_uint16)result;
      set_flag_val(FLAG_CF, result != (emu88_int16)result);
      set_flag_val(FLAG_OF, result != (emu88_int16)result);
      if (!lock_ud) {
        set_flags_zsp16((emu88_uint16)((emu88_uint32)result >> 16));
        set_flag(FLAG_AF);
      }
    }
    break;
  }

  //--- PUSH imm8 (sign-extended) (80186+) ---
  case 0x6A: {
    emu88_int8 imm = (emu88_int8)fetch_ip_byte();
    if (op_size_32) push_dword((emu88_uint32)(emu88_int32)imm);
    else push_word((emu88_uint16)(emu88_int16)imm);
    break;
  }

  //--- IMUL r16, r/m16, imm8 (80186+) ---
  case 0x6B: {
    emu88_uint8 modrm = fetch_ip_byte();
    modrm_result mr = decode_modrm(modrm);
    emu88_int8 imm = (emu88_int8)fetch_ip_byte();
    if (op_size_32) {
      emu88_int32 src = (emu88_int32)get_rm32(mr);
      if (exception_pending) break;
      emu88_int64 result = (emu88_int64)src * (emu88_int64)imm;
      set_reg32(mr.reg_field, (emu88_uint32)result);
      set_flag_val(FLAG_CF, result != (emu88_int32)result);
      set_flag_val(FLAG_OF, result != (emu88_int32)result);
      if (!lock_ud) {
        set_flags_zsp32((emu88_uint32)(result >> 32));
        set_flag(FLAG_AF);
      }
    } else {
      emu88_int16 src = (emu88_int16)get_rm16(mr);
      if (exception_pending) break;
      emu88_int32 result = (emu88_int32)src * (emu88_int32)imm;
      regs[mr.reg_field] = (emu88_uint16)result;
      set_flag_val(FLAG_CF, result != (emu88_int16)result);
      set_flag_val(FLAG_OF, result != (emu88_int16)result);
      if (!lock_ud) {
        set_flags_zsp16((emu88_uint16)((emu88_uint32)result >> 16));
        set_flag(FLAG_AF);
      }
    }
    break;
  }

  //--- ENTER (80186+) ---
  case 0xC8: {
    emu88_uint16 alloc_size = fetch_ip_word();
    emu88_uint8 nesting = fetch_ip_byte() & 0x1F;  // mod 32
    if (op_size_32) {
      // Probe final ESP for write access (ENTER checks final stack pointer)
      emu88_uint32 total_push = (nesting > 0) ? 4 * ((emu88_uint32)nesting + 1) : 4;
      emu88_uint32 final_esp = get_esp() - total_push - alloc_size;
      emu88_uint32 probe_off = stack_32() ? final_esp : (emu88_uint16)final_esp;
      if (!check_segment_write(sregs[seg_SS], probe_off, 1)) break;
      if (paging_enabled()) {
        translate_linear(effective_address(sregs[seg_SS], probe_off), true);
        if (exception_pending) break;
      }
      push_dword(get_reg32(reg_BP));
      emu88_uint32 frame_ptr = get_esp();
      if (nesting > 0) {
        for (int i = 1; i < nesting; i++) {
          set_reg32(reg_BP, get_reg32(reg_BP) - 4);
          push_dword(fetch_dword(sregs[seg_SS], stack_32() ? get_reg32(reg_BP) : regs[reg_BP]));
        }
        push_dword(frame_ptr);
      }
      set_reg32(reg_BP, frame_ptr);
      if (stack_32()) set_esp(get_esp() - alloc_size);
      else regs[reg_SP] -= alloc_size;
    } else {
      // Probe final SP for write access (protected mode only)
      if (protected_mode()) {
        emu88_uint16 total_push = (nesting > 0) ? 2 * (nesting + 1) : 2;
        emu88_uint16 final_sp = regs[reg_SP] - total_push - alloc_size;
        if (!check_segment_write(sregs[seg_SS], final_sp, 1)) break;
        if (paging_enabled()) {
          translate_linear(effective_address(sregs[seg_SS], final_sp), true);
          if (exception_pending) break;
        }
      }
      {
        // Save registers for 286 post-execution exception on ENTER
        emu88_uint16 saved_sp = regs[reg_SP];
        emu88_uint16 saved_bp = regs[reg_BP];
        bool enter_boundary = false;

        // 286: check if initial push BP crosses boundary
        if (!lock_ud && (emu88_uint16)(regs[reg_SP] - 2) == 0xFFFF) {
          // Wrapped push at boundary
          store_byte(sregs[seg_SS], 0xFFFF, regs[reg_BP] & 0xFF);
          store_byte(sregs[seg_SS], 0x0000, (regs[reg_BP] >> 8) & 0xFF);
          regs[reg_SP] -= 2;
          enter_boundary = true;
        } else {
          push_word(regs[reg_BP]);
        }

        emu88_uint16 frame_ptr = regs[reg_SP];
        if (!enter_boundary && nesting > 0) {
          for (int i = 1; i < nesting; i++) {
            regs[reg_BP] -= 2;
            // 286: check fetch boundary
            emu88_uint16 bp_val;
            if (!lock_ud && regs[reg_BP] == 0xFFFF) {
              emu88_uint8 lo = fetch_byte(sregs[seg_SS], 0xFFFF);
              emu88_uint8 hi = fetch_byte(sregs[seg_SS], 0x0000);
              bp_val = lo | ((emu88_uint16)hi << 8);
              enter_boundary = true;
            } else {
              bp_val = fetch_word(sregs[seg_SS], regs[reg_BP]);
            }
            // 286: check push boundary
            if (!lock_ud && (emu88_uint16)(regs[reg_SP] - 2) == 0xFFFF) {
              store_byte(sregs[seg_SS], 0xFFFF, bp_val & 0xFF);
              store_byte(sregs[seg_SS], 0x0000, (bp_val >> 8) & 0xFF);
              regs[reg_SP] -= 2;
              enter_boundary = true;
            } else {
              push_word(bp_val);
            }
            if (enter_boundary) break;
          }
          if (!enter_boundary) {
            // Push frame_ptr
            if (!lock_ud && (emu88_uint16)(regs[reg_SP] - 2) == 0xFFFF) {
              store_byte(sregs[seg_SS], 0xFFFF, frame_ptr & 0xFF);
              store_byte(sregs[seg_SS], 0x0000, (frame_ptr >> 8) & 0xFF);
              regs[reg_SP] -= 2;
              enter_boundary = true;
            } else {
              push_word(frame_ptr);
            }
          }
        }
        if (enter_boundary) {
          // Restore registers, keep memory writes, fire exception
          regs[reg_SP] = saved_sp;
          regs[reg_BP] = saved_bp;
          ip = insn_ip;
          raise_exception(13, 0);
        } else {
          regs[reg_BP] = frame_ptr;
          regs[reg_SP] -= alloc_size;
        }
      }
    }
    break;
  }

  //--- LEAVE (80186+) ---
  case 0xC9: {
    // SP/ESP ← BP/EBP depends on stack address size (B bit), not operand size
    // Pre-check: if the POP would fault (286 segment boundary), raise exception
    // before modifying SP so the fault frame uses the original SP.
    if (!stack_32()) {
      emu88_uint16 new_sp = regs[reg_BP];
      emu88_uint8 pop_width = op_size_32 ? 4 : 2;
      if (!lock_ud && pop_width > 1 && (new_sp & 0xFFFF) + (pop_width - 1) > 0xFFFF) {
        ip = insn_ip;
        raise_exception(13, 0);
        break;
      }
      regs[reg_SP] = new_sp;
    } else {
      set_esp(get_reg32(reg_BP));
    }
    // Pop BP/EBP depends on operand size
    if (op_size_32)
      set_reg32(reg_BP, pop_dword());
    else
      regs[reg_BP] = pop_word();
    break;
  }

  //--- 0x0F: POP CS (8088) or two-byte opcode prefix (286+) ---
  case 0x0F: {
    if (cpu_type == CPU_8088) {
      // 8088: 0x0F = POP CS
      emu88_uint16 val = pop_word();
      load_segment_real(seg_CS, val);
      break;
    }
    emu88_uint8 op2 = fetch_ip_byte();
    switch (op2) {

    // SLDT/STR/LLDT/LTR/VERR/VERW (0x0F 0x00)
    case 0x00: {
      emu88_uint8 modrm = fetch_ip_byte();
      modrm_result mr = decode_modrm(modrm);
      switch (mr.reg_field) {
      case 0: // SLDT: store LDT selector
        fprintf(stderr, "[SLDT] -> %04X at %04X:%08X\n", ldtr, sregs[seg_CS], insn_ip);
        set_rm16(mr, ldtr);
        break;
      case 1: // STR: store Task Register selector
        set_rm16(mr, tr);
        break;
      case 2: { // LLDT: load LDT register
        emu88_uint16 sel = get_rm16(mr);
        fprintf(stderr, "[LLDT] sel=%04X at %04X:%08X (old ldtr=%04X)\n", sel, sregs[seg_CS], insn_ip, ldtr);
        ldtr = sel;
        if ((sel & 0xFFFC) == 0) {
          ldtr_cache.valid = false;
          ldtr_cache.base = 0;
          ldtr_cache.limit = 0;
        } else {
          emu88_uint16 index = sel >> 3;
          if ((emu88_uint32)index * 8 + 7 > (emu88_uint32)gdtr_limit) {
            raise_exception(13, sel & 0xFFFC);
            break;
          }
          emu88_uint8 desc[8];
          read_descriptor(gdtr_base, index, desc);
          parse_descriptor(desc, ldtr_cache);
        }
        break;
      }
      case 3: { // LTR: load Task Register
        emu88_uint16 sel = get_rm16(mr);
        tr = sel;
        if ((sel & 0xFFFC) == 0) {
          tr_cache.valid = false;
          tr_cache.base = 0;
          tr_cache.limit = 0;
        } else {
          emu88_uint16 index = sel >> 3;
          if ((emu88_uint32)index * 8 + 7 > (emu88_uint32)gdtr_limit) {
            raise_exception(13, sel & 0xFFFC);
            break;
          }
          emu88_uint8 desc[8];
          read_descriptor(gdtr_base, index, desc);
          parse_descriptor(desc, tr_cache);
          // Mark TSS as busy (set bit 1 of type field)
          desc[5] |= 0x02;
          emu88_uint32 desc_addr = gdtr_base + (emu88_uint32)index * 8;
          mem->store_mem(desc_addr + 5, desc[5]);
        }
        break;
      }
      case 4: { // VERR: verify segment readable
        emu88_uint16 sel = get_rm16(mr);
        if ((sel & 0xFFFC) == 0) { clear_flag(FLAG_ZF); break; }
        emu88_uint16 index = sel >> 3;
        bool use_ldt = (sel & 4) != 0;
        emu88_uint32 tbase = use_ldt ? ldtr_cache.base : gdtr_base;
        emu88_uint32 tlimit = use_ldt ? ldtr_cache.limit : (emu88_uint32)gdtr_limit;
        if ((emu88_uint32)index * 8 + 7 > tlimit) { clear_flag(FLAG_ZF); break; }
        emu88_uint8 desc[8];
        read_descriptor(tbase, index, desc);
        emu88_uint8 access = desc[5];
        // Must be present, code/data segment
        if (!(access & 0x80) || !(access & 0x10)) { clear_flag(FLAG_ZF); break; }
        // Code segment must be readable (bit 1)
        if ((access & 0x08) && !(access & 0x02)) { clear_flag(FLAG_ZF); break; }
        // Privilege check: for non-conforming code, DPL must be >= MAX(CPL, RPL)
        if ((access & 0x08) && !(access & 0x04)) {
          emu88_uint8 dpl = (access >> 5) & 3;
          emu88_uint8 rpl = sel & 3;
          if (dpl < cpl || dpl < rpl) { clear_flag(FLAG_ZF); break; }
        }
        // Data segments: DPL must be >= MAX(CPL, RPL)
        if (!(access & 0x08)) {
          emu88_uint8 dpl = (access >> 5) & 3;
          emu88_uint8 rpl = sel & 3;
          if (dpl < cpl || dpl < rpl) { clear_flag(FLAG_ZF); break; }
        }
        set_flag(FLAG_ZF);
        break;
      }
      case 5: { // VERW: verify segment writable
        emu88_uint16 sel = get_rm16(mr);
        if ((sel & 0xFFFC) == 0) { clear_flag(FLAG_ZF); break; }
        emu88_uint16 index = sel >> 3;
        bool use_ldt = (sel & 4) != 0;
        emu88_uint32 tbase = use_ldt ? ldtr_cache.base : gdtr_base;
        emu88_uint32 tlimit = use_ldt ? ldtr_cache.limit : (emu88_uint32)gdtr_limit;
        if ((emu88_uint32)index * 8 + 7 > tlimit) { clear_flag(FLAG_ZF); break; }
        emu88_uint8 desc[8];
        read_descriptor(tbase, index, desc);
        emu88_uint8 access = desc[5];
        // Must be present, data segment (not code), and writable
        if (!(access & 0x80) || !(access & 0x10)) { clear_flag(FLAG_ZF); break; }
        if ((access & 0x08) || !(access & 0x02)) { clear_flag(FLAG_ZF); break; }
        // Privilege check: DPL must be >= MAX(CPL, RPL)
        emu88_uint8 dpl = (access >> 5) & 3;
        emu88_uint8 rpl = sel & 3;
        if (dpl < cpl || dpl < rpl) { clear_flag(FLAG_ZF); break; }
        set_flag(FLAG_ZF);
        break;
      }
      default:
        break;
      }
      break;
    }

    // SGDT/SIDT/LGDT/LIDT/SMSW/LMSW/INVLPG (0x0F 0x01)
    case 0x01: {
      emu88_uint8 modrm = fetch_ip_byte();
      modrm_result mr = decode_modrm(modrm);
      switch (mr.reg_field) {
      case 0: // SGDT: store 6 bytes (limit:base) to memory
        fprintf(stderr, "[SGDT] base=%08X limit=%04X at %04X:%08X\n", gdtr_base, gdtr_limit, sregs[seg_CS], insn_ip);
        store_word(mr.seg, mr.offset, gdtr_limit);
        store_word(mr.seg, mr.offset + 2, gdtr_base & 0xFFFF);
        store_byte(mr.seg, mr.offset + 4, (gdtr_base >> 16) & 0xFF);
        store_byte(mr.seg, mr.offset + 5, (gdtr_base >> 24) & 0xFF);
        break;
      case 1: // SIDT: store 6 bytes (limit:base) to memory
        store_word(mr.seg, mr.offset, idtr_limit);
        store_word(mr.seg, mr.offset + 2, idtr_base & 0xFFFF);
        store_byte(mr.seg, mr.offset + 4, (idtr_base >> 16) & 0xFF);
        store_byte(mr.seg, mr.offset + 5, (idtr_base >> 24) & 0xFF);
        break;
      case 2: { // LGDT: load 6 bytes from memory
        uint16_t old_limit = gdtr_limit;
        uint32_t old_base = gdtr_base;
        gdtr_limit = fetch_word(mr.seg, mr.offset);
        gdtr_base = fetch_word(mr.seg, mr.offset + 2) |
                    (emu88_uint32(fetch_byte(mr.seg, mr.offset + 4)) << 16) |
                    (emu88_uint32(fetch_byte(mr.seg, mr.offset + 5)) << 24);
        fprintf(stderr, "[LGDT] base=%08X->%08X limit=%04X->%04X at %04X:%08X\n",
                old_base, gdtr_base, old_limit, gdtr_limit, sregs[seg_CS], insn_ip);
        break;
      }
      case 3: // LIDT: load 6 bytes from memory
        idtr_limit = fetch_word(mr.seg, mr.offset);
        idtr_base = fetch_word(mr.seg, mr.offset + 2) |
                    (emu88_uint32(fetch_byte(mr.seg, mr.offset + 4)) << 16) |
                    (emu88_uint32(fetch_byte(mr.seg, mr.offset + 5)) << 24);
        break;
      case 4: // SMSW: store CR0 low 16 bits
        if (mr.is_register) regs[mr.rm_field] = cr0 & 0xFFFF;
        else store_word(mr.seg, mr.offset, cr0 & 0xFFFF);
        break;
      case 6: // LMSW: load CR0 low 16 bits (PE bit can be set but not cleared)
        {
          emu88_uint16 val = mr.is_register ? regs[mr.rm_field] : get_rm16(mr);
          // PE bit can be set but not cleared via LMSW
          emu88_uint32 new_cr0 = (cr0 & 0xFFFF0000) | val;
          if (cr0 & CR0_PE) new_cr0 |= CR0_PE;  // can't clear PE via LMSW
          cr0 = new_cr0;
        }
        break;
      case 7: // INVLPG: invalidate TLB entry (NOP for emulator, no TLB cache)
        break;
      default:
        break;
      }
      break;
    }

    // LAR r16/r32, r/m16 (0x0F 0x02) — Load Access Rights
    case 0x02: {
      emu88_uint8 modrm = fetch_ip_byte();
      modrm_result mr = decode_modrm(modrm);
      emu88_uint16 sel = get_rm16(mr);
      if ((sel & 0xFFFC) == 0) { clear_flag(FLAG_ZF); break; }
      emu88_uint16 index = sel >> 3;
      bool use_ldt = (sel & 4) != 0;
      emu88_uint32 tbase = use_ldt ? ldtr_cache.base : gdtr_base;
      emu88_uint32 tlimit = use_ldt ? ldtr_cache.limit : (emu88_uint32)gdtr_limit;
      if ((emu88_uint32)index * 8 + 7 > tlimit) { clear_flag(FLAG_ZF); break; }
      emu88_uint8 desc[8];
      read_descriptor(tbase, index, desc);
      // Return access byte and flags in bits 23:8 of result
      emu88_uint32 result = ((emu88_uint32)desc[5] << 8) | ((emu88_uint32)(desc[6] & 0xF0) << 8);
      if (op_size_32) set_reg32(mr.reg_field, result);
      else regs[mr.reg_field] = result & 0xFFFF;
      set_flag(FLAG_ZF);
      break;
    }

    // LSL r16/r32, r/m16 (0x0F 0x03) — Load Segment Limit
    case 0x03: {
      emu88_uint8 modrm = fetch_ip_byte();
      modrm_result mr = decode_modrm(modrm);
      emu88_uint16 sel = get_rm16(mr);
      if ((sel & 0xFFFC) == 0) { clear_flag(FLAG_ZF); break; }
      emu88_uint16 index = sel >> 3;
      bool use_ldt = (sel & 4) != 0;
      emu88_uint32 tbase = use_ldt ? ldtr_cache.base : gdtr_base;
      emu88_uint32 tlimit = use_ldt ? ldtr_cache.limit : (emu88_uint32)gdtr_limit;
      if ((emu88_uint32)index * 8 + 7 > tlimit) { clear_flag(FLAG_ZF); break; }
      emu88_uint8 desc[8];
      read_descriptor(tbase, index, desc);
      SegDescCache tmp;
      parse_descriptor(desc, tmp);
      if (op_size_32) set_reg32(mr.reg_field, tmp.limit);
      else regs[mr.reg_field] = tmp.limit & 0xFFFF;
      set_flag(FLAG_ZF);
      break;
    }

    // MOV r32, CRn (0x0F 0x20)
    case 0x20: {
      emu88_uint8 modrm = fetch_ip_byte();
      emu88_uint8 cr = (modrm >> 3) & 7;
      emu88_uint8 r = modrm & 7;
      switch (cr) {
      case 0: set_reg32(r, cr0); break;
      case 2: set_reg32(r, cr2); break;
      case 3: set_reg32(r, cr3); break;
      case 4: set_reg32(r, cr4); break;
      default: set_reg32(r, 0); break;
      }
      break;
    }

    // MOV r32, DRn (0x0F 0x21) — debug registers
    case 0x21: {
      emu88_uint8 modrm = fetch_ip_byte();
      emu88_uint8 drn = (modrm >> 3) & 7;
      emu88_uint8 r = modrm & 7;
      // DR4/DR5 alias to DR6/DR7
      if (drn == 4) drn = 6;
      if (drn == 5) drn = 7;
      set_reg32(r, dr[drn]);
      break;
    }

    // MOV CRn, r32 (0x0F 0x22)
    case 0x22: {
      emu88_uint8 modrm = fetch_ip_byte();
      emu88_uint8 cr = (modrm >> 3) & 7;
      emu88_uint8 r = modrm & 7;
      switch (cr) {
      case 0: {
        emu88_uint32 old_cr0 = cr0;
        cr0 = get_reg32(r);
        // CR0 mode transition handling:
        // When PE transitions 1->0, set unreal_mode so effective_address
        // keeps using cached segment bases until segments are reloaded.
        if ((old_cr0 & CR0_PE) && !(cr0 & CR0_PE)) {
          unreal_mode = true;
        }
        // When PE transitions 0->1, clear unreal_mode
        if (!(old_cr0 & CR0_PE) && (cr0 & CR0_PE)) {
          unreal_mode = false;
        }
        break;
      }
      case 2: cr2 = get_reg32(r); break;
      case 3: cr3 = get_reg32(r); break;
      case 4: cr4 = get_reg32(r); break;
      default: break;
      }
      break;
    }

    // MOV DRn, r32 (0x0F 0x23) — debug registers
    case 0x23: {
      emu88_uint8 modrm = fetch_ip_byte();
      emu88_uint8 drn = (modrm >> 3) & 7;
      emu88_uint8 r = modrm & 7;
      // DR4/DR5 alias to DR6/DR7
      if (drn == 4) drn = 6;
      if (drn == 5) drn = 7;
      dr[drn] = get_reg32(r);
      break;
    }

    // CLTS (0x0F 0x06) - Clear Task-Switched flag in CR0
    case 0x06:
      cr0 &= ~0x08;
      break;

    // WBINVD (0x0F 0x09) - Write-back and invalidate cache (NOP for emulator)
    case 0x09:
      break;

    // INVD (0x0F 0x08) - Invalidate cache (NOP for emulator)
    case 0x08:
      break;

    // RDTSC (0x0F 0x31) - Read Time-Stamp Counter (Task 6)
    case 0x31:
      set_reg32(reg_AX, (emu88_uint32)(cycles & 0xFFFFFFFF));
      set_reg32(reg_DX, (emu88_uint32)((cycles >> 32) & 0xFFFFFFFF));
      break;

    // CPUID (0x0F 0xA2) - CPU Identification (Task 5)
    case 0xA2: {
      emu88_uint32 eax_in = get_reg32(reg_AX);
      switch (eax_in) {
      case 0:
        // Max CPUID level = 1, vendor = "GenuineIntel"
        set_reg32(reg_AX, 1);
        set_reg32(reg_BX, 0x756E6547);  // "Genu"
        set_reg32(reg_DX, 0x49656E69);  // "ineI"
        set_reg32(reg_CX, 0x6C65746E);  // "ntel"
        break;
      case 1:
        // Family 3 (386), Model 0, Stepping 0
        set_reg32(reg_AX, 0x00000300);
        set_reg32(reg_BX, 0);
        set_reg32(reg_CX, 0);
        // Feature flags: bit 4 = PSE (page size extensions)
        set_reg32(reg_DX, 0x00000010);
        break;
      default:
        set_reg32(reg_AX, 0);
        set_reg32(reg_BX, 0);
        set_reg32(reg_CX, 0);
        set_reg32(reg_DX, 0);
        break;
      }
      break;
    }

    // PUSH FS (0x0F 0xA0)
    case 0xA0: if (op_size_32) push_dword((emu88_uint32)sregs[seg_FS]); else push_word(sregs[seg_FS]); break;
    // POP FS (0x0F 0xA1)
    case 0xA1: load_segment(seg_FS, op_size_32 ? (emu88_uint16)pop_dword() : pop_word()); break;
    // PUSH GS (0x0F 0xA8)
    case 0xA8: if (op_size_32) push_dword((emu88_uint32)sregs[seg_GS]); else push_word(sregs[seg_GS]); break;
    // POP GS (0x0F 0xA9)
    case 0xA9: load_segment(seg_GS, op_size_32 ? (emu88_uint16)pop_dword() : pop_word()); break;
    // MOVZX r16/r32, r/m8 (0x0F 0xB6)
    case 0xB6: {
      emu88_uint8 modrm = fetch_ip_byte();
      modrm_result mr = decode_modrm(modrm);
      if (op_size_32) set_reg32(mr.reg_field, get_rm8(mr));
      else regs[mr.reg_field] = get_rm8(mr);
      break;
    }
    // MOVZX r32, r/m16 (0x0F 0xB7)
    case 0xB7: {
      emu88_uint8 modrm = fetch_ip_byte();
      modrm_result mr = decode_modrm(modrm);
      set_reg32(mr.reg_field, get_rm16(mr));
      break;
    }
    // MOVSX r16/r32, r/m8 (0x0F 0xBE)
    case 0xBE: {
      emu88_uint8 modrm = fetch_ip_byte();
      modrm_result mr = decode_modrm(modrm);
      emu88_int8 val = (emu88_int8)get_rm8(mr);
      if (op_size_32) set_reg32(mr.reg_field, (emu88_uint32)(emu88_int32)val);
      else regs[mr.reg_field] = (emu88_uint16)(emu88_int16)val;
      break;
    }
    // MOVSX r32, r/m16 (0x0F 0xBF)
    case 0xBF: {
      emu88_uint8 modrm = fetch_ip_byte();
      modrm_result mr = decode_modrm(modrm);
      emu88_int16 val = (emu88_int16)get_rm16(mr);
      set_reg32(mr.reg_field, (emu88_uint32)(emu88_int32)val);
      break;
    }
    // SETcc (0x0F 0x90-0x9F)
    case 0x90: case 0x91: case 0x92: case 0x93:
    case 0x94: case 0x95: case 0x96: case 0x97:
    case 0x98: case 0x99: case 0x9A: case 0x9B:
    case 0x9C: case 0x9D: case 0x9E: case 0x9F: {
      emu88_uint8 modrm = fetch_ip_byte();
      modrm_result mr = decode_modrm(modrm);
      bool cond = false;
      switch (op2 & 0x0F) {
        case 0x0: cond = get_flag(FLAG_OF); break;   // SETO
        case 0x1: cond = !get_flag(FLAG_OF); break;  // SETNO
        case 0x2: cond = get_flag(FLAG_CF); break;   // SETB
        case 0x3: cond = !get_flag(FLAG_CF); break;  // SETNB
        case 0x4: cond = get_flag(FLAG_ZF); break;   // SETZ
        case 0x5: cond = !get_flag(FLAG_ZF); break;  // SETNZ
        case 0x6: cond = get_flag(FLAG_CF) || get_flag(FLAG_ZF); break; // SETBE
        case 0x7: cond = !get_flag(FLAG_CF) && !get_flag(FLAG_ZF); break; // SETA
        case 0x8: cond = get_flag(FLAG_SF); break;   // SETS
        case 0x9: cond = !get_flag(FLAG_SF); break;  // SETNS
        case 0xA: cond = get_flag(FLAG_PF); break;   // SETP
        case 0xB: cond = !get_flag(FLAG_PF); break;  // SETNP
        case 0xC: cond = get_flag(FLAG_SF) != get_flag(FLAG_OF); break; // SETL
        case 0xD: cond = get_flag(FLAG_SF) == get_flag(FLAG_OF); break; // SETGE
        case 0xE: cond = get_flag(FLAG_ZF) || (get_flag(FLAG_SF) != get_flag(FLAG_OF)); break; // SETLE
        case 0xF: cond = !get_flag(FLAG_ZF) && (get_flag(FLAG_SF) == get_flag(FLAG_OF)); break; // SETG
      }
      set_rm8(mr, cond ? 1 : 0);
      break;
    }
    // Jcc near (0x0F 0x80-0x8F) - 386+ two-byte conditional jumps
    case 0x80: case 0x81: case 0x82: case 0x83:
    case 0x84: case 0x85: case 0x86: case 0x87:
    case 0x88: case 0x89: case 0x8A: case 0x8B:
    case 0x8C: case 0x8D: case 0x8E: case 0x8F: {
      bool cond = false;
      switch (op2 & 0x0F) {
        case 0x0: cond = get_flag(FLAG_OF); break;
        case 0x1: cond = !get_flag(FLAG_OF); break;
        case 0x2: cond = get_flag(FLAG_CF); break;
        case 0x3: cond = !get_flag(FLAG_CF); break;
        case 0x4: cond = get_flag(FLAG_ZF); break;
        case 0x5: cond = !get_flag(FLAG_ZF); break;
        case 0x6: cond = get_flag(FLAG_CF) || get_flag(FLAG_ZF); break;
        case 0x7: cond = !get_flag(FLAG_CF) && !get_flag(FLAG_ZF); break;
        case 0x8: cond = get_flag(FLAG_SF); break;
        case 0x9: cond = !get_flag(FLAG_SF); break;
        case 0xA: cond = get_flag(FLAG_PF); break;
        case 0xB: cond = !get_flag(FLAG_PF); break;
        case 0xC: cond = get_flag(FLAG_SF) != get_flag(FLAG_OF); break;
        case 0xD: cond = get_flag(FLAG_SF) == get_flag(FLAG_OF); break;
        case 0xE: cond = get_flag(FLAG_ZF) || (get_flag(FLAG_SF) != get_flag(FLAG_OF)); break;
        case 0xF: cond = !get_flag(FLAG_ZF) && (get_flag(FLAG_SF) == get_flag(FLAG_OF)); break;
      }
      if (op_size_32) {
        emu88_int32 disp = (emu88_int32)fetch_ip_dword();
        if (cond) ip += disp;
      } else {
        emu88_int16 disp = (emu88_int16)fetch_ip_word();
        if (cond) ip += disp;
      }
      break;
    }
    // MOV sreg (0x0F extended segment ops) - handle FS/GS load/store
    case 0xB2: { // LSS r16, m16:16 / LSS r32, m16:32
      emu88_uint8 modrm = fetch_ip_byte();
      modrm_result mr = decode_modrm(modrm);
      if (op_size_32) {
        set_reg32(mr.reg_field, fetch_dword(mr.seg, mr.offset));
        load_segment(seg_SS, fetch_word(mr.seg, mr.offset + 4));
      } else {
        regs[mr.reg_field] = get_rm16(mr);
        load_segment(seg_SS, fetch_word(mr.seg, mr.offset + 2));
      }
      break;
    }
    case 0xB4: { // LFS r16, m16:16 / LFS r32, m16:32
      emu88_uint8 modrm = fetch_ip_byte();
      modrm_result mr = decode_modrm(modrm);
      if (op_size_32) {
        set_reg32(mr.reg_field, fetch_dword(mr.seg, mr.offset));
        load_segment(seg_FS, fetch_word(mr.seg, mr.offset + 4));
      } else {
        regs[mr.reg_field] = get_rm16(mr);
        load_segment(seg_FS, fetch_word(mr.seg, mr.offset + 2));
      }
      break;
    }
    case 0xB5: { // LGS r16, m16:16 / LGS r32, m16:32
      emu88_uint8 modrm = fetch_ip_byte();
      modrm_result mr = decode_modrm(modrm);
      if (op_size_32) {
        set_reg32(mr.reg_field, fetch_dword(mr.seg, mr.offset));
        load_segment(seg_GS, fetch_word(mr.seg, mr.offset + 4));
      } else {
        regs[mr.reg_field] = get_rm16(mr);
        load_segment(seg_GS, fetch_word(mr.seg, mr.offset + 2));
      }
      break;
    }
    // IMUL r16, r/m16 (0x0F 0xAF)
    case 0xAF: {
      emu88_uint8 modrm = fetch_ip_byte();
      modrm_result mr = decode_modrm(modrm);
      if (op_size_32) {
        emu88_int32 a = (emu88_int32)get_reg32(mr.reg_field);
        emu88_int32 b = (emu88_int32)get_rm32(mr);
        emu88_int64 result = (emu88_int64)a * (emu88_int64)b;
        set_reg32(mr.reg_field, (emu88_uint32)result);
        set_flag_val(FLAG_CF, result != (emu88_int32)result);
        set_flag_val(FLAG_OF, result != (emu88_int32)result);
      } else {
        emu88_int16 a = (emu88_int16)regs[mr.reg_field];
        emu88_int16 b = (emu88_int16)get_rm16(mr);
        emu88_int32 result = (emu88_int32)a * (emu88_int32)b;
        regs[mr.reg_field] = (emu88_uint16)result;
        set_flag_val(FLAG_CF, result != (emu88_int16)result);
        set_flag_val(FLAG_OF, result != (emu88_int16)result);
      }
      break;
    }

    // SHLD r/m16, r16, imm8 (0x0F 0xA4)
    case 0xA4: {
      emu88_uint8 modrm = fetch_ip_byte();
      modrm_result mr = decode_modrm(modrm);
      emu88_uint8 count = fetch_ip_byte() & 0x1F;
      if (count) {
        if (op_size_32) {
          emu88_uint32 dst = get_rm32(mr);
          emu88_uint32 src = get_reg32(mr.reg_field);
          emu88_uint64 tmp = ((emu88_uint64)dst << 32) | src;
          tmp <<= count;
          emu88_uint32 result = (tmp >> 32) & 0xFFFFFFFF;
          set_rm32(mr, result);
          set_flags_zsp32(result);
          set_flag_val(FLAG_CF, (dst >> (32 - count)) & 1);
        } else {
          emu88_uint16 dst = get_rm16(mr);
          emu88_uint16 src = regs[mr.reg_field];
          emu88_uint32 tmp = ((emu88_uint32)dst << 16) | src;
          tmp <<= count;
          emu88_uint16 result = (tmp >> 16) & 0xFFFF;
          set_rm16(mr, result);
          set_flags_zsp16(result);
          set_flag_val(FLAG_CF, (dst >> (16 - count)) & 1);
        }
      }
      break;
    }

    // SHLD r/m16, r16, CL (0x0F 0xA5)
    case 0xA5: {
      emu88_uint8 modrm = fetch_ip_byte();
      modrm_result mr = decode_modrm(modrm);
      emu88_uint8 count = get_reg8(reg_CL) & 0x1F;
      if (count) {
        if (op_size_32) {
          emu88_uint32 dst = get_rm32(mr);
          emu88_uint32 src = get_reg32(mr.reg_field);
          emu88_uint64 tmp = ((emu88_uint64)dst << 32) | src;
          tmp <<= count;
          emu88_uint32 result = (tmp >> 32) & 0xFFFFFFFF;
          set_rm32(mr, result);
          set_flags_zsp32(result);
          set_flag_val(FLAG_CF, (dst >> (32 - count)) & 1);
        } else {
          emu88_uint16 dst = get_rm16(mr);
          emu88_uint16 src = regs[mr.reg_field];
          emu88_uint32 tmp = ((emu88_uint32)dst << 16) | src;
          tmp <<= count;
          emu88_uint16 result = (tmp >> 16) & 0xFFFF;
          set_rm16(mr, result);
          set_flags_zsp16(result);
          set_flag_val(FLAG_CF, (dst >> (16 - count)) & 1);
        }
      }
      break;
    }

    // SHRD r/m16, r16, imm8 (0x0F 0xAC)
    case 0xAC: {
      emu88_uint8 modrm = fetch_ip_byte();
      modrm_result mr = decode_modrm(modrm);
      emu88_uint8 count = fetch_ip_byte() & 0x1F;
      if (count) {
        if (op_size_32) {
          emu88_uint32 dst = get_rm32(mr);
          emu88_uint32 src = get_reg32(mr.reg_field);
          emu88_uint64 tmp = ((emu88_uint64)src << 32) | dst;
          tmp >>= count;
          emu88_uint32 result = tmp & 0xFFFFFFFF;
          set_rm32(mr, result);
          set_flags_zsp32(result);
          set_flag_val(FLAG_CF, (dst >> (count - 1)) & 1);
        } else {
          emu88_uint16 dst = get_rm16(mr);
          emu88_uint16 src = regs[mr.reg_field];
          emu88_uint32 tmp = ((emu88_uint32)src << 16) | dst;
          tmp >>= count;
          emu88_uint16 result = tmp & 0xFFFF;
          set_rm16(mr, result);
          set_flags_zsp16(result);
          set_flag_val(FLAG_CF, (dst >> (count - 1)) & 1);
        }
      }
      break;
    }

    // SHRD r/m16, r16, CL (0x0F 0xAD)
    case 0xAD: {
      emu88_uint8 modrm = fetch_ip_byte();
      modrm_result mr = decode_modrm(modrm);
      emu88_uint8 count = get_reg8(reg_CL) & 0x1F;
      if (count) {
        if (op_size_32) {
          emu88_uint32 dst = get_rm32(mr);
          emu88_uint32 src = get_reg32(mr.reg_field);
          emu88_uint64 tmp = ((emu88_uint64)src << 32) | dst;
          tmp >>= count;
          emu88_uint32 result = tmp & 0xFFFFFFFF;
          set_rm32(mr, result);
          set_flags_zsp32(result);
          set_flag_val(FLAG_CF, (dst >> (count - 1)) & 1);
        } else {
          emu88_uint16 dst = get_rm16(mr);
          emu88_uint16 src = regs[mr.reg_field];
          emu88_uint32 tmp = ((emu88_uint32)src << 16) | dst;
          tmp >>= count;
          emu88_uint16 result = tmp & 0xFFFF;
          set_rm16(mr, result);
          set_flags_zsp16(result);
          set_flag_val(FLAG_CF, (dst >> (count - 1)) & 1);
        }
      }
      break;
    }

    // BT r/m16, r16 (0x0F 0xA3)
    case 0xA3: {
      emu88_uint8 modrm = fetch_ip_byte();
      modrm_result mr = decode_modrm(modrm);
      if (op_size_32) {
        emu88_uint32 val = get_rm32(mr);
        emu88_uint8 bit = get_reg32(mr.reg_field) & 31;
        set_flag_val(FLAG_CF, (val >> bit) & 1);
      } else {
        emu88_uint16 val = get_rm16(mr);
        emu88_uint8 bit = regs[mr.reg_field] & 15;
        set_flag_val(FLAG_CF, (val >> bit) & 1);
      }
      break;
    }

    // BTS r/m16, r16 (0x0F 0xAB)
    case 0xAB: {
      emu88_uint8 modrm = fetch_ip_byte();
      modrm_result mr = decode_modrm(modrm);
      if (op_size_32) {
        emu88_uint32 val = get_rm32(mr);
        emu88_uint8 bit = get_reg32(mr.reg_field) & 31;
        set_flag_val(FLAG_CF, (val >> bit) & 1);
        set_rm32(mr, val | (1u << bit));
      } else {
        emu88_uint16 val = get_rm16(mr);
        emu88_uint8 bit = regs[mr.reg_field] & 15;
        set_flag_val(FLAG_CF, (val >> bit) & 1);
        set_rm16(mr, val | (1u << bit));
      }
      break;
    }

    // BTR r/m16, r16 (0x0F 0xB3)
    case 0xB3: {
      emu88_uint8 modrm = fetch_ip_byte();
      modrm_result mr = decode_modrm(modrm);
      if (op_size_32) {
        emu88_uint32 val = get_rm32(mr);
        emu88_uint8 bit = get_reg32(mr.reg_field) & 31;
        set_flag_val(FLAG_CF, (val >> bit) & 1);
        set_rm32(mr, val & ~(1u << bit));
      } else {
        emu88_uint16 val = get_rm16(mr);
        emu88_uint8 bit = regs[mr.reg_field] & 15;
        set_flag_val(FLAG_CF, (val >> bit) & 1);
        set_rm16(mr, val & ~(emu88_uint16(1) << bit));
      }
      break;
    }

    // BTC r/m16, r16 (0x0F 0xBB)
    case 0xBB: {
      emu88_uint8 modrm = fetch_ip_byte();
      modrm_result mr = decode_modrm(modrm);
      if (op_size_32) {
        emu88_uint32 val = get_rm32(mr);
        emu88_uint8 bit = get_reg32(mr.reg_field) & 31;
        set_flag_val(FLAG_CF, (val >> bit) & 1);
        set_rm32(mr, val ^ (1u << bit));
      } else {
        emu88_uint16 val = get_rm16(mr);
        emu88_uint8 bit = regs[mr.reg_field] & 15;
        set_flag_val(FLAG_CF, (val >> bit) & 1);
        set_rm16(mr, val ^ (emu88_uint16(1) << bit));
      }
      break;
    }

    // BT/BTS/BTR/BTC r/m16, imm8 (0x0F 0xBA)
    case 0xBA: {
      emu88_uint8 modrm = fetch_ip_byte();
      modrm_result mr = decode_modrm(modrm);
      emu88_uint8 bit = fetch_ip_byte();
      emu88_uint8 op_bt = mr.reg_field;  // 4=BT, 5=BTS, 6=BTR, 7=BTC
      if (op_size_32) {
        emu88_uint32 val = get_rm32(mr);
        bit &= 31;
        set_flag_val(FLAG_CF, (val >> bit) & 1);
        if (op_bt == 5) set_rm32(mr, val | (1u << bit));
        else if (op_bt == 6) set_rm32(mr, val & ~(1u << bit));
        else if (op_bt == 7) set_rm32(mr, val ^ (1u << bit));
      } else {
        emu88_uint16 val = get_rm16(mr);
        bit &= 15;
        set_flag_val(FLAG_CF, (val >> bit) & 1);
        if (op_bt == 5) set_rm16(mr, val | (emu88_uint16(1) << bit));
        else if (op_bt == 6) set_rm16(mr, val & ~(emu88_uint16(1) << bit));
        else if (op_bt == 7) set_rm16(mr, val ^ (emu88_uint16(1) << bit));
      }
      break;
    }

    // BSF r16, r/m16 (0x0F 0xBC)
    case 0xBC: {
      emu88_uint8 modrm = fetch_ip_byte();
      modrm_result mr = decode_modrm(modrm);
      if (op_size_32) {
        emu88_uint32 val = get_rm32(mr);
        if (val == 0) { set_flag(FLAG_ZF); }
        else {
          clear_flag(FLAG_ZF);
          emu88_uint8 bit = 0;
          while (!(val & (1u << bit))) bit++;
          set_reg32(mr.reg_field, bit);
        }
      } else {
        emu88_uint16 val = get_rm16(mr);
        if (val == 0) { set_flag(FLAG_ZF); }
        else {
          clear_flag(FLAG_ZF);
          emu88_uint8 bit = 0;
          while (!(val & (1u << bit))) bit++;
          regs[mr.reg_field] = bit;
        }
      }
      break;
    }

    // BSR r16, r/m16 (0x0F 0xBD)
    case 0xBD: {
      emu88_uint8 modrm = fetch_ip_byte();
      modrm_result mr = decode_modrm(modrm);
      if (op_size_32) {
        emu88_uint32 val = get_rm32(mr);
        if (val == 0) { set_flag(FLAG_ZF); }
        else {
          clear_flag(FLAG_ZF);
          emu88_uint8 bit = 31;
          while (!(val & (1u << bit))) bit--;
          set_reg32(mr.reg_field, bit);
        }
      } else {
        emu88_uint16 val = get_rm16(mr);
        if (val == 0) { set_flag(FLAG_ZF); }
        else {
          clear_flag(FLAG_ZF);
          emu88_uint8 bit = 15;
          while (!(val & (1u << bit))) bit--;
          regs[mr.reg_field] = bit;
        }
      }
      break;
    }

    // BSWAP r32 (0x0F 0xC8-0xCF)
    case 0xC8: case 0xC9: case 0xCA: case 0xCB:
    case 0xCC: case 0xCD: case 0xCE: case 0xCF: {
      emu88_uint8 r = op2 & 7;
      emu88_uint32 val = get_reg32(r);
      set_reg32(r, ((val & 0xFF) << 24) | ((val & 0xFF00) << 8) |
                    ((val >> 8) & 0xFF00) | ((val >> 24) & 0xFF));
      break;
    }

    // CMPXCHG r/m8, r8 (0x0F 0xB0)
    case 0xB0: {
      emu88_uint8 modrm = fetch_ip_byte();
      modrm_result mr = decode_modrm(modrm);
      emu88_uint8 dst = get_rm8(mr);
      emu88_uint8 al = get_reg8(reg_AL);
      set_flags_sub8(al, dst, 0);
      if (al == dst) {
        set_flag(FLAG_ZF);
        set_rm8(mr, get_reg8(mr.reg_field));
      } else {
        clear_flag(FLAG_ZF);
        set_reg8(reg_AL, dst);
      }
      break;
    }

    // CMPXCHG r/m16, r16 (0x0F 0xB1)
    case 0xB1: {
      emu88_uint8 modrm = fetch_ip_byte();
      modrm_result mr = decode_modrm(modrm);
      if (op_size_32) {
        emu88_uint32 dst = get_rm32(mr);
        emu88_uint32 eax = get_reg32(reg_AX);
        set_flags_sub32(eax, dst, 0);
        if (eax == dst) {
          set_flag(FLAG_ZF);
          set_rm32(mr, get_reg32(mr.reg_field));
        } else {
          clear_flag(FLAG_ZF);
          set_reg32(reg_AX, dst);
        }
      } else {
        emu88_uint16 dst = get_rm16(mr);
        emu88_uint16 ax = regs[reg_AX];
        set_flags_sub16(ax, dst, 0);
        if (ax == dst) {
          set_flag(FLAG_ZF);
          set_rm16(mr, regs[mr.reg_field]);
        } else {
          clear_flag(FLAG_ZF);
          regs[reg_AX] = dst;
        }
      }
      break;
    }

    // XADD r/m8, r8 (0x0F 0xC0)
    case 0xC0: {
      emu88_uint8 modrm = fetch_ip_byte();
      modrm_result mr = decode_modrm(modrm);
      emu88_uint8 dst = get_rm8(mr);
      emu88_uint8 src = get_reg8(mr.reg_field);
      emu88_uint8 result = alu_add8(dst, src, 0);
      set_reg8(mr.reg_field, dst);
      set_rm8(mr, result);
      break;
    }

    // XADD r/m16, r16 (0x0F 0xC1)
    case 0xC1: {
      emu88_uint8 modrm = fetch_ip_byte();
      modrm_result mr = decode_modrm(modrm);
      if (op_size_32) {
        emu88_uint32 dst = get_rm32(mr);
        emu88_uint32 src = get_reg32(mr.reg_field);
        emu88_uint32 result = alu_add32(dst, src, 0);
        set_reg32(mr.reg_field, dst);
        set_rm32(mr, result);
      } else {
        emu88_uint16 dst = get_rm16(mr);
        emu88_uint16 src = regs[mr.reg_field];
        emu88_uint16 result = alu_add16(dst, src, 0);
        regs[mr.reg_field] = dst;
        set_rm16(mr, result);
      }
      break;
    }

    // Multi-byte NOP (0x0F 0x1F) — consume modrm and displacement
    case 0x1F: {
      emu88_uint8 modrm = fetch_ip_byte();
      decode_modrm(modrm);  // consume modrm + displacement bytes
      break;
    }

    // RDMSR (0x0F 0x32) — stub: return 0 for all MSRs
    case 0x32: {
      // ECX = MSR index (ignored), return EDX:EAX = 0
      set_reg32(reg_AX, 0);
      set_reg32(reg_DX, 0);
      break;
    }

    // WRMSR (0x0F 0x30) — stub: ignore writes to all MSRs
    case 0x30: {
      // ECX = MSR index, EDX:EAX = value (all ignored)
      break;
    }

    // CMPXCHG8B m64 (0x0F 0xC7)
    case 0xC7: {
      emu88_uint8 modrm = fetch_ip_byte();
      modrm_result mr = decode_modrm(modrm);
      // reg_field must be 1 for CMPXCHG8B
      if (mr.reg_field == 1 && !mr.is_register) {
        // Read 64-bit value from memory (low dword first)
        emu88_uint32 lo = fetch_dword(mr.seg, mr.offset);
        emu88_uint32 hi = fetch_dword(mr.seg, mr.offset + 4);
        emu88_uint32 eax = get_reg32(reg_AX);
        emu88_uint32 edx = get_reg32(reg_DX);
        if (lo == eax && hi == edx) {
          // Equal: set ZF, store ECX:EBX into m64
          set_flag(FLAG_ZF);
          store_dword(mr.seg, mr.offset, get_reg32(reg_BX));
          store_dword(mr.seg, mr.offset + 4, get_reg32(reg_CX));
        } else {
          // Not equal: clear ZF, load m64 into EDX:EAX
          clear_flag(FLAG_ZF);
          set_reg32(reg_AX, lo);
          set_reg32(reg_DX, hi);
        }
      }
      break;
    }

    default:
      emu88_fatal("Unimplemented 0x0F opcode: 0x%02X at %04X:%04X", op2, sregs[seg_CS], ip - 2);
      halted = true;
      break;
    }
    break;
  }

  case 0xD6: // SALC (undocumented: set AL to 0xFF if CF, else 0x00)
    set_reg8(reg_AL, get_flag(FLAG_CF) ? 0xFF : 0x00);
    break;

  default:
    unimplemented_opcode(opcode);
    break;
  }

  // In 16-bit code segments (real mode, V86 mode, or 16-bit pmode),
  // mask EIP to 16 bits so relative jumps/calls wrap correctly.
  if (!code_32())
    ip &= 0xFFFF;
}
