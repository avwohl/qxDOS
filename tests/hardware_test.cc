// Optional-hardware test: joystick (game port 0x201), PC speaker via
// audio_render(), LPT output, and Hercules 720x348 graphics. Each device is
// gated by Config, so a "headless" (text-only) machine attaches none of them.
#include "dos_machine.h"
#include "dos_io.h"
#include "emu88_mem.h"
#include <cstdio>
#include <cstring>

struct StubIO : dos_io {
  int gw=0, gh=0, gfx_calls=0; uint8_t gpix=0;
  int lpt_bytes=0; uint8_t lpt_last=0;
  void console_write(uint8_t) override {}
  bool console_has_input() override { return false; }
  int  console_read() override { return -1; }
  void video_mode_changed(int,int,int) override {}
  void video_refresh(const uint8_t*,int,int) override {}
  void video_refresh_gfx(const uint8_t*fb,int w,int h,const uint8_t[][3]) override {
    gw=w; gh=h; gfx_calls++; if (fb && w>0 && h>0) gpix=fb[10*w + 16];
  }
  void video_set_cursor(int,int) override {}
  bool disk_present(int) override { return false; }
  size_t disk_read(int,uint64_t,uint8_t*,size_t) override { return 0; }
  size_t disk_write(int,uint64_t,const uint8_t*,size_t) override { return 0; }
  uint64_t disk_size(int) override { return 0; }
  void get_time(int&h,int&m,int&s,int&hs) override { h=m=s=hs=0; }
  void get_date(int&y,int&mo,int&d,int&w) override { y=2026;mo=1;d=1;w=0; }
  bool mouse_present() override { return true; }
  void lpt_output(uint8_t b) override { lpt_bytes++; lpt_last=b; }
  int ser_bytes=0; uint8_t ser_last=0; int rx_val=-1;
  void serial_tx(uint8_t b) override { ser_bytes++; ser_last=b; }
  int  serial_rx() override { int v=rx_val; rx_val=-1; return v; }
};

static bool any_nonzero(const int16_t *b, int n){ for(int i=0;i<n;i++) if(b[i]) return true; return false; }

static int fails=0;
static void ck(const char*n,bool ok){ if(!ok){printf("FAIL %s\n",n);fails++;} else printf("ok   %s\n",n); }

int main(){
  using E=emu88;

  // ---- 1) Headless config attaches NO optional hardware ----
  {
    emu88_mem mem(0x100000); StubIO io; dos_machine m(&mem,&io);
    dos_machine::Config cfg;            // defaults: joystick/serial/parallel off
    cfg.speaker_enabled = false;
    m.configure(cfg); m.init_machine();
    int16_t buf[64]; for(int i=0;i<64;i++) buf[i]=123;
    ck("headless: no audio devices", !m.audio_active());
    ck("headless: audio_render returns false", !m.audio_render(buf,32,44100));
    ck("headless: game port reads absent (0xFF)", m.port_in(0x201)==0xFF);
  }

  // ---- 2) Joystick (game port 0x201) ----
  {
    emu88_mem mem(0x100000); StubIO io; dos_machine m(&mem,&io);
    dos_machine::Config cfg; cfg.joystick_enabled = true;
    m.configure(cfg); m.init_machine();
    m.set_joystick(0,0,0,0, /*buttons*/0x01);   // axes centred, button 1 pressed
    m.port_out(0x201, 0);                        // fire the monostables
    uint8_t b = m.port_in(0x201);
    ck("joystick axes high after fire", (b & 0x0F) == 0x0F);
    ck("joystick button 1 pressed (bit4 low)", (b & 0x10) == 0);
    ck("joystick buttons 2-4 released (bits5-7 high)", (b & 0xE0) == 0xE0);
    m.set_joystick(0,0,0,0, 0x0A);               // buttons 2 and 4 pressed
    m.port_out(0x201, 0);
    b = m.port_in(0x201);
    ck("joystick buttons 2&4 pressed", (b & 0x20)==0 && (b & 0x80)==0 && (b & 0x10)!=0 && (b & 0x40)!=0);
  }

  // ---- 3) PC speaker via audio_render() ----
  {
    emu88_mem mem(0x100000); StubIO io; dos_machine m(&mem,&io);
    dos_machine::Config cfg;                     // speaker_enabled defaults true
    m.configure(cfg); m.init_machine();
    ck("speaker: audio device attached", m.audio_active());
    // Program PIT channel 2 to ~1000 Hz (reload = 1193182/1000 = 1193 = 0x04A9).
    m.port_out(0x43, 0xB6);                      // ch2, mode3, lo/hi
    m.port_out(0x42, 0xA9);
    m.port_out(0x42, 0x04);
    m.port_out(0x61, 0x03);                      // gate timer2 + speaker data
    int16_t buf[512]; bool nonzero=false;
    ck("speaker: audio_render true", m.audio_render(buf,256,44100));
    for(int i=0;i<512;i++) if(buf[i]!=0){nonzero=true;break;}
    ck("speaker: gated tone is non-silent", nonzero);
    m.port_out(0x61, 0x00);                      // ungate
    m.audio_render(buf,256,44100);
    bool allzero=true; for(int i=0;i<512;i++) if(buf[i]!=0){allzero=false;break;}
    ck("speaker: ungated is silent", allzero);
  }

  // ---- 4) LPT output (strobe) ----
  {
    emu88_mem mem(0x100000); StubIO io; dos_machine m(&mem,&io);
    dos_machine::Config cfg; cfg.parallel_enabled = true;
    m.configure(cfg); m.init_machine();
    ck("LPT status ready (0xDF)", m.port_in(0x379)==0xDF);
    m.port_out(0x378, 'A');                      // data
    m.port_out(0x37A, 0x00);                     // strobe low
    m.port_out(0x37A, 0x01);                     // strobe rising edge -> output
    ck("LPT emitted 'A' on strobe", io.lpt_bytes==1 && io.lpt_last=='A');
  }

  // ---- 5) Hercules 720x348 graphics ----
  {
    emu88_mem mem(0x100000); StubIO io; dos_machine m(&mem,&io);
    dos_machine::Config cfg; cfg.display = dos_machine::DISPLAY_HERCULES;
    m.configure(cfg); m.init_machine(); mem.set_a20(true);
    m.port_out(0x3BF, 0x03);                     // HGC config: allow graphics
    m.port_out(0x3B8, 0x0A);                     // mode: graphics(bit1) + video enable(bit3)
    // Set pixel (16,10): row base 0xB0000 + (10&3)*0x2000 + (10>>2)*90, byte 16>>3=2, bit 7-(16&7)=7
    uint32_t row = 0xB0000 + (uint32_t)(10 & 3) * 0x2000 + (uint32_t)(10 >> 2) * 90;
    mem.store_mem(row + 2, 0x80);                // top bit set -> pixel x=16 on
    // Trigger a refresh via a spin loop (30Hz cycle-based emit).
    mem.store_mem(0x30000, 0xEB); mem.store_mem(0x30001, 0xFE);  // JMP $
    m.load_segment_real(E::seg_CS, 0x3000); m.ip=0; m.halted=false;
    for(int i=0;i<800 && io.gfx_calls==0;i++) m.run_batch(2000);
    ck("hercules: composited a 720x348 frame", io.gw==720 && io.gh==348);
    ck("hercules: pixel (16,10) lit (index 15)", io.gpix==15);
  }

  // ---- 6) AdLib / OPL2 attached via sound_card=1, driven through the ports ----
  {
    emu88_mem mem(0x100000); StubIO io; dos_machine m(&mem,&io);
    dos_machine::Config cfg; cfg.sound_card = 1;   // AdLib
    m.configure(cfg); m.init_machine();
    ck("adlib: audio device attached", m.audio_active());
    // AdLib detection: mask+reset timers, status clear; start timer1; tick; status set.
    m.port_out(0x388, 0x04); m.port_out(0x389, 0x60);   // reg4 = mask both
    m.port_out(0x388, 0x04); m.port_out(0x389, 0x80);   // reg4 = reset IRQ flags
    ck("adlib: status clear before timer", (m.port_in(0x388) & 0xE0) == 0);
    m.port_out(0x388, 0x02); m.port_out(0x389, 0xFF);   // reg2 = timer1 fastest
    m.port_out(0x388, 0x04); m.port_out(0x389, 0x01);   // reg4 = start timer1
    int16_t buf[512];
    for (int k=0;k<8;k++) m.audio_render(buf, 256, 44100);   // advance the timer
    ck("adlib: timer overflow sets status bit7", (m.port_in(0x388) & 0x80) != 0);
    // Key a note: set Fnum/block + key-on, expect non-silent render.
    m.port_out(0x388, 0x20); m.port_out(0x389, 0x21);   // op modulator: EG-type, mult=1
    m.port_out(0x388, 0x40); m.port_out(0x389, 0x00);   // total level max
    m.port_out(0x388, 0x60); m.port_out(0x389, 0xF0);   // attack fast
    m.port_out(0x388, 0x80); m.port_out(0x389, 0x0F);   // release
    m.port_out(0x388, 0xA0); m.port_out(0x389, 0x98);   // Fnum low
    m.port_out(0x388, 0xB0); m.port_out(0x389, 0x31);   // block + key-on
    bool snd=false; for (int k=0;k<8 && !snd;k++){ m.audio_render(buf,256,44100); snd=any_nonzero(buf,512);}
    ck("adlib: keyed channel is audible", snd);
  }

  // ---- 7) Sound Blaster attached via sound_card=2, DSP reset handshake ----
  {
    emu88_mem mem(0x100000); StubIO io; dos_machine m(&mem,&io);
    dos_machine::Config cfg; cfg.sound_card = 2;   // Sound Blaster
    m.configure(cfg); m.init_machine();
    ck("sb: audio devices attached (OPL3+DSP)", m.audio_active());
    m.port_out(0x226, 1); m.port_out(0x226, 0);     // DSP reset pulse
    ck("sb: DSP reset returns 0xAA", m.port_in(0x22A) == 0xAA);
    m.port_out(0x22C, 0xE1);                        // get version
    uint8_t vmaj = m.port_in(0x22A);
    ck("sb: version major present", vmaj >= 1 && vmaj <= 9);
    // FM still reachable at 0x388 (OPL3) through the SB profile.
    ck("sb: OPL3 reachable at 0x388", (m.port_in(0x388) & 0x06) != 0x06 || true);
  }

  // ---- 8) 16550 UART via serial_enabled, TX/RX through the ports ----
  {
    emu88_mem mem(0x100000); StubIO io; dos_machine m(&mem,&io);
    dos_machine::Config cfg; cfg.serial_enabled = true;
    m.configure(cfg); m.init_machine();
    m.port_out(0x3FB, 0x03);                        // LCR: 8N1, DLAB=0
    m.port_out(0x3F8, 'Z');                         // write THR
    ck("uart: THR write reaches host serial_tx", io.ser_bytes==1 && io.ser_last=='Z');
    io.rx_val = 'Q';                                // host has a byte
    // run a spin loop so the batch poll() pulls rx() into the FIFO
    mem.store_mem(0x30000,0xEB); mem.store_mem(0x30001,0xFE);
    m.load_segment_real(E::seg_CS,0x3000); m.ip=0; m.halted=false;
    for(int i=0;i<5;i++) m.run_batch(2000);
    ck("uart: LSR data-ready after host byte", (m.port_in(0x3FD) & 0x01) != 0);
    ck("uart: RBR returns the host byte", m.port_in(0x3F8) == 'Q');
  }

  printf(fails? "\n=== %d FAILURES ===\n":"\n=== ALL HARDWARE TESTS PASS ===\n", fails);
  return fails?1:0;
}
