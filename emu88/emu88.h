#ifndef EMU88_H
#define EMU88_H

#include "emu88_mem.h"
#include "emu88_trace.h"
#include <cstdio>

class emu88 {
public:
  // 8088 register indices for general-purpose registers
  enum RegIndex16 {
    reg_AX = 0, reg_CX = 1, reg_DX = 2, reg_BX = 3,
    reg_SP = 4, reg_BP = 5, reg_SI = 6, reg_DI = 7
  };

  // 8-bit register indices (as encoded in mod/rm)
  enum RegIndex8 {
    reg_AL = 0, reg_CL = 1, reg_DL = 2, reg_BL = 3,
    reg_AH = 4, reg_CH = 5, reg_DH = 6, reg_BH = 7
  };

  // Segment register indices
  enum SegIndex {
    seg_ES = 0, seg_CS = 1, seg_SS = 2, seg_DS = 3,
    seg_FS = 4, seg_GS = 5
  };

  // FLAGS bit positions
  enum FlagBits {
    FLAG_CF = 0x0001,   // Carry
    FLAG_PF = 0x0004,   // Parity
    FLAG_AF = 0x0010,   // Auxiliary carry
    FLAG_ZF = 0x0040,   // Zero
    FLAG_SF = 0x0080,   // Sign
    FLAG_TF = 0x0100,   // Trap
    FLAG_IF = 0x0200,   // Interrupt enable
    FLAG_DF = 0x0400,   // Direction
    FLAG_OF = 0x0800    // Overflow
  };

  // EFLAGS bit positions (386+)
  enum EFlagBits {
    EFLAG_IOPL_MASK = 0x3000,  // I/O privilege level (bits 12-13)
    EFLAG_NT   = 0x4000,       // Nested task
    EFLAG_RF   = 0x10000,      // Resume flag
    EFLAG_VM   = 0x20000,      // Virtual 8086 mode
    EFLAG_AC   = 0x40000,      // Alignment check
    EFLAG_VIF  = 0x80000,      // Virtual interrupt flag
    EFLAG_VIP  = 0x100000,     // Virtual interrupt pending
    EFLAG_ID   = 0x200000      // ID flag (CPUID support)
  };

  // Segment descriptor cache (hidden part of segment registers)
  struct SegDescCache {
    emu88_uint32 base;      // 32-bit base address
    emu88_uint32 limit;     // Effective limit (granularity-adjusted)
    emu88_uint8 access;     // Access byte: P(7) DPL(6:5) S(4) Type(3:0)
    emu88_uint8 flags;      // Flags: G(3) D/B(2) L(1) AVL(0)
    bool valid;             // Descriptor loaded and present
  };

  // CR0 bit definitions
  enum CR0Bits {
    CR0_PE = 0x00000001,  // Protection Enable
    CR0_MP = 0x00000002,  // Monitor Coprocessor
    CR0_EM = 0x00000004,  // Emulation
    CR0_TS = 0x00000008,  // Task Switched
    CR0_ET = 0x00000010,  // Extension Type
    CR0_NE = 0x00000020,  // Numeric Error
    CR0_WP = 0x00010000,  // Write Protect
    CR0_AM = 0x00040000,  // Alignment Mask
    CR0_NW = 0x20000000,  // Not Write-through
    CR0_CD = 0x40000000,  // Cache Disable
    CR0_PG = 0x80000000   // Paging
  };

  // General-purpose registers (AX, CX, DX, BX, SP, BP, SI, DI)
  emu88_uint16 regs[8];

  // Upper 16 bits of 32-bit registers (386+: EAX=regs_hi[0]:regs[0], etc.)
  emu88_uint16 regs_hi[8];

  // Segment registers (ES, CS, SS, DS, FS, GS)
  emu88_uint16 sregs[6];

  // Segment descriptor caches (hidden part of each segment register)
  SegDescCache seg_cache[6];

  // Instruction pointer (32-bit for protected mode)
  emu88_uint32 ip;
  emu88_uint32 insn_ip;  // Start IP of current instruction (for fault exceptions)

  // Flags register
  emu88_uint16 flags;

  // Upper 16 bits of EFLAGS (386+)
  emu88_uint16 eflags_hi;

  // System registers (286+/386+)
  emu88_uint32 gdtr_base;
  emu88_uint16 gdtr_limit;
  emu88_uint32 idtr_base;
  emu88_uint16 idtr_limit;

  // Control registers
  emu88_uint32 cr0;  // Machine Status Word (MSW) is low 16 bits
  emu88_uint32 cr2;  // Page fault linear address
  emu88_uint32 cr3;  // Page directory base register (PDBR)
  emu88_uint32 cr4;  // Control extensions

  // Debug registers (DR0-DR3: breakpoint addresses, DR6: status, DR7: control)
  emu88_uint32 dr[8];

  // LDT register
  emu88_uint16 ldtr;         // LDT selector
  SegDescCache ldtr_cache;   // LDT descriptor cache

  // Task register
  emu88_uint16 tr;           // Task register selector
  SegDescCache tr_cache;     // TSS descriptor cache

  // Current privilege level
  emu88_uint8 cpl;

  // Memory and trace
  emu88_mem *mem;
  emu88_trace *trace;
  bool debug;

  // Cycle counter
  unsigned long long cycles;

  // Interrupt state
  bool int_pending;
  emu88_uint8 int_vector;
  bool halted;

  // Exception state (to detect double/triple faults)
  bool in_exception;
  bool in_double_fault;
  bool exception_pending;  // Set by raise_exception to abort current instruction
  bool dpmi_exc_dispatched; // Set by DPMI dispatch — inhibits ESP rollback in insn handlers
  bool exc_dispatch_trace; // Debug: trace ESP after DPMI exception dispatch
  bool unreal_mode;        // Set when CR0.PE transitions 1→0; cleared on far JMP/segment reload
  int gp_trace_count;      // Instructions to trace after #GP dispatch (debug)
  int rm_trace_count;      // Instructions to trace in real mode (debug)

  // DPMI post-call trace
  uint16_t dpmi_trace_func;     // 0 = none, 0x500/0x501 = pending trace
  uint16_t dpmi_trace_ret_cs;   // return CS to match
  uint32_t dpmi_trace_ret_eip;  // return EIP to match
  uint32_t dpmi_trace_es_base;  // ES base at time of call (for 0500h buffer dump)
  uint32_t dpmi_trace_edi;      // EDI at time of call (for 0500h buffer dump)

  // INT 2Fh AX=1687h post-return trace
  bool int2f_1687_trace_pending;  // true = waiting for return from INT 2Fh 1687h
  uint16_t int2f_trace_ret_cs;    // return CS to match
  uint32_t int2f_trace_ret_ip;    // return IP to match

  // Segment override state (per-instruction)
  int seg_override;       // -1 = none, 0-5 = seg index

  // REP prefix state
  enum RepType { REP_NONE, REP_REPZ, REP_REPNZ };
  RepType rep_prefix;

  // CPU behavior flags
  bool lock_ud;           // true = LOCK on invalid opcode causes #UD (386+)
  enum CpuType { CPU_8088, CPU_186, CPU_286, CPU_386 };
  CpuType cpu_type;       // Selects instruction set and quirks

  // 386 operand/address size prefix state (per-instruction)
  bool op_size_32;        // true when 32-bit operands active
  bool addr_size_32;      // true when 32-bit addressing active

  // Parity lookup table
  emu88_uint8 parity_table[256];

  // x87 FPU state
  struct FPUState {
    double regs[8];       // ST(0)-ST(7) as host doubles
    uint8_t tags[8];      // Tag per register: 0=valid, 1=zero, 2=special, 3=empty
    uint16_t cw;          // Control word
    uint16_t sw;          // Status word
  } fpu;

  // FPU methods
  void fpu_init();
  void execute_fpu(emu88_uint8 opcode);

  // FPU memory access helpers
  double fpu_read_m32real(uint16_t seg, uint32_t off);
  double fpu_read_m64real(uint16_t seg, uint32_t off);
  double fpu_read_m80real(uint16_t seg, uint32_t off);
  void fpu_write_m32real(uint16_t seg, uint32_t off, double val);
  void fpu_write_m64real(uint16_t seg, uint32_t off, double val);
  void fpu_write_m80real(uint16_t seg, uint32_t off, double val);
  double fpu_round(double val);
  void fpu_compare(double a, double b);

  // Constructor/destructor
  emu88(emu88_mem *memory);
  virtual ~emu88() = default;

  // I/O port operations - override in subclass
  virtual void port_out(emu88_uint16 port, emu88_uint8 value);
  virtual emu88_uint8 port_in(emu88_uint16 port);
  virtual void port_out16(emu88_uint16 port, emu88_uint16 value);
  virtual emu88_uint16 port_in16(emu88_uint16 port);

  // Interrupt support
  virtual void do_interrupt(emu88_uint8 vector);
  void request_int(emu88_uint8 vector);
  bool check_interrupts(void);
  virtual void halt_cpu(void);
  virtual void unimplemented_opcode(emu88_uint8 opcode);

  // DPMI intercept — called before IDT lookup in do_interrupt_pm.
  // Return true if handled (skip IDT dispatch).
  virtual bool intercept_pm_int(emu88_uint8 vector, bool is_software_int,
                                bool has_error_code, emu88_uint32 error_code) {
    (void)vector; (void)is_software_int; (void)has_error_code; (void)error_code;
    return false;
  }

  // Protected mode interrupt dispatch
  void do_interrupt_pm(emu88_uint8 vector, bool has_error_code = false,
                       emu88_uint32 error_code = 0, bool is_software_int = false);

  // Hardware task switching
  void task_switch(emu88_uint16 new_tss_sel, bool is_call, bool is_iret,
                   bool has_error_code = false, emu88_uint32 error_code = 0);

  // Exception handling
  void raise_exception(emu88_uint8 vector, emu88_uint32 error_code = 0);
  void raise_exception_no_error(emu88_uint8 vector);
  void triple_fault(void);

  // Configuration
  virtual void set_debug(bool new_debug) { debug = new_debug; }
  virtual void set_trace(emu88_trace *new_trace) { trace = new_trace; }

  // Register access: 8-bit
  emu88_uint8 get_reg8(emu88_uint8 r) const;
  void set_reg8(emu88_uint8 r, emu88_uint8 val);

  // Register access: 16-bit
  emu88_uint16 get_reg16(emu88_uint8 r) const { return regs[r]; }
  void set_reg16(emu88_uint8 r, emu88_uint16 val) { regs[r] = val; }

  // Register access: 32-bit (386+) — combines regs_hi:regs
  emu88_uint32 get_reg32(emu88_uint8 r) const {
    return EMU88_MK32(regs[r], regs_hi[r]);
  }
  void set_reg32(emu88_uint8 r, emu88_uint32 val) {
    regs[r] = val & 0xFFFF;
    regs_hi[r] = (val >> 16) & 0xFFFF;
  }

  // EIP access (386+)
  emu88_uint32 get_eip() const { return ip; }
  void set_eip(emu88_uint32 val) { ip = val; }

  // ESP access (386+)
  emu88_uint32 get_esp() const { return get_reg32(reg_SP); }
  void set_esp(emu88_uint32 val) { set_reg32(reg_SP, val); }

  // EFLAGS access (386+)
  emu88_uint32 get_eflags() const { return EMU88_MK32(flags, eflags_hi); }
  void set_eflags(emu88_uint32 val) {
    uint8_t old_iopl = (flags >> 12) & 3;
    flags = val & 0xFFFF;
    eflags_hi = (val >> 16) & 0xFFFF;
    uint8_t new_iopl = (flags >> 12) & 3;
    if (new_iopl != old_iopl) {
      static int iopl_log = 0;
      if (iopl_log < 5) { iopl_log++; fprintf(stderr, "[IOPL] %d→%d EFLAGS=%08X CS:IP=%04X:%08X\n", old_iopl, new_iopl, val, sregs[seg_CS], ip); }
    }
  }

  // Operand size helpers
  bool operand_32() const { return op_size_32; }
  bool address_32() const { return addr_size_32; }

  // Protected mode helpers
  bool protected_mode() const { return (cr0 & CR0_PE) != 0; }
  bool paging_enabled() const { return (cr0 & CR0_PG) != 0; }
  bool code_32() const { return protected_mode() && (seg_cache[seg_CS].flags & 0x04); }
  bool stack_32() const { return protected_mode() && (seg_cache[seg_SS].flags & 0x04); }
  emu88_uint8 get_iopl() const { return (flags >> 12) & 3; }

  // Segment register access
  emu88_uint16 get_sreg(emu88_uint8 s) const { return sregs[s]; }
  void set_sreg(emu88_uint8 s, emu88_uint16 val) { sregs[s] = val; }

  // Segment loading (updates descriptor cache)
  void load_segment(int seg_idx, emu88_uint16 selector, int cpl_override = -1);
  void load_segment_real(int seg_idx, emu88_uint16 selector);

  // Far control transfer (handles call gates in protected mode)
  void far_call_or_jmp(emu88_uint16 selector, emu88_uint32 offset, bool is_call);
  void invalidate_segments_for_cpl();  // Null out DS/ES/FS/GS if DPL < CPL

  // V86 mode helper
  bool v86_mode() const { return (get_eflags() & EFLAG_VM) != 0; }

  // I/O permission bitmap check (returns true if I/O is allowed)
  bool check_io_permission(emu88_uint16 port, emu88_uint8 width);

  // Segment limit checking (returns true if access is within limit)
  bool check_segment_limit(int seg_idx, emu88_uint32 offset, emu88_uint8 width) const;

  // Segment access checking for protected mode memory access
  bool check_segment_read(emu88_uint16 seg, emu88_uint32 off, emu88_uint8 width);
  bool check_segment_write(emu88_uint16 seg, emu88_uint32 off, emu88_uint8 width);

  // Descriptor table helpers
  void read_descriptor(emu88_uint32 table_base, emu88_uint16 index,
                       emu88_uint8 desc[8]);
  static void parse_descriptor(const emu88_uint8 desc[8], SegDescCache &cache);

  // Flags helpers
  bool get_flag(emu88_uint16 f) const { return (flags & f) != 0; }
  void set_flag(emu88_uint16 f) { flags |= f; }
  void clear_flag(emu88_uint16 f) { flags &= ~f; }
  void set_flag_val(emu88_uint16 f, bool val) {
    if (val) flags |= f; else flags &= ~f;
  }

  // Flag computation — 8-bit and 16-bit (8088)
  void set_flags_zsp8(emu88_uint8 val);
  void set_flags_zsp16(emu88_uint16 val);
  void set_flags_add8(emu88_uint8 a, emu88_uint8 b, emu88_uint8 carry);
  void set_flags_add16(emu88_uint16 a, emu88_uint16 b, emu88_uint16 carry);
  void set_flags_sub8(emu88_uint8 a, emu88_uint8 b, emu88_uint8 borrow);
  void set_flags_sub16(emu88_uint16 a, emu88_uint16 b, emu88_uint16 borrow);
  void set_flags_logic8(emu88_uint8 result);
  void set_flags_logic16(emu88_uint16 result);

  // Flag computation — 32-bit (386+)
  void set_flags_zsp32(emu88_uint32 val);
  void set_flags_add32(emu88_uint32 a, emu88_uint32 b, emu88_uint32 carry);
  void set_flags_sub32(emu88_uint32 a, emu88_uint32 b, emu88_uint32 borrow);
  void set_flags_logic32(emu88_uint32 result);

  // Memory access (segment-aware, supports protected mode)
  emu88_uint32 effective_address(emu88_uint16 seg, emu88_uint32 off) const;
  emu88_uint16 default_segment(void) const;
  emu88_uint8 fetch_byte(emu88_uint16 seg, emu88_uint32 off);
  void store_byte(emu88_uint16 seg, emu88_uint32 off, emu88_uint8 val);
  emu88_uint16 fetch_word(emu88_uint16 seg, emu88_uint32 off);
  void store_word(emu88_uint16 seg, emu88_uint32 off, emu88_uint16 val);

  // Memory access: 32-bit (386+)
  emu88_uint32 fetch_dword(emu88_uint16 seg, emu88_uint32 off);
  void store_dword(emu88_uint16 seg, emu88_uint32 off, emu88_uint32 val);

  // Linear address memory access (for descriptor table lookups, paging)
  emu88_uint32 translate_linear(emu88_uint32 linear, bool write = false);
  emu88_uint8 read_linear8(emu88_uint32 linear);
  emu88_uint16 read_linear16(emu88_uint32 linear);
  emu88_uint32 read_linear32(emu88_uint32 linear);
  void write_linear8(emu88_uint32 linear, emu88_uint8 val);
  void write_linear16(emu88_uint32 linear, emu88_uint16 val);
  void write_linear32(emu88_uint32 linear, emu88_uint32 val);

  // Instruction stream
  emu88_uint8 fetch_ip_byte(void);
  emu88_uint16 fetch_ip_word(void);
  emu88_uint32 fetch_ip_dword(void);

  // Stack operations (protected mode aware - use ESP when stack_32)
  void push_word(emu88_uint16 val);
  emu88_uint16 pop_word(void);
  void push_dword(emu88_uint32 val);
  emu88_uint32 pop_dword(void);

  // ModR/M decoding
  struct modrm_result {
    emu88_uint16 seg;       // segment selector for memory operand
    emu88_uint32 offset;    // offset for memory operand (32-bit for 386+ addressing)
    emu88_uint8 reg_field;  // reg field (bits 5-3)
    emu88_uint8 rm_field;   // r/m field (bits 2-0)
    emu88_uint8 mod_field;  // mod field (bits 7-6)
    bool is_register;       // true if r/m refers to register, not memory
  };
  modrm_result decode_modrm(emu88_uint8 modrm);
  modrm_result decode_modrm_32(emu88_uint8 modrm);  // 386+ 32-bit addressing with SIB

  // Get/set operand from modrm — 8-bit and 16-bit (8088)
  emu88_uint8 get_rm8(const modrm_result &mr);
  void set_rm8(const modrm_result &mr, emu88_uint8 val);
  emu88_uint16 get_rm16(const modrm_result &mr);
  void set_rm16(const modrm_result &mr, emu88_uint16 val);

  // Get/set operand from modrm — 32-bit (386+)
  emu88_uint32 get_rm32(const modrm_result &mr);
  void set_rm32(const modrm_result &mr, emu88_uint32 val);

  // String operations support
  emu88_uint16 string_src_seg(void) const;

  // Execute one instruction
  virtual void execute(void);

  // Debug
  virtual void debug_dump_regs(const char *label);

  // Initialization
  void setup_parity(void);
  void reset(void);
  void init_seg_caches(void);

private:
  // ALU helpers — 8-bit and 16-bit (8088)
  emu88_uint8 alu_add8(emu88_uint8 a, emu88_uint8 b, emu88_uint8 carry);
  emu88_uint16 alu_add16(emu88_uint16 a, emu88_uint16 b, emu88_uint16 carry);
  emu88_uint8 alu_sub8(emu88_uint8 a, emu88_uint8 b, emu88_uint8 borrow);
  emu88_uint16 alu_sub16(emu88_uint16 a, emu88_uint16 b, emu88_uint16 borrow);
  emu88_uint8 alu_inc8(emu88_uint8 val);
  emu88_uint8 alu_dec8(emu88_uint8 val);
  emu88_uint16 alu_inc16(emu88_uint16 val);
  emu88_uint16 alu_dec16(emu88_uint16 val);

  // ALU helpers — 32-bit (386+)
  emu88_uint32 alu_add32(emu88_uint32 a, emu88_uint32 b, emu88_uint32 carry);
  emu88_uint32 alu_sub32(emu88_uint32 a, emu88_uint32 b, emu88_uint32 borrow);
  emu88_uint32 alu_inc32(emu88_uint32 val);
  emu88_uint32 alu_dec32(emu88_uint32 val);

  // Group instruction helpers
  void execute_alu_rm8_r8(emu88_uint8 op);
  void execute_alu_rm16_r16(emu88_uint8 op);
  void execute_alu_r8_rm8(emu88_uint8 op);
  void execute_alu_r16_rm16(emu88_uint8 op);
  void execute_alu_al_imm8(emu88_uint8 op);
  void execute_alu_ax_imm16(emu88_uint8 op);
  void execute_grp1_rm8(const modrm_result &mr, emu88_uint8 imm);
  void execute_grp1_rm16(const modrm_result &mr, emu88_uint16 imm);
  void execute_grp2_rm8(const modrm_result &mr, emu88_uint8 count);
  void execute_grp2_rm16(const modrm_result &mr, emu88_uint8 count);
  void execute_grp3_rm8(emu88_uint8 modrm);
  void execute_grp3_rm16(emu88_uint8 modrm);
  void execute_grp4_rm8(emu88_uint8 modrm);
  void execute_grp5_rm16(emu88_uint8 modrm);

  // ALU operation dispatch (ADD=0, OR=1, ADC=2, SBB=3, AND=4, SUB=5, XOR=6, CMP=7)
  emu88_uint8 do_alu8(emu88_uint8 op, emu88_uint8 a, emu88_uint8 b);
  emu88_uint16 do_alu16(emu88_uint8 op, emu88_uint16 a, emu88_uint16 b);
  emu88_uint32 do_alu32(emu88_uint8 op, emu88_uint32 a, emu88_uint32 b);

  // Shift/rotate helpers
  emu88_uint8 do_shift8(emu88_uint8 op, emu88_uint8 val, emu88_uint8 count);
  emu88_uint16 do_shift16(emu88_uint8 op, emu88_uint16 val, emu88_uint8 count);
  emu88_uint32 do_shift32(emu88_uint8 op, emu88_uint32 val, emu88_uint8 count);

  // Group instruction helpers — 32-bit (386+)
  void execute_grp1_rm32(const modrm_result &mr, emu88_uint32 imm);
  void execute_grp2_rm32(const modrm_result &mr, emu88_uint8 count);
  void execute_grp3_rm32(emu88_uint8 modrm_byte);
  void execute_grp5_rm32(emu88_uint8 modrm_byte);

  // String instruction helpers
  void execute_string_op(emu88_uint8 opcode);
};

#endif // EMU88_H
