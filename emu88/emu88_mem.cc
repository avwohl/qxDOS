#include "emu88_mem.h"
#include <cstring>
#include <cstdio>

emu88_mem::emu88_mem(emu88_uint32 size)
    : dat(nullptr), mem_size(size), a20_enabled(false) {
  dat = new emu88_uint8[mem_size];
  memset(dat, 0, mem_size);
  memset(vga_planes, 0, sizeof(vga_planes));
}

emu88_mem::~emu88_mem() {
  delete[] dat;
  dat = nullptr;
}

emu88_uint8 emu88_mem::fetch_mem(emu88_uint32 addr) {
  emu88_uint32 masked = mask_addr(addr);
  if (masked >= mem_size) {
    fprintf(stderr, "[MEM] fetch_mem OOB: addr=0x%08X masked=0x%08X mem_size=0x%08X\n", addr, masked, mem_size);
    return 0xFF;
  }
  // VGA plane read: in planar mode, reads from 0xA0000-0xAFFFF return selected plane
  if (vga_planar && masked >= 0xA0000 && masked < 0xB0000) {
    uint32_t offset = masked - 0xA0000;
    return vga_planes[vga_read_map & 3][offset];
  }
  return dat[masked];
}

void emu88_mem::store_mem(emu88_uint32 addr, emu88_uint8 abyte) {
  emu88_uint32 masked = mask_addr(addr);
  if (masked >= mem_size) {
    fprintf(stderr, "[MEM] store_mem OOB: addr=0x%08X masked=0x%08X mem_size=0x%08X\n", addr, masked, mem_size);
    return;
  }
  // VGA plane write: in planar mode, writes to 0xA0000-0xAFFFF go to selected planes
  if (vga_planar && masked >= 0xA0000 && masked < 0xB0000) {
    uint32_t offset = masked - 0xA0000;
    for (int p = 0; p < 4; p++) {
      if (vga_map_mask & (1 << p))
        vga_planes[p][offset] = abyte;
    }
    return;
  }
  if (watchpoint_addr != 0xFFFFFFFF) {
    emu88_uint32 base = watchpoint_addr - 0x2E;
    // Watch [002E], [0098]-[009F], [099E]-[099F]
    if (masked == watchpoint_addr ||
        (masked >= base + 0x98 && masked <= base + 0x9F) ||
        (masked >= base + 0x99E && masked <= base + 0x99F)) {
      fprintf(stderr, "[WATCH] phys 0x%08X: 0x%02X -> 0x%02X (off=+%04X)\n",
              masked, dat[masked], abyte, masked - base);
    }
  }
  // IVT[21h] watchpoint: catch whatever zeroes it (temp debug)
  if (masked >= 0x84 && masked <= 0x87) {
    if (dat[masked] != abyte) {
      ivt21_trap = true;  // Signal CPU to log CS:IP
      fprintf(stderr, "[IVT21-WRITE] phys=%08X: 0x%02X -> 0x%02X\n",
              masked, dat[masked], abyte);
    }
  }
  // Trace writes to DPMI LDT area (entries 12+) - temporary debug
  if (masked >= 0x00FE2860 && masked < 0x00FE6800) {
    static int ldt_write_log = 0;
    if (ldt_write_log < 500) {
      // Only log non-zero writes after the first 100 (skip init zeroing)
      if (ldt_write_log < 100 || abyte != 0x00) {
        ldt_write_log++;
        uint32_t entry = (masked - 0x00FE2800) / 8;
        uint32_t boff = (masked - 0x00FE2800) % 8;
        fprintf(stderr, "[LDT-WRITE] phys=%08X LDT[%d]+%d: 0x%02X -> 0x%02X\n",
                masked, entry, boff, dat[masked], abyte);
      }
    }
  }
  // Trace writes to DPMI GDT area (all entries) - temporary debug
  if (masked >= 0x00FE0000 && masked < 0x00FE2000) {
    static int gdt_write_log = 0;
    if (gdt_write_log < 200) {
      // Only log non-zero writes after the first 50 (skip init zeroing)
      if (gdt_write_log < 50 || abyte != 0x00) {
        gdt_write_log++;
        uint32_t entry = (masked - 0x00FE0000) / 8;
        uint32_t boff = (masked - 0x00FE0000) % 8;
        fprintf(stderr, "[GDT-WRITE] phys=%08X GDT[%d]+%d: 0x%02X -> 0x%02X\n",
                masked, entry, boff, dat[masked], abyte);
      }
    }
  }
  dat[masked] = abyte;
}

emu88_uint16 emu88_mem::fetch_mem16(emu88_uint32 addr) {
  return fetch_mem(addr) | (emu88_uint16(fetch_mem(addr + 1)) << 8);
}

void emu88_mem::store_mem16(emu88_uint32 addr, emu88_uint16 aword) {
  store_mem(addr, aword & 0xFF);
  store_mem(addr + 1, (aword >> 8) & 0xFF);
}

emu88_uint32 emu88_mem::fetch_mem32(emu88_uint32 addr) {
  return fetch_mem16(addr) | (emu88_uint32(fetch_mem16(addr + 2)) << 16);
}

void emu88_mem::store_mem32(emu88_uint32 addr, emu88_uint32 adword) {
  store_mem16(addr, adword & 0xFFFF);
  store_mem16(addr + 2, (adword >> 16) & 0xFFFF);
}
