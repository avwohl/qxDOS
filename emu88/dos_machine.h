#ifndef DOS_MACHINE_H
#define DOS_MACHINE_H

#include "emu88.h"
#include "dos_io.h"
#include "ne2000.h"
#include <chrono>

#ifndef IOSFREEDOS_VERSION
#define IOSFREEDOS_VERSION "dev"
#endif

// PC memory map
static constexpr uint32_t VGA_VRAM_BASE  = 0xA0000;
static constexpr uint32_t MDA_VRAM_BASE  = 0xB0000;
static constexpr uint32_t CGA_VRAM_BASE  = 0xB8000;
static constexpr uint32_t BIOS_ROM_BASE  = 0xF0000;
static constexpr uint32_t BOOT_LOAD_ADDR = 0x07C00;
static constexpr uint16_t BDA_SEG        = 0x0040;

// BIOS trap opcode (0xF1 is undefined on all x86, falls to unimplemented_opcode)
static constexpr uint8_t BIOS_TRAP_OPCODE = 0xF1;

// BIOS Data Area offsets (from 0040:0000)
namespace bda {
  constexpr int EQUIPMENT     = 0x10;
  constexpr int MEM_SIZE_KB   = 0x13;
  constexpr int KBD_FLAGS1    = 0x17;
  constexpr int KBD_FLAGS2    = 0x18;
  constexpr int KBD_BUF_HEAD  = 0x1A;
  constexpr int KBD_BUF_TAIL  = 0x1C;
  constexpr int KBD_BUFFER    = 0x1E;
  constexpr int VIDEO_MODE    = 0x49;
  constexpr int SCREEN_COLS   = 0x4A;
  constexpr int VIDEO_PAGE_SZ = 0x4C;
  constexpr int VIDEO_PAGE_OFF= 0x4E;
  constexpr int CURSOR_POS    = 0x50;  // 8 pages x 2 bytes
  constexpr int CURSOR_SHAPE  = 0x60;
  constexpr int ACTIVE_PAGE   = 0x62;
  constexpr int CRTC_BASE     = 0x63;
  constexpr int TIMER_COUNT   = 0x6C;
  constexpr int TIMER_ROLLOVER= 0x70;
  constexpr int NUM_HDD       = 0x75;
  constexpr int KBD_BUF_START = 0x80;
  constexpr int KBD_BUF_END   = 0x82;
  constexpr int SCREEN_ROWS   = 0x84;
}

class dos_machine : public emu88 {
public:
  // Speed modes
  // CPS values compensate for 8088-based cycle table: 386+ speeds use
  // a ~3x multiplier so programs run at correct wall-clock speed despite
  // the cycle table counting 8088-era costs per instruction.
  enum SpeedMode {
    SPEED_FULL = 0,        // No throttling
    SPEED_PC_4_77 = 1,     // IBM PC 8088 4.77 MHz
    SPEED_AT_8 = 2,        // IBM AT 286 8 MHz
    SPEED_386SX_16 = 3,    // 386SX 16 MHz
    SPEED_386DX_33 = 4,    // 386DX 33 MHz
    SPEED_486DX2_66 = 5    // 486DX2 66 MHz
  };

  // Display adapter
  enum DisplayAdapter {
    DISPLAY_CGA = 0,
    DISPLAY_MDA = 1,
    DISPLAY_HERCULES = 2,
    DISPLAY_CGA_MDA = 3,  // Dual: both adapters active
    DISPLAY_EGA = 4,
    DISPLAY_VGA = 5
  };

  // Machine configuration
  struct Config {
    CpuType cpu = CPU_386;
    DisplayAdapter display = DISPLAY_CGA;
    SpeedMode speed = SPEED_PC_4_77;
    bool mouse_enabled = true;
    bool speaker_enabled = true;
    // Sound card type: 0=none, 1=Adlib, 2=SoundBlaster (future)
    int sound_card = 0;
    bool cdrom_enabled = true;
    // NE2000 NIC
    bool ne2000_enabled = false;
    uint16_t ne2000_iobase = 0x300;
    int ne2000_irq = 3;
  };

  dos_machine(emu88_mem *memory, dos_io *io);
  ~dos_machine();

  // Machine lifecycle
  void configure(const Config &cfg);
  void init_machine();
  bool boot(int drive = 0);
  bool run_batch(int count = 10000);

  // Config access
  const Config &get_config() const { return config; }

  // Speed control
  void set_speed(SpeedMode mode);
  SpeedMode get_speed() const { return speed_mode; }

  // CPU overrides
  void do_interrupt(emu88_uint8 vector) override;
  void port_out(emu88_uint16 port, emu88_uint8 value) override;
  emu88_uint8 port_in(emu88_uint16 port) override;
  void port_out16(emu88_uint16 port, emu88_uint16 value) override;
  emu88_uint16 port_in16(emu88_uint16 port) override;
  void unimplemented_opcode(emu88_uint8 opcode) override;

  // Keyboard input from host
  void queue_key(uint8_t ascii, uint8_t scancode);

  // Check if waiting for keyboard input (for host yield)
  bool is_waiting_for_key() const { return waiting_for_key; }

  // Configure display adapter (must be called before boot)
  void set_display(DisplayAdapter adapter) { config.display = adapter; }

  // DPMI intercept (override from emu88)
  bool intercept_pm_int(emu88_uint8 vector, bool is_software_int,
                        bool has_error_code, emu88_uint32 error_code) override;

  // DPMI state
  struct DpmiState {
    bool active = false;
    bool is_32bit = false;

    // Physical addresses of DPMI structures
    uint32_t base = 0;
    uint32_t gdt_phys = 0;
    uint32_t idt_phys = 0;
    uint32_t ldt_phys = 0;
    uint32_t tss_phys = 0;
    uint32_t pm_stack_top = 0;

    // GDT selectors
    uint16_t ring0_cs = 0x10;
    uint16_t ring0_ds = 0x08;
    uint16_t tss_sel = 0x18;
    uint16_t ldt_gdt_sel = 0x20;
    uint16_t pm_stack_ss = 0x30;  // PM stack SS (base=dpmi.base)

    // LDT allocation (up to 2048 entries)
    static constexpr int LDT_MAX = 2048;
    uint8_t ldt_alloc[LDT_MAX / 8] = {};

    // Client initial selectors
    uint16_t client_psp = 0;

    // Saved real-mode state
    uint16_t saved_rm_ss = 0, saved_rm_sp = 0;

    // Real-mode interrupt vectors (snapshot)
    uint16_t rm_int_off[256] = {};
    uint16_t rm_int_seg[256] = {};

    // PM interrupt vectors (client handlers installed via 0205h)
    struct PMVec { uint16_t sel = 0; uint32_t off = 0; };
    PMVec pm_int[256] = {};
    bool pm_int_installed[256] = {};

    // Exception handlers (installed via 0203h)
    PMVec exc_handler[32] = {};
    bool exc_installed[32] = {};

    // Memory block allocation
    struct MemBlock {
      uint32_t handle = 0;
      uint32_t base = 0;
      uint32_t size = 0;
      bool allocated = false;
    };
    static constexpr int MAX_MEM_BLOCKS = 128;
    MemBlock mem_blocks[MAX_MEM_BLOCKS] = {};
    uint32_t next_mem_base = 0;
    uint32_t next_handle = 1;

    // DOS memory blocks
    struct DosBlock {
      uint16_t segment = 0;
      uint16_t paragraphs = 0;
      uint16_t selector = 0;
      bool allocated = false;
    };
    static constexpr int MAX_DOS_BLOCKS = 32;
    DosBlock dos_blocks[MAX_DOS_BLOCKS] = {};

    // Segment-to-descriptor cache (for 0002h)
    struct SegMap { uint16_t rm_seg = 0; uint16_t pm_sel = 0; bool valid = false; };
    static constexpr int MAX_SEG_MAP = 64;
    SegMap seg_map[MAX_SEG_MAP] = {};

    // Virtual interrupt flag
    bool vif = true;

    // GDT selectors
    uint16_t bios_rom_cs = 0;   // BIOS ROM code segment (base=0xF0000)

    // ROM entry points
    uint16_t mode_switch_off = 0;
    uint16_t rm_return_off = 0;
    uint16_t raw_pm_to_rm_off = 0;
    uint16_t exc_return_off = 0;

    // Nested call state
    bool in_rm_callback = false;
    bool exc_handler_done = false;

    // Dedicated RM reflection stacks (DPMI spec: host provides locked RM stacks)
    // Each stack is 512 bytes. We support up to 8 nesting levels.
    static constexpr int RM_REFLECT_STACK_COUNT = 8;
    static constexpr int RM_REFLECT_STACK_SIZE = 0x200;  // 512 bytes each
    // Physical base: 0x7000 (free after boot, below boot sector at 0x7C00)
    static constexpr uint32_t RM_REFLECT_STACK_BASE = 0x7000;
    int rm_reflect_stack_depth = 0;

    // Low-memory save/restore for RM execution
    // PM code (like DOS4GW) may overwrite the IVT/BDA at physical 0x0000-0x1FFF.
    // We save PM's data and restore the real IVT/BDA for each RM call.
    static constexpr uint32_t LOW_MEM_SAVE_SIZE = 0x2000;  // 8KB: IVT+BDA+DOS data (DOS4GW overwrites this range)
    uint8_t original_low_mem[LOW_MEM_SAVE_SIZE] = {};  // Snapshot at DPMI init
    uint8_t pm_low_mem[LOW_MEM_SAVE_SIZE] = {};         // PM's overwritten data
    bool pm_overwrote_low_mem = false;

    // Exception handler RETF restoration state
    uint32_t exc_frame_base = 0;
    uint16_t exc_save_ds = 0, exc_save_es = 0, exc_save_fs = 0, exc_save_gs = 0;
    emu88::SegDescCache exc_save_seg_cache[6] = {};
  };
  DpmiState dpmi;

private:
  dos_io *io;

  // Video state
  int video_mode;
  int screen_cols;
  int screen_rows;

  // PIC state
  uint8_t pic_imr;
  uint8_t pic_vector_base;
  int pic_init_step;
  bool pic_icw4_needed;

  // PIT state
  uint16_t pit_counter[3];
  uint16_t pit_reload[3];     // Programmed reload value
  uint64_t pit_load_cycle[3]; // CPU cycle when counter was loaded
  uint8_t pit_mode[3];
  uint8_t pit_access[3];
  uint8_t pit_write_phase[3];
  uint8_t pit_read_phase[3];
  bool pit_latch_pending[3];
  uint16_t pit_latch_value[3];

  uint16_t pit_current_count(int ch) const;
  uint8_t port_b;  // Port 0x61

  // Adlib/OPL2 (minimal detection emulation)
  uint8_t adlib_index;       // Index register (port 0x388 write)
  uint8_t adlib_regs[256];   // OPL registers
  uint8_t adlib_status;      // Status register (port 0x388 read)
  bool adlib_timer1_running;
  uint64_t adlib_timer1_start_cycle;

  // Sound Blaster DSP (minimal detection emulation, base 0x220)
  static const uint16_t sb_base = 0x220;
  uint8_t sb_dsp_data[16];   // Read data FIFO
  int sb_dsp_data_head;
  int sb_dsp_data_tail;
  int sb_dsp_data_count;
  uint8_t sb_dsp_cmd;        // Current command being processed
  bool sb_dsp_cmd_pending;   // Waiting for command parameter
  bool sb_dsp_reset_active;  // DSP is being reset

  // CGA/MDA CRTC state
  uint8_t crtc_index;
  uint8_t crtc_regs[256];

  // VGA sequencer state (port 0x3C4/0x3C5)
  uint8_t vga_seq_index;
  uint8_t vga_seq_regs[8];

  // VGA graphics controller state (port 0x3CE/0x3CF)
  uint8_t vga_gc_index;
  uint8_t vga_gc_regs[16];

  // Mode X composite buffer (320x200 = 64000 bytes)
  uint8_t modex_composite[64000];

  // VGA DAC palette (256 colors, RGB 0-63 each)
  uint8_t vga_dac[256][3];
  uint8_t dac_write_index;
  uint8_t dac_read_index;
  uint8_t dac_component;  // 0=R, 1=G, 2=B
  uint8_t dac_pel_mask;   // Palette mask (port 0x3C6)

  // Timer / refresh counters (cycle-based)
  unsigned long long tick_cycle_mark;
  unsigned long long refresh_cycle_mark;

  // 8088 @ 4.77MHz: timer tick fires at 18.2 Hz = every ~262,187 cycles
  // Video refresh at ~30 Hz = every ~159,000 cycles
  static constexpr uint32_t CYCLES_PER_TICK    = 262187;
  static constexpr uint32_t CYCLES_PER_REFRESH = 159000;

  // Speed control
  SpeedMode speed_mode;
  uint32_t target_cps;  // Target cycles per second (0 = unlimited)

  // Boot banner (shown once on first TTY output)
  bool banner_shown;

  // Keyboard wait state
  bool waiting_for_key;
  int kbd_poll_count;  // Consecutive AH=01 no-key responses
  unsigned long long kbd_poll_start_cycle;  // Cycle count when polling started
  std::chrono::steady_clock::time_point idle_tick_time;  // Wall-clock timer for idle ticks


  // How many consecutive AH=01 "no key" polls before yielding.
  // Must be high enough to avoid triggering during DOS Ctrl-C checks
  // during file I/O (~5-20 per batch), but low enough to catch tight
  // polling loops at prompts (~5000+ per batch).
  static constexpr int KBD_POLL_THRESHOLD = 500;
  // Minimum emulated time of continuous polling before yielding (~1 tick = 55ms).
  // Lower values reduce battery drain during idle; the poll count threshold (500)
  // already prevents false triggers during brief Ctrl-C checks.
  static constexpr uint32_t KBD_POLL_MIN_CYCLES = CYCLES_PER_TICK;

  // Keyboard controller command state (for A20 gate)
  uint8_t kbd_cmd_pending;

  // Hardware keyboard scancode buffer (for INT 9 / port 0x60)
  static constexpr int KBD_HW_BUF_SIZE = 16;
  uint8_t kbd_hw_buf[KBD_HW_BUF_SIZE];
  int kbd_hw_head = 0;
  int kbd_hw_tail = 0;
  uint8_t kbd_last_scancode = 0;  // Last scancode read from port 0x60

  // CMOS RTC
  uint8_t cmos_index;
  uint8_t cmos_data[128];
  void init_cmos();

  // NE2000 NIC
  ne2000 *nic;
  uint16_t ne2000_base;
  int ne2000_irq;

  // Machine config
  Config config;

  // Mouse state (INT 33h driver)
  struct {
    int x = 320, y = 100;      // Virtual coords (0-639, 0-199)
    int buttons = 0;            // Bit 0=left, 1=right, 2=middle
    bool visible = false;
    bool installed = false;
    int min_x = 0, max_x = 639;
    int min_y = 0, max_y = 199;
    int sensitivity_x = 8, sensitivity_y = 16;
    // Button press/release counters
    int press_count[3] = {};
    int release_count[3] = {};
    int press_x[3] = {}, press_y[3] = {};
    int release_x[3] = {}, release_y[3] = {};
    // User callback
    uint16_t handler_mask = 0;
    uint16_t handler_seg = 0;
    uint16_t handler_off = 0;
  } mouse;

  // XMS driver state
  static constexpr int XMS_MAX_HANDLES = 32;
  struct xms_handle {
    bool allocated = false;
    uint32_t base = 0;    // Physical address (bytes)
    uint32_t size_kb = 0; // Size in KB
    uint8_t lock_count = 0;
  };
  xms_handle xms_handles[XMS_MAX_HANDLES];
  uint16_t xms_entry_off;  // ROM offset for XMS entry point

  // BIOS ROM entry points (offset within F000 segment)
  uint16_t bios_entry[256];
  uint16_t pm_int_default_entry[256];  // PM default INT handler stubs (RETF)

  // BDA helpers
  void bda_w8(int off, uint8_t v);
  void bda_w16(int off, uint16_t v);
  void bda_w32(int off, uint32_t v);
  uint8_t bda_r8(int off);
  uint16_t bda_r16(int off);
  uint32_t bda_r32(int off);

  // BIOS interrupt handlers (in dos_bios.cc)
  void bios_int08h();   // Timer tick
  void bios_int10h();   // Video services
  void bios_int11h();   // Equipment list
  void bios_int12h();   // Memory size
  void bios_int13h();   // Disk services
  void bios_int14h();   // Serial port
  void bios_int15h();   // System services
  void bios_int16h();   // Keyboard
  void bios_int17h();   // Printer
  void bios_int19h();   // Bootstrap loader
  void bios_int1ah();   // Time/date
  void bios_int2fh();   // Multiplex (XMS)
  void bios_int33h();   // Mouse driver
  void bios_int_e0h();  // Host file services

  // XMS dispatch (called via FAR CALL to ROM entry point)
  void xms_dispatch();

  // BIOS trap dispatch
  void dispatch_bios(uint8_t vector);

  // Video helpers (in dos_bios.cc)
  void video_set_mode(int mode);
  void video_tty(uint8_t ch);
  void video_write_char(uint8_t ch, uint8_t attr, int count);
  void video_scroll(int dir, int top, int left, int bottom, int right,
                    int lines, uint8_t attr);
  uint32_t vram_base() const;
  void cursor_advance();

  // Disk helpers (in dos_bios.cc)
  struct disk_geom { int heads, cyls, spt, sector_size; };
  disk_geom get_geometry(int drive);

  // Init helpers
  void init_ivt();
  void init_bda();
  void install_bios_stubs();

  // DPMI implementation (in dos_dpmi.cc)
  void dpmi_mode_switch();
  void dpmi_raw_rm_to_pm();
  void dpmi_raw_pm_to_rm();
  void dpmi_int31h();
  void dpmi_reflect_to_rm(uint8_t vector, bool preserve_regs = false);
  void dpmi_terminate(uint8_t exit_code);
  void dpmi_dispatch_exception(uint8_t vector, uint32_t error_code, bool has_error_code);
  void dpmi_exec_rm(uint8_t vector, uint32_t struct_addr, uint16_t copy_words, bool use_iret);
  void dpmi_save_pm_low_mem();
  void dpmi_restore_pm_low_mem();

  // DPMI helpers
  uint16_t dpmi_alloc_ldt_sel();
  void dpmi_free_ldt_sel(uint16_t sel);
  void dpmi_write_gdt_entry(int index, uint32_t base, uint32_t limit,
                            uint8_t access, uint8_t flags_nibble);
  void dpmi_write_ldt_entry(uint16_t sel, uint32_t base, uint32_t limit,
                            uint8_t access, uint8_t flags_nibble);
  void dpmi_read_ldt_raw(uint16_t sel, uint8_t desc[8]);
  void dpmi_write_ldt_raw(uint16_t sel, const uint8_t desc[8]);
  void dpmi_write_idt_entry(int vector, uint16_t sel, uint32_t offset,
                            uint8_t dpl, bool is_32bit);
};

#endif // DOS_MACHINE_H
