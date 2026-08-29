// DPMI host end-to-end test.  Drives emu88/dos_dpmi.cc the way a real client
// does: INT 2Fh AX=1687h in real mode, a FAR CALL to the returned entry point
// to enter protected mode, then INT 31h from protected-mode guest code for
// every service.  Nothing here calls a dos_dpmi.cc function directly -- every
// assertion is made against state the guest could observe, or against the raw
// GDT/LDT/IDT/TSS bytes the host laid down in guest-visible memory.
//
// Build & run:
//   g++ -std=c++20 -O2 -Wall -Wextra -I emu88 tests/dpmi_test.cc
//       emu88/emu88.cc emu88/emu88_pmode.cc emu88/emu88_fpu.cc
//       emu88/emu88_mem.cc emu88/dos_machine.cc emu88/dos_bios.cc
//       emu88/dos_dpmi.cc emu88/ne2000.cc emu88/opl.cc
//       emu88/sound_blaster.cc emu88/uart16550.cc -o /tmp/dpmi_test
//   /tmp/dpmi_test
//
// Three kinds of assertion appear below:
//
//   check()  - behaviour that must hold.  A plausible edit to dos_dpmi.cc
//              flips it.
//   check()  with a "divergence:" prefix in the name - pins a deliberate
//              simplification that does NOT match DPMI 0.9.  It is an exact
//              assertion, so it fails loudly if the behaviour ever changes
//              (in either direction) and the pin has to be re-decided.  Same
//              idea as the SST_BASELINE gate in tests/run_suites.sh.
//   bug()    - asserts the CORRECT behaviour for a defect that is present in
//              dos_dpmi.cc today.  Such an assertion FAILS, on purpose, and is
//              listed again in the summary; it retires itself when the defect
//              is fixed.  There are none right now - the machinery is kept in
//              place for the next one.
//
// A 16MB emu88_mem is required.  dpmi_mode_switch() puts the GDT/IDT/LDT/TSS
// at (mem_size - 0x20000) & ~0xFFF, and dpmi.next_mem_base starts at 2MB.
//
// EXIT CODE: 0 while exactly KNOWN_BUGS_EXPECTED bug() assertions are red, the
// way tests/run_suites.sh holds SingleStepTests to a baseline rather than to
// zero.  The baseline is 0: the four defects this harness was written against
// -- 0002h's dead seg_map cache, 0400h's virtual-memory flag, 0303h/0304h's
// leaked callback slots, and the real-mode reflection stack landing 64KB
// outside its reserved window -- are all fixed, and the assertions that caught
// them are ordinary check()s now.  Everything here passes, at 420 checks.
// FIXING a future bug() FAILS this harness on purpose: it prints FIXED and says
// to lower the baseline, because a silent improvement means the number is stale.
//
// The checks were also shown to be able to fail before they were trusted:
// twenty single-line mutations of dos_dpmi.cc (wrong version byte, wrong
// granularity threshold, 0900h returning the new state instead of the old, the
// LDT moved, allocations not page-aligned, a leaked selector on 0101h, a
// 16-bit IDT gate for a 32-bit client, 0201h skipping the real IVT, 0000h
// handing out the null selector, A20 left off, ...) each turned at least one
// check red.

#include "dos_machine.h"
#include "dos_io.h"
#include "emu88_mem.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

//=============================================================================
// Harness
//=============================================================================

// Number of bug() assertions that are still red.  Each one asserts the
// DPMI-0.9-correct behaviour against a defect that is in emu88/dos_dpmi.cc
// today, so they stay red until the defect is fixed.  Lower this (and delete
// the bug() call) when one is.  A bug that starts passing FAILS this harness
// on purpose, the same way tests/run_suites.sh fails on a SingleStepTests
// score ABOVE its baseline: a silent improvement means this number is stale.
static const int KNOWN_BUGS_EXPECTED = 0;

static int g_checks = 0;
static int g_failures = 0;      // real failures - a regression
static int g_bugs = 0;          // bug() assertions still red - expected
static int g_bugs_fixed = 0;    // bug() assertions that started passing
static int g_stuck = 0;          // guest stubs that never reached their HLT
static int g_exceptions = 0;     // unexpected CPU exceptions during a stub

static void check(bool cond, const char *what) {
  g_checks++;
  if (!cond) { g_failures++; std::printf("  FAIL: %s\n", what); }
}

// Asserts the DPMI-0.9-correct behaviour for a known dos_dpmi.cc defect.
// Unused while the ledger is empty; kept in place for the next defect.
[[maybe_unused]] static void bug(bool cond, const char *what) {
  g_checks++;
  if (!cond) { g_bugs++; std::printf("  BUG:  %s\n", what); }
  else { g_bugs_fixed++;
         std::printf("  FIXED (lower KNOWN_BUGS_EXPECTED and drop this bug()): %s\n", what); }
}

static void check_eq(uint32_t got, uint32_t want, const char *what) {
  g_checks++;
  if (got != want) {
    g_failures++;
    std::printf("  FAIL: %s (want 0x%X, got 0x%X)\n", what, want, got);
  }
}

[[maybe_unused]] static void bug_eq(uint32_t got, uint32_t want, const char *what) {
  g_checks++;
  if (got != want) {
    g_bugs++;
    std::printf("  BUG:  %s (correct 0x%X, got 0x%X)\n", what, want, got);
  } else {
    g_bugs_fixed++;
    std::printf("  FIXED (lower KNOWN_BUGS_EXPECTED and drop this bug_eq()): %s\n", what);
  }
}

struct StubIO : dos_io {
  void console_write(uint8_t) override {}
  bool console_has_input() override { return false; }
  int  console_read() override { return -1; }
  void video_mode_changed(int,int,int) override {}
  void video_refresh(const uint8_t*,int,int) override {}
  void video_refresh_direct(const uint8_t*,int,int,int,int,const uint8_t[][3]) override {}
  void video_set_cursor(int,int) override {}
  bool disk_present(int) override { return false; }
  size_t disk_read(int,uint64_t,uint8_t*,size_t) override { return 0; }
  size_t disk_write(int,uint64_t,const uint8_t*,size_t) override { return 0; }
  uint64_t disk_size(int) override { return 0; }
  void get_time(int&h,int&m,int&s,int&hs) override { h=m=s=hs=0; }
  void get_date(int&y,int&mo,int&d,int&w) override { y=2026;mo=1;d=1;w=0; }
};

using E = emu88;

static emu88_mem   *gmem;
static dos_machine *gm;

static uint8_t  rb(uint32_t a) { return gmem->fetch_mem(a); }
static uint16_t rw(uint32_t a) { return gmem->fetch_mem16(a); }
static uint32_t rd(uint32_t a) { return gmem->fetch_mem32(a); }

static void poke(uint32_t a, const uint8_t *b, size_t n) {
  for (size_t i = 0; i < n; i++) gmem->store_mem(a + (uint32_t)i, b[i]);
}

// Compare the 8 raw bytes of an LDT/GDT descriptor.
static void check_desc(uint32_t addr, const uint8_t want[8], const char *what) {
  g_checks++;
  uint8_t got[8];
  for (int i = 0; i < 8; i++) got[i] = rb(addr + (uint32_t)i);
  if (std::memcmp(got, want, 8) != 0) {
    g_failures++;
    std::printf("  FAIL: %s\n"
                "        want %02X %02X %02X %02X %02X %02X %02X %02X\n"
                "        got  %02X %02X %02X %02X %02X %02X %02X %02X\n",
                what,
                want[0],want[1],want[2],want[3],want[4],want[5],want[6],want[7],
                got[0], got[1], got[2], got[3], got[4], got[5], got[6], got[7]);
  }
}

static uint32_t ldt_ent(uint16_t sel) {
  return gm->dpmi.ldt_phys + (uint32_t)(sel >> 3) * 8;
}
static uint32_t gdt_ent(int idx) {
  return gm->dpmi.gdt_phys + (uint32_t)idx * 8;
}

// Guest code layout ----------------------------------------------------------
// 0x0800:0000  real-mode INT 21h handler (swappable variants), scratch at +0100
// 0x2000:....  real-mode / client stack segment
// 0x3000:0000  client code segment: CALL FAR into the DPMI entry, then stubs
// 0x4000:....  client DS
// 0x5000:0000  scratch buffer reached through the flat selector
// 0x9000:0000  client PSP (PSP[2Ch] = environment segment 0x0950)
static constexpr uint16_t RM_INT21_SEG = 0x0800;
static constexpr uint32_t RM_INT21_PHYS = 0x8000;
static constexpr uint32_t RM_INT21_LOG  = 0x8100;   // stub records ES here
static constexpr uint16_t CODE_SEG = 0x3000;
static constexpr uint32_t CODE_PHYS = 0x30000;
static constexpr uint16_t DATA_SEG = 0x4000;
static constexpr uint16_t STACK_SEG = 0x2000;
static constexpr uint16_t PSP_SEG = 0x0900;
static constexpr uint16_t ENV_SEG = 0x0950;
static constexpr uint32_t SCRATCH = 0x50000;

// Offsets inside CODE_SEG.
static constexpr uint32_t OFF_ENTER   = 0x0000;  // 9A DC EF 00 F0 / B8 34 12 / F4
static constexpr uint32_t OFF_INT2F   = 0x0010;  // CD 2F F4
static constexpr uint32_t OFF_INT31   = 0x0020;  // CD 31 F4
static constexpr uint32_t OFF_SETES   = 0x0030;  // B8 08 00 8E C0 F4
static constexpr uint32_t OFF_INT21   = 0x0040;  // CD 21 F4

static void run_guest(uint32_t entry_off, int budget = 4000) {
  gm->ip = entry_off;
  gm->halted = false;
  gm->exception_pending = false;
  gm->in_exception = false;
  int n = 0;
  while (!gm->halted && n < budget) { gm->execute(); n++; }
  if (!gm->halted) g_stuck++;
  if (gm->exception_pending || gm->in_exception) g_exceptions++;
}

// One INT 31h call from protected-mode guest code.
static void int31(uint32_t eax, uint32_t ebx = 0, uint32_t ecx = 0,
                  uint32_t edx = 0, uint32_t esi = 0, uint32_t edi = 0) {
  gm->exception_pending = false;
  gm->in_exception = false;
  gm->set_reg32(E::reg_AX, eax);
  gm->set_reg32(E::reg_BX, ebx);
  gm->set_reg32(E::reg_CX, ecx);
  gm->set_reg32(E::reg_DX, edx);
  gm->set_reg32(E::reg_SI, esi);
  gm->set_reg32(E::reg_DI, edi);
  run_guest(OFF_INT31);
}

static uint16_t AX() { return gm->get_reg16(E::reg_AX); }
static uint16_t BX() { return gm->get_reg16(E::reg_BX); }
static uint16_t CX() { return gm->get_reg16(E::reg_CX); }
static uint16_t DX() { return gm->get_reg16(E::reg_DX); }
static uint16_t SI() { return gm->get_reg16(E::reg_SI); }
static uint16_t DI() { return gm->get_reg16(E::reg_DI); }
static uint32_t EDX() { return gm->get_reg32(E::reg_DX); }
static uint32_t EDI() { return gm->get_reg32(E::reg_DI); }
static uint8_t  AL() { return gm->get_reg8(E::reg_AL); }
static bool CF() { return gm->get_flag(E::FLAG_CF); }

//=============================================================================
// Real-mode INT 21h handler variants
//
// dos_dpmi.cc reaches INT 21h for functions 0100h/0101h by reflecting to real
// mode, so those services can only be exercised with a real INT 21h in the
// IVT.  emu88's BIOS has no INT 21h of its own (DOS supplies it), so the test
// installs its own 8086 stub and swaps the body between cases.  Each variant
// edits the FLAGS image the reflection pushed, at [BP+6], so CF propagates
// back to protected mode exactly as a real DOS would return it.
//=============================================================================

// AH=48h success: returns segment 6000h, CF clear.
static const uint8_t INT21_ALLOC_OK[] = {
  0x55,                    // push bp
  0x89, 0xE5,              // mov  bp,sp
  0xB8, 0x00, 0x60,        // mov  ax,6000h
  0x83, 0x66, 0x06, 0xFE,  // and  word [bp+6],0FFFEh   ; CF = 0
  0x5D,                    // pop  bp
  0xCF,                    // iret
};

// AH=48h failure: CF set, AX=0008h (insufficient memory), BX=0100h (max avail).
static const uint8_t INT21_ALLOC_FAIL[] = {
  0x55,                    // push bp
  0x89, 0xE5,              // mov  bp,sp
  0xB8, 0x08, 0x00,        // mov  ax,0008h
  0xBB, 0x00, 0x01,        // mov  bx,0100h
  0x83, 0x4E, 0x06, 0x01,  // or   word [bp+6],1        ; CF = 1
  0x5D,                    // pop  bp
  0xCF,                    // iret
};

// AH=49h: record the ES the host passed in, return success.
static const uint8_t INT21_FREE[] = {
  0x8C, 0xC0,              // mov  ax,es
  0x2E, 0xA3, 0x00, 0x01,  // mov  [cs:0100h],ax
  0x55,                    // push bp
  0x89, 0xE5,              // mov  bp,sp
  0x31, 0xC0,              // xor  ax,ax
  0x83, 0x66, 0x06, 0xFE,  // and  word [bp+6],0FFFEh   ; CF = 0
  0x5D,                    // pop  bp
  0xCF,                    // iret
};

// A stand-in for DOS's terminate path: stop dead so the post-terminate machine
// state can be inspected.  A real INT 21h AH=4Ch never returns to the caller,
// which is why dpmi_terminate() pushes a halt sentinel rather than a return.
static const uint8_t INT21_HALT[] = { 0xF4 };

static void install_int21(const uint8_t *body, size_t n) {
  for (uint32_t i = 0; i < 0x40; i++) gmem->store_mem(RM_INT21_PHYS + i, 0xCF);
  poke(RM_INT21_PHYS, body, n);
}
#define INSTALL_INT21(v) install_int21(v, sizeof(v))

//=============================================================================

int main() {
  static emu88_mem mem(0x1000000);      // 16MB - dpmi structures live near the top
  static StubIO io;
  static dos_machine m(&mem, &io);
  gmem = &mem; gm = &m;
  m.init_machine();

  //--------------------------------------------------------------------------
  // Pre-switch real-mode state the DPMI host is supposed to capture.
  //--------------------------------------------------------------------------
  const uint16_t ivt_ff_off = rw(0xFF * 4);       // BIOS stub offset for INT FFh
  check_eq(rw(0xFF * 4 + 2), 0xF000, "setup: IVT[FFh] starts on the BIOS stub");

  INSTALL_INT21(INT21_ALLOC_OK);
  // Two real-mode procedures for 0301h/0302h, above the swappable INT 21h body.
  { const uint8_t retf_proc[] = { 0xB8, 0x55, 0xAA, 0xCB };   // mov ax,0AA55h / retf
    const uint8_t iret_proc[] = { 0xB8, 0x5A, 0xA5, 0xCF };   // mov ax,0A55Ah / iret
    poke(RM_INT21_PHYS + 0x40, retf_proc, sizeof(retf_proc));
    poke(RM_INT21_PHYS + 0x50, iret_proc, sizeof(iret_proc)); }
  mem.store_mem16(0x21 * 4,     0x0000);
  mem.store_mem16(0x21 * 4 + 2, RM_INT21_SEG);
  // A distinctive vector, to prove the 256-entry snapshot is a real snapshot.
  mem.store_mem16(0x60 * 4,     0x1234);
  mem.store_mem16(0x60 * 4 + 2, 0xBEEF);
  // PSP with an environment segment, so the 2Ch patch can be checked.
  mem.store_mem16((uint32_t)PSP_SEG * 16 + 0x2C, ENV_SEG);

  // Guest code.
  // call far F000:EFDC / mov ax,1234h / hlt -- the MOV runs in protected mode
  // at the return offset, so AX proves where the switch resumed.
  const uint8_t enter[] = { 0x9A, 0xDC, 0xEF, 0x00, 0xF0, 0xB8, 0x34, 0x12, 0xF4 };
  const uint8_t int2f[] = { 0xCD, 0x2F, 0xF4 };
  const uint8_t int31s[] = { 0xCD, 0x31, 0xF4 };
  const uint8_t setes[] = { 0xB8, 0x08, 0x00, 0x8E, 0xC0, 0xF4 };  // mov ax,8 / mov es,ax / hlt
  poke(CODE_PHYS + OFF_ENTER, enter,  sizeof(enter));
  poke(CODE_PHYS + OFF_INT2F, int2f,  sizeof(int2f));
  poke(CODE_PHYS + OFF_INT31, int31s, sizeof(int31s));
  poke(CODE_PHYS + OFF_SETES, setes,  sizeof(setes));
  const uint8_t int21s[] = { 0xCD, 0x21, 0xF4 };
  poke(CODE_PHYS + OFF_INT21, int21s, sizeof(int21s));

  check(!mem.get_a20(), "setup: A20 starts disabled");
  check(!m.dpmi.active, "setup: no DPMI session yet");
  check(!m.protected_mode(), "setup: machine starts in real mode");

  //==========================================================================
  // 1. INT 2Fh AX=1687h - DPMI detection (real mode)
  //==========================================================================
  m.load_segment_real(E::seg_CS, CODE_SEG);
  m.load_segment_real(E::seg_DS, DATA_SEG);
  m.load_segment_real(E::seg_SS, STACK_SEG);
  m.load_segment_real(E::seg_ES, PSP_SEG);
  m.set_reg32(E::reg_SP, 0xFFFE);
  m.set_reg32(E::reg_AX, 0x1687);
  m.set_reg32(E::reg_BX, 0xFFFF);
  m.set_reg32(E::reg_CX, 0xFFFF);
  m.set_reg32(E::reg_DX, 0xFFFF);
  m.set_reg32(E::reg_SI, 0xFFFF);
  run_guest(OFF_INT2F);

  check_eq(AX(), 0x0000, "1687h: AX=0 (DPMI host present)");
  check_eq(BX(), 0x0001, "1687h: BX bit0 set (32-bit clients supported)");
  check_eq(m.get_reg8(E::reg_CL), 3, "1687h: CL=3 (80386)");
  check_eq(DX(), 0x005A, "1687h: DX=005Ah (DPMI version 0.90)");
  check_eq(SI(), 0x0000, "1687h: SI=0 (no private data paragraphs)");
  check_eq(m.sregs[E::seg_ES], 0xF000, "1687h: ES=F000h (entry segment)");
  check_eq(DI(), 0xEFDC, "1687h: DI=EFDCh (mode switch entry offset)");
  check_eq(DI(), m.dpmi.mode_switch_off, "1687h: DI matches dpmi.mode_switch_off");
  check(!m.protected_mode(), "1687h: detection alone does not enter protected mode");
  check(!m.dpmi.active, "1687h: detection alone does not start a session");
  check_eq(rb(0xF0000 + 0xEFDC), 0xF1, "entry point holds the BIOS trap opcode");
  check_eq(rb(0xF0000 + 0xEFDD), 0xFD, "entry point holds the mode-switch marker");

  //==========================================================================
  // 2. The mode switch - FAR CALL F000:EFDC with AX bit0=1, ES=PSP
  //==========================================================================
  m.load_segment_real(E::seg_CS, CODE_SEG);
  m.load_segment_real(E::seg_DS, DATA_SEG);
  m.load_segment_real(E::seg_SS, STACK_SEG);
  m.load_segment_real(E::seg_ES, PSP_SEG);
  m.set_reg32(E::reg_SP, 0xFFFE);
  m.set_reg32(E::reg_AX, 0x0001);        // bit 0 = 32-bit client
  run_guest(OFF_ENTER);

  check(m.dpmi.active, "switch: session active");
  check(m.dpmi.is_32bit, "switch: 32-bit client recorded");
  check((m.cr0 & E::CR0_PE) != 0, "switch: CR0.PE set");
  check(m.protected_mode(), "switch: protected_mode() true");
  check(mem.get_a20(), "switch: A20 forced on");
  check_eq(m.cpl, 0, "switch: CPL=0 (raw-mode-switch style host)");
  check(m.get_flag(E::FLAG_IF), "switch: IF set on entry to protected mode");
  check_eq(m.dpmi.client_psp, PSP_SEG, "switch: client PSP recorded");
  check_eq(m.dpmi.saved_rm_ss, STACK_SEG, "switch: real-mode SS saved");
  check_eq(m.dpmi.saved_rm_sp, 0xFFFE, "switch: real-mode SP saved (FAR CALL popped)");
  check_eq(AX(), 0x1234, "switch: protected-mode code ran at the FAR CALL return offset");
  check_eq(m.ip, OFF_ENTER + 9, "switch: resumed at offset 5 and ran the 4 bytes there");

  // Structure layout: (16MB - 0x20000) & ~0xFFF.
  check_eq(m.dpmi.base,         0xFE0000, "layout: dpmi.base");
  check_eq(m.dpmi.gdt_phys,     0xFE0000, "layout: GDT at base");
  check_eq(m.dpmi.idt_phys,     0xFE2000, "layout: IDT at base+2000h");
  check_eq(m.dpmi.ldt_phys,     0xFE2800, "layout: LDT at base+2800h");
  check_eq(m.dpmi.tss_phys,     0xFE6800, "layout: TSS at base+6800h");
  check_eq(m.dpmi.pm_stack_top, 0xFE8000, "layout: ring-0 stack top at base+8000h");
  check_eq(m.dpmi.next_mem_base, 0x200000, "layout: DPMI allocations start at 2MB");
  check_eq(m.dpmi.next_handle,   1, "layout: first memory handle is 1");

  check_eq(m.gdtr_base,  0xFE0000, "GDTR base = dpmi.gdt_phys");
  check_eq(m.gdtr_limit, 1024 * 8 - 1, "GDTR limit = 1024 entries");
  check_eq(m.idtr_base,  0xFE2000, "IDTR base = dpmi.idt_phys");
  check_eq(m.idtr_limit, 256 * 8 - 1, "IDTR limit = 256 entries");
  check_eq(m.ldtr, 0x0020, "LDTR = GDT selector 0020h");
  check_eq(m.ldtr_cache.base,  0xFE2800, "LDTR cache base = dpmi.ldt_phys");
  check_eq(m.ldtr_cache.limit, 2048 * 8 - 1, "LDTR cache limit = 2048 entries");
  check_eq(m.tr, 0x0018, "TR = GDT selector 0018h");
  check_eq(m.tr_cache.base,  0xFE6800, "TR cache base = dpmi.tss_phys");
  check_eq(m.tr_cache.limit, 103, "TR cache limit = 103");
  check_eq(m.tr_cache.access, 0x8B, "TR cache marked busy");

  // The GDT the host actually wrote.
  { const uint8_t d[8] = {0,0,0,0,0,0,0,0};
    check_desc(gdt_ent(0), d, "GDT[0] null descriptor"); }
  { const uint8_t d[8] = {0xFF,0xFF,0x00,0x00,0x00,0x92,0xCF,0x00};
    check_desc(gdt_ent(1), d, "GDT 0008h ring0 flat data32 (base 0, 4GB, G=1 D=1)"); }
  { const uint8_t d[8] = {0xFF,0xFF,0x00,0x00,0x00,0x9A,0xCF,0x00};
    check_desc(gdt_ent(2), d, "GDT 0010h ring0 flat code32"); }
  { const uint8_t d[8] = {0x67,0x00,0x00,0x68,0xFE,0x8B,0x00,0x00};
    check_desc(gdt_ent(3), d, "GDT 0018h TSS (base FE6800h, limit 103, busy)"); }
  { const uint8_t d[8] = {0xFF,0x3F,0x00,0x28,0xFE,0x82,0x00,0x00};
    check_desc(gdt_ent(4), d, "GDT 0020h LDT (base FE2800h, limit 3FFFh)"); }
  { const uint8_t d[8] = {0xFF,0xFF,0x00,0x00,0x0F,0x9A,0x00,0x00};
    check_desc(gdt_ent(5), d, "GDT 0028h BIOS ROM code16 (base F0000h)"); }
  { const uint8_t d[8] = {0xFF,0xFF,0x00,0x00,0xFE,0x93,0x00,0x00};
    check_desc(gdt_ent(6), d, "GDT 0030h PM stack SS (base FE0000h, 16-bit)"); }
  check_eq(m.dpmi.bios_rom_cs, 0x0028, "dpmi.bios_rom_cs = 0028h");
  check_eq(m.dpmi.pm_stack_ss, 0x0030, "dpmi.pm_stack_ss = 0030h");
  check_eq(m.dpmi.ring0_cs, 0x0010, "dpmi.ring0_cs = 0010h");
  check_eq(m.dpmi.ring0_ds, 0x0008, "dpmi.ring0_ds = 0008h");

  // TSS fields the host filled in.
  check_eq(rd(m.dpmi.tss_phys + 4), 0xFE8000, "TSS ESP0 = ring-0 stack top");
  check_eq(rw(m.dpmi.tss_phys + 8), 0x0008, "TSS SS0 = ring-0 data selector");
  check_eq(rw(m.dpmi.tss_phys + 102), 104, "TSS I/O map base past the limit");

  // IDT: 256 32-bit interrupt gates at ring0_cs:0, DPL=0.
  { const uint8_t d[8] = {0x00,0x00,0x10,0x00,0x00,0x8E,0x00,0x00};
    check_desc(m.dpmi.idt_phys + 0x31 * 8, d, "IDT[31h] 32-bit int gate, sel 0010h, DPL 0"); }
  { const uint8_t d[8] = {0x00,0x00,0x10,0x00,0x00,0x8E,0x00,0x00};
    check_desc(m.dpmi.idt_phys + 255 * 8, d, "IDT[FFh] 32-bit int gate (all 256 written)"); }

  // The real-mode vector snapshot.
  check_eq(m.dpmi.rm_int_seg[0x60], 0xBEEF, "snapshot: IVT[60h] segment");
  check_eq(m.dpmi.rm_int_off[0x60], 0x1234, "snapshot: IVT[60h] offset");
  check_eq(m.dpmi.rm_int_seg[0x21], RM_INT21_SEG, "snapshot: IVT[21h] segment");
  check_eq(m.dpmi.rm_int_off[0x21], 0x0000, "snapshot: IVT[21h] offset");
  check_eq(m.dpmi.rm_int_seg[0x10], 0xF000, "snapshot: IVT[10h] still the BIOS stub");

  // The four client selectors plus the environment selector: LDT indices 1..5.
  check_eq(m.sregs[E::seg_CS], 0x000C, "switch: CS = LDT selector 000Ch");
  check_eq(m.sregs[E::seg_DS], 0x0014, "switch: DS = LDT selector 0014h");
  check_eq(m.sregs[E::seg_SS], 0x001C, "switch: SS = LDT selector 001Ch");
  check_eq(m.sregs[E::seg_ES], 0x0024, "switch: ES = LDT selector 0024h (PSP)");
  check_eq(m.sregs[E::seg_FS], 0x0000, "switch: FS cleared");
  check_eq(m.sregs[E::seg_GS], 0x0000, "switch: GS cleared");
  check_eq(m.seg_cache[E::seg_CS].base, CODE_PHYS,  "switch: CS base = caller CS<<4");
  check_eq(m.seg_cache[E::seg_DS].base, 0x40000,    "switch: DS base = caller DS<<4");
  check_eq(m.seg_cache[E::seg_SS].base, 0x20000,    "switch: SS base = caller SS<<4");
  check_eq(m.seg_cache[E::seg_ES].base, (uint32_t)PSP_SEG * 16, "switch: ES base = PSP<<4");
  check_eq(m.seg_cache[E::seg_ES].limit, 0x00FF, "switch: ES limit = 255 (PSP is 256 bytes)");
  { const uint8_t d[8] = {0xFF,0xFF,0x00,0x00,0x03,0x9B,0x00,0x00};
    check_desc(ldt_ent(0x000C), d, "LDT 000Ch client CS (base 30000h, ring0 code, readable)"); }
  { const uint8_t d[8] = {0xFF,0xFF,0x00,0x00,0x04,0x93,0x00,0x00};
    check_desc(ldt_ent(0x0014), d, "LDT 0014h client DS (base 40000h, ring0 data, writable)"); }
  { const uint8_t d[8] = {0xFF,0xFF,0x00,0x00,0x02,0x93,0x00,0x00};
    check_desc(ldt_ent(0x001C), d, "LDT 001Ch client SS (base 20000h)"); }
  { const uint8_t d[8] = {0xFF,0x00,0x00,0x90,0x00,0x93,0x00,0x00};
    check_desc(ldt_ent(0x0024), d, "LDT 0024h client ES (base 9000h, limit 255)"); }
  { const uint8_t d[8] = {0xFF,0xFF,0x00,0x95,0x00,0x93,0x00,0x00};
    check_desc(ldt_ent(0x002C), d, "LDT 002Ch environment (base 9500h)"); }
  check_eq(rw((uint32_t)PSP_SEG * 16 + 0x2C), 0x002C,
           "switch: PSP[2Ch] patched from env segment to env selector");
  check(m.dpmi.vif, "switch: virtual interrupt flag starts enabled");

  //--------------------------------------------------------------------------
  // Point ES at the flat ring-0 data selector so ES:EDI structure arguments
  // can address physical memory directly.  Done with real guest instructions.
  //--------------------------------------------------------------------------
  run_guest(OFF_SETES);
  check_eq(m.sregs[E::seg_ES], 0x0008, "guest loaded ES with the flat selector");
  check_eq(m.seg_cache[E::seg_ES].base, 0, "flat ES base = 0");

  //==========================================================================
  // 3. 0400h Get DPMI Version / 0003h Get Selector Increment
  //==========================================================================
  int31(0x0400);
  check(!CF(), "0400h: CF clear");
  check_eq(AX(), 0x005A, "0400h: AX=005Ah (major 0, minor 90 decimal)");
  check_eq(m.get_reg8(E::reg_CL), 3, "0400h: CL=3 (80386)");
  check_eq(m.get_reg8(E::reg_DH), 0x08, "0400h: DH=08h (master PIC base)");
  check_eq(m.get_reg8(E::reg_DL), 0x70, "0400h: DL=70h (slave PIC base)");
  // BX bit 2 is "virtual memory supported".  This host has no paging at all --
  // dpmi_mode_switch never touches CR0.PG or CR3, 0600h/0601h page locking are
  // no-ops, and 0500h reports a swap file size of FFFFFFFFh (none).
  check((BX() & 0x0004) == 0,
        "0400h: BX must not advertise virtual memory (bit 2) - this host has no paging");
  check_eq(BX() & 0x0001, 0x0001, "0400h: BX bit 0 set (32-bit programs supported)");
  // Bit 1 is "the processor returns to REAL mode for reflected interrupts", as
  // opposed to V86.  It is set because that is what this host does: both
  // dpmi_reflect_to_rm and dpmi_exec_rm clear CR0.PE and run the handler in
  // real mode.  A client that reads this bit and prepares a V86 monitor for the
  // other answer would be wrong about the host.
  check_eq(BX() & 0x0002, 0x0002, "0400h: BX bit 1 set (real-mode, not V86, reflection)");
  check_eq(BX(), 0x0003, "0400h: BX=0003h exactly - no other capability claimed");

  int31(0x0003);
  check(!CF(), "0003h: CF clear");
  check_eq(AX(), 8, "0003h: selector increment is 8");

  // 0E00h/0E01h Get and Set Coprocessor Status.  Both fell through to the
  // unsupported-function default until 2026-08-29, so a DPMI 1.0 client asking
  // this host whether there was an x87 was told the question was unsupported -
  // while the BDA equipment word said no and CMOS said yes.
  //
  // The bit the client actually reads for PRESENCE is bit 2 (MPr), not bit 0
  // (MPv, "enabled for this client").  Getting that backwards is easy and was
  // got backwards here first: 0x31 sets MPv and the type but leaves MPr clear,
  // which describes a coprocessor that is enabled and absent.  Every
  // assertion below therefore names the bit it means.
  //
  // Note what does NOT discriminate: the unsupported-function reply is
  // AX=8001h, and 8001h HAPPENS TO HAVE BIT 0 SET - so an "is bit 0 set" test
  // passes against a host that has just said the question is unsupported.
  // That is why CF is checked first and AX pinned exactly.
  int31(0x0E00);
  check(!CF(), "0E00h: CF clear - the function is supported");
  check_eq(AX() & 0x0004, 0x0004, "0E00h: bit 2 (MPr) set - a coprocessor is PRESENT");
  check_eq(AX() & 0x0001, 0x0001, "0E00h: bit 0 (MPv) set - and enabled for the client");
  check_eq((AX() >> 4) & 0x000F, 3, "0E00h: coprocessor type 3 (387 or later)");
  check_eq(AX() & 0x000A, 0, "0E00h: neither EMv nor EMr - nobody is emulating");
  check_eq(AX(), 0x0035, "0E00h: AX=0035h exactly");

  // A request this host can meet: leave the hardware x87 enabled.
  int31(0x0E01, 0x0001);
  check(!CF(), "0E01h: enabling the hardware coprocessor succeeds");

  // One it cannot: EMv asks the host to set CR0.EM and reflect #NM to a
  // client-supplied emulator.  Nothing here does that, so it is refused
  // rather than silently accepted - a client told "yes" would wait forever
  // for faults that never arrive.
  int31(0x0E01, 0x0002);
  check(CF(), "0E01h: a client-emulation request is REFUSED, not silently accepted");
  check_eq(AX(), 0x8021, "0E01h: refused with 8021h (invalid value)");

  //==========================================================================
  // 4. 0000h Allocate / 0001h Free LDT descriptors
  //==========================================================================
  int31(0x0000, 0, 1);
  check(!CF(), "0000h: CF clear on success");
  check_eq(AX(), 0x0034, "0000h: first client allocation is LDT index 6 -> 0034h");
  const uint16_t a1 = AX();

  int31(0x0000, 0, 1);
  check_eq(AX(), 0x003C, "0000h: second allocation is the next index -> 003Ch");
  const uint16_t a2 = AX();
  check_eq(a2 - a1, 8, "0000h: successive selectors differ by the 0003h increment");

  { const uint8_t d[8] = {0x00,0x00,0x00,0x00,0x00,0x92,0x00,0x00};
    check_desc(ldt_ent(a1), d, "0000h: allocated descriptor is base 0, limit 0, data RW"); }

  int31(0x0000, 0, 0);                      // CX=0 means one descriptor
  check(!CF(), "0000h: CX=0 is treated as a request for one descriptor");
  check_eq(AX(), 0x0044, "0000h: CX=0 allocates exactly one selector");
  int31(0x0001, AX());                      // give it back

  int31(0x0000, 0, 3);                      // three contiguous
  check(!CF(), "0000h: CF clear for a run of 3");
  check_eq(AX(), 0x0044, "0000h: run of 3 starts at LDT index 8 -> 0044h");
  const uint16_t a3 = AX();
  { const uint8_t d[8] = {0x00,0x00,0x00,0x00,0x00,0x92,0x00,0x00};
    check_desc(ldt_ent(a3),      d, "0000h: run member 0 initialised");
    check_desc(ldt_ent(a3 + 8),  d, "0000h: run member 1 initialised");
    check_desc(ldt_ent(a3 + 16), d, "0000h: run member 2 initialised"); }
  check(!(m.dpmi.ldt_alloc[10 / 8] & (1 << (10 % 8))) == false,
        "0000h: run of 3 marked all three indices allocated");

  // Free and re-allocate: the freed index must come back.
  int31(0x0001, a2);
  check(!CF(), "0001h: CF clear freeing a live selector");
  { const uint8_t d[8] = {0,0,0,0,0,0,0,0};
    check_desc(ldt_ent(a2), d, "0001h: freed descriptor is zeroed"); }
  check(!(m.dpmi.ldt_alloc[(a2 >> 3) / 8] & (1 << ((a2 >> 3) % 8))),
        "0001h: allocation bit cleared");
  int31(0x0000, 0, 1);
  check_eq(AX(), a2, "0000h: the freed index is handed out again");

  // Error paths.
  int31(0x0001, 0x0004);                    // index 0 (null selector)
  check(CF(), "0001h: CF set freeing the null selector");
  check_eq(AX(), 0x8022, "0001h: AX=8022h (invalid selector) for index 0");
  int31(0x0001, 0x0010);                    // GDT selector (TI=0)
  check(CF(), "0001h: CF set freeing a GDT selector");
  check_eq(AX(), 0x8022, "0001h: AX=8022h for a GDT (TI=0) selector");
  int31(0x0001, (2048u << 3) | 4);          // index == LDT_MAX
  check(CF(), "0001h: CF set freeing an out-of-range index");
  check_eq(AX(), 0x8022, "0001h: AX=8022h for index >= 2048");

  // Double free / never-allocated free.  DPMI 0.9 says 8022h; this host only
  // range-checks the selector, so both are silently accepted.
  int31(0x0001, a2);
  check(!CF(), "0001h: first free of a live selector succeeds");
  int31(0x0001, a2);
  check(!CF(), "divergence: 0001h double-free returns success (DPMI 0.9 says 8022h)");
  int31(0x0001, 0x07FC);                    // index 255, never allocated
  check(!CF(), "divergence: 0001h freeing a never-allocated selector returns "
               "success (DPMI 0.9 says 8022h)");

  //==========================================================================
  // 5. 0002h Segment to Descriptor
  //==========================================================================
  int31(0x0002, 0x1234);
  check(!CF(), "0002h: CF clear");
  const uint16_t s1 = AX();
  check(s1 != 0 && (s1 & 4) != 0, "0002h: returns an LDT selector");
  { const uint8_t d[8] = {0xFF,0xFF,0x40,0x23,0x01,0x92,0x00,0x00};
    check_desc(ldt_ent(s1), d, "0002h: descriptor is base 12340h, limit FFFFh, data RW"); }
  check(m.dpmi.seg_map[0].valid && m.dpmi.seg_map[0].rm_seg == 0x1234,
        "0002h: the mapping is recorded in the seg_map cache");

  int31(0x0002, 0x1234);
  const uint16_t s2 = AX();
  // The cache-hit path must leave the switch case, not just the *for* loop:
  // otherwise the allocate-a-new-descriptor code below it runs anyway, the
  // cached selector is overwritten, and a fresh LDT entry is burned on every
  // call.  A client that maps the same segment in a loop (DJGPP's
  // __dpmi_segment_to_descriptor, DOS4GW's video/PSP mapping) would exhaust
  // the 2047-entry LDT.  dos_dpmi.cc case 0x0002.
  check_eq(s2, s1, "0002h: asking twice for the same real-mode segment must "
                   "return the cached selector, not a new one");

  int31(0x0002, 0x5678);
  const uint16_t s3 = AX();
  check(s3 != s1, "0002h: a different real-mode segment gets a different selector");
  { const uint8_t d[8] = {0xFF,0xFF,0x80,0x67,0x05,0x92,0x00,0x00};
    check_desc(ldt_ent(s3), d, "0002h: second mapping is base 56780h"); }

  //==========================================================================
  // 6. 0006h/0007h base, 0008h limit, 0009h access rights, 000Bh/000Ch raw
  //==========================================================================
  int31(0x0000, 0, 1);
  const uint16_t wb = AX();                 // workbench selector
  check(!CF(), "workbench selector allocated");

  int31(0x0007, wb, 0x0012, 0x3456);
  check(!CF(), "0007h: CF clear");
  { const uint8_t d[8] = {0x00,0x00,0x56,0x34,0x12,0x92,0x00,0x00};
    check_desc(ldt_ent(wb), d, "0007h: base 00123456h written into bytes 2,3,4,7"); }
  int31(0x0006, wb);
  check(!CF(), "0006h: CF clear");
  check_eq(CX(), 0x0012, "0006h: CX = base bits 31..16");
  check_eq(DX(), 0x3456, "0006h: DX = base bits 15..0");

  int31(0x0007, wb, 0xF123, 0x4567);        // exercise the byte-7 path
  { const uint8_t d[8] = {0x00,0x00,0x67,0x45,0x23,0x92,0x00,0xF1};
    check_desc(ldt_ent(wb), d, "0007h: base F1234567h reaches descriptor byte 7"); }
  int31(0x0006, wb);
  check_eq(CX(), 0xF123, "0006h: CX reads back base bits 31..16");
  check_eq(DX(), 0x4567, "0006h: DX reads back base bits 15..0");

  int31(0x0008, wb, 0x0000, 0xFFFF);        // 64KB, byte granular
  check(!CF(), "0008h: CF clear");
  { const uint8_t d[8] = {0xFF,0xFF,0x67,0x45,0x23,0x92,0x00,0xF1};
    check_desc(ldt_ent(wb), d, "0008h: limit FFFFh, G=0"); }
  int31(0x0008, wb, 0x000F, 0xFFFF);        // 1MB-1, still byte granular
  { const uint8_t d[8] = {0xFF,0xFF,0x67,0x45,0x23,0x92,0x0F,0xF1};
    check_desc(ldt_ent(wb), d, "0008h: limit FFFFFh fits without granularity"); }
  int31(0x0008, wb, 0x0010, 0x0000);        // > 1MB -> G=1, limit >> 12
  { const uint8_t d[8] = {0x00,0x01,0x67,0x45,0x23,0x92,0x80,0xF1};
    check_desc(ldt_ent(wb), d, "0008h: limit 100000h sets G and stores 100h pages"); }
  int31(0x0008, wb, 0xFFFF, 0xFFFF);        // 4GB
  { const uint8_t d[8] = {0xFF,0xFF,0x67,0x45,0x23,0x92,0x8F,0xF1};
    check_desc(ldt_ent(wb), d, "0008h: limit FFFFFFFFh -> G=1, FFFFFh pages"); }

  int31(0x0009, wb, 0x40F2);                // CL=access F2h, CH=D/B bit
  check(!CF(), "0009h: CF clear");
  { const uint8_t d[8] = {0xFF,0xFF,0x67,0x45,0x23,0xF2,0x4F,0xF1};
    check_desc(ldt_ent(wb), d, "0009h: CL -> access byte, CH high nibble -> flags nibble"); }
  int31(0x0008, wb, 0x0000, 0xFFFF);        // re-limit: D/B must survive, G must clear
  { const uint8_t d[8] = {0xFF,0xFF,0x67,0x45,0x23,0xF2,0x40,0xF1};
    check_desc(ldt_ent(wb), d, "0008h: preserves the D/B flag and clears G"); }

  // 000Bh Get Descriptor -> ES:EDI
  for (uint32_t i = 0; i < 16; i++) mem.store_mem(SCRATCH + 0x100 + i, 0xA5);
  int31(0x000B, wb, 0, 0, 0, SCRATCH + 0x100);
  check(!CF(), "000Bh: CF clear");
  { const uint8_t d[8] = {0xFF,0xFF,0x67,0x45,0x23,0xF2,0x40,0xF1};
    check_desc(SCRATCH + 0x100, d, "000Bh: the 8 descriptor bytes land at ES:EDI"); }
  check_eq(rb(SCRATCH + 0x108), 0xA5, "000Bh: writes exactly 8 bytes, no more");

  // 000Ch Set Descriptor <- ES:EDI
  { const uint8_t src[8] = {0x11,0x22,0x33,0x44,0x55,0x93,0xC6,0x77};
    poke(SCRATCH + 0x200, src, 8);
    int31(0x000C, wb, 0, 0, 0, SCRATCH + 0x200);
    check(!CF(), "000Ch: CF clear");
    check_desc(ldt_ent(wb), src, "000Ch: all 8 bytes from ES:EDI reach the LDT"); }
  int31(0x000B, wb, 0, 0, 0, SCRATCH + 0x100);
  { const uint8_t d[8] = {0x11,0x22,0x33,0x44,0x55,0x93,0xC6,0x77};
    check_desc(SCRATCH + 0x100, d, "000Bh: reads back what 000Ch wrote"); }

  //==========================================================================
  // 7. 000Ah Create Code Segment Alias
  //==========================================================================
  int31(0x000A, 0x000C);                    // alias the client's own CS
  check(!CF(), "000Ah: CF clear");
  const uint16_t alias = AX();
  check(alias != 0x000C && (alias & 4) != 0, "000Ah: returns a new LDT selector");
  { const uint8_t d[8] = {0xFF,0xFF,0x00,0x00,0x03,0xF2,0x00,0x00};
    check_desc(ldt_ent(alias), d,
      "000Ah: alias keeps base/limit/flags, access becomes F2h (data RW, DPL3)"); }
  { const uint8_t d[8] = {0xFF,0xFF,0x00,0x00,0x03,0x9B,0x00,0x00};
    check_desc(ldt_ent(0x000C), d, "000Ah: the source code selector is untouched"); }
  // DPMI 0.9 says the alias carries the DPL of the segment it aliases.  This
  // host hard-codes DPL=3 while every descriptor it creates elsewhere is DPL=0.
  check_eq((rb(ldt_ent(alias) + 5) >> 5) & 3, 3,
           "divergence: 000Ah forces DPL=3 (source descriptor is DPL=0)");

  //==========================================================================
  // 8. 0100h/0101h/0102h DOS memory blocks
  //==========================================================================
  INSTALL_INT21(INT21_ALLOC_OK);
  // 0100h is the one service here that reflects into real mode, so it is also
  // where the reflection stack can be watched.  dpmi_reflect_to_rm() reserves
  // eight 512-byte locked stacks at physical 7000h..8000h; zero both that
  // window and the 64KB-higher one before the call and see which gets written.
  for (uint32_t a = 0x7000;  a < 0x8000;  a++) mem.store_mem(a, 0);
  for (uint32_t a = 0x17000; a < 0x18000; a++) mem.store_mem(a, 0);

  int31(0x0100, 0x0010);                    // 16 paragraphs

  { uint32_t hit_window = 0, hit_stray = 0;
    for (uint32_t a = 0x7000;  a < 0x8000;  a++) if (rb(a) && !hit_window) hit_window = a;
    for (uint32_t a = 0x17000; a < 0x18000; a++) if (rb(a) && !hit_stray)  hit_stray  = a;
    g_checks++;
    if (hit_stray || !hit_window) {
      g_failures++;
      std::printf("  FAIL: reflection: the real-mode interrupt frame must be pushed inside "
                  "the reserved\n        7000h-8000h locked-stack window "
                  "(in-window write: %s%05X, stray write: %s%05X)\n",
                  hit_window ? "" : "none ", hit_window,
                  hit_stray ? "" : "none ", hit_stray);
    }
  }
  check(!CF(), "0100h: CF clear on success");
  check_eq(AX(), 0x6000, "0100h: AX = real-mode segment from INT 21h AH=48h");
  const uint16_t dos_sel = DX();
  check(dos_sel != 0 && (dos_sel & 4) != 0, "0100h: DX = a fresh LDT selector");
  { const uint8_t d[8] = {0xFF,0x00,0x00,0x00,0x06,0x92,0x00,0x00};
    check_desc(ldt_ent(dos_sel), d,
      "0100h: descriptor base 60000h, limit 16*16-1 = FFh, data RW"); }
  check(m.dpmi.dos_blocks[0].allocated, "0100h: block tracked");
  check_eq(m.dpmi.dos_blocks[0].segment, 0x6000, "0100h: tracked segment");
  check_eq(m.dpmi.dos_blocks[0].paragraphs, 0x0010, "0100h: tracked size");
  check_eq(m.dpmi.dos_blocks[0].selector, dos_sel, "0100h: tracked selector");
  check_eq(m.sregs[E::seg_ES], 0x0008, "0100h: protected-mode ES survives the reflection");

  int31(0x0102, 0x0020, 0, dos_sel);        // resize to 32 paragraphs
  check(!CF(), "0102h: CF clear");
  { const uint8_t d[8] = {0xFF,0x01,0x00,0x00,0x06,0x92,0x00,0x00};
    check_desc(ldt_ent(dos_sel), d, "0102h: limit becomes 32*16-1 = 1FFh"); }
  check_eq(m.dpmi.dos_blocks[0].paragraphs, 0x0020, "0102h: tracked size updated");

  int31(0x0102, 0x0020, 0, 0x0FFC);         // unknown selector
  check(CF(), "0102h: CF set for an unknown selector");
  check_eq(AX(), 0x0009, "0102h: AX=0009h (invalid memory block address)");

  INSTALL_INT21(INT21_FREE);
  mem.store_mem16(RM_INT21_LOG, 0x0000);
  int31(0x0101, 0, 0, dos_sel);
  check(!CF(), "0101h: CF clear");
  check_eq(rw(RM_INT21_LOG), 0x6000,
           "0101h: INT 21h AH=49h saw ES = the block's real-mode segment");
  { const uint8_t d[8] = {0,0,0,0,0,0,0,0};
    check_desc(ldt_ent(dos_sel), d, "0101h: the block's descriptor is zeroed"); }
  check(!m.dpmi.dos_blocks[0].allocated, "0101h: block untracked");
  check_eq(m.sregs[E::seg_ES], 0x0008, "0101h: protected-mode ES restored after the reflection");

  int31(0x0101, 0, 0, dos_sel);             // already freed
  check(CF(), "0101h: CF set freeing an unknown selector");
  check_eq(AX(), 0x0009, "0101h: AX=0009h for an unknown selector");

  INSTALL_INT21(INT21_ALLOC_FAIL);
  int31(0x0100, 0x2000);
  check(CF(), "0100h: CF set when DOS cannot satisfy the request");
  check_eq(AX(), 0x0008, "0100h: AX=0008h (insufficient memory)");
  check_eq(BX(), 0x0100, "0100h: BX = largest available block, from INT 21h");
  INSTALL_INT21(INT21_ALLOC_OK);

  //==========================================================================
  // 8b. 0300h simulate real-mode interrupt, 0301h/0302h call real-mode
  //     procedure with a FAR / IRET return frame
  //==========================================================================
  const uint32_t RMCS = SCRATCH + 0x300;    // DPMI real-mode call structure
  auto clear_rmcs = [&]() {
    for (uint32_t i = 0; i < 0x34; i++) mem.store_mem(RMCS + i, 0);
  };

  INSTALL_INT21(INT21_ALLOC_OK);
  clear_rmcs();
  mem.store_mem32(RMCS + 0x1C, 0x00004800); // EAX = AH 48h
  mem.store_mem32(RMCS + 0x10, 0x00000010); // EBX = 16 paragraphs
  mem.store_mem16(RMCS + 0x22, 0x1111);     // ES
  mem.store_mem16(RMCS + 0x24, 0x2222);     // DS
  int31(0x0300, 0x0021, 0, 0, 0, RMCS);
  check(!CF(), "0300h: CF clear");
  check_eq(rd(RMCS + 0x1C), 0x00006000, "0300h: EAX from the real-mode handler is written back");
  check_eq(rw(RMCS + 0x20) & 1, 0, "0300h: FLAGS image carries the handler's clear CF");
  check_eq(rw(RMCS + 0x22), 0x1111, "0300h: ES written back from the real-mode run");
  check_eq(rw(RMCS + 0x24), 0x2222, "0300h: DS written back from the real-mode run");
  check_eq(AX(), 0x0300, "0300h: protected-mode EAX restored after the call");
  check_eq(BX(), 0x0021, "0300h: protected-mode EBX restored after the call");
  check(m.protected_mode(), "0300h: back in protected mode");
  check_eq(m.sregs[E::seg_CS], 0x000C, "0300h: protected-mode CS restored");
  check_eq(m.sregs[E::seg_ES], 0x0008, "0300h: protected-mode ES restored");

  INSTALL_INT21(INT21_ALLOC_FAIL);
  clear_rmcs();
  mem.store_mem32(RMCS + 0x1C, 0x00004800);
  mem.store_mem32(RMCS + 0x10, 0x00002000);
  int31(0x0300, 0x0021, 0, 0, 0, RMCS);
  check_eq(rd(RMCS + 0x1C), 0x00000008, "0300h: failing handler's EAX propagates");
  check_eq(rd(RMCS + 0x10), 0x00000100, "0300h: failing handler's EBX propagates");
  check_eq(rw(RMCS + 0x20) & 1, 1, "0300h: FLAGS image carries the handler's set CF");
  check(!CF(), "0300h: CF of the INT 31h call itself stays clear on a failing INT");
  INSTALL_INT21(INT21_ALLOC_OK);

  clear_rmcs();
  mem.store_mem16(RMCS + 0x2C, RM_INT21_SEG);   // CS
  mem.store_mem16(RMCS + 0x2A, 0x0040);         // IP -> RETF procedure
  int31(0x0301, 0x0000, 0, 0, 0, RMCS);
  check(!CF(), "0301h: CF clear");
  check_eq(rd(RMCS + 0x1C), 0x0000AA55, "0301h: FAR-return procedure ran and returned EAX");
  check(m.protected_mode(), "0301h: back in protected mode");

  clear_rmcs();
  mem.store_mem16(RMCS + 0x2C, RM_INT21_SEG);
  mem.store_mem16(RMCS + 0x2A, 0x0050);         // IP -> IRET procedure
  int31(0x0302, 0x0000, 0, 0, 0, RMCS);
  check(!CF(), "0302h: CF clear");
  check_eq(rd(RMCS + 0x1C), 0x0000A55A, "0302h: IRET-frame procedure ran and returned EAX");
  check(m.protected_mode(), "0302h: back in protected mode");

  //==========================================================================
  // 9. 0200h/0201h real-mode vectors, 0204h/0205h PM vectors,
  //    0202h/0203h exception handlers
  //==========================================================================
  int31(0x0200, 0x0060);
  check(!CF(), "0200h: CF clear");
  check_eq(CX(), 0xBEEF, "0200h: CX = snapshotted segment of IVT[60h]");
  check_eq(DX(), 0x1234, "0200h: DX = snapshotted offset of IVT[60h]");
  int31(0x0200, 0x0160);                    // BH must be ignored
  check_eq(CX(), 0xBEEF, "0200h: only BL selects the vector");

  int31(0x0201, 0x0060, 0x1111, 0x2222);
  check(!CF(), "0201h: CF clear");
  check_eq(m.dpmi.rm_int_seg[0x60], 0x1111, "0201h: host cache segment updated");
  check_eq(m.dpmi.rm_int_off[0x60], 0x2222, "0201h: host cache offset updated");
  check_eq(rw(0x60 * 4),     0x2222, "0201h: the real IVT offset is written too");
  check_eq(rw(0x60 * 4 + 2), 0x1111, "0201h: the real IVT segment is written too");
  int31(0x0200, 0x0060);
  check_eq(CX(), 0x1111, "0200h: reads back what 0201h set (segment)");
  check_eq(DX(), 0x2222, "0200h: reads back what 0201h set (offset)");

  int31(0x0204, 0x0075);                    // no handler installed
  check(!CF(), "0204h: CF clear");
  check_eq(CX(), 0x0028, "0204h: CX = BIOS ROM selector for the default stub");
  check_eq(EDX(), 0xED00u + 0x75 * 4, "0204h: EDX = the default RETF stub for that vector");
  check(!m.dpmi.pm_int_installed[0x75], "0204h: reading does not install anything");

  int31(0x0205, 0x0075, 0x000C, 0x00001234);
  check(!CF(), "0205h: CF clear");
  check(m.dpmi.pm_int_installed[0x75], "0205h: handler marked installed");
  { const uint8_t d[8] = {0x34,0x12,0x0C,0x00,0x00,0x8E,0x00,0x00};
    check_desc(m.dpmi.idt_phys + 0x75 * 8, d,
      "0205h: IDT[75h] rewritten as a 32-bit gate to sel 000Ch:00001234h"); }
  int31(0x0204, 0x0075);
  check_eq(CX(), 0x000C, "0204h: reads back the installed selector");
  check_eq(EDX(), 0x00001234u, "0204h: reads back the installed 32-bit offset");

  // Re-installing the host's own default stub must count as "not installed",
  // so the vector keeps reflecting to real mode instead of hitting a RETF.
  int31(0x0205, 0x0075, 0x0028, 0xED00u + 0x75 * 4);
  check(!m.dpmi.pm_int_installed[0x75],
        "0205h: re-installing the default stub un-installs the vector");
  int31(0x0204, 0x0075);
  check_eq(CX(), 0x0028, "0204h: default stub reported again after un-install");

  int31(0x0202, 0x000E);                    // #PF, nothing installed
  check(!CF(), "0202h: CF clear");
  check_eq(CX(), 0, "0202h: CX=0 when no exception handler is installed");
  check_eq(EDX(), 0, "0202h: EDX=0 when no exception handler is installed");

  int31(0x0203, 0x000E, 0x000C, 0xDEADBEEF);
  check(!CF(), "0203h: CF clear");
  check(m.dpmi.exc_installed[0x0E], "0203h: exception handler marked installed");
  int31(0x0202, 0x000E);
  check_eq(CX(), 0x000C, "0202h: round-trips the handler selector");
  check_eq(EDX(), 0xDEADBEEFu, "0202h: round-trips the 32-bit handler offset");

  int31(0x0203, 0x000E, 0x0000, 0x00000000);
  check(!m.dpmi.exc_installed[0x0E], "0203h: 0000:00000000 uninstalls the handler");
  int31(0x0202, 0x000E);
  check_eq(CX(), 0, "0202h: reports no handler after uninstall");

  int31(0x0203, 0x0020, 0x000C, 0x11112222); // vector 32 is out of range
  check(!CF(), "divergence: 0203h with an exception number >= 32 returns success "
               "and does nothing (DPMI 0.9 says 8021h)");

  //==========================================================================
  // 10. 0500h Get Free Memory Information
  //==========================================================================
  for (uint32_t i = 0; i < 0x40; i++) mem.store_mem(SCRATCH + i, 0x5A);
  int31(0x0500, 0, 0, 0, 0, SCRATCH);
  check(!CF(), "0500h: CF clear");
  // 16MB total - 2MB allocation floor - 128KB of DPMI structures.
  const uint32_t expect_free = 0x1000000u - 0x200000u - 0x20000u;
  check_eq(rd(SCRATCH + 0x00), expect_free, "0500h: largest free block (bytes)");
  check_eq(rd(SCRATCH + 0x04), expect_free / 4096, "0500h: max unlocked pages");
  check_eq(rd(SCRATCH + 0x08), expect_free / 4096, "0500h: max locked pages");
  check_eq(rd(SCRATCH + 0x0C), expect_free / 4096, "0500h: linear address space pages");
  check_eq(rd(SCRATCH + 0x10), 0, "0500h: total unlocked pages");
  check_eq(rd(SCRATCH + 0x14), expect_free / 4096, "0500h: free pages");
  check_eq(rd(SCRATCH + 0x18), 0x1000000u / 4096, "0500h: total physical pages");
  check_eq(rd(SCRATCH + 0x1C), expect_free / 4096, "0500h: free linear address space");
  check_eq(rd(SCRATCH + 0x20), 0xFFFFFFFFu, "0500h: swap file size = none");
  check_eq(rd(SCRATCH + 0x24), 0xFFFFFFFFu, "0500h: reserved field 24h = -1");
  check_eq(rd(SCRATCH + 0x28), 0xFFFFFFFFu, "0500h: reserved field 28h = -1");
  check_eq(rd(SCRATCH + 0x2C), 0xFFFFFFFFu, "0500h: reserved field 2Ch = -1");
  check_eq(rb(SCRATCH + 0x30), 0x5A, "0500h: writes exactly 48 bytes, no more");

  //==========================================================================
  // 11. 0501h/0502h/0503h DPMI memory blocks
  //==========================================================================
  // A deliberately unrounded size, so the next allocation can only land where
  // it does if the host really rounds the cursor up to a page boundary.
  int31(0x0501, 0x0000, 0x1234);
  check(!CF(), "0501h: CF clear");
  const uint32_t b1 = ((uint32_t)BX() << 16) | CX();
  const uint32_t h1 = ((uint32_t)SI() << 16) | DI();
  check_eq(b1, 0x200000, "0501h: first block is at the 2MB floor");
  check_eq(h1, 1, "0501h: first handle is 1");
  check_eq(m.dpmi.next_mem_base, 0x201234, "0501h: allocation cursor advanced by the exact size");
  check(m.dpmi.mem_blocks[0].allocated, "0501h: block slot 0 in use");
  check_eq(m.dpmi.mem_blocks[0].size, 0x1234, "0501h: tracked size");

  int31(0x0501, 0x0000, 0x1000);
  const uint32_t b2 = ((uint32_t)BX() << 16) | CX();
  const uint32_t h2 = ((uint32_t)SI() << 16) | DI();
  check_eq(b2, 0x202000, "0501h: second block starts at the next page, not at the raw cursor");
  check_eq(h2, 2, "0501h: handles increment");
  check(b2 >= b1 + 0x1234, "0501h: two live allocations do not overlap");
  check((b2 & 0xFFF) == 0, "0501h: allocations are page aligned");

  int31(0x0501, 0x0100, 0x0000);            // 16MB - cannot fit
  check(CF(), "0501h: CF set when the request will not fit");
  check_eq(AX(), 0x8012, "0501h: AX=8012h (linear memory unavailable)");

  int31(0x0502, 0, 0, 0, h1 >> 16, h1 & 0xFFFF);
  check(!CF(), "0502h: CF clear freeing a live handle");
  check(!m.dpmi.mem_blocks[0].allocated, "0502h: slot released");
  int31(0x0502, 0, 0, 0, h1 >> 16, h1 & 0xFFFF);
  check(CF(), "0502h: CF set freeing the same handle twice");
  check_eq(AX(), 0x8023, "0502h: AX=8023h (invalid handle) on double free");
  int31(0x0502, 0, 0, 0, 0x0000, 0x9999);
  check(CF(), "0502h: CF set freeing a handle that never existed");
  check_eq(AX(), 0x8023, "0502h: AX=8023h for an unknown handle");

  // Shrink in place, then grow (which relocates and copies).
  mem.store_mem(b2 + 0x000, 0xC3);
  mem.store_mem(b2 + 0x7FF, 0x5E);
  int31(0x0503, 0x0000, 0x0800, 0, h2 >> 16, h2 & 0xFFFF);
  check(!CF(), "0503h: CF clear shrinking");
  check_eq(((uint32_t)BX() << 16) | CX(), b2, "0503h: shrink keeps the base");
  check_eq(((uint32_t)SI() << 16) | DI(), h2, "0503h: shrink keeps the handle");
  check_eq(m.dpmi.mem_blocks[1].size, 0x800, "0503h: tracked size shrank");

  int31(0x0503, 0x0000, 0x4000, 0, h2 >> 16, h2 & 0xFFFF);
  check(!CF(), "0503h: CF clear growing");
  const uint32_t b2b = ((uint32_t)BX() << 16) | CX();
  check_eq(b2b, 0x203000, "0503h: grow relocates to the next free page");
  check(b2b != b2, "0503h: grow past the old size relocates the block");
  check_eq(((uint32_t)SI() << 16) | DI(), h2, "0503h: grow keeps the handle");
  check_eq(rb(b2b + 0x000), 0xC3, "0503h: grow copies the old contents (first byte)");
  check_eq(rb(b2b + 0x7FF), 0x5E, "0503h: grow copies the old contents (last byte)");
  check_eq(m.dpmi.mem_blocks[1].size, 0x4000, "0503h: tracked size grew");
  check_eq(m.dpmi.next_mem_base, 0x207000, "0503h: allocation cursor past the new block");

  int31(0x0503, 0x0000, 0x1000, 0, 0x0000, 0x9999);
  check(CF(), "0503h: CF set for an unknown handle");
  check_eq(AX(), 0x8023, "0503h: AX=8023h for an unknown handle");
  int31(0x0503, 0x0100, 0x0000, 0, h2 >> 16, h2 & 0xFFFF);   // grow to 16MB
  check(CF(), "0503h: CF set when the grow will not fit");
  check_eq(AX(), 0x8012, "0503h: AX=8012h when the grow will not fit");

  //==========================================================================
  // 12. 0900h/0901h/0902h virtual interrupt state
  //==========================================================================
  check(m.dpmi.vif, "vif: still enabled before the 09xx tests");
  int31(0x0902);
  check(!CF(), "0902h: CF clear");
  check_eq(AL(), 1, "0902h: AL=1 while virtual interrupts are enabled");
  check(m.dpmi.vif, "0902h: pure query, state unchanged");

  int31(0x0900);
  check_eq(AL(), 1, "0900h: returns the PREVIOUS state (enabled)");
  check(!m.dpmi.vif, "0900h: leaves virtual interrupts disabled");
  int31(0x0902);
  check_eq(AL(), 0, "0902h: AL=0 while virtual interrupts are disabled");
  int31(0x0900);
  check_eq(AL(), 0, "0900h: returns the PREVIOUS state (already disabled)");
  check(!m.dpmi.vif, "0900h: still disabled");

  int31(0x0901);
  check_eq(AL(), 0, "0901h: returns the PREVIOUS state (disabled)");
  check(m.dpmi.vif, "0901h: leaves virtual interrupts enabled");
  int31(0x0901);
  check_eq(AL(), 1, "0901h: returns the PREVIOUS state (already enabled)");
  int31(0x0902);
  check_eq(AL(), 1, "0902h: AL=1 again");
  check(!CF(), "0901h/0902h: CF clear");

  //==========================================================================
  // 13. 0305h/0306h, 0800h/0801h, 06xxh, 0A00h, unsupported functions
  //==========================================================================
  int31(0x0306);
  check(!CF(), "0306h: CF clear");
  check_eq(BX(), 0xF000, "0306h: BX = real-to-protected switch segment");
  check_eq(CX(), 0xEFDC, "0306h: CX = real-to-protected switch offset");
  check_eq(SI(), 0x0028, "0306h: SI = protected-to-real switch selector (BIOS ROM CS)");
  check_eq(EDI(), 0xEFE4u, "0306h: EDI = protected-to-real switch offset");
  check_eq(rb(0xF0000 + 0xEFE4), 0xF1, "0306h: PM->RM target is a BIOS trap");
  check_eq(rb(0xF0000 + 0xEFE5), 0xFB, "0306h: PM->RM target carries the raw-switch marker");

  int31(0x0305);
  check(!CF(), "0305h: CF clear");
  check_eq(AX(), 0, "0305h: AX = 0 (state buffer size)");
  check_eq(BX(), 0, "divergence: 0305h returns 0000:0000 as the real-mode "
                    "save/restore address (DPMI 0.9 wants a callable address)");
  check_eq(CX(), 0, "0305h: CX = 0");
  check_eq(SI(), 0x0010, "0305h: SI = ring-0 code selector");
  check_eq(EDI(), 0, "0305h: EDI = 0");

  int31(0x0800, 0x000B, 0x8000);
  check(!CF(), "0800h: CF clear");
  check_eq(BX(), 0x000B, "0800h: identity mapping returns BX unchanged");
  check_eq(CX(), 0x8000, "0800h: identity mapping returns CX unchanged");
  int31(0x0801);
  check(!CF(), "0801h: CF clear");
  for (uint16_t f = 0x0600; f <= 0x0603; f++) {
    int31(f);
    check(!CF(), "06xxh page locking succeeds (no paging in this host)");
  }

  int31(0x0A00);
  check(CF(), "0A00h: CF set (no vendor API implemented)");
  check_eq(AX(), 0x8001, "0A00h: AX=8001h (unsupported function)");

  int31(0x0B00);
  check(CF(), "0B00h: CF set (debug watchpoints unsupported)");
  check_eq(AX(), 0x8001, "0B00h: AX=8001h");
  int31(0x0B03);
  check(CF(), "0B03h: CF set (debug watchpoints unsupported)");
  check_eq(AX(), 0x8001, "0B03h: AX=8001h");
  int31(0x0EFF);
  check(CF(), "unknown function: CF set");
  check_eq(AX(), 0x8001, "unknown function: AX=8001h");
  int31(0x0004);                            // a real DPMI hole
  check(CF(), "0004h (reserved): CF set");
  check_eq(AX(), 0x8001, "0004h (reserved): AX=8001h");

  //==========================================================================
  // 14. 0303h/0304h real-mode callbacks
  //==========================================================================
  int31(0x0303);
  check(!CF(), "0303h: CF clear on the first callback");
  check_eq(CX(), 0x0000, "0303h: callback segment is 0000h");
  check_eq(DX(), 0x6800, "0303h: first callback offset is 6800h");
  check_eq(rb(0x6800), 0xCD, "0303h: thunk starts with INT");
  check_eq(rb(0x6801), 0xFF, "0303h: thunk uses vector FFh");
  check_eq(rb(0x6802), 0xCB, "0303h: thunk ends with RETF");
  check_eq(rw(0xFF * 4 + 2), 0xF000, "0303h: IVT[FFh] hooked to the BIOS segment");
  check_eq(rw(0xFF * 4), ivt_ff_off, "0303h: IVT[FFh] points at the BIOS trap stub");

  int31(0x0303);
  check_eq(DX(), 0x6804, "0303h: second callback is 4 bytes further on");

  // Drain the rest, then confirm the pool really is exhausted.
  int taken = 2;
  for (int i = 0; i < 32; i++) { int31(0x0303); if (CF()) break; taken++; }
  check(CF(), "0303h: CF set once every callback slot is taken");
  check_eq(AX(), 0x8015, "0303h: AX=8015h (callback unavailable)");
  check_eq(taken, 16, "0303h: the host provides exactly 16 real-mode callbacks");

  // The error path, asserted as hard as the happy one: 0304h has to reject an
  // address it never handed out rather than corrupting the allocation record.
  int31(0x0304, 0, 0x1234, 0x6800);         // wrong segment
  check(CF(), "0304h: CF set for a callback in the wrong segment");
  check_eq(AX(), 0x8024, "0304h: AX=8024h (invalid callback address) for a bad segment");
  int31(0x0304, 0, 0x0000, 0x6802);         // inside a thunk, not on its boundary
  check(CF(), "0304h: CF set for an unaligned callback offset");
  check_eq(AX(), 0x8024, "0304h: AX=8024h for an unaligned offset");
  int31(0x0304, 0, 0x0000, 0x6700);         // below the thunk area
  check(CF(), "0304h: CF set for an offset below the thunk area");
  int31(0x0304, 0, 0x0000, 0x6800 + 16 * 4);  // one past the last slot
  check(CF(), "0304h: CF set for an offset past the last slot");

  int31(0x0304, 0, 0x0000, 0x6800);         // hand the first one back
  check(!CF(), "0304h: CF clear");
  int31(0x0304, 0, 0x0000, 0x6800);         // ... and it is not still allocated
  check(CF(), "0304h: CF set on a double free");
  check_eq(AX(), 0x8024, "0304h: AX=8024h on a double free");
  // 0304h must actually reclaim the slot, and the allocation record must live
  // in DpmiState -- per session and per machine.  A client that allocates and
  // frees a callback in a loop (a mouse or timer handler being re-hooked)
  // would otherwise die after 16, and a second DPMI program in the same
  // process would start with however many the first one used.
  int31(0x0303);
  check(!CF(), "0303h: a callback freed by 0304h must become available again");

  //==========================================================================
  // 14b. The descriptor services validate the selector they are handed
  //==========================================================================
  // Until 2026-08-27 none of 0006h-000Ch looked at BX before indexing the LDT
  // with it.  `sel >> 3' runs to 8191 where the table holds 2048 entries, so a
  // GDT selector indexed the LDT anyway and an out-of-range index read or wrote
  // up to 48KB past the end of the table - guest-controlled, and what sits there
  // in this layout is the GDT, the IDT and the TSS.  tests/README.md recorded
  // this as unassertable ("a test could only pin the out-of-bounds access").
  // It is assertable now that the answer is an error code.
  {
    // A GDT selector: TI (bit 2) clear.  0x0008 is GDT entry 1, a real entry in
    // this machine's GDT, so the old code would have read the LDT slot with the
    // same index and returned a plausible-looking base.
    static const struct { uint16_t sel; const char *what; } bad[] = {
      { 0x0008, "a GDT selector (TI clear)" },
      { 0x0000, "the null selector" },
      { 0x0004, "LDT index 0" },
      { 0x4004, "LDT index 2048, one past the end" },
      { 0xFFFF, "LDT index 8191, the largest a 16-bit selector can express" },
    };
    for (const auto &b : bad) {
      char msg[160];
      // 0006h Get Segment Base: a read past the LDT.
      int31(0x0006, b.sel);
      std::snprintf(msg, sizeof msg, "0006h rejects %s with CF", b.what);
      check(CF(), msg);
      std::snprintf(msg, sizeof msg, "0006h returns 8022h for %s", b.what);
      check_eq(AX(), 0x8022, msg);
      // 0007h Set Segment Base: a WRITE past the LDT, which is the one that
      // actually corrupts the machine.
      int31(0x0007, b.sel, 0xDEAD, 0xBEEF);
      std::snprintf(msg, sizeof msg, "0007h rejects %s with CF", b.what);
      check(CF(), msg);
      std::snprintf(msg, sizeof msg, "0007h returns 8022h for %s", b.what);
      check_eq(AX(), 0x8022, msg);
    }
    // The remaining five, once each, against the worst of the five above.
    int31(0x0008, 0xFFFF, 0, 0x1000);
    check(CF() && AX() == 0x8022, "0008h rejects an out-of-range LDT index");
    int31(0x0009, 0xFFFF, 0x00F2);
    check(CF() && AX() == 0x8022, "0009h rejects an out-of-range LDT index");
    int31(0x000A, 0xFFFF);
    check(CF() && AX() == 0x8022, "000Ah rejects an out-of-range LDT index");
    int31(0x000B, 0xFFFF, 0, 0, 0, 0x2000);
    check(CF() && AX() == 0x8022, "000Bh rejects an out-of-range LDT index");
    int31(0x000C, 0xFFFF, 0, 0, 0, 0x2000);
    check(CF() && AX() == 0x8022, "000Ch rejects an out-of-range LDT index");

    // 000Ah must not leak a selector on the rejection path: it allocates the
    // alias only after the source selector is accepted.
    int free_now = 0;
    for (int i = 1; i < 2048; i++)
      if (!(m.dpmi.ldt_alloc[i / 8] & (1 << (i % 8)))) free_now++;
    int31(0x000A, 0xFFFF);
    check(CF(), "000Ah still rejects on a second try");
    int free_after = 0;
    for (int i = 1; i < 2048; i++)
      if (!(m.dpmi.ldt_alloc[i / 8] & (1 << (i % 8)))) free_after++;
    check_eq((uint32_t)free_after, (uint32_t)free_now,
             "000Ah leaks no LDT selector when it rejects the source");

    // And a good selector still works, so the guard rejects only what it should.
    int31(0x0000, 0, 1);
    check(!CF(), "0000h still allocates after the rejections");
    const uint16_t good = AX();
    int31(0x0007, good, 0x0012, 0x3456);
    check(!CF(), "0007h still accepts a freshly allocated LDT selector");
    int31(0x0006, good);
    check(!CF(), "0006h still accepts a freshly allocated LDT selector");
    check_eq(((uint32_t)CX() << 16) | DX(), 0x00123456u,
             "0006h reads back the base 0007h wrote");
    int31(0x0001, good);
    check(!CF(), "0001h frees it again");
  }

  //==========================================================================
  // 15. LDT exhaustion
  //==========================================================================
  int free_before = 0;
  for (int i = 1; i < 2048; i++)
    if (!(m.dpmi.ldt_alloc[i / 8] & (1 << (i % 8)))) free_before++;
  check(free_before > 0 && free_before < 2048, "exhaustion: some selectors are still free");

  int handed_out = 0;
  for (int i = 0; i < 2100; i++) {
    int31(0x0000, 0, 1);
    if (CF()) break;
    handed_out++;
  }
  check_eq(handed_out, (uint32_t)free_before,
           "exhaustion: 0000h hands out exactly the free selectors, no more");
  check(CF(), "exhaustion: CF set once the LDT is full");
  check_eq(AX(), 0x8011, "exhaustion: AX=8011h (descriptor unavailable)");

  bool all_taken = true;
  for (int i = 1; i < 2048; i++)
    if (!(m.dpmi.ldt_alloc[i / 8] & (1 << (i % 8)))) all_taken = false;
  check(all_taken, "exhaustion: every LDT index 1..2047 is allocated");
  check(!(m.dpmi.ldt_alloc[0] & 1), "exhaustion: LDT index 0 (null) is never handed out");

  int31(0x0000, 0, 2);
  check(CF(), "exhaustion: 0000h with count=2 also fails");
  check_eq(AX(), 0x8011, "exhaustion: AX=8011h for a multi-descriptor request");
  int31(0x0002, 0x7000);
  check(CF(), "exhaustion: 0002h fails when no descriptor is free");
  check_eq(AX(), 0x8011, "exhaustion: 0002h reports 8011h");
  int31(0x000A, 0x000C);
  check(CF(), "exhaustion: 000Ah fails when no descriptor is free");
  check_eq(AX(), 0x8011, "exhaustion: 000Ah reports 8011h");

  // 0100h has no such guard: it hands back a null selector and reports success.
  int31(0x0100, 0x0010);
  check(!CF(), "divergence: 0100h still reports success with the LDT full");
  check_eq(DX(), 0x0000, "divergence: 0100h returns selector 0000h when the LDT is full");

  //==========================================================================
  // 16. INT 21h AH=4Ch from protected mode - end of the DPMI session
  //==========================================================================
  const uint16_t ivt50_off = m.dpmi.rm_int_off[0x50];
  check_eq(m.dpmi.rm_int_seg[0x50], 0xF000, "terminate: IVT[50h] snapshot is the BIOS stub");
  mem.store_mem16(0x50 * 4,     0xDEAD);    // client scribbles on the IVT
  mem.store_mem16(0x50 * 4 + 2, 0xBEEF);

  INSTALL_INT21(INT21_HALT);
  m.set_reg32(E::reg_AX, 0x4C05);
  run_guest(OFF_INT21);

  check(!m.dpmi.active, "terminate: session ended");
  check(!m.protected_mode(), "terminate: back in real mode");
  check_eq(m.cr0 & E::CR0_PE, 0, "terminate: CR0.PE cleared");
  check_eq(AX(), 0x4C05, "terminate: AX = 4C00h | exit code, ready for INT 21h");
  check_eq(m.sregs[E::seg_CS], RM_INT21_SEG, "terminate: jumped to the real-mode INT 21h handler");
  check_eq(m.ip, 1, "terminate: the real-mode handler ran");
  check_eq(m.sregs[E::seg_SS], STACK_SEG, "terminate: real-mode SS restored");
  check_eq(m.get_reg32(E::reg_SP), 0xFFF8u, "terminate: real-mode SP restored, minus the IRET frame");
  check_eq(m.sregs[E::seg_DS], PSP_SEG, "terminate: DS = client PSP");
  check_eq(m.sregs[E::seg_ES], PSP_SEG, "terminate: ES = client PSP");
  check_eq(m.seg_cache[E::seg_DS].base, (uint32_t)PSP_SEG * 16, "terminate: DS base is real-mode again");
  check(!m.get_flag(E::FLAG_IF), "terminate: IF cleared for the handler, as INT would");
  check_eq(rw(0x20000 + 0xFFFA), 0xF000, "terminate: pushed sentinel CS = F000h");
  check_eq(rw(0x20000 + 0xFFF8), ivt_ff_off,
           "terminate: pushed the BIOS halt sentinel as the return offset");
  check_eq(rw(0x20000 + 0xFFFC) & (uint16_t)E::FLAG_IF, (uint16_t)E::FLAG_IF,
           "terminate: the pushed FLAGS image still has IF set");
  check_eq(rw(0x50 * 4),     ivt50_off, "terminate: the scribbled IVT entry is restored (offset)");
  check_eq(rw(0x50 * 4 + 2), 0xF000, "terminate: the scribbled IVT entry is restored (segment)");
  check_eq(rw(0x60 * 4),     0x2222, "terminate: IVT restored from the host's cache, "
                                     "including what 0201h changed (offset)");
  check_eq(rw(0x60 * 4 + 2), 0x1111, "terminate: IVT restored from the host's cache, "
                                     "including what 0201h changed (segment)");

  //==========================================================================
  check(g_stuck == 0, "every guest stub reached its HLT");
  check(g_exceptions == 0, "no unexpected CPU exception during any stub");

  std::printf("\n");
  if (g_bugs || g_bugs_fixed) {
    std::printf("known bugs still present: %d (expected %d)\n",
                g_bugs, KNOWN_BUGS_EXPECTED);
  }
  bool baseline_ok = (g_bugs == KNOWN_BUGS_EXPECTED);
  if (!baseline_ok) {
    std::printf("FAIL: known-bug count is %d, baseline says %d - %s\n",
                g_bugs, KNOWN_BUGS_EXPECTED,
                g_bugs_fixed ? "a bug was fixed; lower KNOWN_BUGS_EXPECTED"
                             : "update the baseline deliberately");
  }
  if (g_bugs) {
    std::printf(
      "  Every BUG line above asserts the DPMI 0.9 behaviour against a defect that is\n"
      "  in emu88/dos_dpmi.cc today.  They are meant to be red until the defect is\n"
      "  fixed, at which point this harness fails and tells you to lower the\n"
      "  baseline.\n");
  }
  if (g_failures == 0 && baseline_ok) {
    std::printf("ALL DPMI TESTS PASS (%d checks, %d known bugs held at baseline)\n",
                g_checks, g_bugs);
    return 0;
  }
  std::printf("DPMI TESTS FAILED: %d of %d checks failed\n", g_failures, g_checks);
  return 1;
}
