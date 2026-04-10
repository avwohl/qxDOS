//=============================================================================
// emu88_pmode.cc — 386 Protected Mode support
//
// Segment descriptor loading, protected mode interrupts, paging,
// exception handling, and linear address memory access.
//=============================================================================

#include "emu88.h"
#include <cstdio>
#include <cstring>

//=============================================================================
// Segment descriptor cache initialization
//=============================================================================

void emu88::init_seg_caches(void) {
  for (int i = 0; i < 6; i++) {
    seg_cache[i].base = (emu88_uint32)sregs[i] << 4;
    seg_cache[i].limit = 0xFFFF;
    seg_cache[i].access = 0x93;  // present, DPL 0, data r/w, accessed
    seg_cache[i].flags = 0;
    seg_cache[i].valid = true;
  }
  // CS should be code segment
  seg_cache[seg_CS].access = 0x9B;  // present, DPL 0, code r/x, accessed

  ldtr = 0;
  memset(&ldtr_cache, 0, sizeof(ldtr_cache));
  tr = 0;
  memset(&tr_cache, 0, sizeof(tr_cache));
  cpl = 0;
  cr2 = 0;
  cr3 = 0;
  cr4 = 0;
  in_exception = false;
  unreal_mode = false;
  gp_trace_count = 0;
  rm_trace_count = 0;
  dpmi_trace_func = 0;
  int2f_1687_trace_pending = false;
}

//=============================================================================
// Descriptor table helpers
//=============================================================================

void emu88::read_descriptor(emu88_uint32 table_base, emu88_uint16 index,
                            emu88_uint8 desc[8]) {
  emu88_uint32 addr = table_base + (emu88_uint32)index * 8;
  // GDT/LDT/IDT base addresses are linear — must go through paging
  for (int i = 0; i < 8; i++)
    desc[i] = read_linear8(addr + i);
}

void emu88::parse_descriptor(const emu88_uint8 desc[8], SegDescCache &cache) {
  // Base: bytes 2-3 (low), byte 4 (mid), byte 7 (high)
  cache.base = (emu88_uint32)desc[2] |
               ((emu88_uint32)desc[3] << 8) |
               ((emu88_uint32)desc[4] << 16) |
               ((emu88_uint32)desc[7] << 24);

  // Limit: bytes 0-1 (low), byte 6 bits 0-3 (high)
  cache.limit = (emu88_uint32)desc[0] |
                ((emu88_uint32)desc[1] << 8) |
                ((emu88_uint32)(desc[6] & 0x0F) << 16);

  // Access byte: byte 5
  cache.access = desc[5];

  // Flags: byte 6 bits 4-7 → stored as bits 0-3
  //   bit 3 = G (granularity)
  //   bit 2 = D/B (default size)
  //   bit 1 = L (long mode, ignored for 386)
  //   bit 0 = AVL
  cache.flags = (desc[6] >> 4) & 0x0F;

  // Apply granularity: if G=1, limit is in 4KB pages
  if (cache.flags & 0x08)
    cache.limit = (cache.limit << 12) | 0xFFF;

  // Valid if Present bit is set
  cache.valid = (cache.access & 0x80) != 0;
}

//=============================================================================
// Segment loading
//=============================================================================

void emu88::load_segment_real(int seg_idx, emu88_uint16 selector) {
  sregs[seg_idx] = selector;
  seg_cache[seg_idx].base = (emu88_uint32)selector << 4;
  seg_cache[seg_idx].limit = 0xFFFF;
  seg_cache[seg_idx].access = (seg_idx == seg_CS) ? 0x9B : 0x93;
  seg_cache[seg_idx].flags = 0;
  seg_cache[seg_idx].valid = true;
  // Once all segments have real mode bases, clear the PM→RM transition flag.
  // Check CS specifically since it's the last one reloaded (via far JMP).
  if (seg_idx == seg_CS && unreal_mode) {
    unreal_mode = false;
  }
}

void emu88::load_segment(int seg_idx, emu88_uint16 selector, int cpl_override) {
  if (!protected_mode() || v86_mode()) {
    load_segment_real(seg_idx, selector);
    return;
  }

  // Use cpl_override if provided (for inter-privilege RETF/IRET), else actual CPL
  emu88_uint8 eff_cpl = (cpl_override >= 0) ? (emu88_uint8)cpl_override : cpl;

  // Null selector: allowed for DS, ES, FS, GS but not CS or SS
  if ((selector & 0xFFFC) == 0) {
    if (seg_idx == seg_CS || seg_idx == seg_SS) {
      raise_exception(13, 0);  // #GP(0)
      return;
    }
    sregs[seg_idx] = selector;
    seg_cache[seg_idx].valid = false;
    seg_cache[seg_idx].base = 0;
    seg_cache[seg_idx].limit = 0;
    seg_cache[seg_idx].access = 0;
    seg_cache[seg_idx].flags = 0;
    return;
  }

  // Get descriptor from GDT or LDT
  emu88_uint16 index = (selector >> 3);
  bool use_ldt = (selector & 4) != 0;

  emu88_uint32 table_base = use_ldt ? ldtr_cache.base : gdtr_base;
  emu88_uint32 table_limit = use_ldt ? ldtr_cache.limit : (emu88_uint32)gdtr_limit;

  // Check selector is within table bounds
  emu88_uint32 desc_offset = (emu88_uint32)index * 8;
  if (desc_offset + 7 > table_limit) {
    raise_exception(13, selector & 0xFFFC);  // #GP(selector)
    return;
  }

  // Read and parse the descriptor
  emu88_uint8 desc[8];
  read_descriptor(table_base, index, desc);

  SegDescCache cache;
  parse_descriptor(desc, cache);

  // Check present
  if (!cache.valid) {
    // #NP for data/code segments, #SS for stack segment
    raise_exception(seg_idx == seg_SS ? 12 : 11, selector & 0xFFFC);
    return;
  }

  // Check S bit (must be code/data segment, not system)
  if (!(cache.access & 0x10)) {
    raise_exception(13, selector & 0xFFFC);  // #GP
    return;
  }

  // Type checks and privilege checks (Task 8)
  emu88_uint8 type = cache.access & 0x0F;
  emu88_uint8 desc_dpl = (cache.access >> 5) & 3;
  emu88_uint8 rpl = selector & 3;

  if (seg_idx == seg_CS) {
    // Must be code segment (bit 3 set in type = executable)
    if (!(type & 0x08)) {
      raise_exception(13, selector & 0xFFFC);
      return;
    }
    if (type & 0x04) {
      // Conforming code: DPL must be <= effective CPL
      if (desc_dpl > eff_cpl) {
        raise_exception(13, selector & 0xFFFC);
        return;
      }
      // For conforming code, CPL is not changed
    } else {
      // Non-conforming code: DPL must equal effective CPL, RPL must be <= effective CPL
      if (desc_dpl != eff_cpl) {
        raise_exception(13, selector & 0xFFFC);
        return;
      }
      // CPL does NOT change for direct JMP/CALL — only RETF/IRET/call gates change CPL
      // Adjust selector RPL to effective CPL
      selector = (selector & 0xFFFC) | eff_cpl;
    }
  } else if (seg_idx == seg_SS) {
    // Must be writable data segment
    if ((type & 0x08) || !(type & 0x02)) {
      raise_exception(13, selector & 0xFFFC);
      return;
    }
    // RPL must equal effective CPL, DPL must equal effective CPL
    if (rpl != eff_cpl || desc_dpl != eff_cpl) {
      raise_exception(13, selector & 0xFFFC);
      return;
    }
  } else {
    // DS, ES, FS, GS: must be data or readable code
    if ((type & 0x08) && !(type & 0x02)) {
      // Code segment that's not readable
      raise_exception(13, selector & 0xFFFC);
      return;
    }
    // For data segments and non-conforming code: RPL and CPL must both be <= DPL
    if (!(type & 0x04) || !(type & 0x08)) {
      // Not conforming code — check privilege
      if (rpl > desc_dpl || eff_cpl > desc_dpl) {
        raise_exception(13, selector & 0xFFFC);
        return;
      }
    }
    // Conforming code segments can be loaded into data segment registers without DPL check
  }

  // Mark accessed (GDT/LDT are linear addresses — use paging)
  if (!(desc[5] & 0x01)) {
    desc[5] |= 0x01;
    emu88_uint32 desc_addr = table_base + desc_offset;
    write_linear8(desc_addr + 5, desc[5]);
  }

  sregs[seg_idx] = selector;
  seg_cache[seg_idx] = cache;
}

//=============================================================================
// Effective address computation (protected mode aware)
//=============================================================================

emu88_uint32 emu88::effective_address(emu88_uint16 seg, emu88_uint32 off) const {
  if (!protected_mode()) {
    if (unreal_mode) {
      // PM→RM transition: segment caches still hold protected mode bases.
      // Use cached base until segments are explicitly reloaded.
      for (int i = 0; i < 6; i++) {
        if (sregs[i] == seg) {
          return seg_cache[i].base + off;
        }
      }
    }
    // Real mode: linear = segment * 16 + offset (20-bit)
    return ((emu88_uint32)seg << 4) + (off & 0xFFFF);
  }

  // V86 mode: use real mode addressing (seg << 4 + offset) even though PE is set
  if (v86_mode()) {
    return ((emu88_uint32)seg << 4) + (off & 0xFFFF);
  }

  // Protected mode: find the segment register with this selector value
  // and use its cached base address
  for (int i = 0; i < 6; i++) {
    if (sregs[i] == seg) {
      return seg_cache[i].base + off;
    }
  }

  // Fallback: shouldn't happen if segments are properly loaded
  return off;
}

//=============================================================================
// Segment limit checking (Task 3)
//=============================================================================

bool emu88::check_segment_limit(int seg_idx, emu88_uint32 offset, emu88_uint8 width) const {
  if (!protected_mode() || v86_mode())
    return true;  // No limit checking in real/V86 mode

  const SegDescCache &sc = seg_cache[seg_idx];
  if (!sc.valid)
    return false;

  emu88_uint8 type = sc.access & 0x0F;

  emu88_uint32 last_byte = offset + (emu88_uint32)(width - 1);
  bool wrapped = (last_byte < offset);  // overflow detection

  // Expand-down data segment: type bit 2 set, bit 3 clear (data segment)
  if (!(type & 0x08) && (type & 0x04)) {
    // Expand-down: valid range is (limit+1) to max
    emu88_uint32 max_limit = (sc.flags & 0x04) ? 0xFFFFFFFF : 0xFFFF;
    if (wrapped || offset <= sc.limit || last_byte > max_limit)
      return false;
  } else {
    // Normal segment: offset + width - 1 must be <= limit
    if (wrapped || last_byte > sc.limit)
      return false;
  }
  return true;
}

//=============================================================================
// I/O permission bitmap check (Task 7)
//=============================================================================

bool emu88::check_io_permission(emu88_uint16 port, emu88_uint8 width) {
  // Ring 0 always allowed
  if (cpl == 0 && !v86_mode())
    return true;

  // Check IOPL first — if CPL <= IOPL (and not V86), I/O is allowed
  if (!v86_mode() && cpl <= get_iopl())
    return true;

  // In V86 mode, or CPL > IOPL: check TSS I/O permission bitmap
  if (!tr_cache.valid)
    return false;

  // Read the I/O map base address from TSS (offset 0x66 in 32-bit TSS)
  emu88_uint32 tss_base = tr_cache.base;
  emu88_uint32 tss_limit = tr_cache.limit;

  // I/O map base is at offset 102 (0x66) in a 32-bit TSS
  if (tss_limit < 0x67)
    return false;

  // TSS base is a linear address — must go through paging
  emu88_uint16 io_map_base = read_linear16(tss_base + 0x66);

  // Check each byte the port spans
  for (emu88_uint8 i = 0; i < width; i++) {
    emu88_uint16 p = port + i;
    emu88_uint32 byte_offset = io_map_base + (p / 8);
    if (byte_offset > tss_limit)
      return false;
    emu88_uint8 bitmap_byte = read_linear8(tss_base + byte_offset);
    if (bitmap_byte & (1 << (p % 8)))
      return false;  // Bit set = port access denied
  }
  return true;
}

//=============================================================================
// Linear address translation (paging)
//=============================================================================

emu88_uint32 emu88::translate_linear(emu88_uint32 linear, bool write) {
  if (!paging_enabled())
    return linear;

#ifdef PAGING_DEBUG
  if (linear == 0x0049F000 && cpl == 3) {
    fprintf(stderr, "[PAGE] translate 0x%08X write=%d cpl=%d cr0=%08X cr3=%08X\n",
            linear, write, cpl, cr0, cr3);
  }
#endif

  // Two-level page translation:
  // CR3 → Page Directory (1024 entries × 4 bytes)
  // PDE → Page Table (1024 entries × 4 bytes)
  // PTE → Physical page (4KB)

  emu88_uint32 pde_index = (linear >> 22) & 0x3FF;
  emu88_uint32 pte_index = (linear >> 12) & 0x3FF;
  emu88_uint32 page_offset = linear & 0xFFF;

  // Read Page Directory Entry
  emu88_uint32 pde_addr = (cr3 & 0xFFFFF000) + pde_index * 4;
  emu88_uint32 pde = mem->fetch_mem32(pde_addr);

  // Check PDE present
  if (!(pde & 1)) {
    static int pf_pde_log = 0;
    if (pf_pde_log < 10) {
      pf_pde_log++;
      fprintf(stderr, "[PF-DIAG] #%d PDE miss: linear=%08X pde_idx=%d pde_addr=%08X pde=%08X cr3=%08X cpl=%d\n",
              pf_pde_log, linear, pde_index, pde_addr, pde, cr3, cpl);
      // Dump all 16 PDE entries for context
      fprintf(stderr, "[PF-DIAG] PDEs:");
      for (int i = 0; i < 16; i++) {
        fprintf(stderr, " [%d]=%08X", i, mem->fetch_mem32((cr3 & 0xFFFFF000) + i * 4));
      }
      fprintf(stderr, "\n");
    }
    cr2 = linear;
    // Error code: bit 0 = not present, bit 1 = write, bit 2 = user
    emu88_uint32 error = (write ? 0x02 : 0x00) | (cpl == 3 ? 0x04 : 0x00);
    raise_exception(14, error);  // #PF
    return 0;
  }

  // 4MB page (PSE, CR4 bit 4) — check for large page
  if ((cr4 & 0x10) && (pde & 0x80)) {
    // 4MB page: bits 31:22 from PDE, bits 21:0 from linear address
    emu88_uint32 phys = (pde & 0xFFC00000) | (linear & 0x003FFFFF);
    // Set accessed/dirty bits
    if (!(pde & 0x20) || (write && !(pde & 0x40))) {
      pde |= 0x20;  // accessed
      if (write) pde |= 0x40;  // dirty
      mem->store_mem32(pde_addr, pde);
    }
    return phys;
  }

  // Read Page Table Entry
  emu88_uint32 pt_base = pde & 0xFFFFF000;
  emu88_uint32 pte_addr = pt_base + pte_index * 4;
  emu88_uint32 pte = mem->fetch_mem32(pte_addr);

  // Check PTE present
  if (!(pte & 1)) {
    static int pf_pte_log = 0;
    if (pf_pte_log < 10) {
      pf_pte_log++;
      fprintf(stderr, "[PF-DIAG] #%d PTE miss: linear=%08X pde=%08X pt_base=%08X pte_idx=%d pte_addr=%08X pte=%08X\n",
              pf_pte_log, linear, pde, pt_base, pte_index, pte_addr, pte);
    }
    cr2 = linear;
    emu88_uint32 error = 0x00 | (write ? 0x02 : 0x00) | (cpl == 3 ? 0x04 : 0x00);
    raise_exception(14, error);
    return 0;
  }

  // Combined U/S and R/W from PDE and PTE (effective = AND of both levels)
  bool page_user = (pde & 0x04) && (pte & 0x04);
  bool page_rw   = (pde & 0x02) && (pte & 0x02);

  // User/supervisor check
  if (cpl == 3 && !page_user) {
    cr2 = linear;
    emu88_uint32 error = 0x01 | (write ? 0x02 : 0x00) | 0x04;
    raise_exception(14, error);
    return 0;
  }

  // Write protection check
  if (write) {
    bool wp = (cr0 & CR0_WP) != 0;
    if (cpl == 3 && !page_rw) {
      cr2 = linear;
      raise_exception(14, 0x01 | 0x02 | 0x04);
      return 0;
    }
    if (wp && !page_rw) {
      cr2 = linear;
      emu88_uint32 error = 0x01 | 0x02 | (cpl == 3 ? 0x04 : 0x00);
      raise_exception(14, error);
      return 0;
    }
  }

  // All checks passed — set PDE accessed bit
  if (!(pde & 0x20)) {
    pde |= 0x20;
    mem->store_mem32(pde_addr, pde);
  }

  // Set PTE accessed/dirty bits
  if (!(pte & 0x20) || (write && !(pte & 0x40))) {
    pte |= 0x20;
    if (write) pte |= 0x40;
    mem->store_mem32(pte_addr, pte);
  }

  return (pte & 0xFFFFF000) | page_offset;
}

//=============================================================================
// Linear address memory access
//=============================================================================

emu88_uint8 emu88::read_linear8(emu88_uint32 linear) {
  return mem->fetch_mem(translate_linear(linear, false));
}

emu88_uint16 emu88::read_linear16(emu88_uint32 linear) {
  return mem->fetch_mem16(translate_linear(linear, false));
}

emu88_uint32 emu88::read_linear32(emu88_uint32 linear) {
  return mem->fetch_mem32(translate_linear(linear, false));
}

void emu88::write_linear8(emu88_uint32 linear, emu88_uint8 val) {
  mem->store_mem(translate_linear(linear, true), val);
}

void emu88::write_linear16(emu88_uint32 linear, emu88_uint16 val) {
  mem->store_mem16(translate_linear(linear, true), val);
}

void emu88::write_linear32(emu88_uint32 linear, emu88_uint32 val) {
  mem->store_mem32(translate_linear(linear, true), val);
}

//=============================================================================
// Protected mode interrupt dispatch
//=============================================================================

void emu88::do_interrupt_pm(emu88_uint8 vector, bool has_error_code,
                            emu88_uint32 error_code, bool is_software_int) {
  // Trace all hardware exceptions in PM (vectors 0-31, not software INT)
  if (!is_software_int && vector < 32) {
    static int pm_exc_log = 0;
    if (pm_exc_log < 50) {
      pm_exc_log++;
      fprintf(stderr, "[PM-EXC] #%02X err=%08X at %04X:%08X CPL=%d CS_RPL=%d\n",
              vector, error_code, sregs[seg_CS], insn_ip, cpl, sregs[seg_CS] & 3);
      if (vector == 14)
        fprintf(stderr, "[PM-EXC] #PF CR2=%08X\n", cr2);
    }
  }
  // Detect CPL/CS.RPL mismatch
  if (cpl != (sregs[seg_CS] & 3) && !v86_mode()) {
    static int cpl_mismatch_log = 0;
    if (cpl_mismatch_log < 5) {
      cpl_mismatch_log++;
      fprintf(stderr, "[CPL-BUG] cpl=%d CS=%04X(RPL=%d) at %04X:%08X vec=%02X\n",
              cpl, sregs[seg_CS], sregs[seg_CS] & 3, sregs[seg_CS], insn_ip, vector);
    }
  }
  // Trace DPMI function calls (INT 31h) in PM
  if (is_software_int && vector == 0x31) {
    uint16_t ax = regs[reg_AX];
    // Log set-interrupt-vector calls for INT 21h debugging
    if (ax == 0x0205) {
      fprintf(stderr, "[DPMI-0205] Set PM INT vector %02Xh -> %04X:%08X from %04X:%08X\n",
              regs[reg_BX] & 0xFF, regs[reg_CX], get_reg32(reg_DX), sregs[seg_CS], insn_ip);
    }
    // Track first-time calls from each unique code location
    {
      struct CallSite { uint16_t cs; uint32_t eip; uint16_t ax; };
      static CallSite seen_sites[512];
      static int seen_count = 0;
      bool first_from_here = true;
      uint32_t call_eip = insn_ip;
      for (int i = 0; i < seen_count; i++) {
        if (seen_sites[i].cs == sregs[seg_CS] && seen_sites[i].eip == call_eip && seen_sites[i].ax == ax) {
          first_from_here = false;
          break;
        }
      }
      if (first_from_here && seen_count < 512) {
        seen_sites[seen_count++] = { sregs[seg_CS], call_eip, ax };
      }
      // Log every INT 31h call with full register state
      fprintf(stderr, "[DPMI-31] AX=%04X BX=%04X CX=%04X DX=%04X SI=%04X DI=%04X ES=%04X DS=%04X"
              " EAX=%08X EBX=%08X ECX=%08X EDX=%08X ESI=%08X EDI=%08X"
              " from %04X:%08X%s\n",
              ax, regs[reg_BX], regs[reg_CX], regs[reg_DX],
              regs[reg_SI], regs[reg_DI], sregs[seg_ES], sregs[seg_DS],
              get_reg32(reg_AX), get_reg32(reg_BX), get_reg32(reg_CX), get_reg32(reg_DX),
              get_reg32(reg_SI), get_reg32(reg_DI),
              sregs[seg_CS], insn_ip,
              first_from_here ? " [FIRST]" : "");
    }
    // Set up memory watchpoint on first DPMI call with DS=00A7
    static bool watchpoint_set = false;
    if (!watchpoint_set && sregs[seg_DS] == 0x00A7) {
      // Use seg_cache base directly (already resolved by CPU)
      uint32_t base = seg_cache[seg_DS].base;
      uint32_t watch_linear = base + 0x002E;
      // For low memory (< 1MB), physical = linear (identity mapped)
      fprintf(stderr, "[WATCH-SETUP] sel=00A7 base=%08X watch_addr=%08X\n", base, watch_linear);
      fprintf(stderr, "[WATCH-SETUP] Current value at [002E] = 0x%02X\n",
              mem->fetch_mem(watch_linear));
      // Dump DOS/16M data segment variables for range check at 218C
      uint32_t b = base;
      fprintf(stderr, "[WATCH-SETUP] [0098:009A]=%04X:%04X [009C:009E]=%04X:%04X\n",
              mem->fetch_mem16(b + 0x9A), mem->fetch_mem16(b + 0x98),
              mem->fetch_mem16(b + 0x9E), mem->fetch_mem16(b + 0x9C));
      fprintf(stderr, "[WATCH-SETUP] [002F]=%02X [0022:0024]=%04X:%04X\n",
              mem->fetch_mem(b + 0x2F),
              mem->fetch_mem16(b + 0x24), mem->fetch_mem16(b + 0x22));
      // Check CS:[54E0] for INT 21h handler — selector 00C7 base
      uint32_t c7_base = seg_cache[seg_CS].base;  // might not be 00C7 at this point
      // Look up 00C7 descriptor manually from LDT
      fprintf(stderr, "[WATCH-SETUP] 00C7 base (from seg_cache if CS=00C7): CS=%04X base=%08X\n",
              sregs[seg_CS], c7_base);
      // Try to read 00C7:[54E0] if we can find its base
      // We know from traces that 00C7 base = 0x3A240
      uint32_t handler_seg_sel = mem->fetch_mem16(0x3A240 + 0x54E0);
      fprintf(stderr, "[WATCH-SETUP] 00C7:[54E0] = %04X (selector for INT 21h handler ES)\n",
              handler_seg_sel);
      fprintf(stderr, "[WATCH-SETUP] [099E] in 00A7 = %04X\n",
              mem->fetch_mem16(b + 0x099E));
      // Dump read buffer area at base+0x1394 (first 32 bytes)
      fprintf(stderr, "[WATCH-SETUP] Read buffer [1394..13B3]:");
      for (int i = 0; i < 32; i++) {
        if (i % 16 == 0) fprintf(stderr, "\n  +%04X:", 0x1394 + i);
        fprintf(stderr, " %02X", mem->fetch_mem(b + 0x1394 + i));
      }
      fprintf(stderr, "\n");
      // Dump LDT selector bases to understand memory layout
      {
        uint32_t ldt_base = ldtr_cache.base;
        uint32_t ldt_limit = ldtr_cache.limit;
        fprintf(stderr, "[WATCH-SETUP] LDT base=%08X limit=%04X\n", ldt_base, ldt_limit);
        for (uint32_t idx = 0; idx * 8 <= ldt_limit && idx < 50; idx++) {
          uint32_t desc_addr = ldt_base + idx * 8;
          uint8_t b0 = mem->fetch_mem(desc_addr + 0);
          uint8_t b1 = mem->fetch_mem(desc_addr + 1);
          uint8_t b2 = mem->fetch_mem(desc_addr + 2);
          uint8_t b3 = mem->fetch_mem(desc_addr + 3);
          uint8_t b4 = mem->fetch_mem(desc_addr + 4);
          uint8_t b5 = mem->fetch_mem(desc_addr + 5);
          uint8_t b6 = mem->fetch_mem(desc_addr + 6);
          uint8_t b7 = mem->fetch_mem(desc_addr + 7);
          uint32_t sel_base = b2 | (b3 << 8) | (b4 << 16) | (b7 << 24);
          uint32_t sel_limit = b0 | (b1 << 8) | ((b6 & 0x0F) << 16);
          if (b6 & 0x80) sel_limit = (sel_limit << 12) | 0xFFF;  // granularity
          uint16_t sel = (idx << 3) | 0x04 | 3;  // LDT, RPL=3
          if (b5 & 0x80) {  // present
            fprintf(stderr, "[LDT] sel=%04X base=%08X limit=%08X access=%02X flags=%02X %s\n",
                    sel, sel_base, sel_limit, b5, b6,
                    (b5 & 0x08) ? "CODE" : "DATA");
          }
        }
      }
      mem->watchpoint_addr = watch_linear;
      watchpoint_set = true;
      // PM trace disabled for now
      // gp_trace_count = 3000;
    }

    // Dump state before 0500h/0501h to check if parsing happened
    if (ax == 0x0500 && sregs[seg_DS] == 0x00A7) {
      uint32_t b = seg_cache[seg_DS].base;
      fprintf(stderr, "[PRE-0500] [002E]=%02X [0098]=%08X [009C]=%08X\n",
              mem->fetch_mem(b + 0x2E),
              mem->fetch_mem32(b + 0x98), mem->fetch_mem32(b + 0x9C));
      fprintf(stderr, "[PRE-0500] Buf[1394..13B3]:");
      for (int i = 0; i < 32; i++) {
        if (i % 16 == 0) fprintf(stderr, "\n  +%04X:", 0x1394 + i);
        fprintf(stderr, " %02X", mem->fetch_mem(b + 0x1394 + i));
      }
      fprintf(stderr, "\n");
    }

    // Set up post-call trace for DPMI 0500h/0501h
    if (ax == 0x0500 || ax == 0x0501) {
      dpmi_trace_func = ax;
      dpmi_trace_ret_cs = sregs[seg_CS];
      dpmi_trace_ret_eip = ip;  // ip already points past INT 31h (CD 31)
      if (ax == 0x0500) {
        dpmi_trace_es_base = seg_cache[seg_ES].base;
        dpmi_trace_edi = get_reg32(reg_DI);
      }
    }
    // For real-mode calls (0300/0301/0302), dump the call structure
    if (ax == 0x0300 || ax == 0x0301 || ax == 0x0302) {
      static int rm_call_log = 0;
      if (rm_call_log < 50) {
        rm_call_log++;
        // Read call structure from ES:EDI
        uint32_t struct_base = seg_cache[seg_ES].base + get_reg32(reg_DI);
        uint32_t rm_eax = read_linear32(struct_base + 0x1C);
        uint32_t rm_ebx = read_linear32(struct_base + 0x10);
        uint32_t rm_ecx = read_linear32(struct_base + 0x18);
        uint32_t rm_edx = read_linear32(struct_base + 0x14);
        uint16_t rm_ds  = read_linear16(struct_base + 0x24);
        uint16_t rm_es  = read_linear16(struct_base + 0x22);
        uint16_t rm_cs  = read_linear16(struct_base + 0x2C);
        uint16_t rm_ip  = read_linear16(struct_base + 0x2A);
        uint16_t rm_ss  = read_linear16(struct_base + 0x30);
        uint16_t rm_sp  = read_linear16(struct_base + 0x2E);
        fprintf(stderr, "[RM-CALL] func=%04X: AX=%08X BX=%08X CX=%08X DX=%08X DS=%04X ES=%04X CS:IP=%04X:%04X SS:SP=%04X:%04X\n",
                ax, rm_eax, rm_ebx, rm_ecx, rm_edx, rm_ds, rm_es, rm_cs, rm_ip, rm_ss, rm_sp);
        // Trace after file read returns (AH=3Fh on handle 5)
        uint8_t rm_ah = (rm_eax >> 8) & 0xFF;
        uint16_t rm_bx16 = rm_ebx & 0xFFFF;
        if (rm_ah == 0x3F && rm_bx16 == 5) {
          // Trigger instruction trace after this 0302h returns
          dpmi_trace_func = 0x0302;
          dpmi_trace_ret_cs = sregs[seg_CS];
          dpmi_trace_ret_eip = ip;  // ip after INT 31h (CD 31)
        }
        // If this is a WRITE to stderr/stdout (INT 21h AH=40h, BX=1 or 2), dump the buffer
        if (rm_ah == 0x40 && (rm_bx16 == 1 || rm_bx16 == 2)) {
          uint16_t rm_cx16 = rm_ecx & 0xFFFF;
          uint16_t rm_dx16 = rm_edx & 0xFFFF;
          uint32_t buf_addr = ((uint32_t)rm_ds << 4) + rm_dx16;
          fprintf(stderr, "[RM-CALL] WRITE buf @ %04X:%04X (phys %08X) len=%d: \"",
                  rm_ds, rm_dx16, buf_addr, rm_cx16);
          for (uint16_t i = 0; i < rm_cx16 && i < 80; i++) {
            uint8_t ch = mem->fetch_mem(buf_addr + i);
            fputc((ch >= 0x20 && ch < 0x7F) ? ch : '.', stderr);
          }
          fprintf(stderr, "\"\n");
        }
      }
    }
  }
  // Log hardware IRQ delivery in PM
  if (!is_software_int && vector >= 0x78 && vector <= 0x7F) {
    static int hw_irq_pm_log = 0;
    if (hw_irq_pm_log < 10) {
      hw_irq_pm_log++;
      fprintf(stderr, "[HW-IRQ-PM] vec=0x%02X (IRQ%d) CS:IP=%04X:%08X IDT_base=%08X IDT_limit=%04X\n",
              vector, vector - 0x78, sregs[seg_CS], ip, idtr_base, idtr_limit);
    }
  }

  // DPMI intercept — handle before IDT lookup
  if (intercept_pm_int(vector, is_software_int, has_error_code, error_code)) {
    if (exc_dispatch_trace) {
      fprintf(stderr, "[EXC-ESP] intercept_pm_int returned true for #%02X: ESP=%08X CS:IP=%04X:%08X\n",
              vector, get_esp(), sregs[seg_CS], ip);
    }
    return;
  }

  // Read IDT entry (8 bytes per entry)
  emu88_uint32 idt_offset = (emu88_uint32)vector * 8;
  if (idt_offset + 7 > (emu88_uint32)idtr_limit) {
    static int idt_gp_log = 0;
    if (idt_gp_log < 5) {
      idt_gp_log++;
      fprintf(stderr, "[IDT-GP] vector=%02X idt_offset=%04X > idtr_limit=%04X — #GP!\n",
              vector, idt_offset + 7, idtr_limit);
    }
    raise_exception(13, idt_offset + 2);  // #GP
    return;
  }

  emu88_uint32 idt_addr = idtr_base + idt_offset;
  emu88_uint8 idt_entry[8];
  // IDT base is a linear address — must go through paging
  for (int i = 0; i < 8; i++)
    idt_entry[i] = read_linear8(idt_addr + i);

  emu88_uint16 gate_offset_lo = idt_entry[0] | ((emu88_uint16)idt_entry[1] << 8);
  emu88_uint16 gate_selector  = idt_entry[2] | ((emu88_uint16)idt_entry[3] << 8);
  emu88_uint8  gate_type      = idt_entry[5];
  emu88_uint16 gate_offset_hi = idt_entry[6] | ((emu88_uint16)idt_entry[7] << 8);

  // Check present
  if (!(gate_type & 0x80)) {
    raise_exception(11, idt_offset + 2);  // #NP
    return;
  }

  // For software interrupts (INT n, INT 3, INTO), gate DPL must be >= CPL
  if (is_software_int) {
    emu88_uint8 gate_dpl = (gate_type >> 5) & 3;
    if (gate_dpl < cpl) {
      raise_exception(13, idt_offset + 2);  // #GP(vector*8+2)
      return;
    }
  }

  // Gate type determines 16-bit vs 32-bit and interrupt vs trap
  emu88_uint8 type_nibble = gate_type & 0x0F;
  bool is_32bit = (type_nibble == 0x0E || type_nibble == 0x0F);
  bool is_interrupt_gate = (type_nibble == 0x06 || type_nibble == 0x0E);
  // 0x05 = task gate (not implemented), 0x07/0x0F = trap gate

  if (type_nibble == 0x05) {
    // Task gate — perform hardware task switch
    // gate_selector contains the TSS selector
    task_switch(gate_selector, false, false, has_error_code, error_code);
    return;
  }

  emu88_uint32 new_eip = gate_offset_lo | ((emu88_uint32)gate_offset_hi << 16);

  // Load target code segment descriptor
  emu88_uint16 target_sel = gate_selector;
  if ((target_sel & 0xFFFC) == 0) {
    raise_exception(13, 0);
    return;
  }

  emu88_uint16 index = target_sel >> 3;
  bool use_ldt = (target_sel & 4) != 0;
  emu88_uint32 table_base = use_ldt ? ldtr_cache.base : gdtr_base;
  emu88_uint32 table_limit = use_ldt ? ldtr_cache.limit : (emu88_uint32)gdtr_limit;

  if ((emu88_uint32)index * 8 + 7 > table_limit) {
    raise_exception(13, target_sel & 0xFFFC);
    return;
  }

  emu88_uint8 desc[8];
  read_descriptor(table_base, index, desc);
  SegDescCache new_cs_cache;
  parse_descriptor(desc, new_cs_cache);

  if (!new_cs_cache.valid) {
    raise_exception(11, target_sel & 0xFFFC);
    return;
  }

  // Check for privilege level change (inter-privilege interrupt)
  emu88_uint8 target_dpl = (new_cs_cache.access >> 5) & 3;
  bool conforming = ((new_cs_cache.access & 0x0C) == 0x0C);

  // V86 mode: handler must be non-conforming code with DPL=0
  bool from_v86 = v86_mode();
  if (from_v86 && (conforming || target_dpl != 0)) {
    raise_exception(13, target_sel & 0xFFFC);
    return;
  }

  // Target code segment must be at same or higher privilege (DPL <= CPL)
  if (!from_v86 && !conforming && target_dpl > cpl) {
    raise_exception(13, target_sel & 0xFFFC);
    return;
  }

  emu88_uint8 new_cpl;
  if (conforming) {
    new_cpl = cpl;  // Conforming code: CPL stays unchanged
  } else {
    new_cpl = target_dpl;  // Non-conforming: CPL = DPL
  }

  bool privilege_change = from_v86 || (new_cpl != cpl);

  if (privilege_change) {
    // Get new SS:ESP from TSS
    if (!tr_cache.valid) {
      raise_exception(10, tr & 0xFFFC);  // #TS
      return;
    }

    // Read SS:ESP for new privilege level from TSS (linear address)
    // 32-bit TSS: ESP0 at offset 4, SS0 at offset 8, etc.
    emu88_uint32 tss_base = tr_cache.base;
    emu88_uint32 new_esp = read_linear32(tss_base + 4 + new_cpl * 8);
    emu88_uint16 new_ss = read_linear16(tss_base + 8 + new_cpl * 8);

    // Save old SS:ESP and segment registers
    emu88_uint32 old_esp = get_esp();
    emu88_uint16 old_ss = sregs[seg_SS];
    emu88_uint32 old_eflags = get_eflags();
    emu88_uint16 old_cs = sregs[seg_CS];
    emu88_uint32 old_eip = ip;

    // Save V86 segment registers before switching stack
    emu88_uint16 old_gs = sregs[seg_GS];
    emu88_uint16 old_fs = sregs[seg_FS];
    emu88_uint16 old_ds = sregs[seg_DS];
    emu88_uint16 old_es = sregs[seg_ES];

    // Clear VM flag before loading new SS (so load_segment uses pmode path)
    if (from_v86) {
      eflags_hi &= ~0x0002;  // Clear VM (bit 17 → eflags_hi bit 1)
    }

    // Load new SS:ESP directly (avoid privilege checks during transition)
    sregs[seg_SS] = new_ss;
    {
      // Parse SS descriptor for the new ring
      emu88_uint16 ss_index = new_ss >> 3;
      bool ss_use_ldt = (new_ss & 4) != 0;
      emu88_uint32 ss_tbase = ss_use_ldt ? ldtr_cache.base : gdtr_base;
      emu88_uint8 ss_desc[8];
      read_descriptor(ss_tbase, ss_index, ss_desc);
      parse_descriptor(ss_desc, seg_cache[seg_SS]);
    }
    set_esp(new_esp);
    cpl = new_cpl;

    if (from_v86) {
      // V86 mode: push GS, FS, DS, ES, then SS:ESP, EFLAGS, CS:EIP
      if (is_32bit) {
        push_dword(old_gs);
        push_dword(old_fs);
        push_dword(old_ds);
        push_dword(old_es);
        push_dword(old_ss);
        push_dword(old_esp);
        push_dword(old_eflags);
        push_dword(old_cs);
        push_dword(old_eip);
      } else {
        push_word(old_gs);
        push_word(old_fs);
        push_word(old_ds);
        push_word(old_es);
        push_word(old_ss);
        push_word(old_esp & 0xFFFF);
        push_word(old_eflags & 0xFFFF);
        push_word(old_cs);
        push_word(old_eip & 0xFFFF);
      }
      // Zero out the data segment registers
      sregs[seg_GS] = 0; seg_cache[seg_GS].valid = false;
      sregs[seg_FS] = 0; seg_cache[seg_FS].valid = false;
      sregs[seg_DS] = 0; seg_cache[seg_DS].valid = false;
      sregs[seg_ES] = 0; seg_cache[seg_ES].valid = false;
    } else {
      // Normal privilege change: push old SS:ESP
      if (is_32bit) {
        push_dword(old_ss);
        push_dword(old_esp);
      } else {
        push_word(old_ss);
        push_word(old_esp & 0xFFFF);
      }
      // Push EFLAGS, CS, EIP
      if (is_32bit) {
        push_dword(old_eflags);
        push_dword(old_cs);
        push_dword(old_eip);
      } else {
        push_word(old_eflags & 0xFFFF);
        push_word(old_cs);
        push_word(old_eip & 0xFFFF);
      }
    }
  } else {
    // Same privilege: push EFLAGS, CS, EIP
    if (is_32bit) {
      push_dword(get_eflags());
      push_dword(sregs[seg_CS]);
      push_dword(ip);
    } else {
      push_word(flags);
      push_word(sregs[seg_CS]);
      push_word(ip & 0xFFFF);
    }
  }

  // Push error code if applicable
  if (has_error_code) {
    if (is_32bit) {
      push_dword(error_code);
    } else {
      push_word(error_code & 0xFFFF);
    }
  }

  // Clear IF for interrupt gates (not trap gates)
  if (is_interrupt_gate) {
    clear_flag(FLAG_IF);
  }
  clear_flag(FLAG_TF);
  // Clear NT and VM
  flags &= ~0x4000;      // NT
  eflags_hi &= ~0x0002;  // VM (bit 17 → eflags_hi bit 1)

  // Load new CS:EIP (RPL of CS is always set to new CPL)
  sregs[seg_CS] = (target_sel & 0xFFFC) | new_cpl;
  seg_cache[seg_CS] = new_cs_cache;
  cpl = new_cpl;
  ip = is_32bit ? new_eip : (new_eip & 0xFFFF);

  // Trace #GP/#PF handler dispatch
  if (vector == 0x0D || vector == 0x0E) {
    static int gp_trace = 0;
    if (gp_trace < 0) {  // disabled — using post-call trace instead
      gp_trace++;
      gp_trace_count = 300;  // trace handler flow
      fprintf(stderr, "[EXC-TRACE] Dispatch vec=%02X #%d: handler at %04X:%08X CPL=%d CR2=%08X\n",
              vector, gp_trace, sregs[seg_CS], ip, cpl, cr2);
      // Dump handler code bytes
      uint32_t handler_lin = seg_cache[seg_CS].base + ip;
      fprintf(stderr, "[GP-TRACE] Handler code (lin=%08X): ", handler_lin);
      for (int i = 0; i < 32; i++)
        fprintf(stderr, "%02X ", read_linear8(handler_lin + i));
      fprintf(stderr, "\n");
      // Dump the stack frame we just built
      uint32_t esp_now = get_esp();
      fprintf(stderr, "[GP-TRACE] Ring 0 stack at SS:ESP = %04X:%08X\n",
              sregs[seg_SS], esp_now);
      fprintf(stderr, "[GP-TRACE] Stack frame: ");
      for (int i = 0; i < 28; i += 4) {
        uint32_t val = read_linear32(seg_cache[seg_SS].base + esp_now + i);
        fprintf(stderr, "[ESP+%02X]=%08X ", i, val);
      }
      fprintf(stderr, "\n");
      // Dump DS, ES, FS, GS state
      fprintf(stderr, "[GP-TRACE] DS=%04X ES=%04X FS=%04X GS=%04X SS=%04X\n",
              sregs[seg_DS], sregs[seg_ES], sregs[seg_FS], sregs[seg_GS], sregs[seg_SS]);
      // Dump LDT info
      fprintf(stderr, "[GP-TRACE] LDTR=%04X base=%08X limit=%08X\n",
              ldtr, ldtr_cache.base, ldtr_cache.limit);
    }
  }
}

//=============================================================================
// Invalidate DS/ES/FS/GS after privilege level change (IRET/RETF to outer ring)
// On 386+, if a data segment register has DPL < new CPL, it is set to null.
//=============================================================================
void emu88::invalidate_segments_for_cpl() {
  static const int data_segs[] = { seg_DS, seg_ES, seg_FS, seg_GS };
  for (int s : data_segs) {
    if ((sregs[s] & 0xFFFC) == 0) continue;  // Already null
    emu88_uint8 dpl = (seg_cache[s].access >> 5) & 3;
    bool is_conforming_code = (seg_cache[s].access & 0x18) == 0x18 &&
                              (seg_cache[s].access & 0x04);
    // Null if non-conforming and DPL < CPL, or if RPL > DPL
    if (!is_conforming_code && dpl < cpl) {
      sregs[s] = 0;
      seg_cache[s].valid = false;
      seg_cache[s].base = 0;
      seg_cache[s].limit = 0;
      seg_cache[s].access = 0;
      seg_cache[s].flags = 0;
    }
  }
}

//=============================================================================
// Far CALL/JMP with call gate support (Task 1)
//=============================================================================

void emu88::far_call_or_jmp(emu88_uint16 selector, emu88_uint32 offset, bool is_call) {
  if (!protected_mode() || v86_mode()) {
    // Real mode or V86 mode: direct far transfer
    if (is_call) {
      if (op_size_32) {
        push_dword((emu88_uint32)sregs[seg_CS]);
        push_dword(ip);
      } else {
        push_word(sregs[seg_CS]);
        push_word(ip & 0xFFFF);
      }
    }
    load_segment(seg_CS, selector);
    ip = op_size_32 ? offset : (offset & 0xFFFF);
    return;
  }

  // Protected mode: check if selector references a call gate or a code segment
  if ((selector & 0xFFFC) == 0) {
    raise_exception(13, 0);  // #GP(0) for null selector
    return;
  }

  emu88_uint16 index = selector >> 3;
  bool use_ldt = (selector & 4) != 0;
  emu88_uint32 table_base = use_ldt ? ldtr_cache.base : gdtr_base;
  emu88_uint32 table_limit = use_ldt ? ldtr_cache.limit : (emu88_uint32)gdtr_limit;

  if ((emu88_uint32)index * 8 + 7 > table_limit) {
    raise_exception(13, selector & 0xFFFC);
    return;
  }

  emu88_uint8 desc[8];
  read_descriptor(table_base, index, desc);

  emu88_uint8 access = desc[5];
  bool is_system = !(access & 0x10);  // S bit clear = system descriptor

  if (is_system) {
    // Check if it's a call gate
    emu88_uint8 sys_type = access & 0x0F;

    if (sys_type == 0x04 || sys_type == 0x0C) {
      // 0x04 = 16-bit call gate, 0x0C = 32-bit call gate
      bool gate_32 = (sys_type == 0x0C);

      // Check present
      if (!(access & 0x80)) {
        raise_exception(11, selector & 0xFFFC);  // #NP
        return;
      }

      // Gate descriptor layout:
      // bytes 0-1: offset low 16 bits
      // bytes 2-3: target CS selector
      // byte 4: param count (low 5 bits)
      // byte 5: access (type, DPL, P)
      // bytes 6-7: offset high 16 bits
      emu88_uint16 gate_off_lo = desc[0] | ((emu88_uint16)desc[1] << 8);
      emu88_uint16 gate_cs_sel = desc[2] | ((emu88_uint16)desc[3] << 8);
      emu88_uint8  param_count = desc[4] & 0x1F;
      emu88_uint16 gate_off_hi = desc[6] | ((emu88_uint16)desc[7] << 8);
      emu88_uint32 gate_offset = gate_off_lo | ((emu88_uint32)gate_off_hi << 16);

      // Privilege check: gate DPL must be >= CPL and >= RPL
      emu88_uint8 gate_dpl = (access >> 5) & 3;
      emu88_uint8 rpl = selector & 3;
      if (cpl > gate_dpl || rpl > gate_dpl) {
        raise_exception(13, selector & 0xFFFC);
        return;
      }

      // Load target CS descriptor
      if ((gate_cs_sel & 0xFFFC) == 0) {
        raise_exception(13, 0);
        return;
      }

      emu88_uint16 cs_index = gate_cs_sel >> 3;
      bool cs_use_ldt = (gate_cs_sel & 4) != 0;
      emu88_uint32 cs_tbase = cs_use_ldt ? ldtr_cache.base : gdtr_base;
      emu88_uint32 cs_tlimit = cs_use_ldt ? ldtr_cache.limit : (emu88_uint32)gdtr_limit;

      if ((emu88_uint32)cs_index * 8 + 7 > cs_tlimit) {
        raise_exception(13, gate_cs_sel & 0xFFFC);
        return;
      }

      emu88_uint8 cs_desc[8];
      read_descriptor(cs_tbase, cs_index, cs_desc);
      SegDescCache new_cs_cache;
      parse_descriptor(cs_desc, new_cs_cache);

      if (!new_cs_cache.valid) {
        raise_exception(11, gate_cs_sel & 0xFFFC);
        return;
      }

      // Must be a code segment
      if (!(new_cs_cache.access & 0x10) || !((new_cs_cache.access & 0x08))) {
        raise_exception(13, gate_cs_sel & 0xFFFC);
        return;
      }

      emu88_uint8 cs_dpl = (new_cs_cache.access >> 5) & 3;
      bool conforming = (new_cs_cache.access & 0x04) != 0;

      if (is_call && !conforming && cs_dpl < cpl) {
        // CALL through gate with privilege change
        // Get new SS:ESP from TSS
        if (!tr_cache.valid) {
          raise_exception(10, tr & 0xFFFC);
          return;
        }
        emu88_uint32 tss_base = tr_cache.base;
        emu88_uint32 new_esp = read_linear32(tss_base + 4 + cs_dpl * 8);
        emu88_uint16 new_ss = read_linear16(tss_base + 8 + cs_dpl * 8);

        // Save old SS:ESP
        emu88_uint32 old_esp = get_esp();
        emu88_uint16 old_ss = sregs[seg_SS];
        emu88_uint16 old_cs = sregs[seg_CS];
        emu88_uint32 old_eip = ip;

        // Copy parameters from old stack before switching
        emu88_uint32 params[32];
        for (emu88_uint8 p = 0; p < param_count; p++) {
          if (gate_32) {
            params[p] = fetch_dword(sregs[seg_SS],
              stack_32() ? (old_esp + p * 4) : ((emu88_uint16)(regs[reg_SP] + p * 4)));
          } else {
            params[p] = fetch_word(sregs[seg_SS],
              stack_32() ? (old_esp + p * 2) : ((emu88_uint16)(regs[reg_SP] + p * 2)));
          }
        }

        // Load new SS:ESP
        cpl = cs_dpl;
        // Load SS directly to avoid privilege check issues during transition
        sregs[seg_SS] = new_ss;
        {
          emu88_uint16 ss_idx = new_ss >> 3;
          bool ss_ldt = (new_ss & 4) != 0;
          emu88_uint32 ss_tb = ss_ldt ? ldtr_cache.base : gdtr_base;
          emu88_uint8 ss_d[8];
          read_descriptor(ss_tb, ss_idx, ss_d);
          parse_descriptor(ss_d, seg_cache[seg_SS]);
        }
        set_esp(new_esp);

        // Push old SS:ESP
        if (gate_32) {
          push_dword(old_ss);
          push_dword(old_esp);
        } else {
          push_word(old_ss);
          push_word(old_esp & 0xFFFF);
        }

        // Push parameters (in reverse order, from bottom of old stack)
        for (int p = param_count - 1; p >= 0; p--) {
          if (gate_32) push_dword(params[p]);
          else push_word(params[p] & 0xFFFF);
        }

        // Push old CS:EIP
        if (gate_32) {
          push_dword(old_cs);
          push_dword(old_eip);
        } else {
          push_word(old_cs);
          push_word(old_eip & 0xFFFF);
        }

        // Load new CS:EIP
        // Mark accessed
        if (!(cs_desc[5] & 0x01)) {
          cs_desc[5] |= 0x01;
          write_linear8(cs_tbase + (emu88_uint32)cs_index * 8 + 5, cs_desc[5]);
        }
        sregs[seg_CS] = (gate_cs_sel & 0xFFFC) | cs_dpl;
        seg_cache[seg_CS] = new_cs_cache;
        ip = gate_32 ? gate_offset : (gate_offset & 0xFFFF);
      } else {
        // Same privilege (or JMP, or conforming code)
        if (is_call) {
          if (gate_32) {
            push_dword((emu88_uint32)sregs[seg_CS]);
            push_dword(ip);
          } else {
            push_word(sregs[seg_CS]);
            push_word(ip & 0xFFFF);
          }
        }

        // Mark accessed
        if (!(cs_desc[5] & 0x01)) {
          cs_desc[5] |= 0x01;
          write_linear8(cs_tbase + (emu88_uint32)cs_index * 8 + 5, cs_desc[5]);
        }
        sregs[seg_CS] = gate_cs_sel;
        seg_cache[seg_CS] = new_cs_cache;
        if (!conforming) cpl = cs_dpl;
        ip = gate_32 ? gate_offset : (gate_offset & 0xFFFF);
      }
      return;
    }

    // TSS descriptor → hardware task switch
    if (sys_type == 0x09 || sys_type == 0x01) {
      // 0x09 = 32-bit TSS Available, 0x01 = 16-bit TSS Available
      if (!(access & 0x80)) {
        raise_exception(11, selector & 0xFFFC);  // #NP
        return;
      }
      task_switch(selector, is_call, false);
      return;
    }

    // Task gate → read TSS selector from gate, then task switch
    if (sys_type == 0x05) {
      if (!(access & 0x80)) {
        raise_exception(11, selector & 0xFFFC);  // #NP
        return;
      }
      emu88_uint16 tss_sel = desc[2] | ((emu88_uint16)desc[3] << 8);
      task_switch(tss_sel, is_call, false);
      return;
    }

    // Other system descriptors — #GP
    raise_exception(13, selector & 0xFFFC);
    return;
  }

  // Normal code/data segment — direct far transfer
  if (is_call) {
    if (op_size_32) {
      push_dword((emu88_uint32)sregs[seg_CS]);
      push_dword(ip);
    } else {
      push_word(sregs[seg_CS]);
      push_word(ip & 0xFFFF);
    }
  }
  load_segment(seg_CS, selector);
  if (exception_pending) return;
  ip = op_size_32 ? offset : (offset & 0xFFFF);
}

//=============================================================================
// Hardware task switching (32-bit TSS only)
//=============================================================================

void emu88::task_switch(emu88_uint16 new_tss_sel, bool is_call, bool is_iret,
                        bool has_error_code, emu88_uint32 error_code) {
  // Look up new TSS descriptor
  emu88_uint16 new_index = new_tss_sel >> 3;
  bool use_ldt = (new_tss_sel & 4) != 0;
  emu88_uint32 table_base = use_ldt ? ldtr_cache.base : gdtr_base;
  emu88_uint32 table_limit = use_ldt ? ldtr_cache.limit : (emu88_uint32)gdtr_limit;

  if ((emu88_uint32)new_index * 8 + 7 > table_limit) {
    raise_exception(10, new_tss_sel & 0xFFFC);  // #TS
    return;
  }

  emu88_uint8 new_desc[8];
  read_descriptor(table_base, new_index, new_desc);

  emu88_uint8 new_access = new_desc[5];
  emu88_uint8 new_type = new_access & 0x0F;

  // Must be a 32-bit TSS (type 9=available or B=busy)
  if (new_type != 0x09 && new_type != 0x0B) {
    raise_exception(13, new_tss_sel & 0xFFFC);
    return;
  }
  if (!(new_access & 0x80)) {
    raise_exception(11, new_tss_sel & 0xFFFC);  // #NP
    return;
  }

  // For JMP/INT, new TSS must be Available (type 9); for IRET, must be Busy (type B)
  if (!is_iret && new_type == 0x0B) {
    raise_exception(13, new_tss_sel & 0xFFFC);
    return;
  }
  if (is_iret && new_type != 0x0B) {
    raise_exception(10, new_tss_sel & 0xFFFC);
    return;
  }

  SegDescCache new_tss_cache;
  parse_descriptor(new_desc, new_tss_cache);
  emu88_uint32 new_tss_base = new_tss_cache.base;

  // New TSS must be at least 0x68 bytes (minimum 32-bit TSS)
  if (new_tss_cache.limit < 0x67) {
    raise_exception(10, new_tss_sel & 0xFFFC);
    return;
  }

  // --- Save current task state to current TSS ---
  emu88_uint32 old_tss_base = tr_cache.base;

  // Save general registers
  write_linear32(old_tss_base + 0x20, ip);
  write_linear32(old_tss_base + 0x24, get_eflags());
  write_linear32(old_tss_base + 0x28, get_reg32(reg_AX));
  write_linear32(old_tss_base + 0x2C, get_reg32(reg_CX));
  write_linear32(old_tss_base + 0x30, get_reg32(reg_DX));
  write_linear32(old_tss_base + 0x34, get_reg32(reg_BX));
  write_linear32(old_tss_base + 0x38, get_reg32(reg_SP));
  write_linear32(old_tss_base + 0x3C, get_reg32(reg_BP));
  write_linear32(old_tss_base + 0x40, get_reg32(reg_SI));
  write_linear32(old_tss_base + 0x44, get_reg32(reg_DI));
  write_linear16(old_tss_base + 0x48, sregs[seg_ES]);
  write_linear16(old_tss_base + 0x4C, sregs[seg_CS]);
  write_linear16(old_tss_base + 0x50, sregs[seg_SS]);
  write_linear16(old_tss_base + 0x54, sregs[seg_DS]);
  write_linear16(old_tss_base + 0x58, sregs[seg_FS]);
  write_linear16(old_tss_base + 0x5C, sregs[seg_GS]);

  // --- Update old TSS descriptor ---
  if (!is_call) {
    // JMP or IRET: mark old TSS as Available (clear busy bit)
    emu88_uint32 old_desc_addr = table_base + (emu88_uint32)(tr >> 3) * 8;
    emu88_uint8 old_access = read_linear8(old_desc_addr + 5);
    old_access &= ~0x02;  // Clear busy bit (type B → 9)
    write_linear8(old_desc_addr + 5, old_access);
  }
  // For CALL: old TSS stays Busy

  // --- Mark new TSS as Busy ---
  {
    emu88_uint32 new_desc_addr = table_base + (emu88_uint32)new_index * 8;
    emu88_uint8 na = read_linear8(new_desc_addr + 5);
    na |= 0x02;  // Set busy bit (type 9 → B)
    write_linear8(new_desc_addr + 5, na);
  }

  // --- Load new task state from new TSS ---
  emu88_uint32 new_cr3    = read_linear32(new_tss_base + 0x1C);
  emu88_uint32 new_eip    = read_linear32(new_tss_base + 0x20);
  emu88_uint32 new_eflags = read_linear32(new_tss_base + 0x24);
  emu88_uint32 new_eax    = read_linear32(new_tss_base + 0x28);
  emu88_uint32 new_ecx    = read_linear32(new_tss_base + 0x2C);
  emu88_uint32 new_edx    = read_linear32(new_tss_base + 0x30);
  emu88_uint32 new_ebx    = read_linear32(new_tss_base + 0x34);
  emu88_uint32 new_esp    = read_linear32(new_tss_base + 0x38);
  emu88_uint32 new_ebp    = read_linear32(new_tss_base + 0x3C);
  emu88_uint32 new_esi    = read_linear32(new_tss_base + 0x40);
  emu88_uint32 new_edi    = read_linear32(new_tss_base + 0x44);
  emu88_uint16 new_es     = read_linear16(new_tss_base + 0x48);
  emu88_uint16 new_cs     = read_linear16(new_tss_base + 0x4C);
  emu88_uint16 new_ss     = read_linear16(new_tss_base + 0x50);
  emu88_uint16 new_ds     = read_linear16(new_tss_base + 0x54);
  emu88_uint16 new_fs     = read_linear16(new_tss_base + 0x58);
  emu88_uint16 new_gs     = read_linear16(new_tss_base + 0x5C);
  emu88_uint16 new_ldt    = read_linear16(new_tss_base + 0x60);

  // Save old TR for backlink before updating
  emu88_uint16 old_tr = tr;

  // Switch CR3 (page directory base) — do this BEFORE loading segments
  // so segment descriptor reads use the new address space
  cr3 = new_cr3;

  // Update Task Register
  tr = new_tss_sel;
  tr_cache = new_tss_cache;
  tr_cache.base = new_tss_base;

  // Load LDT from new task
  if (new_ldt & 0xFFFC) {
    // Load LDT descriptor
    emu88_uint16 ldt_index = new_ldt >> 3;
    if ((emu88_uint32)ldt_index * 8 + 7 <= table_limit) {
      emu88_uint8 ldt_desc[8];
      read_descriptor(table_base, ldt_index, ldt_desc);
      parse_descriptor(ldt_desc, ldtr_cache);
      ldtr = new_ldt;
    }
  } else {
    ldtr = 0;
    ldtr_cache = {};
  }

  // Load registers — must use set_eflags to update both flags and eflags_hi
  set_eflags(new_eflags);
  if (is_call) flags |= EFLAG_NT;   // Set Nested Task flag
  if (is_iret) flags &= ~EFLAG_NT;  // Clear NT on IRET

  set_reg32(reg_AX, new_eax);
  set_reg32(reg_CX, new_ecx);
  set_reg32(reg_DX, new_edx);
  set_reg32(reg_BX, new_ebx);
  set_reg32(reg_SP, new_esp);
  set_reg32(reg_BP, new_ebp);
  set_reg32(reg_SI, new_esi);
  set_reg32(reg_DI, new_edi);

  // Load segment registers — use direct assignment + cache load
  // (not load_segment which does privilege checks inappropriate during task switch)
  auto load_task_seg = [&](int seg_idx, emu88_uint16 sel) {
    sregs[seg_idx] = sel;
    if ((sel & 0xFFFC) == 0) {
      seg_cache[seg_idx] = {};
      return;
    }
    emu88_uint16 idx = sel >> 3;
    bool ldt = (sel & 4) != 0;
    emu88_uint32 tbase = ldt ? ldtr_cache.base : gdtr_base;
    emu88_uint32 tlimit = ldt ? ldtr_cache.limit : (emu88_uint32)gdtr_limit;
    if ((emu88_uint32)idx * 8 + 7 > tlimit) {
      seg_cache[seg_idx] = {};
      return;
    }
    emu88_uint8 d[8];
    read_descriptor(tbase, idx, d);
    parse_descriptor(d, seg_cache[seg_idx]);
  };

  load_task_seg(seg_CS, new_cs);
  load_task_seg(seg_SS, new_ss);
  load_task_seg(seg_DS, new_ds);
  load_task_seg(seg_ES, new_es);
  load_task_seg(seg_FS, new_fs);
  load_task_seg(seg_GS, new_gs);

  // CPL = RPL of new CS
  cpl = new_cs & 3;

  ip = new_eip;

  // For CALL: set backlink in new TSS to old TSS selector
  if (is_call) {
    write_linear16(new_tss_base + 0x00, old_tr);  // backlink
  }

  // Set CR0.TS (Task Switched) — real hardware does this on every task switch;
  // CWSDPMI/DOS4GW rely on the resulting #NM for lazy FPU context switching
  cr0 |= CR0_TS;

  // If exception with error code, push it onto new task's stack
  if (has_error_code) {
    push_dword(error_code);
  }
}

//=============================================================================
// Exception handling
//=============================================================================

void emu88::raise_exception(emu88_uint8 vector, emu88_uint32 error_code) {
  // Fault exceptions push the faulting instruction's IP
  ip = insn_ip;
  exception_pending = true;

  static int exc_dump_count = 0;
  static int exc_log_count[256] = {};
  exc_log_count[vector]++;
  if (exc_log_count[vector] <= 3) {
    fprintf(stderr, "[EXC] #%02X err=%08X at %04X:%08X %s%s\n",
            vector, error_code, sregs[seg_CS], ip,
            in_exception ? "(during exception) " : "",
            in_double_fault ? "(double fault!) " : "");
    if (vector == 14) {
      fprintf(stderr, "[EXC] CR2=%08X (page fault linear addr)  CR3=%08X\n", cr2, cr3);
    }
  } else if (exc_log_count[vector] == 4) {
    fprintf(stderr, "[EXC] #%02X — further logging suppressed\n", vector);
  }

  // Full register dump for #GP (vector 13) — always log
  if (vector == 13) {
    fprintf(stderr, "[EXC-GP] === #GP FULL REGISTER DUMP ===\n");
    fprintf(stderr, "[EXC-GP] EAX=%08X ECX=%08X EDX=%08X EBX=%08X\n",
            get_reg32(reg_AX), get_reg32(reg_CX), get_reg32(reg_DX), get_reg32(reg_BX));
    fprintf(stderr, "[EXC-GP] ESP=%08X EBP=%08X ESI=%08X EDI=%08X\n",
            get_reg32(reg_SP), get_reg32(reg_BP), get_reg32(reg_SI), get_reg32(reg_DI));
    fprintf(stderr, "[EXC-GP] CS=%04X (base=%08X limit=%08X acc=%02X flg=%02X)\n",
            sregs[seg_CS], seg_cache[seg_CS].base, seg_cache[seg_CS].limit,
            seg_cache[seg_CS].access, seg_cache[seg_CS].flags);
    fprintf(stderr, "[EXC-GP] DS=%04X (base=%08X limit=%08X acc=%02X flg=%02X)\n",
            sregs[seg_DS], seg_cache[seg_DS].base, seg_cache[seg_DS].limit,
            seg_cache[seg_DS].access, seg_cache[seg_DS].flags);
    fprintf(stderr, "[EXC-GP] ES=%04X (base=%08X limit=%08X acc=%02X flg=%02X)\n",
            sregs[seg_ES], seg_cache[seg_ES].base, seg_cache[seg_ES].limit,
            seg_cache[seg_ES].access, seg_cache[seg_ES].flags);
    fprintf(stderr, "[EXC-GP] SS=%04X (base=%08X limit=%08X acc=%02X flg=%02X)\n",
            sregs[seg_SS], seg_cache[seg_SS].base, seg_cache[seg_SS].limit,
            seg_cache[seg_SS].access, seg_cache[seg_SS].flags);
    fprintf(stderr, "[EXC-GP] FS=%04X (base=%08X limit=%08X acc=%02X flg=%02X)\n",
            sregs[seg_FS], seg_cache[seg_FS].base, seg_cache[seg_FS].limit,
            seg_cache[seg_FS].access, seg_cache[seg_FS].flags);
    fprintf(stderr, "[EXC-GP] GS=%04X (base=%08X limit=%08X acc=%02X flg=%02X)\n",
            sregs[seg_GS], seg_cache[seg_GS].base, seg_cache[seg_GS].limit,
            seg_cache[seg_GS].access, seg_cache[seg_GS].flags);
    fprintf(stderr, "[EXC-GP] EFLAGS=%08X CR0=%08X CR3=%08X CPL=%d IOPL=%d\n",
            get_eflags(), cr0, cr3, cpl, get_iopl());
    fprintf(stderr, "[EXC-GP] GDTR base=%08X limit=%04X  IDTR base=%08X limit=%04X\n",
            gdtr_base, gdtr_limit, idtr_base, idtr_limit);
    fprintf(stderr, "[EXC-GP] LDTR=%04X (base=%08X limit=%08X)  TR=%04X (base=%08X limit=%08X)\n",
            ldtr, ldtr_cache.base, ldtr_cache.limit, tr, tr_cache.base, tr_cache.limit);
    fprintf(stderr, "[EXC-GP] Error code=%08X (", error_code);
    if (error_code == 0) {
      fprintf(stderr, "null selector or non-selector fault");
    } else {
      fprintf(stderr, "sel=%04X %s %s",
              error_code & 0xFFF8,
              (error_code & 0x02) ? "IDT" : ((error_code & 0x04) ? "LDT" : "GDT"),
              (error_code & 0x01) ? "EXT" : "");
    }
    fprintf(stderr, ")\n");
    // Dump instruction bytes at faulting address
    {
      uint32_t lin = protected_mode() ? (seg_cache[seg_CS].base + ip)
                                      : ((uint32_t)sregs[seg_CS] << 4) + ip;
      uint32_t code_phys = lin;
      if (paging_enabled()) {
        uint32_t pde_i = (lin >> 22) & 0x3FF;
        uint32_t pte_i = (lin >> 12) & 0x3FF;
        uint32_t pde = mem->fetch_mem32((cr3 & 0xFFFFF000) + pde_i * 4);
        if (pde & 1) {
          uint32_t pte = mem->fetch_mem32((pde & 0xFFFFF000) + pte_i * 4);
          if (pte & 1) code_phys = (pte & 0xFFFFF000) | (lin & 0xFFF);
        }
      }
      fprintf(stderr, "[EXC-GP] Instruction at %04X:%08X (lin=%08X phys=%08X):",
              sregs[seg_CS], ip, lin, code_phys);
      for (int i = 0; i < 20; i++)
        fprintf(stderr, " %02X", mem->fetch_mem(code_phys + i));
      fprintf(stderr, "\n");
    }
    // Dump the stack around ESP (16 dwords before and after)
    {
      uint32_t esp = get_esp();
      uint32_t ss_base = seg_cache[seg_SS].base;
      fprintf(stderr, "[EXC-GP] Stack at SS:%08X (lin=%08X):", esp, ss_base + esp);
      for (int i = 0; i < 16; i++) {
        uint32_t addr = ss_base + esp + i * 4;
        uint32_t phys = addr;
        if (paging_enabled()) {
          uint32_t pde_i = (addr >> 22) & 0x3FF;
          uint32_t pte_i = (addr >> 12) & 0x3FF;
          uint32_t pde = mem->fetch_mem32((cr3 & 0xFFFFF000) + pde_i * 4);
          if (pde & 1) {
            uint32_t pte = mem->fetch_mem32((pde & 0xFFFFF000) + pte_i * 4);
            if (pte & 1) phys = (pte & 0xFFFFF000) | (addr & 0xFFF);
          }
        }
        if (i % 4 == 0) fprintf(stderr, "\n  ESP+%02X:", i * 4);
        fprintf(stderr, " %08X", mem->fetch_mem32(phys));
      }
      fprintf(stderr, "\n");
    }
  }

  // Dump GDT/IDT state on first few exceptions (use raw mem access to avoid recursive faults)
  if (exc_dump_count < 1) {
    exc_dump_count++;
    fprintf(stderr, "[EXC] GDTR base=%08X limit=%04X  IDTR base=%08X limit=%04X  CR0=%08X CR3=%08X\n",
            gdtr_base, gdtr_limit, idtr_base, idtr_limit, cr0, cr3);
    uint16_t sel = error_code & 0xFFF8;
    uint16_t idx = sel >> 3;
    fprintf(stderr, "[EXC] Faulting selector=%04X (GDT index %d)\n", sel, idx);

    // Translate GDTR base through paging manually (no exceptions)
    uint32_t gdt_phys = gdtr_base;
    if (paging_enabled()) {
      uint32_t pde_i = (gdtr_base >> 22) & 0x3FF;
      uint32_t pte_i = (gdtr_base >> 12) & 0x3FF;
      uint32_t pde = mem->fetch_mem32((cr3 & 0xFFFFF000) + pde_i * 4);
      if (pde & 1) {
        uint32_t pte = mem->fetch_mem32((pde & 0xFFFFF000) + pte_i * 4);
        if (pte & 1) gdt_phys = (pte & 0xFFFFF000) | (gdtr_base & 0xFFF);
        else fprintf(stderr, "[EXC] GDTR PTE not present!\n");
      } else fprintf(stderr, "[EXC] GDTR PDE not present!\n");
    }
    fprintf(stderr, "[EXC] GDTR phys=%08X\n", gdt_phys);

    // Dump first 16 GDT entries using physical addresses
    for (int i = 0; i < 16 && (uint32_t)i * 8 + 7 <= (uint32_t)gdtr_limit; i++) {
      uint8_t d[8];
      uint32_t a = gdt_phys + i * 8;
      for (int j = 0; j < 8; j++) d[j] = mem->fetch_mem(a + j);
      uint32_t base = d[2] | (d[3] << 8) | (d[4] << 16) | (d[7] << 24);
      uint32_t limit = d[0] | (d[1] << 8) | ((d[6] & 0x0F) << 16);
      if (d[6] & 0x80) limit = (limit << 12) | 0xFFF;
      fprintf(stderr, "[GDT] %2d: sel=%04X base=%08X limit=%08X access=%02X flags=%02X %s\n",
              i, i * 8, base, limit, d[5], d[6],
              (d[5] & 0x80) ? "P" : "NP");
    }

    // Dump instruction bytes at fault (translate CS:IP manually)
    uint32_t lin = protected_mode() ? (seg_cache[seg_CS].base + ip)
                                    : ((uint32_t)sregs[seg_CS] << 4) + ip;
    uint32_t code_phys = lin;
    if (paging_enabled()) {
      uint32_t pde_i = (lin >> 22) & 0x3FF;
      uint32_t pte_i = (lin >> 12) & 0x3FF;
      uint32_t pde = mem->fetch_mem32((cr3 & 0xFFFFF000) + pde_i * 4);
      if (pde & 1) {
        uint32_t pte = mem->fetch_mem32((pde & 0xFFFFF000) + pte_i * 4);
        if (pte & 1) code_phys = (pte & 0xFFFFF000) | (lin & 0xFFF);
      }
    }
    fprintf(stderr, "[EXC] Code at %04X:%08X (lin=%08X phys=%08X): ", sregs[seg_CS], ip, lin, code_phys);
    for (int i = 0; i < 16; i++)
      fprintf(stderr, "%02X ", mem->fetch_mem(code_phys + i));
    fprintf(stderr, "\n");

    // Dump IDT gate entry for this vector
    if (protected_mode()) {
      uint32_t idt_off = (uint32_t)vector * 8;
      if (idt_off + 7 <= (uint32_t)idtr_limit) {
        // Translate IDT base through paging manually
        uint32_t idt_phys = idtr_base;
        if (paging_enabled()) {
          uint32_t pde_i = (idtr_base >> 22) & 0x3FF;
          uint32_t pte_i = (idtr_base >> 12) & 0x3FF;
          uint32_t pde = mem->fetch_mem32((cr3 & 0xFFFFF000) + pde_i * 4);
          if (pde & 1) {
            uint32_t pte = mem->fetch_mem32((pde & 0xFFFFF000) + pte_i * 4);
            if (pte & 1) idt_phys = (pte & 0xFFFFF000) | (idtr_base & 0xFFF);
          }
        }
        uint32_t gate_addr = idt_phys + idt_off;
        uint8_t g[8];
        for (int i = 0; i < 8; i++) g[i] = mem->fetch_mem(gate_addr + i);
        uint16_t g_off_lo = g[0] | (g[1] << 8);
        uint16_t g_sel = g[2] | (g[3] << 8);
        uint8_t  g_type = g[5];
        uint16_t g_off_hi = g[6] | (g[7] << 8);
        uint32_t g_offset = g_off_lo | ((uint32_t)g_off_hi << 16);
        fprintf(stderr, "[EXC] IDT[%02X]: sel=%04X off=%08X type=%02X (gate=%s DPL=%d %s)\n",
                vector, g_sel, g_offset, g_type,
                (g_type & 0x0F) == 0x0E ? "INT32" :
                (g_type & 0x0F) == 0x0F ? "TRAP32" :
                (g_type & 0x0F) == 0x06 ? "INT16" :
                (g_type & 0x0F) == 0x05 ? "TASK" : "???",
                (g_type >> 5) & 3,
                (g_type & 0x80) ? "P" : "NP");
        fprintf(stderr, "[EXC] CPL=%d IOPL=%d EFLAGS=%08X\n", cpl, get_iopl(), get_eflags());
      }
    }
  }

  // Detect infinite exception loops (same vector at same CS:IP)
  {
    static uint8_t  last_vec_e = 0xFF;
    static uint16_t last_cs_e  = 0;
    static uint32_t last_ip_e  = 0;
    static int      loop_cnt_e = 0;
    if (vector == last_vec_e && sregs[seg_CS] == last_cs_e && ip == last_ip_e) {
      loop_cnt_e++;
      if (loop_cnt_e >= 1000) {
        fprintf(stderr, "[EXC] Infinite exception loop detected: #%02X at %04X:%08X "
                "(1000 iterations) — halting CPU\n", vector, sregs[seg_CS], ip);
        halted = true;
        exception_pending = false;
        return;
      }
    } else {
      last_vec_e = vector;
      last_cs_e  = sregs[seg_CS];
      last_ip_e  = ip;
      loop_cnt_e = 1;
    }
  }

  if (in_exception) {
    // Exception during double fault dispatch → triple fault
    if (in_double_fault) {
      triple_fault();
      return;
    }
    // First exception during exception dispatch → double fault (#DF)
    fprintf(stderr, "[EXC] Escalating to #DF (double fault)\n");
    in_double_fault = true;
    if (protected_mode()) {
      do_interrupt_pm(8, true, 0);  // #DF with error code 0
    } else {
      do_interrupt(8);
    }
    in_double_fault = false;
    in_exception = false;
    return;
  }

  in_exception = true;

  bool has_error = (vector == 8 || vector == 10 || vector == 11 ||
                    vector == 12 || vector == 13 || vector == 14 ||
                    vector == 17);

  if (protected_mode()) {
    uint32_t esp_before = get_esp();
    do_interrupt_pm(vector, has_error, has_error ? error_code : 0);
    if (get_esp() != esp_before) {
      // do_interrupt_pm modified ESP (maybe through DPMI dispatch)
      // Check if it's still what DPMI set or was modified by IDT push
      static int esp_change_log = 0;
      if (esp_change_log < 10) {
        esp_change_log++;
        fprintf(stderr, "[EXC-ESP] After do_interrupt_pm #%02X: ESP %08X -> %08X CS:IP=%04X:%08X\n",
                vector, esp_before, get_esp(), sregs[seg_CS], ip);
      }
    }
  } else {
    do_interrupt(vector);
  }

  in_exception = false;
}

void emu88::raise_exception_no_error(emu88_uint8 vector) {
  // Fault exceptions push the faulting instruction's IP
  ip = insn_ip;
  exception_pending = true;

  static int exc_ne_log_count[256] = {};
  exc_ne_log_count[vector]++;
  bool should_log = (exc_ne_log_count[vector] <= 50);

  if (should_log) {
    fprintf(stderr, "[EXC] #%02X (no error) at %04X:%08X %s%s\n",
            vector, sregs[seg_CS], ip,
            in_exception ? "(during exception) " : "",
            in_double_fault ? "(double fault!) " : "");
  } else if (exc_ne_log_count[vector] == 51) {
    fprintf(stderr, "[EXC] #%02X — further logging suppressed (50 already)\n", vector);
  }

  // Dump instruction bytes and registers for diagnostics (first few of each type)
  if (should_log && (vector == 1 || vector == 5 || vector == 6)) {
    // In real mode, use seg*16 for linear address (not cached base)
    uint32_t lin = protected_mode() ? (seg_cache[seg_CS].base + ip)
                                    : ((uint32_t)sregs[seg_CS] << 4) + ip;
    uint32_t code_phys = lin;
    if (paging_enabled()) {
      uint32_t pde_i = (lin >> 22) & 0x3FF;
      uint32_t pte_i = (lin >> 12) & 0x3FF;
      uint32_t pde = mem->fetch_mem32((cr3 & 0xFFFFF000) + pde_i * 4);
      if (pde & 1) {
        uint32_t pte = mem->fetch_mem32((pde & 0xFFFFF000) + pte_i * 4);
        if (pte & 1) code_phys = (pte & 0xFFFFF000) | (lin & 0xFFF);
      }
    }
    fprintf(stderr, "[EXC] Code at %04X:%08X (lin=%08X phys=%08X): ",
            sregs[seg_CS], ip, lin, code_phys);
    for (int i = 0; i < 16; i++)
      fprintf(stderr, "%02X ", mem->fetch_mem(code_phys + i));
    fprintf(stderr, "\n");
    fprintf(stderr, "[EXC] EAX=%08X ECX=%08X EDX=%08X EBX=%08X ESP=%08X EBP=%08X ESI=%08X EDI=%08X CR0=%08X\n",
            get_reg32(reg_AX), get_reg32(reg_CX), get_reg32(reg_DX), get_reg32(reg_BX),
            get_reg32(reg_SP), get_reg32(reg_BP), get_reg32(reg_SI), get_reg32(reg_DI), cr0);
    fprintf(stderr, "[EXC] DS=%04X ES=%04X SS=%04X FS=%04X GS=%04X\n",
            sregs[seg_DS], sregs[seg_ES], sregs[seg_SS], sregs[seg_FS], sregs[seg_GS]);
    // For real mode exceptions, dump IVT handler address
    if (!protected_mode()) {
      uint32_t ivt_addr = (uint32_t)vector * 4;
      uint16_t handler_off = mem->fetch_mem16(ivt_addr);
      uint16_t handler_seg = mem->fetch_mem16(ivt_addr + 2);
      uint32_t handler_lin = ((uint32_t)handler_seg << 4) + handler_off;
      fprintf(stderr, "[EXC] IVT[%02X]=%04X:%04X (lin=%05X) handler bytes: ",
              vector, handler_seg, handler_off, handler_lin);
      for (int i = 0; i < 8; i++)
        fprintf(stderr, "%02X ", mem->fetch_mem(handler_lin + i));
      fprintf(stderr, "\n");
    }
  }

  // Detect infinite exception loops (same vector at same CS:IP)
  {
    static uint8_t  last_vec = 0xFF;
    static uint16_t last_cs  = 0;
    static uint32_t last_ip  = 0;
    static int      loop_cnt = 0;
    if (vector == last_vec && sregs[seg_CS] == last_cs && ip == last_ip) {
      loop_cnt++;
      if (loop_cnt >= 1000) {
        fprintf(stderr, "[EXC] Infinite exception loop detected: #%02X at %04X:%08X "
                "(1000 iterations) — halting CPU\n", vector, sregs[seg_CS], ip);
        halted = true;
        exception_pending = false;
        return;
      }
    } else {
      last_vec = vector;
      last_cs  = sregs[seg_CS];
      last_ip  = ip;
      loop_cnt = 1;
    }
  }

  if (in_exception) {
    if (in_double_fault) {
      triple_fault();
      return;
    }
    fprintf(stderr, "[EXC] Escalating to #DF (double fault)\n");
    in_double_fault = true;
    if (protected_mode()) {
      do_interrupt_pm(8, true, 0);  // #DF
    } else {
      do_interrupt(8);
    }
    in_double_fault = false;
    in_exception = false;
    return;
  }
  in_exception = true;
  if (protected_mode()) {
    do_interrupt_pm(vector, false, 0);
  } else {
    do_interrupt(vector);
  }
  in_exception = false;
}

void emu88::triple_fault(void) {
  fprintf(stderr, "[EXC] TRIPLE FAULT at %04X:%08X "
          "EAX=%08X EBX=%08X ECX=%08X EDX=%08X "
          "ESI=%08X EDI=%08X EBP=%08X ESP=%08X "
          "DS=%04X ES=%04X FS=%04X GS=%04X SS=%04X CR0=%08X\n",
          sregs[seg_CS], ip,
          get_reg32(reg_AX), get_reg32(reg_BX),
          get_reg32(reg_CX), get_reg32(reg_DX),
          get_reg32(reg_SI), get_reg32(reg_DI),
          get_reg32(reg_BP), get_reg32(reg_SP),
          sregs[seg_DS], sregs[seg_ES], sregs[seg_FS], sregs[seg_GS],
          sregs[seg_SS], cr0);
  emu88_fatal("Triple fault at %04X:%08X - resetting CPU", sregs[seg_CS], ip);
  reset();
}
