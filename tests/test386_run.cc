// test386_run.cc — Run the PCjs/barotto test386.asm full-system 386 diagnostic
// against the emu88 CPU core. Exercises real mode, protected mode, paging,
// V86 mode, GDT/LDT, call gates, TSS task switching, etc.
//
// The 64KB ROM maps at the top of the first MB (0xF0000); the 386 reset vector
// is at 0xFFFF0. Diagnostic codes are written to POST port 0x190; ASCII output
// (POST 0xEE results) to port 0xE9. Success = POST 0xFF; failure = HLT with the
// last POST code identifying the failing test.

#include "emu88.h"
#include "emu88_mem.h"
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <vector>
#include <string>

static const uint16_t POST_PORT = 0x190;
static const uint16_t OUT_PORT  = 0x00E9;

class T386Mem : public emu88_mem {
public:
  T386Mem() : emu88_mem(0x1000000) { set_a20(true); }  // 16MB, A20 on
};

class T386Cpu : public emu88 {
public:
  uint8_t last_post = 0;
  int post_count = 0;
  bool got_done = false;          // POST 0xFF reached
  std::string ascii;
  T386Cpu(emu88_mem *m) : emu88(m) {}

  void port_out(emu88_uint16 port, emu88_uint8 v) override {
    if (port == POST_PORT) {
      last_post = v;
      post_count++;
      printf("[POST] %02X\n", v);
      fflush(stdout);
      if (v == 0xFF) got_done = true;
    } else if (port == OUT_PORT) {
      ascii.push_back((char)v);
    }
  }
  void port_out16(emu88_uint16 port, emu88_uint16 v) override {
    port_out(port, v & 0xFF);
    port_out(port + 1, (v >> 8) & 0xFF);
  }
  emu88_uint8 port_in(emu88_uint16) override { return 0xFF; }
  emu88_uint16 port_in16(emu88_uint16) override { return 0xFFFF; }
};

int main(int argc, char **argv) {
  const char *rom_path = (argc > 1) ? argv[1] : "tests/data/test386/test386.bin";
  long max_insns = (argc > 2) ? atol(argv[2]) : 500000000L;

  std::ifstream f(rom_path, std::ios::binary);
  if (!f) { fprintf(stderr, "cannot open %s\n", rom_path); return 2; }
  std::vector<uint8_t> rom((std::istreambuf_iterator<char>(f)),
                            std::istreambuf_iterator<char>());
  if (rom.empty()) { fprintf(stderr, "empty ROM\n"); return 2; }

  T386Mem mem;
  T386Cpu cpu(&mem);

  // Map the ROM at the top of the first megabyte (…and mirror to the very top
  // of the 16MB space in case the test uses the high reset alias).
  uint32_t base1 = 0x100000 - (uint32_t)rom.size();
  for (size_t i = 0; i < rom.size(); i++) mem.store_mem(base1 + i, rom[i]);
  uint32_t base2 = 0x1000000 - (uint32_t)rom.size();
  for (size_t i = 0; i < rom.size(); i++) mem.store_mem(base2 + i, rom[i]);

  cpu.reset();   // CS=0xFFFF (base 0xFFFF0), IP=0, real mode, cpu_type=386

  long n = 0;
  uint8_t prev_post = 0xFE;
  long stall = 0;
  while (!cpu.halted && n < max_insns) {
    cpu.execute();
    n++;
    if (cpu.got_done) break;
    // crude liveness: if POST advanced, reset stall budget
    if (cpu.last_post != prev_post) { prev_post = cpu.last_post; stall = 0; }
  }

  printf("\n==== test386 finished: insns=%ld halted=%d last_POST=0x%02X (%d writes) done=%d ====\n",
         n, cpu.halted ? 1 : 0, cpu.last_post, cpu.post_count, cpu.got_done ? 1 : 0);
  if (!cpu.ascii.empty()) {
    printf("---- ASCII output (%zu bytes) ----\n%s\n", cpu.ascii.size(), cpu.ascii.c_str());
  }
  printf("final CS:EIP=%04X:%08X  CR0=%08X  CPL=%d  PE=%d PG=%d VM=%d\n",
         cpu.sregs[emu88::seg_CS], cpu.ip, cpu.cr0, cpu.cpl,
         (cpu.cr0 & emu88::CR0_PE) ? 1 : 0, (cpu.cr0 & emu88::CR0_PG) ? 1 : 0,
         cpu.v86_mode() ? 1 : 0);
  return cpu.got_done ? 0 : 1;
}
