#ifndef EMU88_MEM_H
#define EMU88_MEM_H

#include "emu88_types.h"

// Memory: 1MB conventional + extended memory (up to 16MB)
// A20 gate controls whether address bit 20 is masked (8086 wraparound)

class emu88_mem {
  emu88_uint8 *dat;
  emu88_uint32 mem_size;
  bool a20_enabled;
public:

  // VGA plane state (set by dos_machine for Mode X support)
  bool vga_planar = false;        // true when unchained (Mode X), false for chain-4 (Mode 13h)
  uint8_t vga_map_mask = 0x0F;    // Sequencer reg 2: which planes to write (bits 0-3)
  uint8_t vga_read_map = 0;       // GC reg 4: which plane to read (0-3)
  uint8_t vga_planes[4][65536];   // 4 planes × 64KB

  // --- SVGA / VESA linear framebuffer -------------------------------------
  // Separate store from the legacy 256KB vga_planes / 0xA0000 window. Allocated
  // lazily when the first VESA mode is set. The guest reaches it two ways:
  //   * a bank-switched 64KB window at 0xA0000 (VBE function 4F05), and
  //   * a linear-framebuffer aperture at svga_lfb_phys (the VBE PhysBasePtr),
  //     used by 32-bit / DOS4G / DPMI clients.
  // Both views address the SAME svga_vram buffer.
  emu88_uint8 *svga_vram = nullptr;
  emu88_uint32 svga_vram_size = 0;
  bool         svga_active = false;     // a VESA mode is current -> route 0xA0000 here
  emu88_uint32 svga_window_off = 0;     // byte offset of the 0xA0000 window into svga_vram
  emu88_uint32 svga_lfb_phys = 0;       // physical base of the LFB aperture (0 = disabled)
  void svga_ensure(emu88_uint32 bytes); // grow svga_vram to at least `bytes`
  emu88_uint8 *svga_base() { return svga_vram; }

  emu88_mem(emu88_uint32 size = 0x100000);  // default 1MB
  virtual ~emu88_mem();

  virtual emu88_uint8 *get_mem(void) { return dat; }
  emu88_uint32 get_mem_size(void) const { return mem_size; }

  // A20 gate control
  void set_a20(bool enabled) { a20_enabled = enabled; }
  bool get_a20() const { return a20_enabled; }

  // Address masking: if A20 disabled, bit 20 is forced to 0
  emu88_uint32 mask_addr(emu88_uint32 addr) const {
    if (!a20_enabled) addr &= 0xFFFFF;  // 20-bit wrap
    return addr < mem_size ? addr : addr % mem_size;
  }

  virtual emu88_uint8 fetch_mem(emu88_uint32 addr);
  virtual void store_mem(emu88_uint32 addr, emu88_uint8 abyte);

  virtual emu88_uint16 fetch_mem16(emu88_uint32 addr);
  virtual void store_mem16(emu88_uint32 addr, emu88_uint16 aword);

  virtual emu88_uint32 fetch_mem32(emu88_uint32 addr);
  virtual void store_mem32(emu88_uint32 addr, emu88_uint32 adword);
};

#endif // EMU88_MEM_H
