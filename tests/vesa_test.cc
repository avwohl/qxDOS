// VESA / SVGA (VBE) end-to-end test. Drives the full emu88 + DOS machine
// through the real INT 10h dispatch and verifies the VbeInfoBlock/ModeInfoBlock
// layout, the LFB + bank-switched VRAM routing, the compositor pan clamp, the
// 4F06 logical-scanline set, the 4F0A protected-mode routines (executed), and
// the INT 33h mouse coordinate scaling.
#include "dos_machine.h"
#include "dos_io.h"
#include "emu88_mem.h"
#include <cstdio>
#include <cstring>

struct StubIO : dos_io {
  int last_w=0,last_h=0,last_bpp=0,last_stride=0; const uint8_t *last_fb=nullptr; int direct_calls=0;
  int mx=0,my=0,mb=0;                       // host mouse (frame-pixel space)
  void console_write(uint8_t) override {}
  bool console_has_input() override { return false; }
  int  console_read() override { return -1; }
  void video_mode_changed(int,int,int) override {}
  void video_refresh(const uint8_t*,int,int) override {}
  void video_refresh_direct(const uint8_t*fb,int w,int h,int bpp,int stride,const uint8_t[][3]) override {
    last_fb=fb; last_w=w; last_h=h; last_bpp=bpp; last_stride=stride; direct_calls++;
  }
  void video_set_cursor(int,int) override {}
  bool disk_present(int) override { return false; }
  size_t disk_read(int,uint64_t,uint8_t*,size_t) override { return 0; }
  size_t disk_write(int,uint64_t,const uint8_t*,size_t) override { return 0; }
  uint64_t disk_size(int) override { return 0; }
  void get_time(int&h,int&m,int&s,int&hs) override { h=m=s=hs=0; }
  void get_date(int&y,int&mo,int&d,int&w) override { y=2026;mo=1;d=1;w=0; }
  bool mouse_present() override { return true; }
  void mouse_get_state(int&x,int&y,int&b) override { x=mx; y=my; b=mb; }
};

static emu88_mem *gmem; static dos_machine *gm;
static int fails=0;
static void ck(const char*n,bool ok){ if(!ok){printf("FAIL %s\n",n);fails++;} else printf("ok   %s\n",n); }

// Run one software interrupt: a "CD vec / F4" stub at 0x3000:0 with preset regs.
static void intN(uint8_t vec, uint16_t ax,uint16_t bx,uint16_t cx,uint16_t dx,uint16_t es,uint16_t di){
  using E=emu88;
  gmem->store_mem(0x30000,0xCD); gmem->store_mem(0x30001,vec); gmem->store_mem(0x30002,0xF4);
  gm->load_segment_real(E::seg_CS,0x3000);
  gm->load_segment_real(E::seg_DS,0x4000);
  gm->load_segment_real(E::seg_SS,0x2000);
  gm->load_segment_real(E::seg_ES,es);
  gm->ip=0; gm->set_reg16(E::reg_SP,0xFFFE);
  gm->set_reg16(E::reg_AX,ax); gm->set_reg16(E::reg_BX,bx);
  gm->set_reg16(E::reg_CX,cx); gm->set_reg16(E::reg_DX,dx);
  gm->set_reg16(E::reg_DI,di);
  gm->halted=false;
  for(int i=0;i<2000 && !gm->halted;i++) gm->run_batch(20);
}
static void int10(uint16_t ax,uint16_t bx,uint16_t cx,uint16_t dx,uint16_t es,uint16_t di){
  intN(0x10,ax,bx,cx,dx,es,di);
}
// Far-call a 16-bit routine at seg:off (ends in RETF) with BX/CX/DX inputs.
static void farcall(uint16_t seg,uint16_t off,uint16_t bx,uint16_t cx,uint16_t dx){
  using E=emu88;
  gmem->store_mem(0x30100,0xF4);                  // return lands on HLT at 0x3000:0100
  gmem->store_mem16(0x2FFFC,0x0100);              // return IP
  gmem->store_mem16(0x2FFFE,0x3000);              // return CS
  gm->load_segment_real(E::seg_SS,0x2000);
  gm->set_reg16(E::reg_SP,0xFFFC);
  gm->load_segment_real(E::seg_CS,seg); gm->ip=off;
  gm->set_reg16(E::reg_BX,bx); gm->set_reg16(E::reg_CX,cx); gm->set_reg16(E::reg_DX,dx);
  gm->halted=false;
  for(int i=0;i<2000 && !gm->halted;i++) gm->run_batch(20);
}

int main(){
  using E=emu88;
  emu88_mem mem(0x100000); StubIO io; dos_machine m(&mem,&io);
  gmem=&mem; gm=&m; m.init_machine(); mem.set_a20(true);
  const uint32_t BUF = 0x50000;
  auto rb=[&](uint32_t a){return mem.fetch_mem(a);};
  auto rw=[&](uint32_t a){return (uint16_t)(mem.fetch_mem(a)|(mem.fetch_mem(a+1)<<8));};
  auto rd=[&](uint32_t a){return (uint32_t)(rw(a)|(rw(a+2)<<16));};

  // --- 4F00 controller info ---
  int10(0x4F00,0,0,0,0x5000,0x0000);
  ck("4F00 AX=004F", m.get_reg16(E::reg_AX)==0x004F);
  char sig[5]={(char)rb(BUF),(char)rb(BUF+1),(char)rb(BUF+2),(char)rb(BUF+3),0};
  ck("4F00 sig VESA", strcmp(sig,"VESA")==0);
  ck("4F00 version 0200", rw(BUF+4)==0x0200);
  ck("4F00 TotalMemory 128(=8MB/64K)", rw(BUF+0x12)==128);
  uint32_t modeptr=rd(BUF+0x0E); uint32_t modelin=((modeptr>>16)<<4)+(modeptr&0xFFFF);
  ck("4F00 modelist seg C000", (modeptr>>16)==0xC000);
  ck("4F00 first mode 0x100", rw(modelin)==0x100);

  // --- 4F01 mode info: 8bpp (0x101) and 16bpp (0x111) ---
  int10(0x4F01,0,0x101,0,0x5000,0x0000);
  ck("4F01 AX=004F", m.get_reg16(E::reg_AX)==0x004F);
  ck("4F01 XRes=640", rw(BUF+0x12)==640);
  ck("4F01 YRes=480", rw(BUF+0x14)==480);
  ck("4F01 bpp=8", rb(BUF+0x19)==8);
  ck("4F01 stride=640", rw(BUF+0x10)==640);
  ck("4F01 MemoryModel=4(packed)", rb(BUF+0x1B)==4);
  ck("4F01 attrs LFB bit set", (rw(BUF+0x00)&0x80)!=0);
  ck("4F01 PhysBasePtr=E0000000", rd(BUF+0x28)==0xE0000000u);
  ck("4F01 WinASegment=A000", rw(BUF+0x08)==0xA000);
  ck("4F01 WinGranularity=64", rw(BUF+0x04)==64);
  int10(0x4F01,0,0x111,0,0x5000,0x0000);
  ck("4F01/16 bpp=16", rb(BUF+0x19)==16);
  ck("4F01/16 stride=1280", rw(BUF+0x10)==1280);
  ck("4F01/16 MemModel=6(direct)", rb(BUF+0x1B)==6);
  ck("4F01/16 Red size5 pos11", rb(BUF+0x1F)==5 && rb(BUF+0x20)==11);
  ck("4F01/16 Green size6 pos5", rb(BUF+0x21)==6 && rb(BUF+0x22)==5);
  ck("4F01/16 Blue size5 pos0", rb(BUF+0x23)==5 && rb(BUF+0x24)==0);

  // --- 4F02 set mode 0x101, expect a 640x480x8 direct refresh ---
  io.direct_calls=0;
  int10(0x4F02,0x101,0,0,0,0);
  ck("4F02 AX=004F", m.get_reg16(E::reg_AX)==0x004F);
  ck("4F02 svga_active+LFB", mem.svga_active && mem.svga_lfb_phys==0xE0000000u);
  ck("4F02 emitted 640x480x8 frame", io.direct_calls>=1 && io.last_w==640 && io.last_h==480 && io.last_bpp==8);
  ck("4F02 frame stride=640", io.last_stride==640);

  // --- LFB pixel + 4F05 bank switch ---
  mem.store_mem(0xE0000000u + (10*640 + 20), 0x2A);
  ck("LFB pixel -> svga_vram", mem.svga_base()[10*640+20]==0x2A);
  int10(0x4F05,0x0000,0,2,0,0);
  ck("4F05 AX=004F", m.get_reg16(E::reg_AX)==0x004F);
  ck("4F05 window_off=0x20000", mem.svga_window_off==0x20000u);
  mem.store_mem(0xA0000+5, 0x3C);
  ck("window write -> svga_vram[0x20005]", mem.svga_base()[0x20005]==0x3C);

  // --- 4F06 set a wider logical scan line (virtual screen) ---
  int10(0x4F02,0x101,0,0,0,0);              // 640x480x8, stride 640
  int10(0x4F06,0x0000,1024,0,0,0);          // BL=0 set 1024 pixels/line
  ck("4F06 AX=004F", m.get_reg16(E::reg_AX)==0x004F);
  ck("4F06 BX=1024 bytes/line", m.get_reg16(E::reg_BX)==1024);
  ck("4F06 CX=1024 pixels/line", m.get_reg16(E::reg_CX)==1024);
  int10(0x4F06,0x0001,0,0,0,0);             // BL=1 get
  ck("4F06 get reflects 1024 stride", m.get_reg16(E::reg_BX)==1024);

  // --- 4F0A protected-mode interface: fetch table, then EXECUTE the routines ---
  int10(0x4F02,0x101,0,0,0,0);              // fresh 640x480x8 (stride 640)
  int10(0x4F0A,0x0000,0,0,0,0);
  ck("4F0A AX=004F", m.get_reg16(E::reg_AX)==0x004F);
  ck("4F0A ES=C000", m.sregs[E::seg_ES]==0xC000);
  uint16_t tbl_off=m.get_reg16(E::reg_DI);
  ck("4F0A DI=PMINFO(0x200)", tbl_off==0x0200);
  ck("4F0A CX=length 40", m.get_reg16(E::reg_CX)==40);
  uint32_t tbl=0xC0000+tbl_off;
  uint16_t off_win=rw(tbl+0), off_start=rw(tbl+2), off_pal=rw(tbl+4);
  ck("4F0A SetWindow offset=8", off_win==8);
  ck("4F0A SetDisplayStart offset=15", off_start==15);
  ck("4F0A no palette routine", off_pal==0);
  // Execute SetDisplayWindow(window=0, position=3) -> bank 3 (off 0x30000)
  farcall(0xC000, tbl_off+off_win, /*BX win*/0, /*CX*/0, /*DX pos*/3);
  ck("4F0A SetWindow -> bank 3", mem.svga_window_off==0x30000u);
  // Execute SetDisplayStart(x=10, y=5); read it back via 4F07 get
  farcall(0xC000, tbl_off+off_start, /*BX*/0, /*CX x*/10, /*DX y*/5);
  int10(0x4F07,0x0001,0,0,0,0);             // get display start -> CX=x, DX=y
  ck("4F0A SetDisplayStart -> y=5,x=10", m.get_reg16(E::reg_DX)==5 && m.get_reg16(E::reg_CX)==10);

  // --- compositor pan clamp (4F07 past end of VRAM) ---
  int10(0x4F02,0x101,0,0,0,0);
  int10(0x4F07,0x0080,0,12800,0,0);         // start = 12800*640 = 8192000 (+frame > 8MB)
  io.last_fb=nullptr;
  mem.store_mem(0x30000,0xEB); mem.store_mem(0x30001,0xFE);  // JMP $ spin
  m.load_segment_real(E::seg_CS,0x3000); m.ip=0; m.halted=false;
  for(int i=0;i<800 && io.last_fb==nullptr;i++) m.run_batch(2000);
  ck("pan-OOB clamped to page0", io.last_fb==mem.svga_base());

  // --- INT 33h mouse coordinate scaling ---
  intN(0x33,0x0000,0,0,0,0,0);              // reset/detect
  ck("INT33 mouse installed", m.get_reg16(E::reg_AX)==0xFFFF);
  // SVGA 800x600: program sets range 0..799 / 0..599, host pointer at frame px (400,300)
  int10(0x4F02,0x103,0,0,0,0);              // 800x600x8
  intN(0x33,0x0007,0,0,799,0,0);            // set horiz range 0..799
  intN(0x33,0x0008,0,0,599,0,0);            // set vert range 0..599
  io.mx=400; io.my=300;
  intN(0x33,0x0003,0,0,0,0,0);              // get position
  ck("INT33 SVGA 800x600 maps (400,300)->(400,300)",
     m.get_reg16(E::reg_CX)==400 && m.get_reg16(E::reg_DX)==300);
  // Mode 13h (320x200): default 640x200 range, host frame px x=160 -> guest x≈320
  int10(0x0013,0,0,0,0,0);                  // VGA mode 13h (clears SVGA)
  intN(0x33,0x0000,0,0,0,0,0);              // reset -> default range 640x200
  io.mx=160; io.my=100;
  intN(0x33,0x0003,0,0,0,0,0);
  ck("INT33 mode13h 160px->~320 (640 virtual)",
     m.get_reg16(E::reg_CX)>=318 && m.get_reg16(E::reg_CX)<=322);

  printf(fails? "\n=== %d FAILURES ===\n":"\n=== ALL VESA END-TO-END TESTS PASS ===\n", fails);
  return fails?1:0;
}
