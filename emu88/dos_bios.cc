#include "dos_machine.h"
#include <cstdio>
#include <cstring>
#include <initializer_list>

//=============================================================================
// Video helpers
//=============================================================================

uint32_t dos_machine::vram_base() const {
  return (video_mode == 7) ? MDA_VRAM_BASE : CGA_VRAM_BASE;
}

// Default VGA palette: 16 standard colors + 16 dark + 216 color cube + 24 grays
static void init_default_vga_palette(uint8_t dac[][3]) {
  // Standard 16 CGA colors (indices 0-15)
  static const uint8_t cga16[16][3] = {
    { 0, 0, 0}, { 0, 0,42}, { 0,42, 0}, { 0,42,42},
    {42, 0, 0}, {42, 0,42}, {42,21, 0}, {42,42,42},
    {21,21,21}, {21,21,63}, {21,63,21}, {21,63,63},
    {63,21,21}, {63,21,63}, {63,63,21}, {63,63,63}
  };
  for (int i = 0; i < 16; i++) {
    dac[i][0] = cga16[i][0]; dac[i][1] = cga16[i][1]; dac[i][2] = cga16[i][2];
  }
  // Indices 16-31: darker versions of 0-15
  for (int i = 0; i < 16; i++) {
    dac[16+i][0] = cga16[i][0] / 2;
    dac[16+i][1] = cga16[i][1] / 2;
    dac[16+i][2] = cga16[i][2] / 2;
  }
  // Indices 32-247: 6x6x6 color cube
  int idx = 32;
  for (int r = 0; r < 6; r++)
    for (int g = 0; g < 6; g++)
      for (int b = 0; b < 6; b++) {
        if (idx < 248) {
          dac[idx][0] = r * 12 + 3;
          dac[idx][1] = g * 12 + 3;
          dac[idx][2] = b * 12 + 3;
          idx++;
        }
      }
  // Indices 248-255: grayscale ramp
  for (int i = 0; i < 8; i++) {
    uint8_t v = (uint8_t)(i * 9);
    dac[248+i][0] = v; dac[248+i][1] = v; dac[248+i][2] = v;
  }
}

void dos_machine::video_set_mode(int mode) { video_set_mode_ex(mode, false); }

// `keep_buffer' is INT 10h AH=00's AL bit 7, "do not clear the display buffer".
// Everything else a mode set does - the BDA fields, the sequencer and CRTC
// state, the palette, the cursor reset - happens either way; only the clear is
// suppressed.
void dos_machine::video_set_mode_ex(int mode, bool keep_buffer) {
  // Selecting any legacy (VGA/EGA/CGA/text) mode leaves SVGA: the 0xA0000
  // window reverts to the legacy VGA paths and the VESA framebuffer is idle.
  vesa.active = false;
  mem->svga_active = false;

  video_mode = mode;
  switch (mode) {
    case 0: case 1: screen_cols = 40; screen_rows = 25; break;
    case 2: case 3: screen_cols = 80; screen_rows = 25; break;
    case 7:         screen_cols = 80; screen_rows = 25; break;
    case 0x13:      screen_cols = 40; screen_rows = 25; break;  // 320x200x256 (text cols/rows for BDA)
    default:        screen_cols = 80; screen_rows = 25; break;
  }

  if (mode == 0x13) {
    // VGA mode 13h: 320x200x256 linear framebuffer at A000:0000
    // Set VGA sequencer for chain-4 mode (linear addressing)
    vga_seq_regs[4] = 0x0E;  // Chain-4 on (bit 3), odd/even off (bit 2), ext mem (bit 1)
    vga_seq_regs[2] = 0x0F;  // All planes writable
    mem->vga_planar = false;  // Chain-4 = linear mode
    mem->vga_map_mask = 0x0F;
    mem->vga_read_map = 0;
    memset(mem->vga_planes, 0, sizeof(mem->vga_planes));
    crtc_regs[12] = 0;  // Clear CRTC start address
    crtc_regs[13] = 0;
    if (!keep_buffer)
      for (uint32_t i = 0; i < 64000; i++)
        mem->store_mem(VGA_VRAM_BASE + i, 0);
    init_default_vga_palette(vga_dac);
  } else if (!keep_buffer) {
    // Text mode: clear VRAM
    uint32_t base = vram_base();
    int cells = screen_cols * screen_rows;
    for (int i = 0; i < cells; i++) {
      mem->store_mem(base + i * 2,     0x20);  // space
      mem->store_mem(base + i * 2 + 1, 0x07);  // light gray on black
    }
  }

  // Update BDA
  bda_w8(bda::VIDEO_MODE, mode);
  bda_w16(bda::SCREEN_COLS, screen_cols);
  bda_w8(bda::SCREEN_ROWS, screen_rows - 1);
  bda_w16(bda::CRTC_BASE, (mode == 7) ? 0x3B4 : 0x3D4);
  bda_w16(bda::VIDEO_PAGE_SZ, (mode == 0x13) ? 0xFA00 : screen_cols * screen_rows * 2);
  bda_w16(bda::VIDEO_PAGE_OFF, 0);
  bda_w8(bda::ACTIVE_PAGE, 0);

  // Reset cursor for all pages
  for (int p = 0; p < 8; p++)
    bda_w16(bda::CURSOR_POS + p * 2, 0x0000);
}

void dos_machine::video_scroll(int dir, int top, int left, int bottom,
                               int right, int lines, uint8_t attr) {
  if (top > bottom || left > right) return;
  if (bottom >= screen_rows) bottom = screen_rows - 1;
  if (right >= screen_cols) right = screen_cols - 1;

  uint32_t base = vram_base();
  int width = right - left + 1;

  if (lines == 0 || lines > (bottom - top + 1)) {
    // Clear the window
    for (int r = top; r <= bottom; r++) {
      for (int c = left; c <= right; c++) {
        int off = (r * screen_cols + c) * 2;
        mem->store_mem(base + off,     0x20);
        mem->store_mem(base + off + 1, attr);
      }
    }
    return;
  }

  if (dir == 0) {
    // Scroll up: move lines up, clear bottom
    for (int r = top; r <= bottom - lines; r++) {
      for (int c = left; c <= right; c++) {
        int dst = (r * screen_cols + c) * 2;
        int src = ((r + lines) * screen_cols + c) * 2;
        mem->store_mem(base + dst,     mem->fetch_mem(base + src));
        mem->store_mem(base + dst + 1, mem->fetch_mem(base + src + 1));
      }
    }
    for (int r = bottom - lines + 1; r <= bottom; r++) {
      for (int c = left; c <= right; c++) {
        int off = (r * screen_cols + c) * 2;
        mem->store_mem(base + off,     0x20);
        mem->store_mem(base + off + 1, attr);
      }
    }
  } else {
    // Scroll down: move lines down, clear top
    for (int r = bottom; r >= top + lines; r--) {
      for (int c = left; c <= right; c++) {
        int dst = (r * screen_cols + c) * 2;
        int src = ((r - lines) * screen_cols + c) * 2;
        mem->store_mem(base + dst,     mem->fetch_mem(base + src));
        mem->store_mem(base + dst + 1, mem->fetch_mem(base + src + 1));
      }
    }
    for (int r = top; r < top + lines; r++) {
      for (int c = left; c <= right; c++) {
        int off = (r * screen_cols + c) * 2;
        mem->store_mem(base + off,     0x20);
        mem->store_mem(base + off + 1, attr);
      }
    }
  }

  (void)width;
}

void dos_machine::video_write_char(uint8_t ch, uint8_t attr, int count) {
  int page = bda_r8(bda::ACTIVE_PAGE);
  uint16_t pos = bda_r16(bda::CURSOR_POS + page * 2);
  int row = (pos >> 8) & 0xFF;
  int col = pos & 0xFF;

  uint32_t base = vram_base() + page * bda_r16(bda::VIDEO_PAGE_SZ);
  for (int i = 0; i < count; i++) {
    int off = (row * screen_cols + col) * 2;
    mem->store_mem(base + off, ch);
    if (attr != 0xFF)  // 0xFF = keep existing attribute
      mem->store_mem(base + off + 1, attr);
    col++;
    if (col >= screen_cols) {
      col = 0;
      row++;
      if (row >= screen_rows) row = screen_rows - 1;
    }
  }
  // Note: cursor is NOT advanced for INT 10h AH=09h/0Ah
}

void dos_machine::cursor_advance() {
  int page = bda_r8(bda::ACTIVE_PAGE);
  uint16_t pos = bda_r16(bda::CURSOR_POS + page * 2);
  int row = (pos >> 8) & 0xFF;
  int col = pos & 0xFF;

  col++;
  if (col >= screen_cols) {
    col = 0;
    row++;
  }
  if (row >= screen_rows) {
    row = screen_rows - 1;
    video_scroll(0, 0, 0, screen_rows - 1, screen_cols - 1, 1, 0x07);
  }
  bda_w16(bda::CURSOR_POS + page * 2, (row << 8) | col);
}

void dos_machine::video_tty(uint8_t ch) {
  // Show version banner before the first printable character
  if (!banner_shown && ch >= 0x20) {
    banner_shown = true;
    const char *banner = "iosFreeDOS " IOSFREEDOS_VERSION "\r\n";
    for (int i = 0; banner[i]; i++)
      video_tty(banner[i]);
  }

  int page = bda_r8(bda::ACTIVE_PAGE);
  uint16_t pos = bda_r16(bda::CURSOR_POS + page * 2);
  int row = (pos >> 8) & 0xFF;
  int col = pos & 0xFF;

  switch (ch) {
    case 0x07:  // BEL
      io->speaker_beep(1000, 100);
      return;
    case 0x08:  // BS
      if (col > 0) col--;
      bda_w16(bda::CURSOR_POS + page * 2, (row << 8) | col);
      return;
    case 0x0A:  // LF
      row++;
      if (row >= screen_rows) {
        row = screen_rows - 1;
        video_scroll(0, 0, 0, screen_rows - 1, screen_cols - 1, 1, 0x07);
      }
      bda_w16(bda::CURSOR_POS + page * 2, (row << 8) | col);
      return;
    case 0x0D:  // CR
      col = 0;
      bda_w16(bda::CURSOR_POS + page * 2, (row << 8) | col);
      return;
    default: {
      // Write character at cursor and advance
      uint32_t base = vram_base() + page * bda_r16(bda::VIDEO_PAGE_SZ);
      int off = (row * screen_cols + col) * 2;
      mem->store_mem(base + off, ch);
      // Keep existing attribute for TTY output
      col++;
      if (col >= screen_cols) {
        col = 0;
        row++;
      }
      if (row >= screen_rows) {
        row = screen_rows - 1;
        video_scroll(0, 0, 0, screen_rows - 1, screen_cols - 1, 1, 0x07);
      }
      bda_w16(bda::CURSOR_POS + page * 2, (row << 8) | col);
    }
  }
}

//=============================================================================
// Disk helpers
//=============================================================================

dos_machine::disk_geom dos_machine::get_geometry(int drive) {
  disk_geom g = {0, 0, 0, 512};
  if (!io->disk_present(drive)) return g;

  uint64_t size = io->disk_size(drive);

  if (drive >= 0xE0) {
    // CD-ROM: no CHS geometry, use LBA only
    g.sector_size = 2048;
    g.heads = 0;
    g.spt = 0;
    g.cyls = 0;
    return g;
  }

  if (drive < 0x80) {
    // Floppy - detect geometry from image size
    switch (size) {
      case 163840:  g = {1, 40, 8,  512}; break;  // 160KB
      case 184320:  g = {1, 40, 9,  512}; break;  // 180KB
      case 327680:  g = {2, 40, 8,  512}; break;  // 320KB
      case 368640:  g = {2, 40, 9,  512}; break;  // 360KB
      case 737280:  g = {2, 80, 9,  512}; break;  // 720KB
      case 1228800: g = {2, 80, 15, 512}; break;  // 1.2MB
      case 1474560: g = {2, 80, 18, 512}; break;  // 1.44MB
      case 2949120: g = {2, 80, 36, 512}; break;  // 2.88MB
      default:      g = {2, 80, 18, 512}; break;  // Default 1.44MB
    }
  } else {
    // Hard disk
    g.heads = 16;
    g.spt = 63;
    g.sector_size = 512;
    uint64_t total_sectors = size / 512;
    g.cyls = (int)(total_sectors / (g.heads * g.spt));
    if (g.cyls < 1) g.cyls = 1;
    if (g.cyls > 1024) g.cyls = 1024;
  }
  return g;
}

//=============================================================================
// INT 08h - Timer Tick (IRQ0)
//=============================================================================

void dos_machine::bios_int08h() {
  // Timer count is updated in run_batch, so just chain to INT 1Ch
  // (user timer hook). If 1Ch is still our stub, dispatch_bios handles it.
  // Skip INT 1Ch in PM — it's a real-mode BIOS concept; the PM DPMI client
  // handles timer processing via its own INT 08h handler chain.
  if (!protected_mode()) {
    do_interrupt(0x1C);
  }
}

//=============================================================================
// INT 10h - Video Services
//=============================================================================

void dos_machine::bios_int10h() {
  uint8_t ah = get_reg8(reg_AH);
  uint8_t al = get_reg8(reg_AL);

  switch (ah) {
    case 0x00: {  // Set video mode
      // AL bit 7 means "do not clear the display buffer".  It used to be masked
      // off and then ignored, so a program that re-selects its current mode to
      // reset the CRTC - a normal thing for a text-mode program to do - lost the
      // screen every time.
      video_set_mode_ex(al & 0x7F, (al & 0x80) != 0);
      io->video_mode_changed(video_mode, screen_cols, screen_rows);
      break;
    }
    case 0x4F:    // VESA VBE (SVGA) services
      vesa_bios();
      break;
    case 0x01: {  // Set cursor shape
      uint8_t ch_val = get_reg8(reg_CH);
      uint8_t cl_val = get_reg8(reg_CL);
      bda_w16(bda::CURSOR_SHAPE, (ch_val << 8) | cl_val);
      break;
    }
    case 0x02: {  // Set cursor position
      uint8_t page = get_reg8(reg_BH);
      uint8_t row = get_reg8(reg_DH);
      uint8_t col = get_reg8(reg_DL);
      if (page < 8)
        bda_w16(bda::CURSOR_POS + page * 2, (row << 8) | col);
      break;
    }
    case 0x03: {  // Get cursor position
      uint8_t page = get_reg8(reg_BH);
      if (page >= 8) page = 0;
      uint16_t pos = bda_r16(bda::CURSOR_POS + page * 2);
      uint16_t shape = bda_r16(bda::CURSOR_SHAPE);
      set_reg16(reg_DX, pos);
      set_reg16(reg_CX, shape);
      break;
    }
    case 0x05: {  // Set active page
      bda_w8(bda::ACTIVE_PAGE, al);
      bda_w16(bda::VIDEO_PAGE_OFF, al * bda_r16(bda::VIDEO_PAGE_SZ));
      break;
    }
    case 0x06: {  // Scroll up
      uint8_t attr = get_reg8(reg_BH);
      uint8_t ch_val = get_reg8(reg_CH);
      uint8_t cl_val = get_reg8(reg_CL);
      uint8_t dh_val = get_reg8(reg_DH);
      uint8_t dl_val = get_reg8(reg_DL);
      video_scroll(0, ch_val, cl_val, dh_val, dl_val, al, attr);
      break;
    }
    case 0x07: {  // Scroll down
      uint8_t attr = get_reg8(reg_BH);
      uint8_t ch_val = get_reg8(reg_CH);
      uint8_t cl_val = get_reg8(reg_CL);
      uint8_t dh_val = get_reg8(reg_DH);
      uint8_t dl_val = get_reg8(reg_DL);
      video_scroll(1, ch_val, cl_val, dh_val, dl_val, al, attr);
      break;
    }
    case 0x08: {  // Read char/attr at cursor
      int page = get_reg8(reg_BH);
      uint16_t pos = bda_r16(bda::CURSOR_POS + page * 2);
      int row = (pos >> 8) & 0xFF;
      int col = pos & 0xFF;
      uint32_t base = vram_base() + page * bda_r16(bda::VIDEO_PAGE_SZ);
      int off = (row * screen_cols + col) * 2;
      uint8_t ch_out = mem->fetch_mem(base + off);
      uint8_t at_out = mem->fetch_mem(base + off + 1);
      set_reg8(reg_AL, ch_out);
      set_reg8(reg_AH, at_out);
      break;
    }
    case 0x09: {  // Write char/attr at cursor (no advance)
      uint8_t attr = get_reg8(reg_BL);
      int count = regs[reg_CX];
      if (count == 0) count = 1;
      video_write_char(al, attr, count);
      break;
    }
    case 0x0A: {  // Write char at cursor (keep attr, no advance)
      int count = regs[reg_CX];
      if (count == 0) count = 1;
      video_write_char(al, 0xFF, count);  // 0xFF = keep existing attr
      break;
    }
    case 0x0B: {  // Set color palette (CGA)
      break;
    }
    case 0x0C: {  // Write pixel
      if (video_mode == 0x13) {
        int x = regs[reg_CX];
        int y = regs[reg_DX];
        if (x < 320 && y < 200) {
          uint8_t color = al;
          if (get_reg8(reg_BH) != 0) {
            // Page is ignored for mode 13h but XOR mode uses bit 7
          }
          mem->store_mem(VGA_VRAM_BASE + y * 320 + x, color);
        }
      }
      break;
    }
    case 0x0D: {  // Read pixel
      if (video_mode == 0x13) {
        int x = regs[reg_CX];
        int y = regs[reg_DX];
        if (x < 320 && y < 200)
          set_reg8(reg_AL, mem->fetch_mem(VGA_VRAM_BASE + y * 320 + x));
        else
          set_reg8(reg_AL, 0);
      } else {
        set_reg8(reg_AL, 0);
      }
      break;
    }
    case 0x0E: {  // TTY output
      video_tty(al);
      break;
    }
    case 0x0F: {  // Get current video mode
      set_reg8(reg_AH, screen_cols);
      set_reg8(reg_AL, bda_r8(bda::VIDEO_MODE));
      set_reg8(reg_BH, bda_r8(bda::ACTIVE_PAGE));
      break;
    }
    case 0x10: {  // VGA DAC palette functions
      uint8_t al10 = al;
      switch (al10) {
        case 0x10: {  // Set individual DAC register
          uint8_t reg = get_reg8(reg_BL);
          vga_dac[reg][0] = get_reg8(reg_DH) & 0x3F;
          vga_dac[reg][1] = get_reg8(reg_CH) & 0x3F;
          vga_dac[reg][2] = get_reg8(reg_CL) & 0x3F;
          break;
        }
        case 0x12: {  // Set block of DAC registers
          uint16_t start = regs[reg_BX];
          uint16_t count = regs[reg_CX];
          uint32_t tbl = EMU88_MK20(sregs[seg_ES], regs[reg_DX]);
          for (uint16_t i = 0; i < count && start + i < 256; i++) {
            vga_dac[start + i][0] = mem->fetch_mem(tbl + i * 3) & 0x3F;
            vga_dac[start + i][1] = mem->fetch_mem(tbl + i * 3 + 1) & 0x3F;
            vga_dac[start + i][2] = mem->fetch_mem(tbl + i * 3 + 2) & 0x3F;
          }
          break;
        }
        case 0x15: {  // Read individual DAC register
          uint8_t reg = get_reg8(reg_BL);
          set_reg8(reg_DH, vga_dac[reg][0]);
          set_reg8(reg_CH, vga_dac[reg][1]);
          set_reg8(reg_CL, vga_dac[reg][2]);
          break;
        }
        case 0x17: {  // Read block of DAC registers
          uint16_t start = regs[reg_BX];
          uint16_t count = regs[reg_CX];
          uint32_t tbl = EMU88_MK20(sregs[seg_ES], regs[reg_DX]);
          for (uint16_t i = 0; i < count && start + i < 256; i++) {
            mem->store_mem(tbl + i * 3,     vga_dac[start + i][0]);
            mem->store_mem(tbl + i * 3 + 1, vga_dac[start + i][1]);
            mem->store_mem(tbl + i * 3 + 2, vga_dac[start + i][2]);
          }
          break;
        }
        default: break;
      }
      break;
    }
    case 0x11: {  // Character generator - stub
      // AH=11h AL=30h: Get font info
      if (al == 0x30) {
        set_reg16(reg_CX, 16);  // char height
        set_reg8(reg_DL, bda_r8(bda::SCREEN_ROWS));
      }
      break;
    }
    case 0x12: {  // Alternate function select
      if (get_reg8(reg_BL) == 0x10) {
        // Get EGA info
        set_reg8(reg_BH, 0);   // Color mode
        set_reg8(reg_BL, 3);   // 256K EGA memory
        set_reg8(reg_CH, 0);
        set_reg8(reg_CL, 0);
      }
      break;
    }
    case 0x13: {  // Write string
      // AL: write mode (bit0=update cursor, bit1=string has attrs)
      // BH: page, BL: attr (modes 0-1), CX: length
      // DH: row, DL: col, ES:BP -> string
      uint8_t mode = al;
      uint8_t page = get_reg8(reg_BH);
      uint8_t attr = get_reg8(reg_BL);
      int count = regs[reg_CX];
      int row = get_reg8(reg_DH);
      int col = get_reg8(reg_DL);
      uint32_t str_addr = EMU88_MK20(sregs[seg_ES], regs[reg_BP]);

      // Save and set cursor position for writing
      uint16_t old_pos = bda_r16(bda::CURSOR_POS + page * 2);
      bda_w16(bda::CURSOR_POS + page * 2, (row << 8) | col);

      uint32_t base = vram_base() + page * bda_r16(bda::VIDEO_PAGE_SZ);

      for (int i = 0; i < count; i++) {
        uint8_t ch;
        uint8_t char_attr;
        if (mode & 0x02) {
          // String contains char/attr pairs
          ch = mem->fetch_mem(str_addr++);
          char_attr = mem->fetch_mem(str_addr++);
        } else {
          ch = mem->fetch_mem(str_addr++);
          char_attr = attr;
        }

        // Handle control characters via TTY-like behavior
        if (ch == 0x07 || ch == 0x08 || ch == 0x0A || ch == 0x0D) {
          video_tty(ch);
        } else {
          // Write char at current cursor position
          uint16_t pos = bda_r16(bda::CURSOR_POS + page * 2);
          int cr = (pos >> 8) & 0xFF;
          int cc = pos & 0xFF;
          int off = (cr * screen_cols + cc) * 2;
          mem->store_mem(base + off, ch);
          mem->store_mem(base + off + 1, char_attr);
          // Advance cursor
          cc++;
          if (cc >= screen_cols) { cc = 0; cr++; }
          if (cr >= screen_rows) {
            cr = screen_rows - 1;
            video_scroll(0, 0, 0, screen_rows - 1, screen_cols - 1, 1, char_attr);
          }
          bda_w16(bda::CURSOR_POS + page * 2, (cr << 8) | cc);
        }
      }

      // Restore cursor if mode says don't update
      if (!(mode & 0x01))
        bda_w16(bda::CURSOR_POS + page * 2, old_pos);
      break;
    }
    case 0x1A: {  // Get/set display combination code
      if (al == 0x00) {
        set_reg8(reg_AL, 0x1A);  // Function supported
        // Report adapter type based on config (DCC codes)
        switch (config.display) {
          case DISPLAY_MDA:
            set_reg8(reg_BL, 0x01);  // MDA monochrome
            set_reg8(reg_BH, 0x00);
            break;
          case DISPLAY_HERCULES:
            set_reg8(reg_BL, 0x01);  // Hercules reports as MDA
            set_reg8(reg_BH, 0x00);
            break;
          case DISPLAY_CGA:
            set_reg8(reg_BL, 0x02);  // CGA color
            set_reg8(reg_BH, 0x00);
            break;
          case DISPLAY_EGA:
            set_reg8(reg_BL, 0x04);  // EGA color
            set_reg8(reg_BH, 0x00);
            break;
          case DISPLAY_CGA_MDA:
            set_reg8(reg_BL, 0x08);  // Primary: VGA color
            set_reg8(reg_BH, 0x01);  // Secondary: MDA
            break;
          case DISPLAY_VGA:
          default:
            set_reg8(reg_BL, 0x08);  // VGA color
            set_reg8(reg_BH, 0x00);
            break;
        }
      }
      break;
    }
    default:
      break;
  }
}

//=============================================================================
// INT 11h - Equipment List
//=============================================================================

void dos_machine::bios_int11h() {
  regs[reg_AX] = bda_r16(bda::EQUIPMENT);
}

//=============================================================================
// INT 12h - Memory Size
//=============================================================================

void dos_machine::bios_int12h() {
  regs[reg_AX] = bda_r16(bda::MEM_SIZE_KB);
}

//=============================================================================
// INT 13h - Disk Services
//=============================================================================

void dos_machine::bios_int13h() {
  uint8_t ah = get_reg8(reg_AH);
  uint8_t dl = get_reg8(reg_DL);  // Drive number

  switch (ah) {
    case 0x00: {  // Reset disk system
      set_reg8(reg_AH, 0);
      clear_flag(FLAG_CF);
      break;
    }
    case 0x01: {  // Get status
      set_reg8(reg_AH, 0);  // Last status = OK
      clear_flag(FLAG_CF);
      break;
    }
    case 0x02: {  // Read sectors
      uint8_t count = get_reg8(reg_AL);
      uint8_t ch_val = get_reg8(reg_CH);
      uint8_t cl_val = get_reg8(reg_CL);
      uint8_t dh_val = get_reg8(reg_DH);

      int sector = cl_val & 0x3F;
      int cyl = ch_val | ((cl_val & 0xC0) << 2);
      int head = dh_val;

      disk_geom g = get_geometry(dl);
      if (g.spt == 0 || !io->disk_present(dl)) {
        set_reg8(reg_AH, 0x01);  // Invalid command
        set_reg8(reg_AL, 0);
        set_flag(FLAG_CF);
        break;
      }

      // CHS sector numbers are 1-based.  With sector 0 the unsigned `sector - 1'
      // below evaluates to 2^64-1 and the byte offset to 2^64-512, which went to
      // dos_io::disk_read/disk_write as-is; a host backend seeking with a signed
      // off_t seeks backwards.  Head and cylinder are bounded too: an
      // out-of-range head does not produce a nonsense offset, it produces a
      // PLAUSIBLE one that aliases onto the next cylinder's data, which is worse
      // than an error.  Note what the cylinder bound means on a large disk:
      // get_geometry clamps a hard disk to 1024 cylinders, so CHS cannot reach
      // past about 504 MB - which is the same limit AH=08 reports to the guest,
      // and the reason INT 13h's LBA extensions (AH=42/43) exist.
      if (sector == 0 || sector > g.spt || head >= g.heads || cyl >= g.cyls) {
        set_reg8(reg_AH, 0x04);  // Sector not found
        set_reg8(reg_AL, 0);
        set_flag(FLAG_CF);
        break;
      }

      uint64_t lba = ((uint64_t)cyl * g.heads + head) * g.spt + (sector - 1);
      uint64_t offset = lba * g.sector_size;

      uint32_t buf_addr = EMU88_MK20(sregs[seg_ES], regs[reg_BX]);

      uint8_t sectors_read = 0;
      for (int i = 0; i < count; i++) {
        uint8_t buf[512];
        size_t n = io->disk_read(dl, offset, buf, 512);
        if (n < 512) break;
        for (int j = 0; j < 512; j++)
          mem->store_mem(buf_addr + j, buf[j]);
        buf_addr += 512;
        offset += 512;
        sectors_read++;
      }

      set_reg8(reg_AH, sectors_read == count ? 0 : 0x04);
      set_reg8(reg_AL, sectors_read);
      set_flag_val(FLAG_CF, sectors_read != count);
      break;
    }
    case 0x03: {  // Write sectors
      uint8_t count = get_reg8(reg_AL);
      uint8_t ch_val = get_reg8(reg_CH);
      uint8_t cl_val = get_reg8(reg_CL);
      uint8_t dh_val = get_reg8(reg_DH);

      int sector = cl_val & 0x3F;
      int cyl = ch_val | ((cl_val & 0xC0) << 2);
      int head = dh_val;

      disk_geom g = get_geometry(dl);
      if (g.spt == 0 || !io->disk_present(dl)) {
        set_reg8(reg_AH, 0x01);
        set_reg8(reg_AL, 0);
        set_flag(FLAG_CF);
        break;
      }

      // CHS sector numbers are 1-based.  With sector 0 the unsigned `sector - 1'
      // below evaluates to 2^64-1 and the byte offset to 2^64-512, which went to
      // dos_io::disk_read/disk_write as-is; a host backend seeking with a signed
      // off_t seeks backwards.  Head and cylinder are bounded too: an
      // out-of-range head does not produce a nonsense offset, it produces a
      // PLAUSIBLE one that aliases onto the next cylinder's data, which is worse
      // than an error.  Note what the cylinder bound means on a large disk:
      // get_geometry clamps a hard disk to 1024 cylinders, so CHS cannot reach
      // past about 504 MB - which is the same limit AH=08 reports to the guest,
      // and the reason INT 13h's LBA extensions (AH=42/43) exist.
      if (sector == 0 || sector > g.spt || head >= g.heads || cyl >= g.cyls) {
        set_reg8(reg_AH, 0x04);  // Sector not found
        set_reg8(reg_AL, 0);
        set_flag(FLAG_CF);
        break;
      }

      uint64_t lba = ((uint64_t)cyl * g.heads + head) * g.spt + (sector - 1);
      uint64_t offset = lba * g.sector_size;

      uint32_t buf_addr = EMU88_MK20(sregs[seg_ES], regs[reg_BX]);

      uint8_t sectors_written = 0;
      for (int i = 0; i < count; i++) {
        uint8_t buf[512];
        for (int j = 0; j < 512; j++)
          buf[j] = mem->fetch_mem(buf_addr + j);
        size_t n = io->disk_write(dl, offset, buf, 512);
        if (n < 512) break;
        buf_addr += 512;
        offset += 512;
        sectors_written++;
      }

      set_reg8(reg_AH, sectors_written == count ? 0 : 0x04);
      set_reg8(reg_AL, sectors_written);
      set_flag_val(FLAG_CF, sectors_written != count);
      break;
    }
    case 0x04: {  // Verify sectors (no-op, just validate params)
      set_reg8(reg_AH, 0);
      clear_flag(FLAG_CF);
      break;
    }
    case 0x08: {  // Get drive parameters
      if (!io->disk_present(dl)) {
        set_reg8(reg_AH, 0x07);  // Drive not ready
        set_flag(FLAG_CF);
        break;
      }

      disk_geom g = get_geometry(dl);
      int max_cyl = g.cyls - 1;
      int max_head = g.heads - 1;
      int max_sector = g.spt;

      set_reg8(reg_AH, 0);
      set_reg8(reg_CH, max_cyl & 0xFF);
      set_reg8(reg_CL, (max_sector & 0x3F) | ((max_cyl >> 2) & 0xC0));
      set_reg8(reg_DH, max_head);

      // DL = number of drives
      if (dl < 0x80) {
        int nfloppy = 0;
        for (int d = 0; d < 4; d++)
          if (io->disk_present(d)) nfloppy++;
        set_reg8(reg_DL, nfloppy ? nfloppy : 1);
        set_reg8(reg_BL, 0x04);  // 1.44MB type
      } else {
        set_reg8(reg_DL, bda_r8(bda::NUM_HDD));
      }

      // ES:DI -> disk parameter table
      sregs[seg_ES] = 0xF000;
      regs[reg_DI] = 0xEFC7;

      clear_flag(FLAG_CF);
      break;
    }
    case 0x15: {  // Get disk type
      if (!io->disk_present(dl)) {
        // AH=00h IS the answer for "drive not present", and it is a SUCCESS
        // return: CF clear.  Setting CF made a caller that branches on CF first
        // read a missing drive as an I/O error.
        set_reg8(reg_AH, 0);
        clear_flag(FLAG_CF);
        break;
      }
      if (dl < 0x80) {
        set_reg8(reg_AH, 1);  // Floppy without change detect
      } else {
        set_reg8(reg_AH, 3);  // Hard disk
        uint64_t sectors = io->disk_size(dl) / 512;
        regs[reg_CX] = (sectors >> 16) & 0xFFFF;
        regs[reg_DX] = sectors & 0xFFFF;
      }
      clear_flag(FLAG_CF);
      break;
    }
    case 0x41: {  // INT 13h extensions check
      if (!io->disk_present(dl)) {
        set_reg8(reg_AH, 0x01);
        set_flag(FLAG_CF);
        break;
      }
      // Check BX == 0x55AA
      if (regs[reg_BX] != 0x55AA) {
        set_reg8(reg_AH, 0x01);
        set_flag(FLAG_CF);
        break;
      }
      // Extensions supported: fixed disk access (bit 0)
      set_reg8(reg_AH, 0x21);  // Version 2.1
      regs[reg_BX] = 0xAA55;   // Signature
      regs[reg_CX] = 0x0001;   // Fixed disk access subset
      clear_flag(FLAG_CF);
      break;
    }
    case 0x42: {  // Extended read sectors
      if (!io->disk_present(dl)) {
        set_reg8(reg_AH, 0x01);
        set_flag(FLAG_CF);
        break;
      }
      // DS:SI -> Disk Address Packet
      uint32_t dap_addr = EMU88_MK20(sregs[seg_DS], regs[reg_SI]);
      uint8_t dap_size = mem->fetch_mem(dap_addr);
      if (dap_size < 16) {
        set_reg8(reg_AH, 0x01);
        set_flag(FLAG_CF);
        break;
      }
      uint16_t count = mem->fetch_mem16(dap_addr + 2);
      uint16_t buf_off = mem->fetch_mem16(dap_addr + 4);
      uint16_t buf_seg = mem->fetch_mem16(dap_addr + 6);
      uint32_t lba_lo = mem->fetch_mem16(dap_addr + 8) |
                        ((uint32_t)mem->fetch_mem16(dap_addr + 10) << 16);
      uint32_t lba_hi = mem->fetch_mem16(dap_addr + 12) |
                        ((uint32_t)mem->fetch_mem16(dap_addr + 14) << 16);
      (void)lba_hi;  // Ignore high 32 bits

      int sec_size = io->disk_sector_size(dl);
      uint64_t offset = (uint64_t)lba_lo * sec_size;
      uint32_t buf_addr = EMU88_MK20(buf_seg, buf_off);

      uint16_t sectors_read = 0;
      for (uint16_t i = 0; i < count; i++) {
        uint8_t buf[2048];
        size_t n = io->disk_read(dl, offset, buf, sec_size);
        if ((int)n < sec_size) break;
        for (int j = 0; j < sec_size; j++)
          mem->store_mem(buf_addr + j, buf[j]);
        buf_addr += sec_size;
        offset += sec_size;
        sectors_read++;
      }

      // Update count in DAP
      mem->store_mem(dap_addr + 2, sectors_read & 0xFF);
      mem->store_mem(dap_addr + 3, (sectors_read >> 8) & 0xFF);

      set_reg8(reg_AH, sectors_read == count ? 0 : 0x04);
      set_flag_val(FLAG_CF, sectors_read != count);
      break;
    }
    case 0x43: {  // Extended write sectors
      if (!io->disk_present(dl)) {
        set_reg8(reg_AH, 0x01);
        set_flag(FLAG_CF);
        break;
      }
      uint32_t dap_addr = EMU88_MK20(sregs[seg_DS], regs[reg_SI]);
      uint8_t dap_size = mem->fetch_mem(dap_addr);
      if (dap_size < 16) {
        set_reg8(reg_AH, 0x01);
        set_flag(FLAG_CF);
        break;
      }
      uint16_t count = mem->fetch_mem16(dap_addr + 2);
      uint16_t buf_off = mem->fetch_mem16(dap_addr + 4);
      uint16_t buf_seg = mem->fetch_mem16(dap_addr + 6);
      uint32_t lba_lo = mem->fetch_mem16(dap_addr + 8) |
                        ((uint32_t)mem->fetch_mem16(dap_addr + 10) << 16);

      int sec_size = io->disk_sector_size(dl);
      uint64_t offset = (uint64_t)lba_lo * sec_size;
      uint32_t buf_addr = EMU88_MK20(buf_seg, buf_off);

      uint16_t sectors_written = 0;
      for (uint16_t i = 0; i < count; i++) {
        uint8_t buf[2048];
        for (int j = 0; j < sec_size; j++)
          buf[j] = mem->fetch_mem(buf_addr + j);
        size_t n = io->disk_write(dl, offset, buf, sec_size);
        if ((int)n < sec_size) break;
        buf_addr += sec_size;
        offset += sec_size;
        sectors_written++;
      }

      mem->store_mem(dap_addr + 2, sectors_written & 0xFF);
      mem->store_mem(dap_addr + 3, (sectors_written >> 8) & 0xFF);

      set_reg8(reg_AH, sectors_written == count ? 0 : 0x04);
      set_flag_val(FLAG_CF, sectors_written != count);
      break;
    }
    case 0x48: {  // Get extended drive parameters
      if (!io->disk_present(dl)) {
        set_reg8(reg_AH, 0x01);
        set_flag(FLAG_CF);
        break;
      }
      uint32_t buf_addr = EMU88_MK20(sregs[seg_DS], regs[reg_SI]);
      uint16_t buf_size = mem->fetch_mem16(buf_addr);
      if (buf_size < 26) {
        set_reg8(reg_AH, 0x01);
        set_flag(FLAG_CF);
        break;
      }

      int sec_size = io->disk_sector_size(dl);
      uint64_t total_sectors = io->disk_size(dl) / sec_size;

      // Fill result buffer (26 bytes minimum)
      mem->store_mem16(buf_addr + 0, 26);       // Size
      mem->store_mem16(buf_addr + 2, 0x0002);   // Info flags (removable if CD)
      // Cylinders, heads, sectors per track (set to 0 for LBA-only)
      mem->store_mem16(buf_addr + 4, 0); mem->store_mem16(buf_addr + 6, 0);
      mem->store_mem16(buf_addr + 8, 0); mem->store_mem16(buf_addr + 10, 0);
      mem->store_mem16(buf_addr + 12, 0); mem->store_mem16(buf_addr + 14, 0);
      // Total sectors (64-bit)
      mem->store_mem16(buf_addr + 16, total_sectors & 0xFFFF);
      mem->store_mem16(buf_addr + 18, (total_sectors >> 16) & 0xFFFF);
      mem->store_mem16(buf_addr + 20, (total_sectors >> 32) & 0xFFFF);
      mem->store_mem16(buf_addr + 22, (total_sectors >> 48) & 0xFFFF);
      // Bytes per sector
      mem->store_mem16(buf_addr + 24, sec_size);

      set_reg8(reg_AH, 0);
      clear_flag(FLAG_CF);
      break;
    }
    case 0x4B: {  // Get bootable CD-ROM status (El Torito)
      if (!io->disk_present(dl)) {
        set_reg8(reg_AH, 0x01);
        set_flag(FLAG_CF);
        break;
      }
      // Only valid for CD-ROM drives
      if (dl < 0xE0) {
        set_reg8(reg_AH, 0x01);
        set_flag(FLAG_CF);
        break;
      }
      // AL=0: terminate emulation, AL=1: get status
      // Return specification packet in DS:SI buffer
      uint32_t buf_addr = EMU88_MK20(sregs[seg_DS], regs[reg_SI]);
      // 19-byte specification packet
      mem->store_mem(buf_addr + 0, 0x13);  // Size (19)
      mem->store_mem(buf_addr + 1, 0x00);  // Media type (no emulation)
      mem->store_mem(buf_addr + 2, dl);    // Drive number
      // Rest zeroed
      for (int i = 3; i < 19; i++)
        mem->store_mem(buf_addr + i, 0);
      set_reg8(reg_AH, 0);
      clear_flag(FLAG_CF);
      break;
    }
    default:
      set_reg8(reg_AH, 0x01);  // Invalid function
      set_flag(FLAG_CF);
      break;
  }
}

//=============================================================================
// INT 14h - Serial Port (stub)
//=============================================================================

void dos_machine::bios_int14h() {
  set_reg8(reg_AH, 0x80);  // Timeout
}

//=============================================================================
// INT 15h - System Services
//=============================================================================

void dos_machine::bios_int15h() {
  uint8_t ah = get_reg8(reg_AH);

  switch (ah) {
    case 0x41:  // Wait for external event - not supported
      set_flag(FLAG_CF);
      break;
    case 0x86:  // Wait (delay) - return immediately
      clear_flag(FLAG_CF);
      break;
    case 0x87: {  // Extended memory block move
      // ES:SI -> GDT with source/dest descriptors
      // CX = number of words to move
      uint32_t gdt_addr = EMU88_MK20(sregs[seg_ES], regs[reg_SI]);
      // GDT entries: [0]=dummy, [1]=GDT, [2]=source, [3]=dest, [4]=BIOS CS, [5]=SS
      // Each entry is 8 bytes. Source descriptor at offset 16, dest at offset 24.
      // Descriptor format: limit(2), base_lo(2), base_mid(1), access(1), ...
      uint32_t src_base = mem->fetch_mem(gdt_addr + 16 + 2) |
                          ((uint32_t)mem->fetch_mem(gdt_addr + 16 + 3) << 8) |
                          ((uint32_t)mem->fetch_mem(gdt_addr + 16 + 4) << 16);
      uint32_t dst_base = mem->fetch_mem(gdt_addr + 24 + 2) |
                          ((uint32_t)mem->fetch_mem(gdt_addr + 24 + 3) << 8) |
                          ((uint32_t)mem->fetch_mem(gdt_addr + 24 + 4) << 16);
      uint16_t words = regs[reg_CX];

      // Temporarily enable A20 for the copy
      bool old_a20 = mem->get_a20();
      mem->set_a20(true);
      for (uint32_t i = 0; i < (uint32_t)words * 2; i++) {
        uint8_t b = mem->fetch_mem(src_base + i);
        mem->store_mem(dst_base + i, b);
      }
      mem->set_a20(old_a20);

      set_reg8(reg_AH, 0);
      clear_flag(FLAG_CF);
      break;
    }
    case 0x88: {  // Extended memory size (KB above 1MB)
      uint32_t total = mem->get_mem_size();
      uint32_t ext_kb = total > 0x100000 ? (total - 0x100000) / 1024 : 0;
      if (ext_kb > 0xFFFF) ext_kb = 0xFFFF;  // Cap at 64MB
      regs[reg_AX] = ext_kb;
      clear_flag(FLAG_CF);
      break;
    }
    case 0x91:  // Interrupt complete (device post) - no-op
      clear_flag(FLAG_CF);
      break;
    case 0xC0: {  // Get system configuration
      // Return pointer to system config table at F000:E6F5
      sregs[seg_ES] = 0xF000;
      regs[reg_BX] = 0xE6F5;
      set_reg8(reg_AH, 0);
      clear_flag(FLAG_CF);
      break;
    }
    case 0x4F:  // Keyboard intercept - let key through
      set_flag(FLAG_CF);  // CF=1 = process key normally
      break;
    case 0x24:  // A20 gate support
      set_reg8(reg_AH, 0x86);
      set_flag(FLAG_CF);
      break;
    default:
      set_reg8(reg_AH, 0x86);
      set_flag(FLAG_CF);
      break;
  }
}

//=============================================================================
// INT 16h - Keyboard Services
//=============================================================================

void dos_machine::bios_int16h() {
  uint8_t ah = get_reg8(reg_AH);

  switch (ah) {
    case 0x00:    // Read key (blocking)
    case 0x10: {  // Enhanced read key
      uint16_t head = bda_r16(bda::KBD_BUF_HEAD);
      uint16_t tail = bda_r16(bda::KBD_BUF_TAIL);

      if (head == tail) {
        // No key available — signal callers (do_interrupt / unimplemented_opcode)
        // to rewind IP and retry on the next batch.
        waiting_for_key = true;
        return;
      }

      // Read key from buffer
      uint8_t ascii = mem->fetch_mem(0x400 + head);
      uint8_t scan = mem->fetch_mem(0x400 + head + 1);

      // Advance head
      uint16_t next = head + 2;
      if (next >= bda_r16(bda::KBD_BUF_END))
        next = bda_r16(bda::KBD_BUF_START);
      bda_w16(bda::KBD_BUF_HEAD, next);

      set_reg8(reg_AL, ascii);
      set_reg8(reg_AH, scan);
      break;
    }
    case 0x01:    // Check key (non-blocking)
    case 0x11: {  // Enhanced check key
      uint16_t head = bda_r16(bda::KBD_BUF_HEAD);
      uint16_t tail = bda_r16(bda::KBD_BUF_TAIL);

      if (head == tail) {
        set_flag(FLAG_ZF);  // No key
        // Track consecutive no-key polls. Use both a count threshold AND
        // a minimum emulated time to avoid false triggers during:
        // - DOS Ctrl-C checks (~5-20 polls per batch, brief)
        // - FreeDOS boot "Press F8" prompt (~2 sec timer-based timeout)
        // Only yield when the program has been polling continuously for
        // both enough polls AND enough emulated time (~165ms = 3 ticks).
        if (kbd_poll_count == 0)
          kbd_poll_start_cycle = cycles;
        kbd_poll_count++;
        if (kbd_poll_count >= KBD_POLL_THRESHOLD &&
            (cycles - kbd_poll_start_cycle) >= KBD_POLL_MIN_CYCLES)
          waiting_for_key = true;
        return;
      }
      kbd_poll_count = 0;

      // Peek at key (don't remove)
      uint8_t ascii = mem->fetch_mem(0x400 + head);
      uint8_t scan = mem->fetch_mem(0x400 + head + 1);
      set_reg8(reg_AL, ascii);
      set_reg8(reg_AH, scan);
      clear_flag(FLAG_ZF);  // Key available
      break;
    }
    case 0x02:    // Get shift flags
    case 0x12: {  // Enhanced get shift flags
      set_reg8(reg_AL, bda_r8(bda::KBD_FLAGS1));
      if (ah == 0x12)
        set_reg8(reg_AH, bda_r8(bda::KBD_FLAGS2));
      break;
    }
    case 0x03:  // Set typematic rate - stub (no hardware to control)
      break;
    case 0x05: {  // Store key in buffer
      uint8_t ascii = get_reg8(reg_CL);
      uint8_t scan = get_reg8(reg_CH);
      queue_key(ascii, scan);
      set_reg8(reg_AL, 0);
      break;
    }
    case 0x09:  // Get keyboard functionality - report enhanced keyboard
      set_reg8(reg_AL, 0x20);  // Enhanced keyboard support
      break;
    default:
      break;
  }
}

//=============================================================================
// INT 17h - Printer (stub)
//=============================================================================

void dos_machine::bios_int17h() {
  set_reg8(reg_AH, 0x20);  // Printer not available (timeout)
}

//=============================================================================
// INT 19h - Bootstrap Loader
//=============================================================================

void dos_machine::bios_int19h() {
  // Try to boot from drive 0 (A:), then 0x80 (C:)
  for (int drive : {0, 0x80}) {
    if (!io->disk_present(drive)) continue;

    uint8_t sector[512];
    if (io->disk_read(drive, 0, sector, 512) < 512) continue;
    if (sector[510] != 0x55 || sector[511] != 0xAA) continue;

    for (int i = 0; i < 512; i++)
      mem->store_mem(BOOT_LOAD_ADDR + i, sector[i]);

    sregs[seg_CS] = 0x0000;
    ip = 0x7C00;
    regs[reg_DX] = drive;
    return;
  }

  halted = true;
}

//=============================================================================
// INT 1Ah - Time/Date Services
//=============================================================================

static uint8_t to_bcd(int val) {
  return ((val / 10) << 4) | (val % 10);
}

void dos_machine::bios_int1ah() {
  uint8_t ah = get_reg8(reg_AH);

  switch (ah) {
    case 0x00: {  // Get timer tick count
      uint32_t ticks = bda_r32(bda::TIMER_COUNT);
      regs[reg_CX] = (ticks >> 16) & 0xFFFF;
      regs[reg_DX] = ticks & 0xFFFF;
      set_reg8(reg_AL, bda_r8(bda::TIMER_ROLLOVER));
      bda_w8(bda::TIMER_ROLLOVER, 0);
      break;
    }
    case 0x01: {  // Set timer tick count
      uint32_t ticks = ((uint32_t)regs[reg_CX] << 16) | regs[reg_DX];
      bda_w32(bda::TIMER_COUNT, ticks);
      // The AT BIOS clears the midnight-passed flag as part of this, because
      // setting the count is what a program does AFTER handling a rollover.
      // Leaving it set made the very next AH=00 report a rollover that had
      // already been consumed.
      bda_w8(bda::TIMER_ROLLOVER, 0);
      break;
    }
    case 0x02: {  // Get RTC time (BCD)
      int h, m, s, hs;
      io->get_time(h, m, s, hs);
      set_reg8(reg_CH, to_bcd(h));
      set_reg8(reg_CL, to_bcd(m));
      set_reg8(reg_DH, to_bcd(s));
      set_reg8(reg_DL, 0);  // DST flag
      clear_flag(FLAG_CF);
      break;
    }
    case 0x03:  // Set RTC time - ignore
      clear_flag(FLAG_CF);
      break;
    case 0x04: {  // Get RTC date (BCD)
      int y, m, d, w;
      io->get_date(y, m, d, w);
      set_reg8(reg_CH, to_bcd(y / 100));
      set_reg8(reg_CL, to_bcd(y % 100));
      set_reg8(reg_DH, to_bcd(m));
      set_reg8(reg_DL, to_bcd(d));
      clear_flag(FLAG_CF);
      break;
    }
    case 0x05:  // Set RTC date - ignore
      clear_flag(FLAG_CF);
      break;
    default:
      set_flag(FLAG_CF);
      break;
  }
}

//=============================================================================
// INT 33h - Mouse Driver
//=============================================================================

void dos_machine::bios_int33h() {
  uint16_t ax = regs[reg_AX];

  // If mouse not installed, only function 0 works (and reports not installed)
  if (!mouse.installed && ax != 0x0000) return;

  // Poll host for current mouse position/buttons
  if (mouse.installed) {
    int hx, hy, hb;
    io->mouse_get_state(hx, hy, hb);

    // The host reports the pointer in displayed-frame pixels (0..w-1, 0..h-1).
    // Map that onto the guest's INT 33h virtual range [min..max] using the
    // resolution emu88 itself renders, so the pointer spans the whole screen in
    // any mode: a VGA 320x200 (mode 13h) keeps the classic 0..639 x range, while
    // an 800x600 VESA mode whose program set a 0..799 range maps 1:1.
    int sw = 0, sh = 0;
    mouse_screen_dims(sw, sh);
    if (sw > 1) hx = mouse.min_x + (int)((long long)hx * (mouse.max_x - mouse.min_x) / (sw - 1));
    if (sh > 1) hy = mouse.min_y + (int)((long long)hy * (mouse.max_y - mouse.min_y) / (sh - 1));

    // Clamp to the virtual coordinate range
    if (hx < mouse.min_x) hx = mouse.min_x;
    if (hx > mouse.max_x) hx = mouse.max_x;
    if (hy < mouse.min_y) hy = mouse.min_y;
    if (hy > mouse.max_y) hy = mouse.max_y;

    // Track button transitions for press/release counters
    int old_buttons = mouse.buttons;
    mouse.x = hx;
    mouse.y = hy;
    mouse.buttons = hb;

    for (int b = 0; b < 3; b++) {
      int mask = 1 << b;
      if ((hb & mask) && !(old_buttons & mask)) {
        mouse.press_count[b]++;
        mouse.press_x[b] = hx;
        mouse.press_y[b] = hy;
      }
      if (!(hb & mask) && (old_buttons & mask)) {
        mouse.release_count[b]++;
        mouse.release_x[b] = hx;
        mouse.release_y[b] = hy;
      }
    }
  }

  switch (ax) {
    case 0x0000: {  // Reset / detect mouse
      if (mouse.installed) {
        regs[reg_AX] = 0xFFFF;  // Mouse installed
        regs[reg_BX] = 3;       // 3 buttons
        mouse.visible = false;
        mouse.min_x = 0; mouse.max_x = 639;
        mouse.min_y = 0; mouse.max_y = 199;
        mouse.handler_mask = 0;
        for (int b = 0; b < 3; b++) {
          mouse.press_count[b] = 0;
          mouse.release_count[b] = 0;
        }
      } else {
        regs[reg_AX] = 0x0000;  // Not installed
        regs[reg_BX] = 0;
      }
      break;
    }
    case 0x0001:  // Show cursor
      mouse.visible = true;
      break;
    case 0x0002:  // Hide cursor
      mouse.visible = false;
      break;
    case 0x0003:  // Get position and button status
      regs[reg_BX] = mouse.buttons;
      regs[reg_CX] = mouse.x;
      regs[reg_DX] = mouse.y;
      break;
    case 0x0004:  // Set position
      mouse.x = regs[reg_CX];
      mouse.y = regs[reg_DX];
      break;
    case 0x0005: {  // Get button press info
      int btn = regs[reg_BX] & 0x03;
      regs[reg_AX] = mouse.buttons;
      regs[reg_BX] = mouse.press_count[btn];
      regs[reg_CX] = mouse.press_x[btn];
      regs[reg_DX] = mouse.press_y[btn];
      mouse.press_count[btn] = 0;
      break;
    }
    case 0x0006: {  // Get button release info
      int btn = regs[reg_BX] & 0x03;
      regs[reg_AX] = mouse.buttons;
      regs[reg_BX] = mouse.release_count[btn];
      regs[reg_CX] = mouse.release_x[btn];
      regs[reg_DX] = mouse.release_y[btn];
      mouse.release_count[btn] = 0;
      break;
    }
    case 0x0007:  // Set horizontal limits
      mouse.min_x = regs[reg_CX];
      mouse.max_x = regs[reg_DX];
      break;
    case 0x0008:  // Set vertical limits
      mouse.min_y = regs[reg_CX];
      mouse.max_y = regs[reg_DX];
      break;
    case 0x000A:  // Set text cursor (ignored - software cursor)
      break;
    case 0x000B:  // Get motion counters (mickeys)
      regs[reg_CX] = 0;  // No motion delta tracking
      regs[reg_DX] = 0;
      break;
    case 0x000C:  // Set user subroutine
      mouse.handler_mask = regs[reg_CX];
      mouse.handler_seg = sregs[seg_ES];
      mouse.handler_off = regs[reg_DX];
      break;
    case 0x000F:  // Set mickey/pixel ratio
      mouse.sensitivity_x = regs[reg_CX];
      mouse.sensitivity_y = regs[reg_DX];
      break;
    case 0x0015:  // Get sensitivity
      regs[reg_BX] = mouse.sensitivity_x;
      regs[reg_CX] = mouse.sensitivity_y;
      regs[reg_DX] = 50;  // Double-speed threshold
      break;
    case 0x001A:  // Set sensitivity
      mouse.sensitivity_x = regs[reg_BX];
      mouse.sensitivity_y = regs[reg_CX];
      break;
    case 0x0021:  // Software reset
      regs[reg_AX] = mouse.installed ? 0xFFFF : 0x0021;
      regs[reg_BX] = mouse.installed ? 3 : 0;
      break;
    case 0x0024:  // Get mouse info
      regs[reg_BX] = 0x0801;  // Version 8.01
      regs[reg_CX] = 0x04;    // PS/2 type
      regs[reg_DX] = 0x00;    // IRQ 0 (not real)
      break;
    default:
      break;
  }
}

//=============================================================================
// INT E0h - Host File Services (for R.COM / W.COM)
//=============================================================================

void dos_machine::bios_int_e0h() {
  uint8_t ah = get_reg8(reg_AH);

  switch (ah) {
    case 0x01: {  // Open host file for reading
      // DS:DX -> ASCIIZ path string
      uint32_t addr = EMU88_MK20(sregs[seg_DS], regs[reg_DX]);
      char path[256];
      int i;
      for (i = 0; i < 255; i++) {
        uint8_t ch = mem->fetch_mem(addr + i);
        if (ch == 0) break;
        path[i] = (char)ch;
      }
      path[i] = 0;

      if (io->host_file_open_read(path)) {
        clear_flag(FLAG_CF);
      } else {
        set_flag(FLAG_CF);
      }
      break;
    }
    case 0x02: {  // Open host file for writing
      uint32_t addr = EMU88_MK20(sregs[seg_DS], regs[reg_DX]);
      char path[256];
      int i;
      for (i = 0; i < 255; i++) {
        uint8_t ch = mem->fetch_mem(addr + i);
        if (ch == 0) break;
        path[i] = (char)ch;
      }
      path[i] = 0;

      if (io->host_file_open_write(path)) {
        clear_flag(FLAG_CF);
      } else {
        set_flag(FLAG_CF);
      }
      break;
    }
    case 0x03: {  // Read one byte from host file
      int ch = io->host_file_read_byte();
      if (ch < 0) {
        set_flag(FLAG_CF);   // EOF or error
      } else {
        set_reg8(reg_AL, (uint8_t)ch);
        clear_flag(FLAG_CF);
      }
      break;
    }
    case 0x04: {  // Write one byte to host file
      uint8_t byte = get_reg8(reg_DL);
      if (io->host_file_write_byte(byte)) {
        clear_flag(FLAG_CF);
      } else {
        set_flag(FLAG_CF);
      }
      break;
    }
    case 0x05: {  // Close host file
      uint8_t side = get_reg8(reg_AL);
      if (side == 0) {
        io->host_file_close_read();
      } else {
        io->host_file_close_write();
      }
      clear_flag(FLAG_CF);
      break;
    }
    default:
      set_flag(FLAG_CF);
      break;
  }
}

//=============================================================================
// INT 2Fh - Multiplex Interrupt (XMS detection)
//=============================================================================

void dos_machine::bios_int2fh() {
  uint16_t ax = regs[reg_AX];

  if (ax == 0x1680) {
    // Release Current VM Time Slice — standard DOS idle API.
    // Programs call this to declare they are idle.  Return AL=0
    // to indicate support.  Signal idle so the host can yield.
    set_reg8(reg_AL, 0x00);
    waiting_for_key = true;
    return;
  }

  if (ax == 0x1687) {
    // DPMI detection — return our built-in DPMI server entry point
    regs[reg_AX] = 0;          // DPMI available
    regs[reg_BX] = 0x0001;     // 32-bit programs supported
    set_reg8(reg_CL, 3);       // Processor type: 386
    regs[reg_DX] = 0x005A;     // DPMI version 0.90
    regs[reg_SI] = 0;          // No private data paragraph needed
    sregs[seg_ES] = 0xF000;
    regs[reg_DI] = dpmi.mode_switch_off;
    return;
  }

  if (ax == 0x4300) {
    // XMS installed check - return AL=80h if extended memory available
    uint32_t ext_kb = 0;
    if (mem->get_mem_size() > 0x100000)
      ext_kb = (mem->get_mem_size() - 0x100000) / 1024;
    if (ext_kb > 0) {
      set_reg8(reg_AL, 0x80);  // XMS driver installed
    }
    return;
  }

  if (ax == 0x4310) {
    // Get XMS driver entry point -> ES:BX
    sregs[seg_ES] = 0xF000;
    regs[reg_BX] = xms_entry_off;
    return;
  }
}

//=============================================================================
// XMS dispatch - called via FAR CALL to F000:EFD8
//=============================================================================

void dos_machine::xms_dispatch() {
  uint8_t func = get_reg8(reg_AH);

  switch (func) {
    case 0x00: {  // Get XMS version
      regs[reg_AX] = 0x0300;  // XMS version 3.00
      regs[reg_BX] = 0x0100;  // Driver version 1.00
      regs[reg_DX] = 1;       // HMA exists
      break;
    }
    case 0x01: {  // Request HMA
      mem->set_a20(true);
      regs[reg_AX] = 1;
      regs[reg_BX] = 0;
      break;
    }
    case 0x02: {  // Release HMA
      regs[reg_AX] = 1;
      break;
    }
    case 0x03: {  // Global enable A20
      mem->set_a20(true);
      regs[reg_AX] = 1;
      regs[reg_BX] = 0;
      break;
    }
    case 0x04: {  // Global disable A20
      mem->set_a20(false);
      regs[reg_AX] = 1;
      regs[reg_BX] = 0;
      break;
    }
    case 0x05: {  // Local enable A20
      mem->set_a20(true);
      regs[reg_AX] = 1;
      regs[reg_BX] = 0;
      break;
    }
    case 0x06: {  // Local disable A20
      mem->set_a20(false);
      regs[reg_AX] = 1;
      regs[reg_BX] = 0;
      break;
    }
    case 0x07: {  // Query A20 state
      regs[reg_AX] = mem->get_a20() ? 1 : 0;
      regs[reg_BX] = 0;
      break;
    }
    case 0x08: {  // Query free extended memory
      uint32_t total_kb = 0;
      if (mem->get_mem_size() > 0x100000)
        total_kb = (mem->get_mem_size() - 0x100000) / 1024;
      // Subtract HMA (64KB)
      if (total_kb > 64) total_kb -= 64;
      else total_kb = 0;
      // Subtract allocated blocks
      uint32_t alloc_kb = 0;
      for (int i = 0; i < XMS_MAX_HANDLES; i++)
        if (xms_handles[i].allocated)
          alloc_kb += xms_handles[i].size_kb;
      uint32_t free_kb = total_kb > alloc_kb ? total_kb - alloc_kb : 0;
      regs[reg_AX] = free_kb > 0xFFFF ? 0xFFFF : (uint16_t)free_kb;
      regs[reg_DX] = free_kb > 0xFFFF ? 0xFFFF : (uint16_t)free_kb;
      regs[reg_BX] = 0;
      break;
    }
    case 0x09: {  // Allocate extended memory block
      uint16_t req_kb = regs[reg_DX];
      int handle = -1;
      for (int i = 0; i < XMS_MAX_HANDLES; i++) {
        if (!xms_handles[i].allocated) { handle = i; break; }
      }
      if (handle < 0) {
        regs[reg_AX] = 0;
        regs[reg_BX] = 0xA1;  // All handles in use
        break;
      }
      // Sequential allocation above HMA (0x110000+)
      uint32_t alloc_base = 0x110000;
      for (int i = 0; i < XMS_MAX_HANDLES; i++) {
        if (xms_handles[i].allocated) {
          uint32_t end = xms_handles[i].base + xms_handles[i].size_kb * 1024;
          if (end > alloc_base) alloc_base = end;
        }
      }
      uint32_t needed = (uint32_t)req_kb * 1024;
      if (alloc_base + needed > mem->get_mem_size()) {
        regs[reg_AX] = 0;
        regs[reg_BX] = 0xA0;  // Not enough memory
        break;
      }
      xms_handles[handle].allocated = true;
      xms_handles[handle].base = alloc_base;
      xms_handles[handle].size_kb = req_kb;
      xms_handles[handle].lock_count = 0;
      regs[reg_AX] = 1;
      regs[reg_DX] = handle + 1;  // Handle (1-based)
      break;
    }
    case 0x0A: {  // Free extended memory block
      int handle = regs[reg_DX] - 1;
      if (handle < 0 || handle >= XMS_MAX_HANDLES || !xms_handles[handle].allocated) {
        regs[reg_AX] = 0;
        regs[reg_BX] = 0xA2;  // Invalid handle
        break;
      }
      xms_handles[handle].allocated = false;
      xms_handles[handle].base = 0;
      xms_handles[handle].size_kb = 0;
      xms_handles[handle].lock_count = 0;
      regs[reg_AX] = 1;
      break;
    }
    case 0x0B: {  // Move extended memory block
      // DS:SI -> move structure
      uint32_t struct_addr = EMU88_MK20(sregs[seg_DS], regs[reg_SI]);
      uint32_t length = mem->fetch_mem16(struct_addr) |
                        ((uint32_t)mem->fetch_mem16(struct_addr + 2) << 16);
      uint16_t src_handle = mem->fetch_mem16(struct_addr + 4);
      uint32_t src_off = mem->fetch_mem16(struct_addr + 6) |
                         ((uint32_t)mem->fetch_mem16(struct_addr + 8) << 16);
      uint16_t dst_handle = mem->fetch_mem16(struct_addr + 10);
      uint32_t dst_off = mem->fetch_mem16(struct_addr + 12) |
                         ((uint32_t)mem->fetch_mem16(struct_addr + 14) << 16);

      // Length, offsets and handles are all guest-supplied, and the copy below
      // is a raw store_mem loop.  Until 2026-08-27 none of it was checked, so a
      // client that asked to move more bytes than the destination block holds
      // got a SUCCESS return and a write straight past the end of the block into
      // whatever the next allocation is.  XMS 3.0 gives error codes for exactly
      // these: A7h = invalid source offset, A8h = invalid destination offset,
      // A9h = length is not even.
      //
      // A handle of 0 means "conventional memory, the offset is a seg:off pair"
      // and has no block to bound the move against; the A20-on address space is
      // the only limit there, and store_mem already bounds that.
      if (length & 1) { regs[reg_AX] = 0; regs[reg_BX] = 0xA9; break; }

      // Resolve source address
      uint32_t src_addr;
      if (src_handle == 0) {
        src_addr = ((src_off >> 16) << 4) + (src_off & 0xFFFF);
      } else {
        int h = src_handle - 1;
        if (h < 0 || h >= XMS_MAX_HANDLES || !xms_handles[h].allocated) {
          regs[reg_AX] = 0; regs[reg_BX] = 0xA3; break;
        }
        uint64_t block = (uint64_t)xms_handles[h].size_kb * 1024;
        if ((uint64_t)src_off + length > block) {
          regs[reg_AX] = 0; regs[reg_BX] = 0xA7;  // Invalid source offset
          break;
        }
        src_addr = xms_handles[h].base + src_off;
      }

      // Resolve dest address
      uint32_t dst_addr;
      if (dst_handle == 0) {
        dst_addr = ((dst_off >> 16) << 4) + (dst_off & 0xFFFF);
      } else {
        int h = dst_handle - 1;
        if (h < 0 || h >= XMS_MAX_HANDLES || !xms_handles[h].allocated) {
          regs[reg_AX] = 0; regs[reg_BX] = 0xA3; break;
        }
        uint64_t block = (uint64_t)xms_handles[h].size_kb * 1024;
        if ((uint64_t)dst_off + length > block) {
          regs[reg_AX] = 0; regs[reg_BX] = 0xA8;  // Invalid destination offset
          break;
        }
        dst_addr = xms_handles[h].base + dst_off;
      }

      bool old_a20 = mem->get_a20();
      mem->set_a20(true);
      for (uint32_t i = 0; i < length; i++)
        mem->store_mem(dst_addr + i, mem->fetch_mem(src_addr + i));
      mem->set_a20(old_a20);
      regs[reg_AX] = 1;
      break;
    }
    case 0x0C: {  // Lock extended memory block
      int handle = regs[reg_DX] - 1;
      if (handle < 0 || handle >= XMS_MAX_HANDLES || !xms_handles[handle].allocated) {
        regs[reg_AX] = 0; regs[reg_BX] = 0xA2; break;
      }
      xms_handles[handle].lock_count++;
      regs[reg_DX] = (uint16_t)(xms_handles[handle].base >> 16);
      regs[reg_BX] = (uint16_t)(xms_handles[handle].base & 0xFFFF);
      regs[reg_AX] = 1;
      break;
    }
    case 0x0D: {  // Unlock extended memory block
      int handle = regs[reg_DX] - 1;
      if (handle < 0 || handle >= XMS_MAX_HANDLES || !xms_handles[handle].allocated) {
        regs[reg_AX] = 0; regs[reg_BX] = 0xA2; break;
      }
      if (xms_handles[handle].lock_count > 0)
        xms_handles[handle].lock_count--;
      regs[reg_AX] = 1;
      break;
    }
    case 0x0E: {  // Get handle information
      int handle = regs[reg_DX] - 1;
      if (handle < 0 || handle >= XMS_MAX_HANDLES || !xms_handles[handle].allocated) {
        regs[reg_AX] = 0; regs[reg_BX] = 0xA2; break;
      }
      set_reg8(reg_BH, xms_handles[handle].lock_count);
      int free_h = 0;
      for (int i = 0; i < XMS_MAX_HANDLES; i++)
        if (!xms_handles[i].allocated) free_h++;
      set_reg8(reg_BL, free_h > 255 ? 255 : free_h);
      regs[reg_DX] = xms_handles[handle].size_kb > 0xFFFF ?
                     0xFFFF : (uint16_t)xms_handles[handle].size_kb;
      regs[reg_AX] = 1;
      break;
    }
    case 0x0F: {  // Reallocate extended memory block
      int handle = regs[reg_DX] - 1;
      uint16_t new_kb = regs[reg_BX];
      if (handle < 0 || handle >= XMS_MAX_HANDLES || !xms_handles[handle].allocated) {
        regs[reg_AX] = 0; regs[reg_BX] = 0xA2; break;
      }
      uint32_t new_end = xms_handles[handle].base + (uint32_t)new_kb * 1024;
      if (new_end > mem->get_mem_size()) {
        regs[reg_AX] = 0; regs[reg_BX] = 0xA0; break;
      }
      xms_handles[handle].size_kb = new_kb;
      regs[reg_AX] = 1;
      break;
    }
    case 0x88: {  // Query Any Free Extended Memory (XMS 3.0, 32-bit)
      uint32_t total_kb = 0;
      if (mem->get_mem_size() > 0x100000)
        total_kb = (mem->get_mem_size() - 0x100000) / 1024;
      if (total_kb > 64) total_kb -= 64;
      else total_kb = 0;
      uint32_t alloc_kb = 0;
      for (int i = 0; i < XMS_MAX_HANDLES; i++)
        if (xms_handles[i].allocated)
          alloc_kb += xms_handles[i].size_kb;
      uint32_t free_kb = total_kb > alloc_kb ? total_kb - alloc_kb : 0;
      set_reg32(reg_AX, free_kb);   // EAX = largest free block in KB
      set_reg32(reg_DX, free_kb);   // EDX = total free in KB
      set_reg32(reg_CX, free_kb > 0 ? 0x00110000 : 0);  // ECX = highest ending address
      regs[reg_BX] = 0;
      break;
    }
    case 0x89: {  // Allocate Any Extended Memory Block (XMS 3.0, 32-bit)
      uint32_t req_kb = get_reg32(reg_DX);
      int handle = -1;
      for (int i = 0; i < XMS_MAX_HANDLES; i++) {
        if (!xms_handles[i].allocated) { handle = i; break; }
      }
      if (handle < 0) {
        regs[reg_AX] = 0;
        regs[reg_BX] = 0xA1;
        break;
      }
      uint32_t alloc_base = 0x110000;
      for (int i = 0; i < XMS_MAX_HANDLES; i++) {
        if (xms_handles[i].allocated) {
          uint32_t end = xms_handles[i].base + xms_handles[i].size_kb * 1024;
          if (end > alloc_base) alloc_base = end;
        }
      }
      uint32_t needed = req_kb * 1024;
      if (req_kb > 0x100000 || alloc_base + needed > mem->get_mem_size()) {
        regs[reg_AX] = 0;
        regs[reg_BX] = 0xA0;
        break;
      }
      xms_handles[handle].allocated = true;
      xms_handles[handle].base = alloc_base;
      xms_handles[handle].size_kb = req_kb;
      xms_handles[handle].lock_count = 0;
      regs[reg_AX] = 1;
      regs[reg_DX] = handle + 1;
      break;
    }
    case 0x8E: {  // Get EMB Handle Information (XMS 3.0, 32-bit)
      int handle = regs[reg_DX] - 1;
      if (handle < 0 || handle >= XMS_MAX_HANDLES || !xms_handles[handle].allocated) {
        regs[reg_AX] = 0; regs[reg_BX] = 0xA2; break;
      }
      set_reg32(reg_DX, xms_handles[handle].size_kb);
      regs[reg_BX] = xms_handles[handle].lock_count;
      // Count free handles
      int free_handles = 0;
      for (int i = 0; i < XMS_MAX_HANDLES; i++)
        if (!xms_handles[i].allocated) free_handles++;
      set_reg32(reg_CX, free_handles);
      regs[reg_AX] = 1;
      break;
    }
    case 0x10: {  // Request upper memory block
      regs[reg_AX] = 0;
      regs[reg_BX] = 0xB1;  // No UMBs available
      regs[reg_DX] = 0;
      break;
    }
    case 0x11: {  // Release upper memory block
      regs[reg_AX] = 0;
      regs[reg_BX] = 0xB2;
      break;
    }
    default:
      set_reg32(reg_AX, 0);  // Zero full EAX to avoid stale upper bits
      regs[reg_BX] = 0x80;   // Function not implemented
      break;
  }
}

//============================================================================
// VESA VBE 2.0 (SVGA) — INT 10h AH=4Fh
//
// emu88 advertises an S3-class SVGA card with an 8MB linear framebuffer.
// Programs query the VBE controller/mode info, then SetMode (4F02) a packed-
// pixel mode; pixels are written either through the bank-switched 64KB window
// at 0xA0000 (4F05) or the linear-framebuffer aperture (PhysBasePtr). Both
// views address emu88_mem::svga_vram. The compositor (svga_composite) hands the
// framebuffer to the host bridge, which converts 8/15/16/24/32bpp -> RGBA.
//============================================================================

namespace {
struct VesaMode { uint16_t num; uint16_t w; uint16_t h; uint8_t bpp; };
static const VesaMode kVesaModes[] = {
  // 8bpp packed-pixel (palettized)
  {0x100, 640, 400, 8}, {0x101, 640, 480, 8}, {0x103, 800, 600, 8},
  {0x105,1024, 768, 8}, {0x107,1280,1024, 8},
  // 15bpp RGB555
  {0x10D, 320, 200,15}, {0x110, 640, 480,15}, {0x113, 800, 600,15},
  {0x116,1024, 768,15}, {0x119,1280,1024,15},
  // 16bpp RGB565
  {0x10E, 320, 200,16}, {0x111, 640, 480,16}, {0x114, 800, 600,16},
  {0x117,1024, 768,16}, {0x11A,1280,1024,16},
  // 24bpp RGB888
  {0x10F, 320, 200,24}, {0x112, 640, 480,24}, {0x115, 800, 600,24},
  {0x118,1024, 768,24}, {0x11B,1280,1024,24},
  // 32bpp XRGB8888 (vendor mode numbers)
  {0x140, 320, 200,32}, {0x141, 640, 480,32}, {0x142, 800, 600,32},
  {0x143,1024, 768,32}, {0x144,1280,1024,32},
};
static const int kNumVesaModes = (int)(sizeof(kVesaModes)/sizeof(kVesaModes[0]));

static const VesaMode *vesa_find(uint16_t num) {
  num &= 0x3FFF;  // strip LFB(bit14) / no-clear(bit15) request bits
  for (int i = 0; i < kNumVesaModes; i++)
    if (kVesaModes[i].num == num) return &kVesaModes[i];
  return nullptr;
}
static int vesa_bpp_bytes(int bpp) {
  return bpp <= 8 ? 1 : bpp <= 16 ? 2 : bpp <= 24 ? 3 : 4;
}

// Persistent VBE structures live in the video-BIOS ROM region (segment 0xC000).
static const uint32_t VBE_ROM_SEG     = 0xC000;
static const uint32_t VBE_ROM_LIN     = 0xC0000;
static const uint16_t VBE_OFF_OEM     = 0x0010;  // OEM string
static const uint16_t VBE_OFF_VENDOR  = 0x0050;
static const uint16_t VBE_OFF_PRODUCT = 0x0080;
static const uint16_t VBE_OFF_REV     = 0x00C0;
static const uint16_t VBE_OFF_MODES   = 0x0100;  // uint16 mode list, 0xFFFF terminated
static const uint16_t VBE_OFF_PMINFO  = 0x0200;  // 4F0A protected-mode interface table
// Virtual I/O ports written by the 4F0A protected-mode routines (the emulator
// owns both the emitted code and these ports, so any unused port range works).
static const uint16_t VBE_PM_PORT_WIN   = 0x9000;  // SetWindow: AX = bank (64KB granules)
static const uint16_t VBE_PM_PORT_STARTX = 0x9002; // SetDisplayStart: latch pixel-in-line
static const uint16_t VBE_PM_PORT_STARTY = 0x9004; // SetDisplayStart: scan line -> commit
static const uint16_t VBE_PMINFO_LEN  = 40;        // header(8) + win(7) + start(15) + ports(10)
}  // namespace

void dos_machine::vesa_init_rom() {
  if (vesa.rom_ready) return;
  // Video BIOS ROM signature so probes find a ROM at 0xC0000.
  mem->store_mem(VBE_ROM_LIN + 0, 0x55);
  mem->store_mem(VBE_ROM_LIN + 1, 0xAA);
  mem->store_mem(VBE_ROM_LIN + 2, 0x40);  // ROM size in 512-byte blocks (32KB)
  auto put_str = [&](uint16_t off, const char *s) {
    for (uint32_t i = 0; ; i++) {
      mem->store_mem(VBE_ROM_LIN + off + i, (uint8_t)s[i]);
      if (!s[i]) break;
    }
  };
  put_str(VBE_OFF_OEM,     "S3 Incorporated. Trio64 (emu88)");
  put_str(VBE_OFF_VENDOR,  "S3 Incorporated.");
  put_str(VBE_OFF_PRODUCT, "Trio64 (emu88 VESA)");
  put_str(VBE_OFF_REV,     "Rev 1.0");
  uint32_t mp = VBE_ROM_LIN + VBE_OFF_MODES;
  for (int i = 0; i < kNumVesaModes; i++) { mem->store_mem16(mp, kVesaModes[i].num); mp += 2; }
  mem->store_mem16(mp, 0xFFFF);

  // 4F0A protected-mode interface table: a header of routine offsets followed by
  // real 16-bit machine code that performs the bank switch / display-start by
  // OUTing to the emulator's virtual VBE ports (above). A PM client copies this
  // and calls the routines without a real-mode round-trip.
  uint32_t t = VBE_ROM_LIN + VBE_OFF_PMINFO;
  uint16_t off_win = 8, off_start = 15, off_ports = 30;
  mem->store_mem16(t + 0, off_win);    // SetDisplayWindow routine offset
  mem->store_mem16(t + 2, off_start);  // SetDisplayStart routine offset
  mem->store_mem16(t + 4, 0);          // SetPalette routine (0 = use 4F09)
  mem->store_mem16(t + 6, off_ports);  // I/O port / memory list offset
  auto emit = [&](uint32_t off, std::initializer_list<uint8_t> code) {
    uint32_t a = t + off;
    for (uint8_t b : code) mem->store_mem(a++, b);
  };
  // SetDisplayWindow: in BX=window(ignored, only A), DX=position(64KB granules).
  //   MOV AX,DX ; MOV DX,9000h ; OUT DX,AX ; RETF
  emit(off_win, {0x89, 0xD0, 0xBA, 0x00, 0x90, 0xEF, 0xCB});
  // SetDisplayStart: in BX=0, CX=pixel-in-line, DX=scan line.
  //   MOV BX,DX ; MOV AX,CX ; MOV DX,9002h ; OUT DX,AX ;
  //   MOV AX,BX ; MOV DX,9004h ; OUT DX,AX ; RETF
  emit(off_start, {0x89, 0xD3, 0x89, 0xC8, 0xBA, 0x02, 0x90, 0xEF,
                   0x89, 0xD8, 0xBA, 0x04, 0x90, 0xEF, 0xCB});
  // Port list: the three virtual ports, 0xFFFF (end ports), 0xFFFF (end memory).
  mem->store_mem16(t + off_ports + 0, VBE_PM_PORT_WIN);
  mem->store_mem16(t + off_ports + 2, VBE_PM_PORT_STARTX);
  mem->store_mem16(t + off_ports + 4, VBE_PM_PORT_STARTY);
  mem->store_mem16(t + off_ports + 6, 0xFFFF);
  mem->store_mem16(t + off_ports + 8, 0xFFFF);

  vesa.rom_ready = true;
}

bool dos_machine::vesa_set_mode(uint16_t modenum) {
  bool no_clear = (modenum & 0x8000) != 0;   // bit15: preserve framebuffer
  const VesaMode *m = vesa_find(modenum);
  if (!m) return false;
  int bypp = vesa_bpp_bytes(m->bpp);
  uint32_t stride = (uint32_t)m->w * bypp;
  uint32_t fbsize = stride * m->h;
  uint32_t want = svga_vram_bytes < fbsize ? fbsize : svga_vram_bytes;
  mem->svga_ensure(want);
  if (!no_clear && mem->svga_base()) memset(mem->svga_base(), 0, mem->svga_vram_size);
  vesa.active = true;
  vesa.mode = m->num;
  vesa.xres = m->w; vesa.yres = m->h; vesa.bpp = m->bpp;
  vesa.bytes_per_scanline = (int)stride;
  vesa.display_start = 0;
  mem->svga_active = true;
  mem->svga_window_off = 0;
  mem->svga_lfb_phys = SVGA_LFB_PHYS;
  mem->vga_planar = false;
  if (m->bpp == 8) init_default_vga_palette(vga_dac);
  io->video_mode_changed(0x4000 | m->num, m->w, m->h);
  emit_video_frame();
  return true;
}

void dos_machine::svga_composite() {
  const uint8_t *fb = mem->svga_base();
  if (!fb || !vesa.active || vesa.yres <= 0) return;
  // The whole frame (display_start .. display_start + last-pixel) must fit in
  // VRAM, or the bridge's compositor would read past svga_vram. A 4F07 pan can
  // set display_start anywhere; bound the entire frame, not just its origin.
  uint64_t need = (uint64_t)(vesa.yres - 1) * (uint32_t)vesa.bytes_per_scanline
                + (uint64_t)vesa.xres * vesa_bpp_bytes(vesa.bpp);
  uint32_t off = vesa.display_start;
  if ((uint64_t)off + need > mem->svga_vram_size) off = 0;
  io->video_refresh_direct(fb + off, vesa.xres, vesa.yres, vesa.bpp,
                           vesa.bytes_per_scanline, vga_dac);
}

void dos_machine::mouse_screen_dims(int &w, int &h) const {
  if (vesa.active && vesa.xres > 0) { w = vesa.xres; h = vesa.yres; return; }
  if (video_mode == 0x13)           { w = 320; h = 200; return; }
  // Text modes render at cell granularity (8x16); this matches the frame the
  // host receives. (The default INT 33h range is 640x200, so a standard 80x25
  // frame maps to the conventional mouse space.)
  w = screen_cols * 8;
  h = screen_rows * 16;
}

void dos_machine::herc_composite() {
  // Hercules 720x348 mono: 1bpp, four interleaved 8KB banks. Scanline y lives at
  // base + (y & 3)*0x2000 + (y>>2)*90; pixel (x,y) is bit 7-(x&7) of byte x>>3.
  static uint8_t buf[720 * 348];
  static uint8_t pal[256][3];
  static bool pal_init = false;
  if (!pal_init) {
    memset(pal, 0, sizeof(pal));
    pal[15][0] = 63; pal[15][1] = 42; pal[15][2] = 0;  // amber-on-black
    pal_init = true;
  }
  uint32_t base = herc_page1 ? 0xB8000 : 0xB0000;
  for (int y = 0; y < 348; y++) {
    uint32_t row = base + (uint32_t)(y & 3) * 0x2000 + (uint32_t)(y >> 2) * 90;
    uint8_t *dst = buf + (size_t)y * 720;
    for (int x = 0; x < 720; x++) {
      uint8_t byte = mem->fetch_mem(row + (x >> 3));
      dst[x] = ((byte >> (7 - (x & 7))) & 1) ? 15 : 0;
    }
  }
  io->video_refresh_gfx(buf, 720, 348, pal);
}

void dos_machine::emit_video_frame() {
  if (vesa.active) { svga_composite(); return; }
  if (herc_gfx_on)  { herc_composite(); return; }
  if (video_mode == 0x13) {
    if (mem->vga_planar) {
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

void dos_machine::vesa_bios() {
  vesa_init_rom();
  uint8_t al = get_reg8(reg_AL);
  auto es_di_lin = [&]() -> uint32_t {
    if (protected_mode() && !v86_mode())
      return seg_cache[seg_ES].base + get_reg32(reg_DI);
    return EMU88_MK20(sregs[seg_ES], get_reg16(reg_DI));
  };

  switch (al) {
    case 0x00: {  // Return VBE controller information -> ES:DI (VbeInfoBlock, 512 bytes)
      uint32_t p = es_di_lin();
      for (int i = 0; i < 512; i++) mem->store_mem(p + i, 0);
      mem->store_mem(p + 0, 'V'); mem->store_mem(p + 1, 'E');
      mem->store_mem(p + 2, 'S'); mem->store_mem(p + 3, 'A');
      mem->store_mem16(p + 0x04, 0x0200);                              // VBE 2.0
      mem->store_mem32(p + 0x06, (VBE_ROM_SEG << 16) | VBE_OFF_OEM);   // OemStringPtr
      mem->store_mem32(p + 0x0A, 0x00000000);                         // Capabilities (6-bit DAC, no switch)
      mem->store_mem32(p + 0x0E, (VBE_ROM_SEG << 16) | VBE_OFF_MODES); // VideoModePtr
      mem->store_mem16(p + 0x12, (uint16_t)(svga_vram_bytes / 65536)); // TotalMemory (64KB units)
      mem->store_mem16(p + 0x14, 0x0100);                             // OEM software rev
      mem->store_mem32(p + 0x16, (VBE_ROM_SEG << 16) | VBE_OFF_VENDOR);
      mem->store_mem32(p + 0x1A, (VBE_ROM_SEG << 16) | VBE_OFF_PRODUCT);
      mem->store_mem32(p + 0x1E, (VBE_ROM_SEG << 16) | VBE_OFF_REV);
      set_reg16(reg_AX, 0x004F);
      break;
    }
    case 0x01: {  // Return VBE mode information -> ES:DI (ModeInfoBlock, 256 bytes)
      uint16_t modenum = get_reg16(reg_CX);
      const VesaMode *m = vesa_find(modenum);
      uint32_t p = es_di_lin();
      for (int i = 0; i < 256; i++) mem->store_mem(p + i, 0);
      if (!m) { set_reg16(reg_AX, 0x014F); break; }
      int bypp = vesa_bpp_bytes(m->bpp);
      uint32_t stride = (uint32_t)m->w * bypp;
      uint32_t fbsize = stride * m->h;
      mem->store_mem16(p + 0x00, 0x009B);   // attrs: supported|info|color|graphics|LFB
      mem->store_mem(p + 0x02, 0x07);       // WinA: relocatable|readable|writable
      mem->store_mem(p + 0x03, 0x00);       // WinB
      mem->store_mem16(p + 0x04, 64);       // WinGranularity (KB)
      mem->store_mem16(p + 0x06, 64);       // WinSize (KB)
      mem->store_mem16(p + 0x08, 0xA000);   // WinASegment
      mem->store_mem16(p + 0x0A, 0x0000);   // WinBSegment
      mem->store_mem32(p + 0x0C, 0);        // WinFuncPtr (programs use 4F05)
      mem->store_mem16(p + 0x10, (uint16_t)stride);   // BytesPerScanLine
      mem->store_mem16(p + 0x12, m->w);     // XResolution
      mem->store_mem16(p + 0x14, m->h);     // YResolution
      mem->store_mem(p + 0x16, 8);          // XCharSize
      mem->store_mem(p + 0x17, 16);         // YCharSize
      mem->store_mem(p + 0x18, 1);          // NumberOfPlanes
      mem->store_mem(p + 0x19, (uint8_t)m->bpp);      // BitsPerPixel
      mem->store_mem(p + 0x1A, 1);          // NumberOfBanks
      mem->store_mem(p + 0x1B, m->bpp <= 8 ? 4 : 6);  // MemoryModel: 4=packed, 6=direct
      mem->store_mem(p + 0x1C, 0);          // BankSize
      uint8_t pages = (fbsize && svga_vram_bytes >= fbsize)
                        ? (uint8_t)((svga_vram_bytes / fbsize) - 1) : 0;
      mem->store_mem(p + 0x1D, pages);      // NumberOfImagePages
      mem->store_mem(p + 0x1E, 1);          // reserved (=1 for VBE>=1.2)
      auto setmask = [&](uint8_t rs, uint8_t rp, uint8_t gs, uint8_t gp,
                         uint8_t bs, uint8_t bp, uint8_t xs, uint8_t xp) {
        mem->store_mem(p + 0x1F, rs); mem->store_mem(p + 0x20, rp);
        mem->store_mem(p + 0x21, gs); mem->store_mem(p + 0x22, gp);
        mem->store_mem(p + 0x23, bs); mem->store_mem(p + 0x24, bp);
        mem->store_mem(p + 0x25, xs); mem->store_mem(p + 0x26, xp);
        mem->store_mem(p + 0x36, rs); mem->store_mem(p + 0x37, rp);  // VBE3 linear masks
        mem->store_mem(p + 0x38, gs); mem->store_mem(p + 0x39, gp);
        mem->store_mem(p + 0x3A, bs); mem->store_mem(p + 0x3B, bp);
        mem->store_mem(p + 0x3C, xs); mem->store_mem(p + 0x3D, xp);
      };
      switch (m->bpp) {
        case 15: setmask(5, 10, 5, 5, 5, 0, 1, 15); break;  // RGB555
        case 16: setmask(5, 11, 6, 5, 5, 0, 0, 0);  break;  // RGB565
        case 24: setmask(8, 16, 8, 8, 8, 0, 0, 0);  break;  // RGB888
        case 32: setmask(8, 16, 8, 8, 8, 0, 8, 24); break;  // XRGB8888
        default: break;                                     // 8bpp: palettized
      }
      mem->store_mem(p + 0x27, 0);                  // DirectColorModeInfo
      mem->store_mem32(p + 0x28, SVGA_LFB_PHYS);    // PhysBasePtr (linear FB)
      mem->store_mem16(p + 0x32, (uint16_t)stride); // LinBytesPerScanLine (VBE3)
      mem->store_mem(p + 0x34, pages);              // BnkNumberOfImagePages
      mem->store_mem(p + 0x35, pages);              // LinNumberOfImagePages
      set_reg16(reg_AX, 0x004F);
      break;
    }
    case 0x02: {  // Set VBE mode (BX = mode, bit14=LFB, bit15=no clear)
      uint16_t modenum = get_reg16(reg_BX);
      set_reg16(reg_AX, vesa_set_mode(modenum) ? 0x004F : 0x014F);
      break;
    }
    case 0x03: {  // Return current VBE mode -> BX
      set_reg16(reg_BX, vesa.active ? (uint16_t)(vesa.mode | 0x4000)
                                    : (uint16_t)(video_mode & 0xFF));
      set_reg16(reg_AX, 0x004F);
      break;
    }
    case 0x05: {  // Display window control (bank switch)
      uint8_t bh = get_reg8(reg_BH);   // 0=set, 1=get
      uint8_t bl = get_reg8(reg_BL);   // window number (0=A)
      if (bl != 0) { set_reg16(reg_AX, 0x014F); break; }
      if (bh == 0) {
        mem->svga_window_off = (uint32_t)get_reg16(reg_DX) * 65536u;  // DX in 64KB units
        set_reg16(reg_AX, 0x004F);
      } else if (bh == 1) {
        set_reg16(reg_DX, (uint16_t)(mem->svga_window_off / 65536u));
        set_reg16(reg_AX, 0x004F);
      } else set_reg16(reg_AX, 0x014F);
      break;
    }
    case 0x06: {  // Get/Set logical scan line length
      uint8_t bl = get_reg8(reg_BL);
      int bypp = vesa_bpp_bytes(vesa.bpp ? vesa.bpp : 8);
      // BL=00 set in pixels (CX), BL=02 set in bytes (CX). A logical scanline
      // wider than the displayed width gives a virtual screen the program pans
      // across with 4F07. Clamp to >= visible width and to what VRAM holds.
      if (vesa.active && (bl == 0x00 || bl == 0x02)) {
        uint32_t want = (bl == 0x00) ? (uint32_t)get_reg16(reg_CX) * bypp
                                     : (uint32_t)get_reg16(reg_CX);
        uint32_t min_bytes = (uint32_t)vesa.xres * bypp;
        if (want < min_bytes) want = min_bytes;
        if (vesa.yres > 0 && want * (uint32_t)vesa.yres <= svga_vram_bytes)
          vesa.bytes_per_scanline = (int)want;
      }
      uint32_t stride = vesa.bytes_per_scanline ? (uint32_t)vesa.bytes_per_scanline : 1;
      set_reg16(reg_BX, (uint16_t)stride);                       // bytes per scan line
      set_reg16(reg_CX, (uint16_t)(stride / (bypp ? bypp : 1))); // pixels per scan line
      set_reg16(reg_DX, (uint16_t)(svga_vram_bytes / stride));   // max scan lines that fit
      set_reg16(reg_AX, 0x004F);
      break;
    }
    case 0x07: {  // Get/Set display start (pan)
      uint8_t bl = get_reg8(reg_BL);
      int bypp = vesa_bpp_bytes(vesa.bpp ? vesa.bpp : 8);
      uint32_t stride = vesa.bytes_per_scanline ? (uint32_t)vesa.bytes_per_scanline : 1;
      if (bl == 0x00 || bl == 0x80) {                      // set
        vesa.display_start = (uint32_t)get_reg16(reg_DX) * stride
                           + (uint32_t)get_reg16(reg_CX) * bypp;
        set_reg16(reg_AX, 0x004F);
      } else if (bl == 0x01) {                             // get
        set_reg16(reg_DX, (uint16_t)(vesa.display_start / stride));
        set_reg16(reg_CX, (uint16_t)((vesa.display_start % stride) / (bypp ? bypp : 1)));
        set_reg16(reg_AX, 0x004F);
      } else set_reg16(reg_AX, 0x014F);
      break;
    }
    case 0x08: {  // Get/Set DAC palette format — we keep a 6-bit DAC
      set_reg8(reg_BH, 6);
      set_reg16(reg_AX, 0x004F);
      break;
    }
    case 0x09: {  // Get/Set palette data (entries are B,G,R,align)
      uint8_t bl = get_reg8(reg_BL);
      uint16_t count = get_reg16(reg_CX);
      uint16_t start = get_reg16(reg_DX);
      uint32_t tbl = es_di_lin();
      if (bl == 0x00 || bl == 0x80) {                      // set palette
        for (uint16_t i = 0; i < count && (start + i) < 256; i++) {
          uint8_t b = mem->fetch_mem(tbl + i * 4 + 0);
          uint8_t g = mem->fetch_mem(tbl + i * 4 + 1);
          uint8_t r = mem->fetch_mem(tbl + i * 4 + 2);
          vga_dac[start + i][0] = r & 0x3F;
          vga_dac[start + i][1] = g & 0x3F;
          vga_dac[start + i][2] = b & 0x3F;
        }
        set_reg16(reg_AX, 0x004F);
      } else if (bl == 0x01) {                             // get palette
        for (uint16_t i = 0; i < count && (start + i) < 256; i++) {
          mem->store_mem(tbl + i * 4 + 0, vga_dac[start + i][2]);  // B
          mem->store_mem(tbl + i * 4 + 1, vga_dac[start + i][1]);  // G
          mem->store_mem(tbl + i * 4 + 2, vga_dac[start + i][0]);  // R
          mem->store_mem(tbl + i * 4 + 3, 0);
        }
        set_reg16(reg_AX, 0x004F);
      } else set_reg16(reg_AX, 0x014F);
      break;
    }
    case 0x0A: {  // Return VBE 2.0 protected-mode interface
      if (get_reg8(reg_BL) != 0x00) { set_reg16(reg_AX, 0x014F); break; }
      // ES:DI -> the PM interface table in the VBE ROM; CX = its length.
      load_segment_real(seg_ES, VBE_ROM_SEG);
      set_reg16(reg_DI, VBE_OFF_PMINFO);
      set_reg16(reg_CX, VBE_PMINFO_LEN);
      set_reg16(reg_AX, 0x004F);
      break;
    }
    case 0x04:  // Save/restore state (optional)
    default:
      set_reg16(reg_AX, 0x014F);  // AL=4F (supported), AH=01 (failed/unsupported)
      break;
  }
}
