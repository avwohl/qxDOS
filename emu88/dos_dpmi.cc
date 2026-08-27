//=============================================================================
// dos_dpmi.cc — DPMI 0.9 server implementation
//
// Provides DOS Protected Mode Interface services for DOS extenders
// like DOS4GW (used by DOOM). Replaces the need for CWSDPMI.
//=============================================================================

#include "dos_machine.h"
#include <cstdio>
#include <cstring>

//=============================================================================
// Descriptor helpers
//=============================================================================

void dos_machine::dpmi_write_gdt_entry(int index, uint32_t base, uint32_t limit,
                                        uint8_t access, uint8_t flags_nibble) {
  uint32_t addr = dpmi.gdt_phys + index * 8;
  // Limit bits 0-15
  mem->store_mem(addr + 0, limit & 0xFF);
  mem->store_mem(addr + 1, (limit >> 8) & 0xFF);
  // Base bits 0-15
  mem->store_mem(addr + 2, base & 0xFF);
  mem->store_mem(addr + 3, (base >> 8) & 0xFF);
  // Base bits 16-23
  mem->store_mem(addr + 4, (base >> 16) & 0xFF);
  // Access byte
  mem->store_mem(addr + 5, access);
  // Flags (high nibble) | limit bits 16-19 (low nibble)
  mem->store_mem(addr + 6, (flags_nibble << 4) | ((limit >> 16) & 0x0F));
  // Base bits 24-31
  mem->store_mem(addr + 7, (base >> 24) & 0xFF);
}

void dos_machine::dpmi_write_ldt_entry(uint16_t sel, uint32_t base, uint32_t limit,
                                        uint8_t access, uint8_t flags_nibble) {
  uint16_t index = (sel >> 3);
  uint32_t addr = dpmi.ldt_phys + index * 8;
  mem->store_mem(addr + 0, limit & 0xFF);
  mem->store_mem(addr + 1, (limit >> 8) & 0xFF);
  mem->store_mem(addr + 2, base & 0xFF);
  mem->store_mem(addr + 3, (base >> 8) & 0xFF);
  mem->store_mem(addr + 4, (base >> 16) & 0xFF);
  mem->store_mem(addr + 5, access);
  mem->store_mem(addr + 6, (flags_nibble << 4) | ((limit >> 16) & 0x0F));
  mem->store_mem(addr + 7, (base >> 24) & 0xFF);
}

void dos_machine::dpmi_read_ldt_raw(uint16_t sel, uint8_t desc[8]) {
  uint16_t index = (sel >> 3);
  uint32_t addr = dpmi.ldt_phys + index * 8;
  for (int i = 0; i < 8; i++)
    desc[i] = mem->fetch_mem(addr + i);
}

// Every DPMI descriptor service takes a selector in BX and indexes the LDT with
// it.  Unvalidated, a GDT selector (TI=0) indexes the LDT anyway, and an index
// past LDT_MAX reads or writes as much as 48KB beyond the end of the table -
// `sel >> 3' runs to 8191 where the table holds 2048 entries.  The address is
// entirely guest-controlled, and what sits past the LDT in this machine's layout
// is the GDT, the IDT and the TSS.  8022h is what DPMI 0.9 specifies for a
// selector the host will not accept; 0001h already validated this way and the
// rest did not.
bool dos_machine::dpmi_bad_selector(uint16_t sel) {
  uint16_t index = (sel >> 3);
  if (!(sel & 4) || index == 0 || index >= DpmiState::LDT_MAX) {
    set_flag(FLAG_CF);
    regs[reg_AX] = 0x8022;  // Invalid selector
    return true;
  }
  return false;
}

void dos_machine::dpmi_write_ldt_raw(uint16_t sel, const uint8_t desc[8]) {
  uint16_t index = (sel >> 3);
  uint32_t addr = dpmi.ldt_phys + index * 8;
  for (int i = 0; i < 8; i++)
    mem->store_mem(addr + i, desc[i]);
}

void dos_machine::dpmi_write_idt_entry(int vector, uint16_t sel, uint32_t offset,
                                        uint8_t dpl, bool is_32bit) {
  uint32_t addr = dpmi.idt_phys + vector * 8;
  // Offset low
  mem->store_mem(addr + 0, offset & 0xFF);
  mem->store_mem(addr + 1, (offset >> 8) & 0xFF);
  // Selector
  mem->store_mem(addr + 2, sel & 0xFF);
  mem->store_mem(addr + 3, (sel >> 8) & 0xFF);
  // Reserved
  mem->store_mem(addr + 4, 0);
  // Type: P=1, DPL, 0, type (0E=32-bit int gate, 06=16-bit int gate)
  uint8_t type_byte = 0x80 | ((dpl & 3) << 5) | (is_32bit ? 0x0E : 0x06);
  mem->store_mem(addr + 5, type_byte);
  // Offset high
  mem->store_mem(addr + 6, (offset >> 16) & 0xFF);
  mem->store_mem(addr + 7, (offset >> 24) & 0xFF);
}

//=============================================================================
// LDT selector allocation
//=============================================================================

uint16_t dos_machine::dpmi_alloc_ldt_sel() {
  for (int i = 1; i < DpmiState::LDT_MAX; i++) {
    int byte = i / 8, bit = i % 8;
    if (!(dpmi.ldt_alloc[byte] & (1 << bit))) {
      dpmi.ldt_alloc[byte] |= (1 << bit);
      uint16_t sel = (i << 3) | 4 | 0;  // LDT, RPL=0
      // Zero out the descriptor
      dpmi_write_ldt_entry(sel, 0, 0, 0, 0);
      return sel;
    }
  }
  return 0;  // No free selectors
}

void dos_machine::dpmi_free_ldt_sel(uint16_t sel) {
  uint16_t index = (sel >> 3);
  if (index > 0 && index < DpmiState::LDT_MAX) {
    int byte = index / 8, bit = index % 8;
    dpmi.ldt_alloc[byte] &= ~(1 << bit);
    dpmi_write_ldt_entry(sel, 0, 0, 0, 0);
  }
}

//=============================================================================
// DPMI mode switch — called when client FAR CALLs the mode switch entry
//=============================================================================

void dos_machine::dpmi_mode_switch() {
  bool is_32bit = (regs[reg_AX] & 1) != 0;
  uint16_t psp_seg = sregs[seg_ES];

  // Pop FAR CALL return address (still in real mode)
  uint16_t ret_ip = pop_word();
  uint16_t ret_cs = pop_word();

  // Save real-mode state
  dpmi.saved_rm_ss = sregs[seg_SS];
  dpmi.saved_rm_sp = regs[reg_SP];
  dpmi.client_psp = psp_seg;

  // Snapshot all real-mode interrupt vectors
  for (int i = 0; i < 256; i++) {
    dpmi.rm_int_off[i] = mem->fetch_mem16(i * 4);
    dpmi.rm_int_seg[i] = mem->fetch_mem16(i * 4 + 2);
  }

  // Snapshot low memory (IVT+BDA+DOS data area) for save/restore around RM calls.
  // PM code like DOS4GW may overwrite this region via flat selectors.
  memcpy(dpmi.original_low_mem, mem->get_mem(), DpmiState::LOW_MEM_SAVE_SIZE);

  // Enable A20 gate — DPMI requires access to extended memory above 1MB
  mem->set_a20(true);

  // Allocate physical memory for DPMI structures at top of memory
  uint32_t mem_top = mem->get_mem_size();
  dpmi.base = (mem_top - 0x20000) & ~0xFFF;  // 128KB from top, page-aligned
  dpmi.gdt_phys = dpmi.base;                  // 8KB for GDT (1024 entries max)
  dpmi.idt_phys = dpmi.base + 0x2000;         // 2KB for IDT (256 entries)
  dpmi.ldt_phys = dpmi.base + 0x2800;         // 16KB for LDT (2048 entries)
  dpmi.tss_phys = dpmi.base + 0x6800;         // 4KB for TSS
  dpmi.pm_stack_top = dpmi.base + 0x8000;     // Ring 0 stack (grows down from here)

  // Start DPMI memory allocations at 2MB (above typical XMS use at 0x110000+)
  dpmi.next_mem_base = 0x200000;
  dpmi.next_handle = 1;


  // Clear structures
  for (uint32_t i = 0; i < 0x9000; i++)
    mem->store_mem(dpmi.base + i, 0);
  memset(dpmi.ldt_alloc, 0, sizeof(dpmi.ldt_alloc));
  memset(dpmi.pm_int_installed, 0, sizeof(dpmi.pm_int_installed));
  memset(dpmi.exc_installed, 0, sizeof(dpmi.exc_installed));
  memset(dpmi.rm_callback_used, 0, sizeof(dpmi.rm_callback_used));
  // Value-initialize rather than memset: the default member initializers on
  // MemBlock, DosBlock and SegMap make them non-trivial types, and clearing a
  // non-trivial type with memset is -Wclass-memaccess.  (They are trivially
  // copyable; it is default-construction that is not trivial.)  Every member
  // is a scalar initialized to zero, so this leaves the member values memset
  // left.  It does not rewrite the 3, 1 and 1 bytes of tail padding - nothing
  // reads those, and the arrays' own `= {}` zeroed them at construction.
  // Assignment stays correct if a member ever stops being a scalar.
  for (auto &b : dpmi.mem_blocks) b = {};
  for (auto &b : dpmi.dos_blocks) b = {};
  for (auto &m : dpmi.seg_map)    m = {};

  // === GDT Setup ===
  // All at ring 0 — DOS4GW expects ring 0 operation (like CWSDPMI raw mode)
  // Entry 0: Null
  dpmi_write_gdt_entry(0, 0, 0, 0, 0);
  // Entry 1: Ring 0 Data32 (base=0, limit=4GB) — sel 0x08
  // DOS4GW expects sel 0x08 to be writable (uses DS=0x08 for flat memory access)
  dpmi_write_gdt_entry(1, 0, 0xFFFFF, 0x92, 0x0C);  // G=1, D=1
  // Entry 2: Ring 0 Code32 (base=0, limit=4GB) — sel 0x10
  dpmi_write_gdt_entry(2, 0, 0xFFFFF, 0x9A, 0x0C);  // G=1, D=1
  // Entry 3: TSS — sel 0x18
  dpmi_write_gdt_entry(3, dpmi.tss_phys, 103, 0x89, 0x00);
  // Entry 4: LDT — sel 0x20
  dpmi_write_gdt_entry(4, dpmi.ldt_phys, DpmiState::LDT_MAX * 8 - 1, 0x82, 0x00);

  // Entry 5: BIOS ROM Code16 (base=0xF0000, limit=0xFFFF) — sel 0x28
  // Used for PM→RM raw mode switch trap (keeps EDI in 16-bit range)
  dpmi_write_gdt_entry(5, 0xF0000, 0xFFFF, 0x9A, 0x00);  // G=0, D=0

  // Entry 6: PM Stack SS (base=dpmi.base, limit=0xFFFF) — sel 0x30
  // 16-bit stack segment so exception handlers (which may be 16-bit code)
  // can access the exception frame through 16-bit BP
  dpmi_write_gdt_entry(6, dpmi.base, 0xFFFF, 0x93, 0x00);  // G=0, B=0

  dpmi.ring0_cs = 0x10;
  dpmi.ring0_ds = 0x08;
  dpmi.tss_sel = 0x18;
  dpmi.ldt_gdt_sel = 0x20;
  dpmi.bios_rom_cs = 0x28;
  dpmi.pm_stack_ss = 0x30;

  // === TSS Setup ===
  // ESP0 = ring 0 stack top
  mem->store_mem32(dpmi.tss_phys + 4, dpmi.pm_stack_top);
  // SS0 = ring 0 data selector
  mem->store_mem16(dpmi.tss_phys + 8, dpmi.ring0_ds);
  // I/O map base = beyond TSS limit (no I/O bitmap, all ports allowed via IOPL)
  mem->store_mem16(dpmi.tss_phys + 102, 104);

  // === IDT Setup ===
  // All 256 entries: 32-bit interrupt gates, DPL=0 (client runs at ring 0)
  // We intercept before IDT lookup, but these must be valid for the CPU
  for (int i = 0; i < 256; i++) {
    dpmi_write_idt_entry(i, dpmi.ring0_cs, 0, 0, true);
  }

  // === Create initial LDT selectors for the client ===
  uint16_t client_cs = dpmi_alloc_ldt_sel();
  uint16_t client_ds = dpmi_alloc_ldt_sel();
  uint16_t client_ss = dpmi_alloc_ldt_sel();
  uint16_t client_es = dpmi_alloc_ldt_sel();

  uint16_t saved_ds = sregs[seg_DS];
  uint16_t saved_ss = sregs[seg_SS];

  // CS: maps to caller's real-mode code segment — ring 0 code, readable
  // 0x9B = P=1, DPL=0, S=1, type=code+readable+accessed
  dpmi_write_ldt_entry(client_cs, (uint32_t)ret_cs << 4, 0xFFFF, 0x9B, 0x00);
  // DS: maps to caller's DS — ring 0 data, writable
  // 0x93 = P=1, DPL=0, S=1, type=data+writable+accessed
  dpmi_write_ldt_entry(client_ds, (uint32_t)saved_ds << 4, 0xFFFF, 0x93, 0x00);
  // SS: maps to caller's SS
  dpmi_write_ldt_entry(client_ss, (uint32_t)saved_ss << 4, 0xFFFF, 0x93, 0x00);
  // ES: maps to PSP (256 bytes)
  dpmi_write_ldt_entry(client_es, (uint32_t)psp_seg << 4, 0x00FF, 0x93, 0x00);

  // Per DPMI spec: create a descriptor for the DOS environment and patch PSP[0x2C]
  // PSP[0x2C] contains the real-mode environment segment. We must replace it with a PM selector.
  uint32_t psp_phys = (uint32_t)psp_seg << 4;
  uint16_t env_seg = mem->fetch_mem16(psp_phys + 0x2C);
  if (env_seg != 0) {
    uint16_t env_sel = dpmi_alloc_ldt_sel();
    if (env_sel) {
      dpmi_write_ldt_entry(env_sel, (uint32_t)env_seg << 4, 0xFFFF, 0x93, 0x00);
      // Patch PSP[0x2C] with the PM selector (through the raw memory, before PM is fully up)
      mem->store_mem16(psp_phys + 0x2C, env_sel);
    }
  }


  // === Enable Protected Mode ===
  cr0 |= CR0_PE;

  // Load GDTR
  gdtr_base = dpmi.gdt_phys;
  gdtr_limit = 1024 * 8 - 1;  // 1024 entries (8KB) — DOS4GW writes GDT entries directly

  // Load IDTR
  idtr_base = dpmi.idt_phys;
  idtr_limit = 256 * 8 - 1;

  // Load LDTR
  ldtr = dpmi.ldt_gdt_sel;
  ldtr_cache.base = dpmi.ldt_phys;
  ldtr_cache.limit = DpmiState::LDT_MAX * 8 - 1;
  ldtr_cache.access = 0x82;
  ldtr_cache.flags = 0;
  ldtr_cache.valid = true;

  // Load TR
  tr = dpmi.tss_sel;
  tr_cache.base = dpmi.tss_phys;
  tr_cache.limit = 103;
  tr_cache.access = 0x8B;  // Busy TSS
  tr_cache.flags = 0;
  tr_cache.valid = true;
  // Mark TSS busy in GDT (entry 3)
  mem->store_mem(dpmi.gdt_phys + 3 * 8 + 5, 0x8B);

  // Load segment registers with LDT selectors
  sregs[seg_CS] = client_cs;
  seg_cache[seg_CS].base = (uint32_t)ret_cs << 4;
  seg_cache[seg_CS].limit = 0xFFFF;
  seg_cache[seg_CS].access = 0x9B;
  seg_cache[seg_CS].flags = 0x00;
  seg_cache[seg_CS].valid = true;

  sregs[seg_DS] = client_ds;
  seg_cache[seg_DS].base = (uint32_t)saved_ds << 4;
  seg_cache[seg_DS].limit = 0xFFFF;
  seg_cache[seg_DS].access = 0x93;
  seg_cache[seg_DS].flags = 0x00;
  seg_cache[seg_DS].valid = true;

  sregs[seg_SS] = client_ss;
  seg_cache[seg_SS].base = (uint32_t)saved_ss << 4;
  seg_cache[seg_SS].limit = 0xFFFF;
  seg_cache[seg_SS].access = 0x93;
  seg_cache[seg_SS].flags = 0x00;
  seg_cache[seg_SS].valid = true;

  sregs[seg_ES] = client_es;
  seg_cache[seg_ES].base = (uint32_t)psp_seg << 4;
  seg_cache[seg_ES].limit = 0x00FF;
  seg_cache[seg_ES].access = 0x93;
  seg_cache[seg_ES].flags = 0x00;
  seg_cache[seg_ES].valid = true;

  sregs[seg_FS] = 0;
  seg_cache[seg_FS] = {};
  sregs[seg_GS] = 0;
  seg_cache[seg_GS] = {};

  // Set EIP to return address
  ip = ret_ip;

  // CPL = 0 (DOS4GW expects ring 0 like CWSDPMI raw mode)
  cpl = 0;

  // IOPL = 0 (ring 0, all I/O allowed by CPL)
  flags = (flags & ~(uint16_t)EFLAG_IOPL_MASK) | (0 << 12);
  flags |= FLAG_IF;

  dpmi.active = true;
  dpmi.is_32bit = is_32bit;
  dpmi.vif = true;

}

//=============================================================================
// Raw mode switch: PM → RM
// Client in PM does FAR JMP to PM→RM entry with:
//   AX=new DS, CX=new ES, DX=new SS, BX=new SP, SI=new CS, DI=new IP
//=============================================================================

void dos_machine::dpmi_raw_pm_to_rm() {
  uint16_t new_ds = regs[reg_AX];
  uint16_t new_es = regs[reg_CX];
  uint16_t new_ss = regs[reg_DX];
  uint16_t new_sp = regs[reg_BX];
  uint16_t new_cs = regs[reg_SI];
  uint16_t new_ip = regs[reg_DI];


  // Switch to real mode
  cr0 &= ~CR0_PE;
  cpl = 0;

  // Load RM segments
  load_segment_real(seg_CS, new_cs);
  ip = new_ip;
  load_segment_real(seg_DS, new_ds);
  load_segment_real(seg_ES, new_es);
  load_segment_real(seg_SS, new_ss);
  regs[reg_SP] = new_sp;
  regs_hi[reg_SP] = 0;
  load_segment_real(seg_FS, 0);
  load_segment_real(seg_GS, 0);

  // Save RM SS:SP for later use by DPMI reflect
  dpmi.saved_rm_ss = new_ss;
  dpmi.saved_rm_sp = new_sp;
}

//=============================================================================
// Raw mode switch: RM → PM
// Client in RM does FAR JMP to RM→PM entry with:
//   AX=new DS, CX=new ES, DX=new SS, EBX=new ESP, SI=new CS, EDI=new EIP
//=============================================================================

void dos_machine::dpmi_raw_rm_to_pm() {
  uint16_t new_ds = regs[reg_AX];
  uint16_t new_es = regs[reg_CX];
  uint16_t new_ss = regs[reg_DX];
  uint32_t new_esp = get_reg32(reg_BX);
  uint16_t new_cs = regs[reg_SI];
  uint32_t new_eip = get_reg32(reg_DI);


  // Switch to protected mode
  cr0 |= CR0_PE;
  cpl = 0;

  // Load PM segment registers (with descriptor cache from GDT/LDT)
  load_segment(seg_CS, new_cs);
  ip = new_eip;
  load_segment(seg_SS, new_ss);
  set_esp(new_esp);
  load_segment(seg_DS, new_ds);
  load_segment(seg_ES, new_es);
  load_segment(seg_FS, 0);
  load_segment(seg_GS, 0);

}

//=============================================================================
// DPMI PM interrupt intercept
//=============================================================================

bool dos_machine::intercept_pm_int(emu88_uint8 vector, bool is_software_int,
                                    bool has_error_code, emu88_uint32 error_code) {
  if (!dpmi.active) return false;

  // INT 31h — DPMI API
  if (is_software_int && vector == 0x31) {
    dpmi_int31h();
    return true;
  }

  // Other software INTs — check for client PM handler, else reflect to RM
  if (is_software_int) {
    // INT 21h AH=4Ch: DPMI client terminate — end session and return to real mode
    if (vector == 0x21 && (regs[reg_AX] >> 8) == 0x4C) {
      dpmi_terminate(regs[reg_AX] & 0xFF);
      return true;
    }
    if (dpmi.pm_int_installed[vector]) {
      // Dispatch to client's PM handler via IDT (let normal IDT path handle it)
      // We need to update the IDT entry to point to the client's handler
      dpmi_write_idt_entry(vector, dpmi.pm_int[vector].sel,
                           dpmi.pm_int[vector].off, 0, dpmi.is_32bit);
      return false;  // Let do_interrupt_pm handle it via IDT
    }
    // Reflect to real mode
    dpmi_reflect_to_rm(vector);
    return true;
  }

  // Check if this vector is a hardware IRQ (PIC-mapped range)
  // Key distinction: CPU exceptions that push error codes (has_error_code=true) are
  // NEVER hardware IRQs, even if the vector falls in the PIC range (8-15).
  bool is_hw_irq = false;
  if (!is_software_int && !has_error_code) {
    // Master PIC: pic_vector_base .. pic_vector_base+7
    if (vector >= pic_vector_base && vector < pic_vector_base + 8)
      is_hw_irq = true;
    // Slave PIC: vectors 0x70-0x77 (always at 0x70 in standard AT setup)
    if (vector >= 0x70 && vector < 0x78)
      is_hw_irq = true;
  }

  // Hardware interrupts — check for client PM handler, else reflect to RM
  if (is_hw_irq || vector >= 32) {
    if (dpmi.pm_int_installed[vector]) {
      dpmi_write_idt_entry(vector, dpmi.pm_int[vector].sel,
                           dpmi.pm_int[vector].off, 0, dpmi.is_32bit);
      return false;  // Let IDT dispatch to client's handler
    }
    // Reflect hardware interrupt to real mode (preserve PM registers)
    dpmi_reflect_to_rm(vector, true);
    return true;
  }

  // CPU exceptions (vectors 0-31, confirmed not hardware IRQ)
  if (dpmi.exc_installed[vector]) {
    // Build DPMI exception frame and dispatch to handler
    dpmi_dispatch_exception(vector, error_code, has_error_code);
    return true;
  }

  // No exception handler installed — terminate for CPU faults, reflect for others
  dpmi_reflect_to_rm(vector);
  return true;
}

//=============================================================================
// DPMI client termination (INT 21h AH=4Ch from protected mode)
//=============================================================================

void dos_machine::dpmi_terminate(uint8_t exit_code) {

  // End the DPMI session
  dpmi.active = false;

  // Switch back to real mode
  cr0 &= ~CR0_PE;
  cpl = 0;

  // Restore the original real-mode IVT from our cached snapshot
  for (int i = 0; i < 256; i++) {
    mem->store_mem16(i * 4,     dpmi.rm_int_off[i]);
    mem->store_mem16(i * 4 + 2, dpmi.rm_int_seg[i]);
  }

  // Restore real-mode stack
  load_segment_real(seg_SS, dpmi.saved_rm_ss);
  regs[reg_SP] = dpmi.saved_rm_sp;
  regs_hi[reg_SP] = 0;

  // Set up real-mode segments from the saved PSP
  load_segment_real(seg_DS, dpmi.client_psp);
  load_segment_real(seg_ES, dpmi.client_psp);

  // Set AX = 4C00 + exit_code for INT 21h
  regs[reg_AX] = 0x4C00 | exit_code;
  regs_hi[reg_AX] = 0;

  // Look up RM INT 21h handler and jump to it
  uint16_t handler_off = mem->fetch_mem16(0x21 * 4);
  uint16_t handler_seg = mem->fetch_mem16(0x21 * 4 + 2);

  // Push interrupt return frame for the INT 21h handler
  push_word(flags);
  push_word(0xF000);
  push_word(bios_entry[0xFF]);  // Halt sentinel — we won't return from terminate
  flags &= ~(uint16_t)FLAG_IF;

  // Jump to INT 21h handler
  load_segment_real(seg_CS, handler_seg);
  ip = handler_off;

}

//=============================================================================
// Real-mode interrupt reflection
//=============================================================================

void dos_machine::dpmi_reflect_to_rm(uint8_t vector, bool preserve_regs) {
  // Build a DPMI real-mode call structure from current registers
  // and execute the real-mode interrupt

  // Save PM state
  uint16_t save_cs = sregs[seg_CS], save_ds = sregs[seg_DS];
  uint16_t save_es = sregs[seg_ES], save_ss = sregs[seg_SS];
  uint16_t save_fs = sregs[seg_FS], save_gs = sregs[seg_GS];
  uint32_t save_eip = ip, save_esp = get_esp();
  uint32_t save_eflags = get_eflags();
  uint8_t save_cpl = cpl;
  SegDescCache save_seg_cache[6];
  memcpy(save_seg_cache, seg_cache, sizeof(seg_cache));

  // Save general registers (needed for hardware IRQ preservation)
  uint32_t save_eax = get_reg32(reg_AX);
  uint32_t save_ebx = get_reg32(reg_BX);
  uint32_t save_ecx = get_reg32(reg_CX);
  uint32_t save_edx = get_reg32(reg_DX);
  uint32_t save_esi = get_reg32(reg_SI);
  uint32_t save_edi = get_reg32(reg_DI);
  uint32_t save_ebp = get_reg32(reg_BP);

  // Save PM's low memory and restore IVT/BDA for real-mode execution
  dpmi_save_pm_low_mem();

  // Switch to real mode
  cr0 &= ~CR0_PE;
  cpl = 0;

  // Set up real mode segments
  // Use dedicated RM reflection stack (DPMI spec: host provides locked RM stacks)
  // to avoid overlapping with the PM stack in physical memory
  int stack_idx = dpmi.rm_reflect_stack_depth++;
  if (stack_idx >= DpmiState::RM_REFLECT_STACK_COUNT)
    stack_idx = DpmiState::RM_REFLECT_STACK_COUNT - 1;
  uint32_t stack_top = DpmiState::RM_REFLECT_STACK_BASE +
                       (stack_idx + 1) * DpmiState::RM_REFLECT_STACK_SIZE;
  // SS points at the base of the reserved window and SP is the offset within
  // it: every level's stack_top is 512-byte aligned, so `stack_top & 0x0F' was
  // always 0 and the first push wrapped SP to FFFEh, putting the frame 64KB
  // above the window.
  uint16_t rm_ss = (uint16_t)(DpmiState::RM_REFLECT_STACK_BASE >> 4);
  uint16_t rm_sp = (uint16_t)(stack_top - DpmiState::RM_REFLECT_STACK_BASE);
  load_segment_real(seg_SS, rm_ss);
  regs[reg_SP] = rm_sp;
  regs_hi[reg_SP] = 0;

  // Map the current PM segment registers to RM segments where possible
  // DS, ES default to the PSP segment
  load_segment_real(seg_DS, (uint16_t)(save_seg_cache[seg_DS].base >> 4));
  load_segment_real(seg_ES, (uint16_t)(save_seg_cache[seg_ES].base >> 4));
  load_segment_real(seg_FS, 0);
  load_segment_real(seg_GS, 0);

  // Look up the IVT for this vector
  uint16_t handler_off = dpmi.rm_int_off[vector];
  uint16_t handler_seg = dpmi.rm_int_seg[vector];

  // Check if it's our BIOS trap — fast path
  if (handler_seg == 0xF000 && handler_off == bios_entry[vector]) {
    dispatch_bios(vector);
  } else {
    // Need to execute RM handler in a nested loop
    // Push interrupt frame: FLAGS, CS (sentinel), IP (sentinel)
    push_word(flags);
    push_word(0xF000);
    push_word(dpmi.rm_return_off);
    flags &= ~(uint16_t)FLAG_IF;

    // Jump to handler
    load_segment_real(seg_CS, handler_seg);
    ip = handler_off;

    // Nested execute loop
    dpmi.in_rm_callback = true;
    int safety = 0;
    while (dpmi.in_rm_callback && safety < 5000000) {
      execute();
      check_interrupts();
      // Timer ticks during RM execution
      if (cycles - tick_cycle_mark >= CYCLES_PER_TICK) {
        tick_cycle_mark = cycles;
        uint32_t ticks = bda_r32(bda::TIMER_COUNT) + 1;
        if (ticks >= 0x1800B0) { ticks = 0; bda_w8(bda::TIMER_ROLLOVER, 1); }
        bda_w32(bda::TIMER_COUNT, ticks);
      }
      safety++;
    }
    if (safety >= 5000000) {
      emu88_fatal("[DPMI] WARNING: RM callback for INT %02Xh exceeded 5M insns at "
                  "%04X:%04X SS:SP=%04X:%04X (handler %04X:%04X)",
                  vector, sregs[seg_CS], (uint16_t)ip, sregs[seg_SS],
                  (uint16_t)regs[reg_SP], handler_seg, handler_off);
    }
  }

  // Restore PM's low memory data after RM execution
  dpmi_restore_pm_low_mem();

  // Save RM results (flags, AX, etc.)
  uint16_t rm_flags = flags;
  uint32_t rm_eax = get_reg32(reg_AX);
  uint32_t rm_ebx = get_reg32(reg_BX);
  uint32_t rm_ecx = get_reg32(reg_CX);
  uint32_t rm_edx = get_reg32(reg_DX);
  uint32_t rm_esi = get_reg32(reg_SI);
  uint32_t rm_edi = get_reg32(reg_DI);
  uint32_t rm_ebp = get_reg32(reg_BP);

  // Switch back to PM
  cr0 |= CR0_PE;
  cpl = save_cpl;

  // Restore segment registers
  sregs[seg_CS] = save_cs;
  sregs[seg_DS] = save_ds;
  sregs[seg_ES] = save_es;
  sregs[seg_SS] = save_ss;
  sregs[seg_FS] = save_fs;
  sregs[seg_GS] = save_gs;
  memcpy(seg_cache, save_seg_cache, sizeof(seg_cache));

  ip = save_eip;
  set_esp(save_esp);

  if (preserve_regs) {
    // Hardware IRQ reflection: restore ALL PM general registers
    // The RM handler's register changes should not affect PM code
    set_reg32(reg_AX, save_eax);
    set_reg32(reg_BX, save_ebx);
    set_reg32(reg_CX, save_ecx);
    set_reg32(reg_DX, save_edx);
    set_reg32(reg_SI, save_esi);
    set_reg32(reg_DI, save_edi);
    set_reg32(reg_BP, save_ebp);
  } else {
    // Software INT reflection: copy RM results back to PM
    set_reg32(reg_AX, rm_eax);
    set_reg32(reg_BX, rm_ebx);
    set_reg32(reg_CX, rm_ecx);
    set_reg32(reg_DX, rm_edx);
    set_reg32(reg_SI, rm_esi);
    set_reg32(reg_DI, rm_edi);
    set_reg32(reg_BP, rm_ebp);
  }

  if (preserve_regs) {
    // Hardware IRQ reflection: restore ALL PM flags — RM handler must not
    // affect interrupted PM code's arithmetic flags (e.g., ZF from LAR)
    flags = save_eflags & 0xFFFF;
    eflags_hi = (save_eflags >> 16) & 0xFFFF;
  } else {
    // Software INT reflection: propagate arithmetic flags from RM result
    static constexpr uint16_t RESULT_FLAGS =
      FLAG_CF | FLAG_PF | FLAG_AF | FLAG_ZF | FLAG_SF | FLAG_OF;
    flags = (flags & ~RESULT_FLAGS) | (rm_flags & RESULT_FLAGS);
    // Restore IF and IOPL (FLAG_IF and EFLAG_IOPL_MASK are separate enums that
    // both name bits of the same low-16 FLAGS word — combine as plain bits)
    static constexpr uint16_t IF_IOPL_MASK =
      (uint16_t)FLAG_IF | (uint16_t)EFLAG_IOPL_MASK;
    flags = (flags & ~IF_IOPL_MASK) | (save_eflags & IF_IOPL_MASK);
    eflags_hi = (save_eflags >> 16) & 0xFFFF;
  }

  // Release RM reflection stack level
  if (dpmi.rm_reflect_stack_depth > 0)
    dpmi.rm_reflect_stack_depth--;
}

//=============================================================================
// DPMI Exception Handler Dispatch
//
// Builds a DPMI exception stack frame and calls the client's handler.
// The handler was installed via INT 31h/0203h.
//
// DPMI 0.9 exception frame (32-bit, pushed on the locked stack):
//   ESP+00: Return EIP (host return - for RETF)
//   ESP+04: Return CS  (host return - for RETF)
//   ESP+08: Error code
//   ESP+0C: Faulting EIP
//   ESP+10: Faulting CS
//   ESP+14: Faulting EFLAGS
//   ESP+18: Faulting ESP
//   ESP+1C: Faulting SS
//
// The handler does RETF to return. It may modify the faulting CS:EIP/SS:ESP.
//=============================================================================

void dos_machine::dpmi_dispatch_exception(uint8_t vector, uint32_t error_code,
                                           bool has_error_code) {
  uint16_t handler_sel = dpmi.exc_handler[vector].sel;
  uint32_t handler_off = dpmi.exc_handler[vector].off;

  // Save the faulting state
  uint16_t fault_cs = sregs[seg_CS];
  uint32_t fault_eip = ip;  // insn_ip was saved by raise_exception
  uint32_t fault_eflags = get_eflags();
  uint32_t fault_esp = get_esp();
  uint16_t fault_ss = sregs[seg_SS];

  // Save all segment registers and caches
  uint16_t save_ds = sregs[seg_DS], save_es = sregs[seg_ES];
  uint16_t save_fs = sregs[seg_FS], save_gs = sregs[seg_GS];
  SegDescCache save_seg_cache[6];
  memcpy(save_seg_cache, seg_cache, sizeof(seg_cache));

  // Use the PM stack segment for the exception frame.
  // This SS has base=dpmi.base so that frame offsets fit in 16 bits,
  // allowing 16-bit handler code (e.g. DOS4GW stubs with D=0) to
  // access the frame via BP-relative addressing.
  uint32_t stack_off = dpmi.pm_stack_top - dpmi.base;  // SS-relative offset
  sregs[seg_SS] = dpmi.pm_stack_ss;
  seg_cache[seg_SS].base = dpmi.base;
  seg_cache[seg_SS].limit = 0xFFFF;
  seg_cache[seg_SS].access = 0x93;
  seg_cache[seg_SS].flags = 0x00;  // G=0, B=0 (16-bit stack)
  seg_cache[seg_SS].valid = true;

  // Build the DPMI exception frame (push in reverse order)
  // All fields are 32-bit dwords (32-bit DPMI client)
  // Return address for RETF: bios_rom_cs:exc_return_off → BIOS trap
  uint32_t ss_base = dpmi.base;
  stack_off -= 4; mem->store_mem32(ss_base + stack_off, fault_ss);      // +1C: SS
  stack_off -= 4; mem->store_mem32(ss_base + stack_off, fault_esp);     // +18: ESP
  stack_off -= 4; mem->store_mem32(ss_base + stack_off, fault_eflags);  // +14: EFLAGS
  stack_off -= 4; mem->store_mem32(ss_base + stack_off, fault_cs);      // +10: CS
  stack_off -= 4; mem->store_mem32(ss_base + stack_off, fault_eip);     // +0C: EIP
  stack_off -= 4; mem->store_mem32(ss_base + stack_off, has_error_code ? error_code : 0); // +08: Error code
  // Push return address for RETF (BIOS trap sentinel)
  stack_off -= 4; mem->store_mem32(ss_base + stack_off, dpmi.bios_rom_cs); // +04: Return CS (0x0028)
  stack_off -= 4; mem->store_mem32(ss_base + stack_off, dpmi.exc_return_off); // +00: Return EIP (0xEFE8)

  set_esp(stack_off);

  dpmi_exc_dispatched = true;  // Inhibit ESP rollback in instruction handlers

  // Jump to the handler
  sregs[seg_CS] = handler_sel;
  seg_cache[seg_CS] = save_seg_cache[seg_CS]; // Keep current CS cache (handler is in same segment usually)
  // Re-parse CS descriptor for the handler's selector
  {
    uint16_t idx = handler_sel >> 3;
    bool use_ldt = (handler_sel & 4) != 0;
    uint32_t tbase = use_ldt ? ldtr_cache.base : gdtr_base;
    uint8_t desc[8];
    read_descriptor(tbase, idx, desc);
    parse_descriptor(desc, seg_cache[seg_CS]);
  }
  ip = handler_off;

  // DPMI spec: exception handlers run with interrupts disabled (IF=0)
  // This prevents pending hardware interrupts from being delivered between
  // the exception dispatch and the handler's first instruction, which would
  // push a hardware frame on the exception stack and corrupt ESP.
  clear_flag(FLAG_IF);

  // Clear in_exception flag — the exception dispatch is complete, the handler
  // is running as normal code and should be able to trigger new exceptions
  in_exception = false;

  // Save frame base (SS-relative) and segment state for potential RETF restoration
  dpmi.exc_frame_base = stack_off;
  dpmi.exc_save_ds = save_ds;
  dpmi.exc_save_es = save_es;
  dpmi.exc_save_fs = save_fs;
  dpmi.exc_save_gs = save_gs;
  memcpy(dpmi.exc_save_seg_cache, save_seg_cache, sizeof(save_seg_cache));
}

//=============================================================================
// INT 31h — DPMI API dispatch
//=============================================================================

void dos_machine::dpmi_int31h() {
  uint16_t func = regs[reg_AX];

  // Default: success (carry clear)
  clear_flag(FLAG_CF);

  switch (func) {
    //=== LDT Descriptor Management ===

    case 0x0000: {  // Allocate LDT Descriptors
      uint16_t count = regs[reg_CX];
      if (count == 0) count = 1;
      // Allocate 'count' contiguous selectors
      // Find first run of 'count' free entries
      uint16_t first_sel = 0;
      for (int start = 1; start + count <= DpmiState::LDT_MAX; start++) {
        bool all_free = true;
        for (int j = 0; j < count; j++) {
          int idx = start + j;
          if (dpmi.ldt_alloc[idx / 8] & (1 << (idx % 8))) {
            all_free = false;
            start = idx;  // Skip ahead
            break;
          }
        }
        if (all_free) {
          first_sel = (start << 3) | 4 | 0;
          // Mark all as allocated and initialize as data segments
          for (int j = 0; j < count; j++) {
            int idx = start + j;
            dpmi.ldt_alloc[idx / 8] |= (1 << (idx % 8));
            uint16_t sel = ((start + j) << 3) | 4 | 0;
            dpmi_write_ldt_entry(sel, 0, 0, 0x92, 0x00);  // ring0 data, base=0, limit=0
          }
          break;
        }
      }
      if (first_sel) {
        regs[reg_AX] = first_sel;
      } else {
        set_flag(FLAG_CF);
        regs[reg_AX] = 0x8011;  // Descriptor unavailable
      }
      break;
    }

    case 0x0001: {  // Free LDT Descriptor
      uint16_t sel = regs[reg_BX];
      uint16_t idx = sel >> 3;
      if (idx == 0 || idx >= DpmiState::LDT_MAX || !(sel & 4)) {
        set_flag(FLAG_CF);
        regs[reg_AX] = 0x8022;  // Invalid selector
        break;
      }
      dpmi_free_ldt_sel(sel);
      break;
    }

    case 0x0002: {  // Segment to Descriptor
      uint16_t rm_seg = regs[reg_BX];
      // Check cache first
      for (int i = 0; i < DpmiState::MAX_SEG_MAP; i++) {
        if (dpmi.seg_map[i].valid && dpmi.seg_map[i].rm_seg == rm_seg) {
          regs[reg_AX] = dpmi.seg_map[i].pm_sel;
          // `break' here would leave the for loop, not the switch case, and the
          // allocate-a-new-descriptor path below would run anyway.  Nothing
          // follows the switch in this function, so returning is the cache hit.
          return;
        }
      }
      // Allocate new
      uint16_t sel = dpmi_alloc_ldt_sel();
      if (!sel) {
        set_flag(FLAG_CF);
        regs[reg_AX] = 0x8011;
        break;
      }
      dpmi_write_ldt_entry(sel, (uint32_t)rm_seg << 4, 0xFFFF, 0x92, 0x00);
      // Cache it
      for (int i = 0; i < DpmiState::MAX_SEG_MAP; i++) {
        if (!dpmi.seg_map[i].valid) {
          dpmi.seg_map[i] = { rm_seg, sel, true };
          break;
        }
      }
      regs[reg_AX] = sel;
      break;
    }

    case 0x0003:  // Get Selector Increment Value
      regs[reg_AX] = 8;  // Each selector is 8 bytes apart in LDT
      break;

    case 0x0006: {  // Get Segment Base Address
      uint16_t sel = regs[reg_BX];
      if (dpmi_bad_selector(sel)) break;
      uint8_t desc[8];
      dpmi_read_ldt_raw(sel, desc);
      uint32_t base = desc[2] | (desc[3] << 8) | (desc[4] << 16) | (desc[7] << 24);
      regs[reg_CX] = (base >> 16) & 0xFFFF;
      regs[reg_DX] = base & 0xFFFF;
      break;
    }

    case 0x0007: {  // Set Segment Base Address
      uint16_t sel = regs[reg_BX];
      if (dpmi_bad_selector(sel)) break;
      uint32_t base = ((uint32_t)regs[reg_CX] << 16) | regs[reg_DX];
      uint8_t desc[8];
      dpmi_read_ldt_raw(sel, desc);
      desc[2] = base & 0xFF;
      desc[3] = (base >> 8) & 0xFF;
      desc[4] = (base >> 16) & 0xFF;
      desc[7] = (base >> 24) & 0xFF;
      dpmi_write_ldt_raw(sel, desc);
      // Update seg_cache if this selector is currently loaded
      for (int s = 0; s < 6; s++) {
        if (sregs[s] == sel) {
          seg_cache[s].base = base;
        }
      }
      break;
    }

    case 0x0008: {  // Set Segment Limit
      uint16_t sel = regs[reg_BX];
      if (dpmi_bad_selector(sel)) break;
      uint32_t limit = ((uint32_t)regs[reg_CX] << 16) | regs[reg_DX];
      uint8_t desc[8];
      dpmi_read_ldt_raw(sel, desc);
      uint8_t g_flag = 0;
      uint32_t raw_limit = limit;
      if (limit > 0xFFFFF) {
        // Need granularity bit
        g_flag = 0x80;  // G bit in byte 6
        raw_limit = limit >> 12;
      }
      desc[0] = raw_limit & 0xFF;
      desc[1] = (raw_limit >> 8) & 0xFF;
      desc[6] = (desc[6] & 0x70) | g_flag | ((raw_limit >> 16) & 0x0F);
      dpmi_write_ldt_raw(sel, desc);
      // Update seg_cache
      for (int s = 0; s < 6; s++) {
        if (sregs[s] == sel) {
          seg_cache[s].limit = limit;
          seg_cache[s].flags = (desc[6] >> 4) & 0x0F;
        }
      }
      break;
    }

    case 0x0009: {  // Set Descriptor Access Rights
      uint16_t sel = regs[reg_BX];
      if (dpmi_bad_selector(sel)) break;
      uint16_t rights = regs[reg_CX];
      uint8_t desc[8];
      dpmi_read_ldt_raw(sel, desc);
      desc[5] = rights & 0xFF;           // Access byte
      desc[6] = (desc[6] & 0x0F) | (rights & 0xF0);  // Flags nibble (high of CL) -> high nibble of byte 6
      // Wait, CX format: CL = access byte, CH = extended type/flags
      // Actually DPMI spec: CL = access rights byte (byte 5), CH = 386 extended type (byte 6 high nibble)
      desc[5] = rights & 0xFF;
      desc[6] = (desc[6] & 0x0F) | ((rights >> 8) & 0xF0);
      dpmi_write_ldt_raw(sel, desc);
      // Update seg_cache
      for (int s = 0; s < 6; s++) {
        if (sregs[s] == sel) {
          SegDescCache c;
          parse_descriptor(desc, c);
          seg_cache[s] = c;
        }
      }
      break;
    }

    case 0x000A: {  // Create Code Segment Alias Descriptor
      uint16_t sel = regs[reg_BX];
      if (dpmi_bad_selector(sel)) break;
      uint8_t desc[8];
      dpmi_read_ldt_raw(sel, desc);
      // Create a data segment with the same base/limit
      uint16_t alias = dpmi_alloc_ldt_sel();
      if (!alias) {
        set_flag(FLAG_CF);
        regs[reg_AX] = 0x8011;
        break;
      }
      uint8_t new_desc[8];
      memcpy(new_desc, desc, 8);
      // Change to data segment: clear code bit (bit 3 of access type nibble)
      new_desc[5] = (new_desc[5] & 0xF0) | 0x02;  // Data, read/write
      // Keep DPL = 3
      new_desc[5] = (new_desc[5] & 0x8F) | 0x60;  // DPL=3
      new_desc[5] |= 0x90;  // Present, S=1
      dpmi_write_ldt_raw(alias, new_desc);
      regs[reg_AX] = alias;
      break;
    }

    case 0x000B: {  // Get Descriptor
      uint16_t sel = regs[reg_BX];
      if (dpmi_bad_selector(sel)) break;
      uint8_t desc[8];
      dpmi_read_ldt_raw(sel, desc);
      // Copy to ES:EDI
      uint32_t dst = seg_cache[seg_ES].base + get_reg32(reg_DI);
      for (int i = 0; i < 8; i++)
        mem->store_mem(dst + i, desc[i]);
      break;
    }

    case 0x000C: {  // Set Descriptor
      uint16_t sel = regs[reg_BX];
      if (dpmi_bad_selector(sel)) break;
      // Read from ES:EDI
      uint32_t src = seg_cache[seg_ES].base + get_reg32(reg_DI);
      uint8_t desc[8];
      for (int i = 0; i < 8; i++)
        desc[i] = mem->fetch_mem(src + i);
      dpmi_write_ldt_raw(sel, desc);
      // Update seg_cache
      for (int s = 0; s < 6; s++) {
        if (sregs[s] == sel) {
          parse_descriptor(desc, seg_cache[s]);
        }
      }
      break;
    }

    //=== DOS Memory Management ===

    case 0x0100: {  // Allocate DOS Memory Block
      uint16_t paragraphs = regs[reg_BX];
      // Use INT 21h AH=48h in real mode to allocate
      // Save PM state, switch to RM, call INT 21h, switch back
      // (AX is not preserved: it carries the DPMI return value — the allocated
      //  real-mode segment on success, or the error code on failure.)
      regs[reg_AX] = 0x4800;
      // BX already has paragraphs
      dpmi_reflect_to_rm(0x21);
      if (get_flag(FLAG_CF)) {
        // Failed — BX has max available paragraphs
        regs[reg_AX] = 0x0008;  // Insufficient memory
        set_flag(FLAG_CF);
      } else {
        // AX = segment of allocated block
        uint16_t seg = regs[reg_AX];
        // Create a descriptor for this block
        uint16_t sel = dpmi_alloc_ldt_sel();
        if (sel) {
          dpmi_write_ldt_entry(sel, (uint32_t)seg << 4,
                               (uint32_t)paragraphs * 16 - 1, 0x92, 0x00);
          // Track it
          for (int i = 0; i < DpmiState::MAX_DOS_BLOCKS; i++) {
            if (!dpmi.dos_blocks[i].allocated) {
              dpmi.dos_blocks[i] = { seg, paragraphs, sel, true };
              break;
            }
          }
        }
        regs[reg_AX] = seg;
        regs[reg_DX] = sel;
        clear_flag(FLAG_CF);
      }
      break;
    }

    case 0x0101: {  // Free DOS Memory Block
      uint16_t sel = regs[reg_DX];
      // Find the block
      for (int i = 0; i < DpmiState::MAX_DOS_BLOCKS; i++) {
        if (dpmi.dos_blocks[i].allocated && dpmi.dos_blocks[i].selector == sel) {
          // Free via INT 21h AH=49h — must set ES=segment for RM call
          // Save PM ES (it's a valid PM selector) before setting RM segment
          uint16_t pm_es = sregs[seg_ES];
          SegDescCache pm_es_cache = seg_cache[seg_ES];
          regs[reg_AX] = 0x4900;
          sregs[seg_ES] = dpmi.dos_blocks[i].segment;
          seg_cache[seg_ES].base = (uint32_t)dpmi.dos_blocks[i].segment << 4;
          dpmi_reflect_to_rm(0x21);
          // Restore PM ES
          sregs[seg_ES] = pm_es;
          seg_cache[seg_ES] = pm_es_cache;
          dpmi_free_ldt_sel(sel);
          dpmi.dos_blocks[i].allocated = false;
          clear_flag(FLAG_CF);
          return;
        }
      }
      set_flag(FLAG_CF);
      regs[reg_AX] = 0x0009;  // Invalid block
      break;
    }

    case 0x0102: {  // Resize DOS Memory Block
      // Simplified: just update the descriptor limit
      uint16_t sel = regs[reg_DX];
      uint16_t new_para = regs[reg_BX];
      for (int i = 0; i < DpmiState::MAX_DOS_BLOCKS; i++) {
        if (dpmi.dos_blocks[i].allocated && dpmi.dos_blocks[i].selector == sel) {
          dpmi_write_ldt_entry(sel, (uint32_t)dpmi.dos_blocks[i].segment << 4,
                               (uint32_t)new_para * 16 - 1, 0x92, 0x00);
          dpmi.dos_blocks[i].paragraphs = new_para;
          clear_flag(FLAG_CF);
          return;
        }
      }
      set_flag(FLAG_CF);
      regs[reg_AX] = 0x0009;
      break;
    }

    //=== Interrupt Vectors ===

    case 0x0200: {  // Get Real Mode Interrupt Vector
      uint8_t vec = regs[reg_BX] & 0xFF;
      regs[reg_CX] = dpmi.rm_int_seg[vec];
      regs[reg_DX] = dpmi.rm_int_off[vec];
      break;
    }

    case 0x0201: {  // Set Real Mode Interrupt Vector
      uint8_t vec = regs[reg_BX] & 0xFF;
      dpmi.rm_int_seg[vec] = regs[reg_CX];
      dpmi.rm_int_off[vec] = regs[reg_DX];
      // Also update the actual IVT in memory
      mem->store_mem16(vec * 4, regs[reg_DX]);
      mem->store_mem16(vec * 4 + 2, regs[reg_CX]);
      break;
    }

    case 0x0202: {  // Get Processor Exception Handler Vector
      uint8_t exc = regs[reg_BX] & 0xFF;
      if (exc < 32 && dpmi.exc_installed[exc]) {
        regs[reg_CX] = dpmi.exc_handler[exc].sel;
        set_reg32(reg_DX, dpmi.exc_handler[exc].off);
      } else {
        regs[reg_CX] = 0;
        set_reg32(reg_DX, 0);
      }
      break;
    }

    case 0x0203: {  // Set Processor Exception Handler Vector
      uint8_t exc = regs[reg_BX] & 0xFF;
      if (exc < 32) {
        uint16_t sel = regs[reg_CX];
        uint32_t off = get_reg32(reg_DX);
        dpmi.exc_handler[exc].sel = sel;
        dpmi.exc_handler[exc].off = off;
        // Setting handler to 0000:00000000 means "uninstall" (restore default)
        dpmi.exc_installed[exc] = (sel != 0 || off != 0);
      }
      break;
    }

    case 0x0204: {  // Get Protected Mode Interrupt Vector
      uint8_t vec = regs[reg_BX] & 0xFF;
      if (dpmi.pm_int_installed[vec]) {
        regs[reg_CX] = dpmi.pm_int[vec].sel;
        set_reg32(reg_DX, dpmi.pm_int[vec].off);
      } else {
        // Return default handler stub in BIOS ROM (callable via CALL FAR)
        regs[reg_CX] = dpmi.bios_rom_cs;
        set_reg32(reg_DX, pm_int_default_entry[vec]);
      }
      break;
    }

    case 0x0205: {  // Set Protected Mode Interrupt Vector
      uint8_t vec = regs[reg_BX] & 0xFF;
      uint16_t sel = regs[reg_CX];
      uint32_t off = get_reg32(reg_DX);
      dpmi.pm_int[vec].sel = sel;
      dpmi.pm_int[vec].off = off;
      // Detect if client is re-installing our default stub — treat as "not installed"
      // so interrupts reflect to RM instead of dispatching via IDT to a RETF stub
      bool is_default = (sel == dpmi.bios_rom_cs && off == pm_int_default_entry[vec]);
      dpmi.pm_int_installed[vec] = !is_default;
      if (!is_default)
        dpmi_write_idt_entry(vec, sel, off, 0, dpmi.is_32bit);
      break;
    }

    //=== Real Mode Translation Services ===

    case 0x0300:    // Simulate Real Mode Interrupt
    case 0x0301:    // Call RM Procedure With Far Return Frame
    case 0x0302: {  // Call RM Procedure With IRET Frame
      uint8_t vec = regs[reg_BX] & 0xFF;
      uint16_t copy_words = regs[reg_CX];
      uint32_t struct_addr = seg_cache[seg_ES].base + get_reg32(reg_DI);
      dpmi_exec_rm(vec, struct_addr, copy_words, (func == 0x0300 || func == 0x0302));
      break;
    }

    //=== DPMI Version ===

    case 0x0400: {  // Get DPMI Version
      regs[reg_AX] = 0x005A;  // Version 0.90
      // Bit 0: 32-bit programs supported.  Bit 1: the host returns to REAL mode
      // for reflected interrupts (dpmi_reflect_to_rm clears CR0.PE), not V86.
      // Bit 2 (virtual memory) stays clear: there is no paging in this host.
      regs[reg_BX] = 0x0003;  // Flags: 32-bit programs, real-mode reflection, no virtual memory
      set_reg8(reg_CL, 3);    // Processor type: 386
      set_reg8(reg_DH, pic_vector_base);  // Master PIC base
      set_reg8(reg_DL, 0x70);             // Slave PIC base
      break;
    }

    //=== Memory Information ===

    case 0x0500: {  // Get Free Memory Information
      uint32_t dst = seg_cache[seg_ES].base + get_reg32(reg_DI);
      uint32_t total_phys = mem->get_mem_size();
      uint32_t free_mem = total_phys > dpmi.next_mem_base ?
                          total_phys - dpmi.next_mem_base - 0x20000 : 0;
      // Fill 48-byte structure
      mem->store_mem32(dst + 0x00, free_mem);      // Largest available free block
      mem->store_mem32(dst + 0x04, free_mem / 4096); // Max unlocked page allocation
      mem->store_mem32(dst + 0x08, free_mem / 4096); // Max locked page allocation
      mem->store_mem32(dst + 0x0C, free_mem / 4096); // Linear address space (pages)
      mem->store_mem32(dst + 0x10, 0);               // Total unlocked pages
      mem->store_mem32(dst + 0x14, free_mem / 4096); // Free pages
      mem->store_mem32(dst + 0x18, total_phys / 4096); // Total physical pages
      mem->store_mem32(dst + 0x1C, free_mem / 4096); // Free linear address space
      mem->store_mem32(dst + 0x20, 0xFFFFFFFF);      // Swap file size (no swap)
      // Bytes 0x24-0x2F: reserved, fill with -1
      for (int i = 0x24; i < 0x30; i += 4)
        mem->store_mem32(dst + i, 0xFFFFFFFF);
      break;
    }

    case 0x0501: {  // Allocate Memory Block
      uint32_t size = ((uint32_t)regs[reg_BX] << 16) | regs[reg_CX];
      if (size == 0) size = 1;
      // Page-align
      uint32_t alloc_base = (dpmi.next_mem_base + 0xFFF) & ~0xFFF;
      if (alloc_base + size > dpmi.base) {
        set_flag(FLAG_CF);
        regs[reg_AX] = 0x8012;  // Linear memory unavailable
        break;
      }
      // Find free block slot
      int slot = -1;
      for (int i = 0; i < DpmiState::MAX_MEM_BLOCKS; i++) {
        if (!dpmi.mem_blocks[i].allocated) { slot = i; break; }
      }
      if (slot < 0) {
        set_flag(FLAG_CF);
        regs[reg_AX] = 0x8012;
        break;
      }
      uint32_t handle = dpmi.next_handle++;
      dpmi.mem_blocks[slot] = { handle, alloc_base, size, true };
      dpmi.next_mem_base = alloc_base + size;

      // Return: BX:CX = linear address, SI:DI = handle
      regs[reg_BX] = (alloc_base >> 16) & 0xFFFF;
      regs[reg_CX] = alloc_base & 0xFFFF;
      regs[reg_SI] = (handle >> 16) & 0xFFFF;
      regs[reg_DI] = handle & 0xFFFF;
      break;
    }

    case 0x0502: {  // Free Memory Block
      uint32_t handle = ((uint32_t)regs[reg_SI] << 16) | regs[reg_DI];
      bool found = false;
      for (int i = 0; i < DpmiState::MAX_MEM_BLOCKS; i++) {
        if (dpmi.mem_blocks[i].allocated && dpmi.mem_blocks[i].handle == handle) {
          dpmi.mem_blocks[i].allocated = false;
          found = true;
          break;
        }
      }
      if (!found) {
        set_flag(FLAG_CF);
        regs[reg_AX] = 0x8023;  // Invalid handle
      }
      break;
    }

    case 0x0503: {  // Resize Memory Block
      uint32_t new_size = ((uint32_t)regs[reg_BX] << 16) | regs[reg_CX];
      uint32_t handle = ((uint32_t)regs[reg_SI] << 16) | regs[reg_DI];
      if (new_size == 0) new_size = 1;
      for (int i = 0; i < DpmiState::MAX_MEM_BLOCKS; i++) {
        if (dpmi.mem_blocks[i].allocated && dpmi.mem_blocks[i].handle == handle) {
          uint32_t old_base = dpmi.mem_blocks[i].base;
          uint32_t old_size = dpmi.mem_blocks[i].size;
          if (new_size <= old_size) {
            // Shrink in place
            dpmi.mem_blocks[i].size = new_size;
          } else {
            // Need to relocate — allocate new block
            uint32_t alloc_base = (dpmi.next_mem_base + 0xFFF) & ~0xFFF;
            if (alloc_base + new_size > dpmi.base) {
              set_flag(FLAG_CF);
              regs[reg_AX] = 0x8012;
              return;
            }
            // Copy old data
            bool old_a20 = mem->get_a20();
            mem->set_a20(true);
            for (uint32_t j = 0; j < old_size; j++)
              mem->store_mem(alloc_base + j, mem->fetch_mem(old_base + j));
            mem->set_a20(old_a20);
            dpmi.mem_blocks[i].base = alloc_base;
            dpmi.mem_blocks[i].size = new_size;
            dpmi.next_mem_base = alloc_base + new_size;
            old_base = alloc_base;  // For return value
          }
          regs[reg_BX] = (dpmi.mem_blocks[i].base >> 16) & 0xFFFF;
          regs[reg_CX] = dpmi.mem_blocks[i].base & 0xFFFF;
          regs[reg_SI] = (handle >> 16) & 0xFFFF;
          regs[reg_DI] = handle & 0xFFFF;
          return;
        }
      }
      set_flag(FLAG_CF);
      regs[reg_AX] = 0x8023;
      break;
    }

    //=== Page Locking (no-ops — no paging) ===

    case 0x0600:  // Lock Linear Region
    case 0x0601:  // Unlock Linear Region
    case 0x0602:  // Mark Real Mode Region as Pageable
    case 0x0603:  // Relock Real Mode Region
      clear_flag(FLAG_CF);  // Always succeed
      break;

    //=== Physical Address Mapping ===

    case 0x0800: {  // Physical Address Mapping
      uint32_t phys_addr = ((uint32_t)regs[reg_BX] << 16) | regs[reg_CX];
      // Identity mapping (linear = physical) since we have no paging, so the
      // requested size in SI:DI needs no page-table work and is ignored.
      regs[reg_BX] = (phys_addr >> 16) & 0xFFFF;
      regs[reg_CX] = phys_addr & 0xFFFF;
      break;
    }

    case 0x0801:  // Free Physical Address Mapping
      clear_flag(FLAG_CF);
      break;

    //=== Virtual Interrupt State ===

    case 0x0900: {  // Get and Disable Virtual Interrupt State
      set_reg8(reg_AL, dpmi.vif ? 1 : 0);
      dpmi.vif = false;
      break;
    }

    case 0x0901: {  // Get and Enable Virtual Interrupt State
      set_reg8(reg_AL, dpmi.vif ? 1 : 0);
      dpmi.vif = true;
      break;
    }

    case 0x0902: {  // Get Virtual Interrupt State
      set_reg8(reg_AL, dpmi.vif ? 1 : 0);
      break;
    }

    //=== State Save/Restore and Raw Mode Switch ===

    case 0x0303: {  // Allocate Real Mode Callback Address
      // DS:ESI = PM procedure to call (selector:offset)
      // ES:EDI = real-mode call structure (buffer for RM registers)
      // Returns: CX:DX = real-mode callback address (seg:off)
      //
      // When real-mode code calls this address, the DPMI server:
      // 1. Saves RM state into the call structure
      // 2. Switches to PM
      // 3. Calls the PM procedure
      //
      // We create a small stub in low memory that does INT FFh (unused),
      // which we intercept.
      int slot = -1;
      for (int i = 0; i < DpmiState::MAX_RM_CALLBACKS; i++) {
        if (!dpmi.rm_callback_used[i]) { slot = i; break; }
      }
      if (slot < 0) {
        set_flag(FLAG_CF);
        regs[reg_AX] = 0x8015;  // Callback unavailable
        break;
      }
      dpmi.rm_callback_used[slot] = true;
      // Store callback info for later dispatch
      // Use a small thunk area at 0000:6800 (unused conventional memory)
      uint16_t thunk_seg = DpmiState::RM_CALLBACK_SEG;
      uint16_t thunk_off = (uint16_t)(DpmiState::RM_CALLBACK_OFF + slot * 4);
      // Write: CD FF CB (INT FFh, RETF) at the thunk address
      uint32_t thunk_addr = thunk_off;
      mem->store_mem(thunk_addr, 0xCD);      // INT
      mem->store_mem(thunk_addr + 1, 0xFF);  // FFh
      mem->store_mem(thunk_addr + 2, 0xCB);  // RETF

      // Record callback info in IVT entry FFh area (or separate table)
      // We'll use a simple array. Store pm_sel:pm_off and rm_struct address.
      // For now, just save in the dpmi state.
      if (slot == 0) {
        // First time: hook INT FFh to our BIOS trap
        // Make sure IVT[FF] points to our BIOS stub
        mem->store_mem16(0xFF * 4, bios_entry[0xFF]);
        mem->store_mem16(0xFF * 4 + 2, 0xF000);
      }

      regs[reg_CX] = thunk_seg;
      regs[reg_DX] = thunk_off;
      break;
    }

    case 0x0304: {  // Free Real Mode Callback Address
      // CX:DX = the callback address 0303h handed out.  Validate it and give
      // the slot back, so allocate/free in a loop does not run out after 16.
      uint16_t cb_seg = regs[reg_CX];
      uint16_t cb_off = regs[reg_DX];
      int slot = (cb_off - DpmiState::RM_CALLBACK_OFF) / 4;
      if (cb_seg != DpmiState::RM_CALLBACK_SEG ||
          cb_off < DpmiState::RM_CALLBACK_OFF ||
          (cb_off - DpmiState::RM_CALLBACK_OFF) % 4 != 0 ||
          slot >= DpmiState::MAX_RM_CALLBACKS ||
          !dpmi.rm_callback_used[slot]) {
        set_flag(FLAG_CF);
        regs[reg_AX] = 0x8024;  // Invalid callback address (DPMI 0.9, 0304h)
        break;
      }
      dpmi.rm_callback_used[slot] = false;
      break;
    }

    case 0x0305: {  // Get State Save/Restore Addresses
      // Return addresses for saving/restoring DPMI client state
      // We don't need real save/restore, but must return valid addresses
      // Real-mode: AX = size of buffer, BX:CX = RM save/restore address
      // Protected-mode: AX = size, SI:EDI = PM save/restore address
      regs[reg_AX] = 0;  // No save buffer needed (we manage state internally)
      regs[reg_BX] = 0;
      regs[reg_CX] = 0;
      regs[reg_SI] = dpmi.ring0_cs;
      set_reg32(reg_DI, 0);
      break;
    }

    case 0x0306: {  // Get Raw Mode Switch Addresses
      // BX:CX = real-to-protected switch address (RM→PM)
      // SI:EDI = protected-to-real switch address (PM→RM)
      // DOS4GW uses these for direct mode switching.
      // RM→PM: same trap entry as initial mode switch (F000:EFDC)
      //   differentiated by dpmi.active flag
      regs[reg_BX] = 0xF000;
      regs[reg_CX] = dpmi.mode_switch_off;
      // PM→RM: trap at bios_rom_cs:raw_pm_to_rm_off (base=0xF0000 + off)
      //   Use bios_rom_cs (base=0xF0000) so EDI stays in 16-bit range
      //   This avoids corrupting high 16 bits of EDI for 16-bit DOS4GW code
      regs[reg_SI] = dpmi.bios_rom_cs;
      set_reg32(reg_DI, dpmi.raw_pm_to_rm_off);  // Just 0xEFE4
      break;
    }

    case 0x0A00: {  // Get Vendor-Specific API Entry Point
      // DS:ESI points at the vendor name string.  No vendor API is
      // implemented, so the name is not read: the 31-byte copy that used to
      // stand here filled a buffer nothing compared against (-Wunused-but-
      // set-variable).  Implementing a vendor API means reading it here and
      // returning ES:EDI for a match; until then the answer is the same for
      // every name.
      // Not supported — return CF
      set_flag(FLAG_CF);
      regs[reg_AX] = 0x8001;
      break;
    }

    //=== Debug Registers ===

    case 0x0B00:  // Set Debug Watchpoint
    case 0x0B01:  // Clear Debug Watchpoint
    case 0x0B02:  // Get State of Debug Watchpoint
    case 0x0B03:  // Reset Debug Watchpoint
      set_flag(FLAG_CF);  // Not supported
      regs[reg_AX] = 0x8001;  // Unsupported function
      break;

    default:
      set_flag(FLAG_CF);
      regs[reg_AX] = 0x8001;  // Unsupported function
      break;
  }
}

//=============================================================================
// Real Mode Call Execution (for INT 31h 0300h/0301h/0302h)
//=============================================================================

// Low-memory save/restore for RM execution.
// DOS4GW (and other extenders) may overwrite the IVT/BDA at physical 0.
// We detect this and swap PM data ↔ real IVT/BDA on each PM↔RM transition.

void dos_machine::dpmi_save_pm_low_mem() {
  // Check if PM code has overwritten the IVT (quick canary check on IVT[21h])
  if (!dpmi.pm_overwrote_low_mem) {
    uint16_t ivt21_off = mem->fetch_mem16(0x21 * 4);
    uint16_t ivt21_seg = mem->fetch_mem16(0x21 * 4 + 2);
    if (ivt21_seg != dpmi.rm_int_seg[0x21] || ivt21_off != dpmi.rm_int_off[0x21]) {
      dpmi.pm_overwrote_low_mem = true;
    }
  }
  if (dpmi.pm_overwrote_low_mem) {
    uint8_t *raw = mem->get_mem();
    // Save PM's data at physical 0
    memcpy(dpmi.pm_low_mem, raw, DpmiState::LOW_MEM_SAVE_SIZE);
    // Restore original low memory for RM execution:
    //  - IVT (0x000-0x3FF): from rm_int_off/seg cache (authoritative, may be updated by 0201h)
    //  - BDA+DOS (0x400-0x5FF): from original_low_mem snapshot
    for (int i = 0; i < 256; i++) {
      raw[i * 4 + 0] = dpmi.rm_int_off[i] & 0xFF;
      raw[i * 4 + 1] = (dpmi.rm_int_off[i] >> 8) & 0xFF;
      raw[i * 4 + 2] = dpmi.rm_int_seg[i] & 0xFF;
      raw[i * 4 + 3] = (dpmi.rm_int_seg[i] >> 8) & 0xFF;
    }
    memcpy(raw + 0x400, dpmi.original_low_mem + 0x400,
           DpmiState::LOW_MEM_SAVE_SIZE - 0x400);
  }
}

void dos_machine::dpmi_restore_pm_low_mem() {
  if (dpmi.pm_overwrote_low_mem) {
    uint8_t *raw = mem->get_mem();
    // Update caches from physical memory (RM code may have changed vectors or BDA)
    for (int i = 0; i < 256; i++) {
      dpmi.rm_int_off[i] = mem->fetch_mem16(i * 4);
      dpmi.rm_int_seg[i] = mem->fetch_mem16(i * 4 + 2);
    }
    memcpy(dpmi.original_low_mem + 0x400, raw + 0x400,
           DpmiState::LOW_MEM_SAVE_SIZE - 0x400);
    // Restore PM's data to physical 0
    memcpy(raw, dpmi.pm_low_mem, DpmiState::LOW_MEM_SAVE_SIZE);
  }
}

// DPMI Real Mode Call Structure offsets:
// 0x00: EDI  0x04: ESI  0x08: EBP  0x0C: reserved
// 0x10: EBX  0x14: EDX  0x18: ECX  0x1C: EAX
// 0x20: FLAGS  0x22: ES  0x24: DS  0x26: FS  0x28: GS
// 0x2A: IP  0x2C: CS  0x2E: SP  0x30: SS

void dos_machine::dpmi_exec_rm(uint8_t vector, uint32_t struct_addr,
                                uint16_t copy_words, bool use_iret) {
  // Save entire PM state
  uint16_t save_segs[6];
  SegDescCache save_cache[6];
  uint32_t save_eip = ip;
  uint32_t save_esp = get_esp();
  uint32_t save_eflags = get_eflags();
  uint8_t save_cpl = cpl;
  uint32_t save_cr0 = cr0;
  for (int i = 0; i < 6; i++) {
    save_segs[i] = sregs[i];
    save_cache[i] = seg_cache[i];
  }
  uint32_t save_eax = get_reg32(reg_AX);
  uint32_t save_ebx = get_reg32(reg_BX);
  uint32_t save_ecx = get_reg32(reg_CX);
  uint32_t save_edx = get_reg32(reg_DX);
  uint32_t save_esi = get_reg32(reg_SI);
  uint32_t save_edi = get_reg32(reg_DI);
  uint32_t save_ebp = get_reg32(reg_BP);

  // Read call structure
  uint32_t rm_edi = mem->fetch_mem32(struct_addr + 0x00);
  uint32_t rm_esi = mem->fetch_mem32(struct_addr + 0x04);
  uint32_t rm_ebp = mem->fetch_mem32(struct_addr + 0x08);
  uint32_t rm_ebx = mem->fetch_mem32(struct_addr + 0x10);
  uint32_t rm_edx = mem->fetch_mem32(struct_addr + 0x14);
  uint32_t rm_ecx = mem->fetch_mem32(struct_addr + 0x18);
  uint32_t rm_eax = mem->fetch_mem32(struct_addr + 0x1C);
  uint16_t rm_flags = mem->fetch_mem16(struct_addr + 0x20);
  uint16_t rm_es = mem->fetch_mem16(struct_addr + 0x22);
  uint16_t rm_ds = mem->fetch_mem16(struct_addr + 0x24);
  uint16_t rm_fs = mem->fetch_mem16(struct_addr + 0x26);
  uint16_t rm_gs = mem->fetch_mem16(struct_addr + 0x28);
  uint16_t rm_ip = mem->fetch_mem16(struct_addr + 0x2A);
  uint16_t rm_cs = mem->fetch_mem16(struct_addr + 0x2C);
  uint16_t rm_sp = mem->fetch_mem16(struct_addr + 0x2E);
  uint16_t rm_ss = mem->fetch_mem16(struct_addr + 0x30);

  // Save PM's low memory and restore IVT/BDA for real-mode execution
  dpmi_save_pm_low_mem();

  // Switch to real mode
  cr0 &= ~CR0_PE;
  cpl = 0;

  // Load RM registers
  set_reg32(reg_AX, rm_eax);
  set_reg32(reg_BX, rm_ebx);
  set_reg32(reg_CX, rm_ecx);
  set_reg32(reg_DX, rm_edx);
  set_reg32(reg_SI, rm_esi);
  set_reg32(reg_DI, rm_edi);
  set_reg32(reg_BP, rm_ebp);

  // Set up stack
  if (rm_ss == 0 && rm_sp == 0) {
    // Use DPMI-provided RM stack
    rm_ss = 0x0000;
    rm_sp = 0x7000;
  }
  load_segment_real(seg_SS, rm_ss);
  regs[reg_SP] = rm_sp;
  regs_hi[reg_SP] = 0;

  // Copy words from PM stack to RM stack
  if (copy_words > 0) {
    uint32_t pm_stack_base = save_cache[seg_SS].base + save_esp;
    for (int i = copy_words - 1; i >= 0; i--) {
      uint16_t w = mem->fetch_mem16(pm_stack_base + i * 2);
      push_word(w);
    }
  }

  load_segment_real(seg_DS, rm_ds);
  load_segment_real(seg_ES, rm_es);
  load_segment_real(seg_FS, rm_fs);
  load_segment_real(seg_GS, rm_gs);

  flags = rm_flags;

  // Determine handler address based on original DPMI function
  uint16_t dpmi_func = save_eax & 0xFFFF;
  bool is_int = (dpmi_func == 0x0300);
  uint16_t handler_seg, handler_off;
  if (is_int) {
    // 0300h: Simulate RM interrupt — use IVT
    handler_off = dpmi.rm_int_off[vector];
    handler_seg = dpmi.rm_int_seg[vector];
  } else {
    // 0301h/0302h: Call RM procedure — use CS:IP from structure
    handler_off = rm_ip;
    handler_seg = rm_cs;
    if (handler_seg == 0 && handler_off == 0) {
      // CS:IP=0:0 in call structure — DOS4GW sometimes does this.
      // Fall back: use cached IVT[21h] (DOS INT handler)
      // The AH value suggests DOS function call
      uint8_t ah = (rm_eax >> 8) & 0xFF;
      if (ah >= 0x01 && ah <= 0x6C) {
        handler_off = dpmi.rm_int_off[0x21];
        handler_seg = dpmi.rm_int_seg[0x21];
      }
    }
  }
  if (is_int && handler_seg == 0xF000 && handler_off == bios_entry[vector]) {
    dispatch_bios(vector);
  } else {
    // Push return frame (IRET frame or FAR frame)
    if (is_int || use_iret) {
      // IRET frame: push FLAGS, CS, IP (sentinel)
      push_word(flags);
    }
    // Push sentinel return address
    push_word(0xF000);
    push_word(dpmi.rm_return_off);

    if (is_int) flags &= ~(uint16_t)FLAG_IF;

    // Jump to handler
    if (is_int) {
      handler_off = dpmi.rm_int_off[vector];
      handler_seg = dpmi.rm_int_seg[vector];
    }
    load_segment_real(seg_CS, handler_seg);
    ip = handler_off;

    // Nested execute loop
    dpmi.in_rm_callback = true;
    int safety = 0;
    while (dpmi.in_rm_callback && safety < 10000000) {
      execute();
      check_interrupts();
      if (cycles - tick_cycle_mark >= CYCLES_PER_TICK) {
        tick_cycle_mark = cycles;
        uint32_t ticks = bda_r32(bda::TIMER_COUNT) + 1;
        if (ticks >= 0x1800B0) { ticks = 0; bda_w8(bda::TIMER_ROLLOVER, 1); }
        bda_w32(bda::TIMER_COUNT, ticks);
      }
      safety++;
    }
    if (safety >= 10000000) {
      emu88_fatal("[DPMI] WARNING: RM exec for INT %02Xh exceeded 10M insns", vector);
    }
  }

  // Restore PM's low memory data after RM execution
  dpmi_restore_pm_low_mem();

  // Write results back to call structure
  mem->store_mem32(struct_addr + 0x00, get_reg32(reg_DI));
  mem->store_mem32(struct_addr + 0x04, get_reg32(reg_SI));
  mem->store_mem32(struct_addr + 0x08, get_reg32(reg_BP));
  mem->store_mem32(struct_addr + 0x10, get_reg32(reg_BX));
  mem->store_mem32(struct_addr + 0x14, get_reg32(reg_DX));
  mem->store_mem32(struct_addr + 0x18, get_reg32(reg_CX));
  mem->store_mem32(struct_addr + 0x1C, get_reg32(reg_AX));
  mem->store_mem16(struct_addr + 0x20, flags);
  mem->store_mem16(struct_addr + 0x22, sregs[seg_ES]);
  mem->store_mem16(struct_addr + 0x24, sregs[seg_DS]);
  mem->store_mem16(struct_addr + 0x26, sregs[seg_FS]);
  mem->store_mem16(struct_addr + 0x28, sregs[seg_GS]);

  // Restore PM state
  cr0 = save_cr0;
  cpl = save_cpl;
  for (int i = 0; i < 6; i++) {
    sregs[i] = save_segs[i];
    seg_cache[i] = save_cache[i];
  }
  ip = save_eip;
  set_esp(save_esp);
  set_reg32(reg_AX, save_eax);
  set_reg32(reg_BX, save_ebx);
  set_reg32(reg_CX, save_ecx);
  set_reg32(reg_DX, save_edx);
  set_reg32(reg_SI, save_esi);
  set_reg32(reg_DI, save_edi);
  set_reg32(reg_BP, save_ebp);
  flags = save_eflags & 0xFFFF;
  eflags_hi = (save_eflags >> 16) & 0xFFFF;

  clear_flag(FLAG_CF);  // Success
}
