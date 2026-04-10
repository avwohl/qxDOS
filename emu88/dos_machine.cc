#include "dos_machine.h"
#include <cstdio>
#include <cstring>

//=============================================================================
// Constructor
//=============================================================================

dos_machine::dos_machine(emu88_mem *memory, dos_io *io)
    : emu88(memory), io(io),
      video_mode(3), screen_cols(80), screen_rows(25),
      pic_imr(0xFF), pic_vector_base(0x08), pic_init_step(0),
      port_b(0), crtc_index(0),
      tick_cycle_mark(0), refresh_cycle_mark(0),
      speed_mode(SPEED_FULL), target_cps(0),
      banner_shown(false),
      waiting_for_key(false),
      kbd_poll_count(0),
      kbd_poll_start_cycle(0),
      kbd_cmd_pending(0),
      cmos_index(0),
      nic(nullptr), ne2000_base(0x300), ne2000_irq(3)
{
  memset(pit_counter, 0, sizeof(pit_counter));
  memset(pit_reload, 0, sizeof(pit_reload));
  memset(pit_load_cycle, 0, sizeof(pit_load_cycle));
  memset(pit_mode, 0, sizeof(pit_mode));
  memset(pit_access, 0, sizeof(pit_access));
  memset(pit_write_phase, 0, sizeof(pit_write_phase));
  memset(pit_read_phase, 0, sizeof(pit_read_phase));
  memset(pit_latch_pending, 0, sizeof(pit_latch_pending));
  memset(pit_latch_value, 0, sizeof(pit_latch_value));
  memset(crtc_regs, 0, sizeof(crtc_regs));
  memset(bios_entry, 0, sizeof(bios_entry));
  memset(vga_dac, 0, sizeof(vga_dac));
  memset(cmos_data, 0, sizeof(cmos_data));
  vga_seq_index = 0;
  memset(vga_seq_regs, 0, sizeof(vga_seq_regs));
  vga_gc_index = 0;
  memset(vga_gc_regs, 0, sizeof(vga_gc_regs));
  memset(modex_composite, 0, sizeof(modex_composite));
  dac_write_index = 0;
  dac_read_index = 0;
  dac_component = 0;
  dac_pel_mask = 0xFF;
  adlib_index = 0;
  memset(adlib_regs, 0, sizeof(adlib_regs));
  adlib_status = 0;
  adlib_timer1_running = false;
  adlib_timer1_start_cycle = 0;
  memset(sb_dsp_data, 0, sizeof(sb_dsp_data));
  sb_dsp_data_head = sb_dsp_data_tail = sb_dsp_data_count = 0;
  sb_dsp_cmd = 0;
  sb_dsp_cmd_pending = false;
  sb_dsp_reset_active = false;
}

dos_machine::~dos_machine() {
  delete nic;
}

// Simulate PIT counter decrement based on elapsed CPU cycles.
// PIT runs at 1.193182 MHz; we approximate as cycles/4 for 4.77 MHz base.
uint16_t dos_machine::pit_current_count(int ch) const {
  // PIT count of 0 means 65536 — must use uint32_t to hold this
  uint32_t reload = pit_reload[ch] ? pit_reload[ch] : 65536u;
  uint64_t elapsed_cycles = cycles - pit_load_cycle[ch];
  // PIT ticks ≈ CPU cycles / 4 (1.193 MHz vs 4.77 MHz)
  uint64_t pit_ticks = elapsed_cycles / 4;
  uint32_t pos = (uint32_t)(pit_ticks % reload);
  uint16_t count = (uint16_t)((reload - pos) & 0xFFFF);
  // Counter reads as 0 only transiently; for mode 2/3 it reloads immediately
  return count ? count : (uint16_t)(reload & 0xFFFF);
}

//=============================================================================
// BDA helpers
//=============================================================================

void dos_machine::bda_w8(int off, uint8_t v) {
  mem->store_mem(0x400 + off, v);
}
void dos_machine::bda_w16(int off, uint16_t v) {
  mem->store_mem16(0x400 + off, v);
}
void dos_machine::bda_w32(int off, uint32_t v) {
  mem->store_mem16(0x400 + off, v & 0xFFFF);
  mem->store_mem16(0x400 + off + 2, (v >> 16) & 0xFFFF);
}
uint8_t dos_machine::bda_r8(int off) {
  return mem->fetch_mem(0x400 + off);
}
uint16_t dos_machine::bda_r16(int off) {
  return mem->fetch_mem16(0x400 + off);
}
uint32_t dos_machine::bda_r32(int off) {
  return mem->fetch_mem16(0x400 + off) |
         ((uint32_t)mem->fetch_mem16(0x400 + off + 2) << 16);
}

//=============================================================================
// CMOS initialization
//=============================================================================

void dos_machine::init_cmos() {
  memset(cmos_data, 0, sizeof(cmos_data));

  uint32_t total = mem->get_mem_size();
  uint32_t base_kb = total >= 0xA0000 ? 640 : total / 1024;
  uint32_t ext_kb = total > 0x100000 ? (total - 0x100000) / 1024 : 0;
  if (ext_kb > 0xFFFF) ext_kb = 0xFFFF;

  // Equipment byte: FPU present, boot from disk, VGA
  cmos_data[0x14] = 0x2F;  // 80x25 VGA, 1 floppy, FPU

  // Base memory (in KB, typically 640)
  cmos_data[0x15] = base_kb & 0xFF;
  cmos_data[0x16] = (base_kb >> 8) & 0xFF;

  // Extended memory (KB above 1MB) - two register pairs report same value
  cmos_data[0x17] = ext_kb & 0xFF;
  cmos_data[0x18] = (ext_kb >> 8) & 0xFF;
  cmos_data[0x30] = ext_kb & 0xFF;
  cmos_data[0x31] = (ext_kb >> 8) & 0xFF;

  // Diagnostic status: POST OK
  cmos_data[0x0E] = 0x00;
  // Shutdown status: normal
  cmos_data[0x0F] = 0x00;

  // Hard disk type: 47 = user-defined (in high nibble for drive 0)
  cmos_data[0x12] = 0xF0;  // Type 15 (>8 heads) for drive 0
  cmos_data[0x19] = 47;    // Extended type for drive 0
}

//=============================================================================
// Configuration
//=============================================================================

void dos_machine::configure(const Config &cfg) {
  config = cfg;
  cpu_type = cfg.cpu;
  lock_ud = (cfg.cpu >= CPU_386);
  set_speed(cfg.speed);
}

//=============================================================================
// Machine initialization
//=============================================================================

void dos_machine::init_machine() {
  reset();
  init_ivt();
  init_bda();
  install_bios_stubs();

  // Initialize PIC (8259A) - normally done by BIOS POST
  // ICW1→ICW2→ICW3→ICW4, then set IMR
  pic_vector_base = 0x08;  // IRQ 0-7 → INT 08h-0Fh
  pic_init_step = 0;
  pic_icw4_needed = false;
  pic_imr = 0xBC;  // Unmask IRQ0 (timer), IRQ1 (keyboard), IRQ6 (floppy)

  // Initialize PIT (8254) - channel 0 to mode 3 (square wave), count 0 (=65536)
  // This matches what a real BIOS does during POST for the 18.2 Hz timer tick
  pit_access[0] = 3;      // lobyte/hibyte
  pit_mode[0] = 3;        // square wave generator
  pit_counter[0] = 0;     // count 0 = 65536
  pit_reload[0] = 0;      // 0 means 65536
  pit_load_cycle[0] = 0;  // loaded at cycle 0
  pit_write_phase[0] = 0;
  pit_read_phase[0] = 0;

  // Initialize CMOS with memory size and equipment info
  init_cmos();

  // Install mouse if enabled
  if (config.mouse_enabled && io->mouse_present()) {
    mouse.installed = true;
    mouse.x = 320;
    mouse.y = 100;
    mouse.buttons = 0;
    mouse.visible = false;
  }

  // Initialize NE2000 NIC if enabled
  if (config.ne2000_enabled) {
    if (!nic) nic = new ne2000();
    nic->reset();
    uint8_t mac[] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
    nic->set_mac(mac);
    ne2000_base = config.ne2000_iobase;
    ne2000_irq = config.ne2000_irq;
    nic->on_transmit = [this](const uint8_t *data, int len) {
      io->net_send(data, len);
    };
  }
}

void dos_machine::init_ivt() {
  // Clear IVT (256 vectors x 4 bytes = 1KB)
  for (int i = 0; i < 0x400; i++)
    mem->store_mem(i, 0);

  // Place BIOS trap stubs in ROM at F000:E000 + vector*4
  // Each stub: BIOS_TRAP_OPCODE, vector_number, IRET(0xCF)
  uint16_t rom_base = 0xE000;

  for (int vec = 0; vec < 256; vec++) {
    uint16_t entry = rom_base + vec * 4;
    uint32_t addr = BIOS_ROM_BASE + entry;
    mem->store_mem(addr,     BIOS_TRAP_OPCODE);
    mem->store_mem(addr + 1, (uint8_t)vec);
    mem->store_mem(addr + 2, 0xCF);  // IRET fallback
    bios_entry[vec] = entry;
  }

  // Set ALL IVT entries to point to BIOS ROM stubs (IRET fallbacks)
  // This prevents unhandled INTs from jumping to 0000:0000
  for (int vec = 0; vec < 256; vec++) {
    uint32_t ivt_addr = vec * 4;
    mem->store_mem16(ivt_addr,     bios_entry[vec]);
    mem->store_mem16(ivt_addr + 2, 0xF000);
  }

  // PM default INT handler stubs at F000:ED00 + vec*4
  // Each stub: F1 (BIOS trap), vector_byte, CB (RETF)
  // Used as "previous handler" when DPMI clients chain interrupts.
  // Called via CALL FAR so they must RETF (not IRET).
  for (int vec = 0; vec < 256; vec++) {
    uint16_t entry = 0xED00 + vec * 4;
    uint32_t addr = BIOS_ROM_BASE + entry;
    mem->store_mem(addr,     BIOS_TRAP_OPCODE);
    mem->store_mem(addr + 1, (uint8_t)vec);
    mem->store_mem(addr + 2, 0xCB);  // RETF (for CALL FAR chains)
    pm_int_default_entry[vec] = entry;
  }

  // INT 1Eh -> Disk parameter table (set up in install_bios_stubs)
  // INT 1Fh -> Graphics char table (not needed for text modes)
}

void dos_machine::init_bda() {
  for (int i = 0; i < 256; i++)
    mem->store_mem(0x400 + i, 0);

  // Equipment word: bit0=floppy present, bits4-5=initial video mode
  // 00=EGA/VGA, 01=40x25 CGA, 10=80x25 CGA, 11=MDA/Hercules
  uint16_t equip = 0x0001;  // floppy present
  bool use_mda = (config.display == DISPLAY_MDA || config.display == DISPLAY_HERCULES);
  bool use_ega_vga = (config.display == DISPLAY_EGA || config.display == DISPLAY_VGA);
  if (use_mda) {
    equip |= 0x0030;  // bits 4-5 = 11 (MDA)
  } else if (use_ega_vga) {
    equip |= 0x0000;  // bits 4-5 = 00 (EGA/VGA)
  } else {
    equip |= 0x0020;  // bits 4-5 = 10 (80x25 CGA)
  }
  // Mouse: bit 2 (PS/2 pointing device)
  if (config.mouse_enabled)
    equip |= 0x0004;
  bda_w16(bda::EQUIPMENT, equip);

  // Conventional memory: 640KB
  bda_w16(bda::MEM_SIZE_KB, 640);

  // Video setup based on display adapter
  int init_mode = use_mda ? 7 : 3;
  bda_w8(bda::VIDEO_MODE, init_mode);
  bda_w16(bda::SCREEN_COLS, 80);
  bda_w16(bda::VIDEO_PAGE_SZ, 4096);
  bda_w16(bda::VIDEO_PAGE_OFF, 0);
  bda_w8(bda::ACTIVE_PAGE, 0);
  bda_w16(bda::CRTC_BASE, use_mda ? 0x3B4 : 0x3D4);
  bda_w8(bda::SCREEN_ROWS, 24);  // 25 rows - 1
  bda_w16(bda::CURSOR_SHAPE, 0x0607);

  // Keyboard buffer (empty circular buffer)
  bda_w16(bda::KBD_BUF_HEAD, 0x1E);
  bda_w16(bda::KBD_BUF_TAIL, 0x1E);
  bda_w16(bda::KBD_BUF_START, 0x1E);
  bda_w16(bda::KBD_BUF_END, 0x3E);

  // Hard disk count
  int nhdd = 0;
  for (int d = 0x80; d < 0x84; d++)
    if (io->disk_present(d)) nhdd++;
  bda_w8(bda::NUM_HDD, nhdd);
}

void dos_machine::install_bios_stubs() {
  // BIOS date at F000:FFF5
  const char *date = "03/08/26";
  for (int i = 0; i < 8; i++)
    mem->store_mem(BIOS_ROM_BASE + 0xFFF5 + i, date[i]);

  // Model ID at F000:FFFE (0xFF = IBM PC)
  mem->store_mem(BIOS_ROM_BASE + 0xFFFE, 0xFF);

  // Reset vector at F000:FFF0 -> JMP FAR to bootstrap trap
  uint16_t boot_entry = bios_entry[0x19];  // INT 19h entry
  uint32_t reset = BIOS_ROM_BASE + 0xFFF0;
  mem->store_mem(reset,     0xEA);         // JMP FAR
  mem->store_mem16(reset+1, boot_entry);   // offset
  mem->store_mem16(reset+3, 0xF000);       // segment

  // System configuration table at F000:E6F5 (for INT 15h AH=C0h)
  uint32_t sct = BIOS_ROM_BASE + 0xE6F5;
  uint8_t sct_data[] = {
    0x08, 0x00,  // Table length (8 bytes following)
    0xFF,        // Model ID (0xFF = IBM PC)
    0x00,        // Submodel (0x00)
    0x01,        // BIOS revision level
    0x74,        // Feature byte 1: DMA ch3, cascade int, RTC, kbd intercept
    0x00,        // Feature byte 2
    0x00,        // Feature byte 3
    0x00, 0x00   // Reserved
  };
  for (int i = 0; i < 10; i++)
    mem->store_mem(sct + i, sct_data[i]);

  // Disk parameter table at F000:EFC7 (for INT 1Eh)
  uint32_t dpt = BIOS_ROM_BASE + 0xEFC7;
  uint8_t dpt_data[] = {
    0xDF, 0x02, 0x25, 0x02, 18, 0x1B, 0xFF, 0x54, 0xF6, 0x0F, 0x08
  };
  for (int i = 0; i < 11; i++)
    mem->store_mem(dpt + i, dpt_data[i]);
  mem->store_mem16(0x1E * 4,     0xEFC7);
  mem->store_mem16(0x1E * 4 + 2, 0xF000);

  // XMS entry point at F000:EFD8 - called via FAR CALL by XMS clients
  // Uses BIOS trap opcode (0xF1) with vector 0xFE (reserved for XMS dispatch)
  // Then RETF to return to caller
  xms_entry_off = 0xEFD8;
  uint32_t xms_addr = BIOS_ROM_BASE + xms_entry_off;
  mem->store_mem(xms_addr,     BIOS_TRAP_OPCODE);  // 0xF1
  mem->store_mem(xms_addr + 1, 0xFE);              // XMS vector marker
  mem->store_mem(xms_addr + 2, 0xCB);              // RETF

  // Initialize XMS handles
  for (int i = 0; i < XMS_MAX_HANDLES; i++) {
    xms_handles[i].allocated = false;
    xms_handles[i].base = 0;
    xms_handles[i].size_kb = 0;
    xms_handles[i].lock_count = 0;
  }

  // DPMI mode switch entry point at F000:EFDC
  // F1 FD = BIOS trap opcode + DPMI mode switch marker
  // CB    = RETF (never reached; trap handler sets up PM and returns directly)
  dpmi.mode_switch_off = 0xEFDC;
  uint32_t dpmi_sw_addr = BIOS_ROM_BASE + dpmi.mode_switch_off;
  mem->store_mem(dpmi_sw_addr,     BIOS_TRAP_OPCODE);
  mem->store_mem(dpmi_sw_addr + 1, 0xFD);  // DPMI mode switch marker
  mem->store_mem(dpmi_sw_addr + 2, 0xCB);  // RETF

  // DPMI real-mode callback return stub at F000:EFE0
  // F1 FC = BIOS trap opcode + DPMI RM return marker
  // CF    = IRET (never reached)
  dpmi.rm_return_off = 0xEFE0;
  uint32_t dpmi_ret_addr = BIOS_ROM_BASE + dpmi.rm_return_off;
  mem->store_mem(dpmi_ret_addr,     BIOS_TRAP_OPCODE);
  mem->store_mem(dpmi_ret_addr + 1, 0xFC);  // DPMI RM return marker
  mem->store_mem(dpmi_ret_addr + 2, 0xCF);  // IRET

  // DPMI raw PM→RM mode switch trap at F000:EFE4
  // F1 FB = BIOS trap opcode + PM→RM raw switch marker
  // CB    = RETF (never reached)
  dpmi.raw_pm_to_rm_off = 0xEFE4;
  uint32_t dpmi_raw_addr = BIOS_ROM_BASE + dpmi.raw_pm_to_rm_off;
  mem->store_mem(dpmi_raw_addr,     BIOS_TRAP_OPCODE);
  mem->store_mem(dpmi_raw_addr + 1, 0xFB);  // PM→RM raw switch marker
  mem->store_mem(dpmi_raw_addr + 2, 0xCB);  // RETF (never reached)

  // DPMI exception handler return trap at F000:EFE8
  // F1 FA = BIOS trap opcode + exception return marker
  dpmi.exc_return_off = 0xEFE8;
  uint32_t dpmi_exc_ret_addr = BIOS_ROM_BASE + dpmi.exc_return_off;
  mem->store_mem(dpmi_exc_ret_addr,     BIOS_TRAP_OPCODE);
  mem->store_mem(dpmi_exc_ret_addr + 1, 0xFA);  // Exception return marker
  mem->store_mem(dpmi_exc_ret_addr + 2, 0xCB);  // RETF (never reached)

  dpmi.active = false;
}

//=============================================================================
// Boot
//=============================================================================

bool dos_machine::boot(int drive) {
  init_machine();

  if (!io->disk_present(drive)) {
    fprintf(stderr, "Boot drive 0x%02X not present\n", drive);
    return false;
  }

  // Read boot sector to 0000:7C00
  uint8_t sector[512];
  size_t n = io->disk_read(drive, 0, sector, 512);
  if (n < 512) {
    fprintf(stderr, "Failed to read boot sector\n");
    return false;
  }
  if (sector[510] != 0x55 || sector[511] != 0xAA)
    fprintf(stderr, "Warning: boot sector missing 55AA signature\n");

  for (int i = 0; i < 512; i++)
    mem->store_mem(BOOT_LOAD_ADDR + i, sector[i]);

  // Set CPU state for boot
  sregs[seg_CS] = 0x0000;
  ip = 0x7C00;
  sregs[seg_DS] = 0x0000;
  sregs[seg_ES] = 0x0000;
  sregs[seg_SS] = 0x0000;
  regs[reg_SP] = 0x7C00;
  regs[reg_DX] = drive;   // DL = boot drive
  halted = false;

  bool use_mda = (config.display == DISPLAY_MDA || config.display == DISPLAY_HERCULES);
  int init_mode = use_mda ? 7 : 3;
  video_set_mode(init_mode);
  banner_shown = false;

  io->video_mode_changed(init_mode, 80, 25);
  return true;
}

//=============================================================================
// Run loop
//=============================================================================

void dos_machine::set_speed(SpeedMode mode) {
  speed_mode = mode;
  // CPS values for 386+ speeds are inflated ~3-4x to compensate for
  // the 8088-based cycle table (8088 instructions cost ~3x more cycles
  // than 386 instructions, so we need higher CPS to match wall-clock speed).
  switch (mode) {
    case SPEED_FULL:       target_cps = 0; break;
    case SPEED_PC_4_77:    target_cps = 4770000; break;
    case SPEED_AT_8:       target_cps = 8000000; break;
    case SPEED_386SX_16:   target_cps = 48000000; break;
    case SPEED_386DX_33:   target_cps = 100000000; break;
    case SPEED_486DX2_66:  target_cps = 260000000; break;
  }
}

bool dos_machine::run_batch(int count) {
  waiting_for_key = false;
  // Note: kbd_poll_count is NOT reset here — it must accumulate across
  // batches so the AH=01 polling threshold can be reached.  Non-keyboard
  // interrupts in do_interrupt() reset it when the program does real work.

  for (int i = 0; i < count; i++) {
    if (waiting_for_key) {
      // Program is genuinely idle at a keyboard prompt (passed both
      // poll count AND emulated time thresholds). Yield to host.

      // Deliver timer ticks at real-time 18.2 Hz rate using wall clock.
      // We do NOT fast-forward the cycle counter because inflated cycles
      // confuse the host-side speed throttle (causes multi-second key
      // echo latency).  Wall-clock ticks let guest timeouts (e.g.
      // FreeDOS F5/F8 boot prompt) expire at the correct real-time rate.
      auto now = std::chrono::steady_clock::now();
      if (idle_tick_time == std::chrono::steady_clock::time_point{})
        idle_tick_time = now;
      auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
          now - idle_tick_time).count();
      if (elapsed_us >= 54925) {  // 1/18.2 Hz ≈ 54.925ms
        idle_tick_time += std::chrono::microseconds(54925);
        uint32_t ticks = bda_r32(bda::TIMER_COUNT) + 1;
        if (ticks >= 0x1800B0) {
          ticks = 0;
          bda_w8(bda::TIMER_ROLLOVER, 1);
        }
        bda_w32(bda::TIMER_COUNT, ticks);
        if (get_flag(FLAG_IF) && !(pic_imr & 0x01))
          request_int(pic_vector_base);
      }
      // Keep tick_cycle_mark in sync so the normal timer check doesn't
      // fire immediately when the CPU resumes after idle.
      tick_cycle_mark = cycles;

      if (video_mode == 0x13) {
        if (mem->vga_planar) {
          // Mode X: composite 4 planes into linear buffer
          // CRTC start address determines which page is displayed
          uint16_t crtc_start = (crtc_regs[12] << 8) | crtc_regs[13];
          for (int i = 0; i < 320 * 200; i++)
            modex_composite[i] = mem->vga_planes[i & 3][crtc_start + (i >> 2)];
          io->video_refresh_gfx(modex_composite, 320, 200, vga_dac);
        } else {
          io->video_refresh_gfx(mem->get_mem() + VGA_VRAM_BASE, 320, 200, vga_dac);
        }
      } else {
        uint32_t base = vram_base();
        io->video_refresh(mem->get_mem() + base, screen_cols, screen_rows);
        int page = bda_r8(bda::ACTIVE_PAGE);
        uint16_t pos = bda_r16(bda::CURSOR_POS + page * 2);
        io->video_set_cursor((pos >> 8) & 0xFF, pos & 0xFF);
      }
      // Reset poll count so the CPU gets a full batch of instructions
      // on the next call before re-triggering idle.  This lets timer-based
      // timeouts (e.g. FreeDOS F5/F8 boot prompt) make progress.
      kbd_poll_count = 0;
      return true;
    }

    // Handle HLT: fast-forward to next timer tick and deliver interrupt,
    // then exit batch so the host can yield CPU time (DOSBox-style idle)
    if (halted) {
      if (!get_flag(FLAG_IF)) return false;  // HLT with IF=0 is permanent halt
      cycles = tick_cycle_mark + CYCLES_PER_TICK;
    } else {
      execute();
      static long exec_count = 0;
      exec_count++;
      if (exec_count == 1 || exec_count == 100000 || exec_count == 1000000 ||
          (exec_count % 10000000 == 0))
        fprintf(stderr, "[EXEC-COUNT] execute() called %ld times, halted=%d, IP=%04X:%08X cycles=%llu planar=%d vmode=%d\n",
                exec_count, halted, sregs[seg_CS], ip, cycles, mem->vga_planar?1:0, video_mode);
      // Dump boot sector at the stuck address
      if (exec_count == 100000 && ip == 0x7C31) {
        fprintf(stderr, "[BOOT-STUCK] Code at 7C20-7C50:\n");
        for (int a = 0x7C20; a < 0x7C50; a += 16) {
          fprintf(stderr, "  %04X:", a);
          for (int b = 0; b < 16; b++)
            fprintf(stderr, " %02X", mem->fetch_mem(a + b));
          fprintf(stderr, "\n");
        }
        fprintf(stderr, "[BOOT-STUCK] AX=%04X BX=%04X CX=%04X DX=%04X flags=%04X\n",
                regs[reg_AX], regs[reg_BX], regs[reg_CX], regs[reg_DX], flags);
      }
    }

    // Timer tick (cycle-based: 18.2 Hz at 4.77 MHz)
    if (cycles - tick_cycle_mark >= CYCLES_PER_TICK) {
      tick_cycle_mark = cycles;
      uint32_t ticks = bda_r32(bda::TIMER_COUNT) + 1;
      if (ticks >= 0x1800B0) {
        ticks = 0;
        bda_w8(bda::TIMER_ROLLOVER, 1);
      }
      bda_w32(bda::TIMER_COUNT, ticks);

      static int timer_log = 0;
      if (timer_log < 10 || (timer_log % 1000 == 0)) {
        // Check DOOM's ticcount at linear 0x498E3C (CS_base 0x400000 + 0x98E3C)
        uint32_t ticcount_addr = 0x498E3C;
        int32_t doom_ticcount = (mem->get_mem_size() > ticcount_addr + 4) ?
          (int32_t)(mem->fetch_mem(ticcount_addr) | (mem->fetch_mem(ticcount_addr+1) << 8) |
                    (mem->fetch_mem(ticcount_addr+2) << 16) | (mem->fetch_mem(ticcount_addr+3) << 24)) : -1;
        // Check BDA timer count and VGA framebuffer
        uint32_t bda_timer = bda_r32(bda::TIMER_COUNT);
        uint32_t vga_sum = 0;
        if (video_mode == 0x13) {
          if (mem->vga_planar) {
            for (int i = 0; i < 16000; i += 250)
              vga_sum += mem->vga_planes[0][i];
          } else {
            for (int i = 0; i < 320*200; i += 1000)
              vga_sum += mem->fetch_mem(VGA_VRAM_BASE + i);
          }
        }
        fprintf(stderr, "[TIMER-TICK] #%d vec=0x%02X imr=0x%02X IF=%d pm=%d ticcount=%d bda=%u vga=%u CS:IP=%04X:%08X cycles=%llu\n",
                timer_log, pic_vector_base, pic_imr, get_flag(FLAG_IF)?1:0, protected_mode()?1:0, doom_ticcount,
                bda_timer, vga_sum, sregs[seg_CS], ip, cycles);
      }
      timer_log++;

      if (get_flag(FLAG_IF) && !(pic_imr & 0x01))
        request_int(pic_vector_base);

      // After delivering the timer tick that wakes from HLT, exit the
      // batch so the host run loop can yield (saves battery on mobile)
      if (halted) {
        check_interrupts();  // deliver the tick interrupt to un-halt
        break;
      }
    }

    // Video refresh (cycle-based: ~30 Hz)
    if (cycles - refresh_cycle_mark >= CYCLES_PER_REFRESH) {
      refresh_cycle_mark = cycles;
      if (video_mode == 0x13) {
        if (mem->vga_planar) {
          // Mode X: composite 4 planes into linear buffer
          // CRTC start address determines which page is displayed
          uint16_t crtc_start = (crtc_regs[12] << 8) | crtc_regs[13];
          for (int i = 0; i < 320 * 200; i++)
            modex_composite[i] = mem->vga_planes[i & 3][crtc_start + (i >> 2)];
          io->video_refresh_gfx(modex_composite, 320, 200, vga_dac);
        } else {
          io->video_refresh_gfx(mem->get_mem() + VGA_VRAM_BASE, 320, 200, vga_dac);
        }
      } else {
        uint32_t base = vram_base();
        io->video_refresh(mem->get_mem() + base, screen_cols, screen_rows);

        int page = bda_r8(bda::ACTIVE_PAGE);
        uint16_t pos = bda_r16(bda::CURSOR_POS + page * 2);
        io->video_set_cursor((pos >> 8) & 0xFF, pos & 0xFF);
      }
    }

    // NE2000: poll for incoming packets and deliver IRQ (every 1024 insns)
    if (nic && (i & 0x3FF) == 0) {
      if (io->net_available()) {
        uint8_t pkt[1600];
        int len = io->net_receive(pkt, sizeof(pkt));
        if (len > 0) nic->receive(pkt, len);
      }
      if (nic->irq_active() && get_flag(FLAG_IF) &&
          !(pic_imr & (1 << ne2000_irq)))
        request_int(pic_vector_base + ne2000_irq);
    }

    check_interrupts();
  }
  // HLT with IF=1 is just "waiting for interrupt" (idle), not a dead halt
  return !halted || get_flag(FLAG_IF);
}

//=============================================================================
// Interrupt dispatch - BIOS trapping
//=============================================================================

void dos_machine::dispatch_bios(uint8_t vector) {
  // Reset keyboard poll counter on interrupts that indicate program activity.
  // Exclude timer (08/1C), time (1A), keyboard (16), and video (10).
  // Video is excluded because vask-style polling loops interleave INT 16h AH=01
  // (check key) with INT 10h AH=02 (cursor blink) - resetting on INT 10h would
  // prevent the poll counter from ever reaching the threshold.
  if (vector != 0x08 && vector != 0x1C && vector != 0x1A && vector != 0x16
      && vector != 0x10)
    kbd_poll_count = 0;

  switch (vector) {
    case 0x08: bios_int08h(); break;
    case 0x10: bios_int10h(); break;
    case 0x11: bios_int11h(); break;
    case 0x12: bios_int12h(); break;
    case 0x13: bios_int13h(); break;
    case 0x14: bios_int14h(); break;
    case 0x15: bios_int15h(); break;
    case 0x16: bios_int16h(); break;
    case 0x17: bios_int17h(); break;
    case 0x19: bios_int19h(); break;
    case 0x1A: bios_int1ah(); break;
    case 0x1C: break;  // User timer hook - default is no-op
    case 0x2F: bios_int2fh(); break;
    case 0x33: bios_int33h(); break;
    case 0xE0: bios_int_e0h(); break;  // Host file services
    default: break;
  }
}

void dos_machine::do_interrupt(emu88_uint8 vector) {
  // Debug: count interrupts to verify do_interrupt is called
  static long int_count = 0;
  int_count++;
  if (int_count == 1 || int_count == 100000 || int_count == 1000000)
    fprintf(stderr, "[INT-COUNT] do_interrupt called %ld times, vector=%02X pm=%d\n", int_count, vector, protected_mode());

  // Trace DPMI and program termination
  if (vector == 0x2F && !protected_mode()) {
    uint16_t ax = regs[reg_AX];
    if (ax == 0x1687) {
      static int dpmi_log = 0;
      if (dpmi_log++ < 5)
        fprintf(stderr, "[DPMI-DETECT] INT 2Fh AX=1687h called from %04X:%04X (all regs: AX=%04X BX=%04X CX=%04X DX=%04X SI=%04X DI=%04X DS=%04X ES=%04X)\n",
                sregs[seg_CS], insn_ip,
                regs[reg_AX], regs[reg_BX], regs[reg_CX], regs[reg_DX],
                regs[reg_SI], regs[reg_DI], sregs[seg_DS], sregs[seg_ES]);
      // Set up post-return trace to log what the handler returns
      int2f_1687_trace_pending = true;
      int2f_trace_ret_cs = sregs[seg_CS];
      int2f_trace_ret_ip = ip;  // ip already points past INT 2Fh (CD 2F = 2 bytes)
      // Enable real-mode trace after DPMI detection (reduced)
      rm_trace_count = 200;
    }
  }
  if (vector == 0x21 && !protected_mode()) {
    uint8_t ah = regs[reg_AX] >> 8;
    if (ah == 0x4C) {
      fprintf(stderr, "[EXIT] INT 21h AH=4Ch AL=%02X from %04X:%04X\n",
              regs[reg_AX] & 0xFF, sregs[seg_CS], insn_ip);
      // Dump VGA text buffer to see error messages
      fprintf(stderr, "[EXIT] VGA text screen:\n");
      for (int row = 0; row < 25; row++) {
        char line[81];
        bool has_content = false;
        for (int col = 0; col < 80; col++) {
          uint8_t ch = mem->fetch_mem(0xB8000 + (row * 80 + col) * 2);
          line[col] = (ch >= 0x20 && ch < 0x7F) ? ch : ' ';
          if (ch > 0x20 && ch < 0x7F) has_content = true;
        }
        line[80] = 0;
        if (has_content) fprintf(stderr, "[SCR %02d] %s\n", row, line);
      }
    } else if (ah == 0x4B) {
      // EXEC - load and execute program
      // DS:DX = filename
      uint32_t fn_addr = ((uint32_t)sregs[seg_DS] << 4) + regs[reg_DX];
      char fn[64];
      for (int i = 0; i < 63; i++) {
        fn[i] = mem->fetch_mem(fn_addr + i);
        if (!fn[i]) break;
      }
      fn[63] = 0;
      fprintf(stderr, "[EXEC] INT 21h AH=4Bh AL=%02X file='%s' from %04X:%04X\n",
              regs[reg_AX] & 0xFF, fn, sregs[seg_CS], insn_ip);
    }
  }
  // Log divide-by-zero exceptions with full context for debugging
  if (vector == 0) {
    fprintf(stderr, "[DIV0] %s CS=%04X IP=%04X AX=%04X BX=%04X CX=%04X DX=%04X "
            "SI=%04X DI=%04X SP=%04X BP=%04X DS=%04X ES=%04X SS=%04X "
            "EAX=%08X EDX=%08X cycles=%llu\n",
            protected_mode() ? "PM" : "RM",
            sregs[seg_CS], ip,
            regs[reg_AX], regs[reg_BX], regs[reg_CX], regs[reg_DX],
            regs[reg_SI], regs[reg_DI], regs[reg_SP], regs[reg_BP],
            sregs[seg_DS], sregs[seg_ES], sregs[seg_SS],
            get_reg32(reg_AX), get_reg32(reg_DX), cycles);
  }

  // DPMI detection — intercept INT 2Fh AX=1687h before any chain
  if (vector == 0x2F && !protected_mode() && regs[reg_AX] == 0x1687) {
    bios_int2fh();
    return;
  }

  // In protected mode, skip the IVT fast-path check — interrupts go through IDT
  if (protected_mode()) {
    emu88::do_interrupt(vector);
    return;
  }

  // Check if IVT still points to our BIOS stub (not hooked by DOS)
  uint32_t ivt = vector * 4;
  uint16_t off = mem->fetch_mem16(ivt);
  uint16_t seg = mem->fetch_mem16(ivt + 2);

  if (seg == 0xF000 && off == bios_entry[vector]) {
    // Fast path: trap directly, no push/jump
    dispatch_bios(vector);
    if (waiting_for_key) {
      // Rewind IP to re-execute this INT instruction next batch.
      // IP currently points past the INT xx bytes (2 bytes).
      ip -= 2;
    }
    return;
  }

  // Hooked by DOS/TSR - let normal interrupt flow happen.
  // Our ROM stub will catch it via unimplemented_opcode when the
  // chain reaches our entry point.
  // Reset keyboard poll counter since non-BIOS interrupt = program activity
  // (exclude timer, time, video, and keyboard itself - these are not
  // indicators of program activity and shouldn't reset the idle detector)
  if (vector != 0x08 && vector != 0x1C && vector != 0x1A && vector != 0x10 && vector != 0x16)
    kbd_poll_count = 0;
  emu88::do_interrupt(vector);
}

//=============================================================================
// Unimplemented opcode - catches BIOS ROM trap stubs
//=============================================================================

void dos_machine::unimplemented_opcode(emu88_uint8 opcode) {
  if (opcode == BIOS_TRAP_OPCODE) {
    uint8_t vector = fetch_ip_byte();
    if (vector == 0xFE) {
      // XMS entry point - reached via FAR CALL, not INT
      // Stack has IP, CS (no FLAGS) - RETF follows in ROM
      xms_dispatch();
      return;
    }
    if (vector == 0xFD) {
      if (dpmi.active) {
        // Raw RM→PM mode switch (DPMI already initialized)
        dpmi_raw_rm_to_pm();
      } else {
        // Initial DPMI mode switch entry - reached via FAR CALL in real mode
        dpmi_mode_switch();
      }
      return;  // Mode switch sets IP directly; skip the RETF
    }
    if (vector == 0xFB) {
      // Raw PM→RM mode switch - reached via FAR JMP from protected mode
      dpmi_raw_pm_to_rm();
      return;
    }
    if (vector == 0xFA) {
      // DPMI exception handler return sentinel
      // Reached via RETF from the client's exception handler.
      // The RETF already popped return EIP and CS (our sentinel),
      // so ESP now points to the error code field of the exception frame.
      //
      // Frame layout at current ESP:
      //   +00: Error code
      //   +04: Faulting EIP (possibly modified by handler)
      //   +08: Faulting CS
      //   +0C: Faulting EFLAGS
      //   +10: Faulting ESP
      //   +14: Faulting SS

      // SS-relative offset → physical address
      uint32_t frame_phys = seg_cache[seg_SS].base + get_esp();
      /* uint32_t err  = mem->fetch_mem32(frame_phys + 0x00); */ // not needed
      uint32_t new_eip    = mem->fetch_mem32(frame_phys + 0x04);
      uint16_t new_cs     = (uint16_t)mem->fetch_mem32(frame_phys + 0x08);
      uint32_t new_eflags = mem->fetch_mem32(frame_phys + 0x0C);
      uint32_t new_esp    = mem->fetch_mem32(frame_phys + 0x10);
      uint16_t new_ss     = (uint16_t)mem->fetch_mem32(frame_phys + 0x14);

      fprintf(stderr, "[DPMI-EXC] Handler RETF: restoring %04X:%08X EFLAGS=%08X SS:ESP=%04X:%08X\n",
              new_cs, new_eip, new_eflags, new_ss, new_esp);

      // Restore CS
      sregs[seg_CS] = new_cs;
      {
        uint16_t idx = new_cs >> 3;
        bool use_ldt = (new_cs & 4) != 0;
        uint32_t tbase = use_ldt ? ldtr_cache.base : gdtr_base;
        uint8_t desc[8];
        read_descriptor(tbase, idx, desc);
        parse_descriptor(desc, seg_cache[seg_CS]);
      }
      ip = new_eip;

      // Restore SS
      sregs[seg_SS] = new_ss;
      {
        uint16_t idx = new_ss >> 3;
        bool use_ldt = (new_ss & 4) != 0;
        uint32_t tbase = use_ldt ? ldtr_cache.base : gdtr_base;
        uint8_t desc[8];
        read_descriptor(tbase, idx, desc);
        parse_descriptor(desc, seg_cache[seg_SS]);
      }
      set_esp(new_esp);

      // Restore EFLAGS
      flags = (uint16_t)(new_eflags & 0xFFFF);
      eflags_hi = (uint16_t)((new_eflags >> 16) & 0xFFFF);

      // Restore DS, ES, FS, GS from saved pre-exception state
      sregs[seg_DS] = dpmi.exc_save_ds;
      sregs[seg_ES] = dpmi.exc_save_es;
      sregs[seg_FS] = dpmi.exc_save_fs;
      sregs[seg_GS] = dpmi.exc_save_gs;
      seg_cache[seg_DS] = dpmi.exc_save_seg_cache[seg_DS];
      seg_cache[seg_ES] = dpmi.exc_save_seg_cache[seg_ES];
      seg_cache[seg_FS] = dpmi.exc_save_seg_cache[seg_FS];
      seg_cache[seg_GS] = dpmi.exc_save_seg_cache[seg_GS];

      dpmi.exc_handler_done = true;
      return;
    }
    if (vector == 0xFC) {
      // DPMI real-mode callback return sentinel
      // Reached via IRET/RETF from a real-mode handler during dpmi_exec_rm
      dpmi.in_rm_callback = false;
      return;  // The nested execute loop in dpmi_exec_rm will exit
    }
    // Normal BIOS trap stub (reached via interrupt chain)
    dispatch_bios(vector);
    if (waiting_for_key) {
      // Don't IRET — rewind IP to the start of this F1 xx stub so the
      // trap re-fires on the next batch.  The stack (return address from
      // the INT/CALL that got us here) stays intact; when a key finally
      // arrives, the handler sets AX and waiting_for_key stays false,
      // so we fall through to the IRET below and return to the caller.
      ip -= 2;
      return;
    }
    if (protected_mode()) {
      // In PM, BIOS stubs are reached via 32-bit CALL FAR from DPMI clients.
      // The caller pushed 32-bit CS and 32-bit EIP. Pop them manually since
      // the stub's CS segment is 16-bit and RETF would pop 16-bit values.
      uint32_t ret_eip = pop_dword();
      uint16_t ret_cs = (uint16_t)pop_dword();
      ip = ret_eip;
      load_segment(seg_CS, ret_cs);
      return;
    }
    // Real mode: IRET — pop IP, CS, FLAGS (pushed by the INT instruction).
    // The BIOS handler modified `flags` directly (e.g. CF for error status).
    // Merge those result flags into the saved FLAGS so callers see them —
    // this matters when reached through the interrupt chain or via V86
    // reflection (DPMI servers like CWSDPMI).
    uint16_t bios_flags = flags;
    ip = pop_word();
    load_segment_real(seg_CS, pop_word());
    flags = pop_word();
    // Propagate arithmetic/status flags from the BIOS handler; preserve
    // control flags (IF, TF, DF, IOPL) from the saved flags.
    static constexpr uint16_t RESULT_FLAGS =
      FLAG_CF | FLAG_PF | FLAG_AF | FLAG_ZF | FLAG_SF | FLAG_OF;
    flags = (flags & ~RESULT_FLAGS) | (bios_flags & RESULT_FLAGS);
    return;
  }
  emu88::unimplemented_opcode(opcode);
}

//=============================================================================
// Port I/O
//=============================================================================

void dos_machine::port_out(emu88_uint16 port, emu88_uint8 value) {
  switch (port) {
    // --- PIC (8259A) Master ---
    case 0x20:
      if (value & 0x10) {
        pic_init_step = 1;  // ICW1
        pic_icw4_needed = (value & 0x01);
      }
      // else: OCW (EOI etc.)
      break;
    case 0x21:
      if (pic_init_step == 1) {
        pic_vector_base = value; pic_init_step = 2;  // ICW2
        fprintf(stderr, "[PIC] ICW2: vector_base=0x%02X pm=%d\n", value, protected_mode());
      } else if (pic_init_step == 2) {
        pic_init_step = pic_icw4_needed ? 3 : 0;  // ICW3, then ICW4 if needed
      } else if (pic_init_step == 3) {
        pic_init_step = 0;  // ICW4
      } else {
        pic_imr = value;     // OCW1
        fprintf(stderr, "[PIC] IMR=0x%02X pm=%d\n", value, protected_mode());
      }
      break;

    // --- PIT (8253) ---
    case 0x40: case 0x41: case 0x42: {
      int ch = port - 0x40;
      if (pit_access[ch] == 3) {
        if (pit_write_phase[ch] == 0) {
          pit_counter[ch] = (pit_counter[ch] & 0xFF00) | value;
          pit_write_phase[ch] = 1;
        } else {
          pit_counter[ch] = (pit_counter[ch] & 0x00FF) | ((uint16_t)value << 8);
          pit_write_phase[ch] = 0;
          pit_reload[ch] = pit_counter[ch];
          pit_load_cycle[ch] = cycles;
        }
      } else if (pit_access[ch] == 1) {
        pit_counter[ch] = value;
        pit_reload[ch] = pit_counter[ch];
        pit_load_cycle[ch] = cycles;
      } else if (pit_access[ch] == 2) {
        pit_counter[ch] = (uint16_t)value << 8;
        pit_reload[ch] = pit_counter[ch];
        pit_load_cycle[ch] = cycles;
      }
      break;
    }
    case 0x43: {
      int ch = (value >> 6) & 3;
      if (ch == 3) break;
      int access = (value >> 4) & 3;
      if (access == 0) {
        pit_latch_value[ch] = pit_current_count(ch);
        pit_latch_pending[ch] = true;
        pit_read_phase[ch] = 0;
      } else {
        pit_access[ch] = access;
        pit_mode[ch] = (value >> 1) & 7;
        pit_write_phase[ch] = 0;
        pit_read_phase[ch] = 0;
      }
      break;
    }

    // --- Keyboard controller / A20 ---
    case 0x60:
      // Keyboard data port - also used for A20 control
      if (kbd_cmd_pending == 0xD1) {  // Write output port
        bool a20 = (value & 0x02) != 0;
        mem->set_a20(a20);
        kbd_cmd_pending = 0;
      }
      break;
    case 0x61: port_b = value; break;
    case 0x64:
      // Keyboard command port
      if (value == 0xD1) {
        kbd_cmd_pending = 0xD1;  // Next byte to port 0x60 = output port
      } else if (value == 0xDD) {
        mem->set_a20(false);  // Disable A20
      } else if (value == 0xDF) {
        mem->set_a20(true);   // Enable A20
      } else if (value == 0xD0) {
        kbd_cmd_pending = 0xD0;  // Read output port (handled in port_in 0x60)
      }
      break;

    // --- Fast A20 gate (port 0x92) ---
    case 0x92:
      mem->set_a20((value & 0x02) != 0);
      break;

    // --- CMOS RTC ---
    case 0x70:
      cmos_index = value & 0x7F;
      break;
    case 0x71:
      cmos_data[cmos_index] = value;
      break;

    // --- CGA CRTC ---
    case 0x3D4: case 0x3B4:
      crtc_index = value;
      break;
    case 0x3D5: case 0x3B5:
      crtc_regs[crtc_index] = value;
      break;
    case 0x3D8: case 0x3B8: break;  // Mode control
    case 0x3D9: break;               // Color select

    // --- VGA Sequencer ---
    case 0x3C4: vga_seq_index = value; break;
    case 0x3C5:
      vga_seq_regs[vga_seq_index & 7] = value;
      if (vga_seq_index == 2) {
        // Map Mask register — which planes to write
        mem->vga_map_mask = value & 0x0F;
      }
      if (vga_seq_index == 4) {
        // Memory Mode register — bit 3 is chain-4
        bool chain4 = (value & 0x08) != 0;
        mem->vga_planar = !chain4;
      }
      break;

    // --- VGA Graphics Controller ---
    case 0x3CE: vga_gc_index = value; break;
    case 0x3CF:
      vga_gc_regs[vga_gc_index & 0x0F] = value;
      if (vga_gc_index == 4) {
        // Read Map Select register — which plane to read
        mem->vga_read_map = value & 3;
      }
      break;

    // --- VGA DAC ---
    case 0x3C6: dac_pel_mask = value; break;
    case 0x3C7: dac_read_index = value; dac_component = 0; break;
    case 0x3C8: dac_write_index = value; dac_component = 0; break;
    case 0x3C9:
      vga_dac[dac_write_index][dac_component] = value & 0x3F;
      dac_component++;
      if (dac_component >= 3) {
        dac_component = 0;
        dac_write_index++;
      }
      break;

    // --- Adlib / OPL2 (ports 0x388-0x389) ---
    case 0x388:
      adlib_index = value;
      break;
    case 0x389:
      fprintf(stderr, "[ADLIB] write reg 0x%02X = 0x%02X\n", adlib_index, value);
      adlib_regs[adlib_index] = value;
      if (adlib_index == 0x04) {
        // Timer control register
        if (value & 0x80) {
          // Reset IRQ flags
          adlib_status = 0;
        }
        if (value & 0x01) {
          // Start timer 1
          adlib_timer1_running = true;
          adlib_timer1_start_cycle = cycles;
        } else {
          adlib_timer1_running = false;
        }
      }
      break;

    default:
      // Sound Blaster DSP (base 0x220, 16 ports)
      if (port >= sb_base && port < sb_base + 0x10) {
        uint8_t sb_port = port - sb_base;
        switch (sb_port) {
          case 0x06: // DSP Reset
            if (value & 0x01) {
              sb_dsp_reset_active = true;
              sb_dsp_data_count = 0;
              sb_dsp_data_head = sb_dsp_data_tail = 0;
            } else if (sb_dsp_reset_active) {
              sb_dsp_reset_active = false;
              // Queue 0xAA (DSP ready signature)
              sb_dsp_data[sb_dsp_data_tail] = 0xAA;
              sb_dsp_data_tail = (sb_dsp_data_tail + 1) % 16;
              sb_dsp_data_count = 1;
              sb_dsp_cmd_pending = false;
            }
            break;
          case 0x0C: // DSP Write Command/Data
            if (sb_dsp_cmd_pending) {
              // Parameter byte for current command
              switch (sb_dsp_cmd) {
                case 0x40: // Set Time Constant
                  // value = time constant (256 - 1000000/rate)
                  // Just accept and ignore
                  break;
                case 0x48: // Set DMA Block Size (low byte first, then high)
                  break;
                default: break;
              }
              sb_dsp_cmd_pending = false;
            } else {
              sb_dsp_cmd = value;
              switch (value) {
                case 0xE1: // Get DSP Version
                  // SB 2.0 = version 2.01
                  sb_dsp_data[sb_dsp_data_tail] = 0x02;
                  sb_dsp_data_tail = (sb_dsp_data_tail + 1) % 16;
                  sb_dsp_data[sb_dsp_data_tail] = 0x01;
                  sb_dsp_data_tail = (sb_dsp_data_tail + 1) % 16;
                  sb_dsp_data_count += 2;
                  break;
                case 0x40: // Set Time Constant (param follows)
                case 0x48: // Set DMA Block Size (param follows)
                  sb_dsp_cmd_pending = true;
                  break;
                case 0xD1: // Enable speaker
                case 0xD3: // Disable speaker
                case 0xD0: // Halt DMA
                case 0xDA: // Exit auto-init DMA
                  break; // No-op
                case 0x14: // 8-bit single-cycle DMA output
                case 0x1C: // 8-bit auto-init DMA output
                case 0x91: // 8-bit high-speed auto-init DMA
                  sb_dsp_cmd_pending = true;
                  break;
                default: break;
              }
            }
            break;
        }
        return;
      }
      // NE2000 NIC (32 ports at ne2000_base)
      if (nic && port >= ne2000_base && port < ne2000_base + 0x20) {
        nic->iowrite(port - ne2000_base, value);
        return;
      }
      break;
  }
}

emu88_uint8 dos_machine::port_in(emu88_uint16 port) {
  switch (port) {
    // --- PIC ---
    case 0x20: return 0;
    case 0x21: return pic_imr;

    // --- PIT ---
    case 0x40: case 0x41: case 0x42: {
      int ch = port - 0x40;
      uint16_t val = pit_latch_pending[ch] ? pit_latch_value[ch] : pit_current_count(ch);
      if (pit_access[ch] == 3) {
        if (pit_read_phase[ch] == 0) {
          pit_read_phase[ch] = 1;
          return val & 0xFF;
        } else {
          pit_read_phase[ch] = 0;
          pit_latch_pending[ch] = false;
          return (val >> 8) & 0xFF;
        }
      } else if (pit_access[ch] == 1) {
        pit_latch_pending[ch] = false;
        return val & 0xFF;
      } else {
        pit_latch_pending[ch] = false;
        return (val >> 8) & 0xFF;
      }
    }

    // --- Keyboard controller ---
    case 0x60:
      if (kbd_cmd_pending == 0xD0) {
        // Read output port: bit 1 = A20 status
        kbd_cmd_pending = 0;
        return mem->get_a20() ? 0x02 : 0x00;
      }
      // Return buffered scancode from hardware keyboard queue
      if (kbd_hw_head != kbd_hw_tail) {
        kbd_last_scancode = kbd_hw_buf[kbd_hw_head];
        kbd_hw_head = (kbd_hw_head + 1) % KBD_HW_BUF_SIZE;
        // If more scancodes pending, re-trigger IRQ 1
        if (kbd_hw_head != kbd_hw_tail &&
            get_flag(FLAG_IF) && !(pic_imr & 0x02))
          request_int(pic_vector_base + 1);
      }
      return kbd_last_scancode;
    case 0x61: return port_b;
    case 0x64: return 0x14;  // Input buffer empty, self-test passed

    // --- Fast A20 gate ---
    case 0x92: return mem->get_a20() ? 0x02 : 0x00;

    // --- CMOS RTC ---
    case 0x71:
      if (cmos_index >= 0x15 && cmos_index <= 0x35)
        fprintf(stderr, "[CMOS-RD] reg=0x%02X val=0x%02X\n", cmos_index, cmos_data[cmos_index]);
      return cmos_data[cmos_index];

    // --- CGA status ---
    case 0x3DA: {
      static uint8_t toggle = 0;
      toggle ^= 0x09;  // retrace bits toggle
      return toggle;
    }
    // --- MDA status ---
    case 0x3BA: {
      static uint8_t mtoggle = 0;
      mtoggle ^= 0x09;
      return mtoggle;
    }

    // --- VGA Sequencer ---
    case 0x3C4: return vga_seq_index;
    case 0x3C5: return vga_seq_regs[vga_seq_index & 7];

    // --- VGA Graphics Controller ---
    case 0x3CE: return vga_gc_index;
    case 0x3CF: return vga_gc_regs[vga_gc_index & 0x0F];

    // --- VGA DAC ---
    case 0x3C6: return dac_pel_mask;
    case 0x3C7: return 0x03;  // DAC state: read mode
    case 0x3C9: {
      uint8_t val = vga_dac[dac_read_index][dac_component] & 0x3F;
      dac_component++;
      if (dac_component >= 3) {
        dac_component = 0;
        dac_read_index++;
      }
      return val;
    }

    // --- CRTC ---
    case 0x3D5: case 0x3B5:
      return crtc_regs[crtc_index];

    // --- Adlib / OPL2 status ---
    case 0x388: {
      // Update timer 1 status: fires after ~80μs (≈380 CPU cycles at 4.77MHz)
      if (adlib_timer1_running && (cycles - adlib_timer1_start_cycle) > 320) {
        adlib_status |= 0xC0;  // Timer 1 expired + IRQ flag
        adlib_timer1_running = false;
      }
      static int adlib_read_log = 0;
      if (adlib_read_log < 30) {
        adlib_read_log++;
        fprintf(stderr, "[ADLIB] read status = 0x%02X (timer1_running=%d elapsed=%llu)\n",
                adlib_status, adlib_timer1_running, adlib_timer1_running ? cycles - adlib_timer1_start_cycle : 0ULL);
      }
      return adlib_status;
    }

    default:
      // Sound Blaster DSP (base 0x220)
      if (port >= sb_base && port < sb_base + 0x10) {
        uint8_t sb_port = port - sb_base;
        switch (sb_port) {
          case 0x0A: // DSP Read Data
            if (sb_dsp_data_count > 0) {
              uint8_t val = sb_dsp_data[sb_dsp_data_head];
              sb_dsp_data_head = (sb_dsp_data_head + 1) % 16;
              sb_dsp_data_count--;
              return val;
            }
            return 0xFF;
          case 0x0C: // DSP Write Status (bit 7: 0=ready to write)
            return 0x00;  // Always ready
          case 0x0E: // DSP Read Status (bit 7: 1=data available)
            return sb_dsp_data_count > 0 ? 0x80 : 0x00;
          default:
            return 0xFF;
        }
      }
      // NE2000 NIC (32 ports at ne2000_base)
      if (nic && port >= ne2000_base && port < ne2000_base + 0x20)
        return nic->ioread(port - ne2000_base);
      return 0xFF;
  }
}

//=============================================================================
// 16-bit Port I/O (for NE2000 data port)
//=============================================================================

void dos_machine::port_out16(emu88_uint16 port, emu88_uint16 value) {
  if (nic && port >= ne2000_base && port < ne2000_base + 0x20) {
    nic->iowrite16(port - ne2000_base, value);
    return;
  }
  // Default: two byte writes
  port_out(port, value & 0xFF);
  port_out(port + 1, (value >> 8) & 0xFF);
}

emu88_uint16 dos_machine::port_in16(emu88_uint16 port) {
  if (nic && port >= ne2000_base && port < ne2000_base + 0x20)
    return nic->ioread16(port - ne2000_base);
  // Default: two byte reads
  return port_in(port) | ((uint16_t)port_in(port + 1) << 8);
}

//=============================================================================
// Keyboard input from host
//=============================================================================

void dos_machine::queue_key(uint8_t ascii, uint8_t scancode) {
  // BIOS keyboard buffer (for INT 16h)
  uint16_t head = bda_r16(bda::KBD_BUF_HEAD);
  uint16_t tail = bda_r16(bda::KBD_BUF_TAIL);
  uint16_t buf_end = bda_r16(bda::KBD_BUF_END);
  uint16_t buf_start = bda_r16(bda::KBD_BUF_START);

  uint16_t next_tail = tail + 2;
  if (next_tail >= buf_end) next_tail = buf_start;
  if (next_tail == head) return;  // Buffer full

  // Store: low byte = ASCII, high byte = scancode
  mem->store_mem(0x400 + tail,     ascii);
  mem->store_mem(0x400 + tail + 1, scancode);
  bda_w16(bda::KBD_BUF_TAIL, next_tail);

  // Hardware keyboard buffer (for port 0x60 / IRQ 1)
  // Queue make code (key press) and break code (key release)
  int next_hw = (kbd_hw_tail + 1) % KBD_HW_BUF_SIZE;
  if (next_hw != kbd_hw_head) {
    kbd_hw_buf[kbd_hw_tail] = scancode;  // make code
    kbd_hw_tail = next_hw;
    // Also queue break code (scancode | 0x80)
    next_hw = (kbd_hw_tail + 1) % KBD_HW_BUF_SIZE;
    if (next_hw != kbd_hw_head) {
      kbd_hw_buf[kbd_hw_tail] = scancode | 0x80;
      kbd_hw_tail = next_hw;
    }
    // Fire keyboard IRQ (IRQ 1 = pic_vector_base + 1)
    if (get_flag(FLAG_IF) && !(pic_imr & 0x02))
      request_int(pic_vector_base + 1);
  }

  waiting_for_key = false;
  kbd_poll_count = 0;
  halted = false;  // Wake from HLT so CPU can process the key
  idle_tick_time = {};  // Reset so next idle period starts fresh
}
