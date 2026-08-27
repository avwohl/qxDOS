// sst386.cc — Run SingleStepTests/80386 (real-mode) MOO test suites against
// the emu88 CPU core, standalone.
//
// Build: see tests/build.sh
// Usage:
//   sst386 [options] <file_or_dir> ...
//     --summary           one line per file: PASS/TOTAL
//     --show N            print diffs for up to N failing tests per file (default 5)
//     --stop-on-fail      stop a file after the first failing test
//     --only HEX          only run tests whose opcode-file basename matches HEX
//     --quiet             suppress per-file lines for files that fully pass
// Exit code: 0 if all tests pass, 1 otherwise.

#include "emu88.h"
#include "emu88_mem.h"

#ifndef MOO_USE_ZLIB
#define MOO_USE_ZLIB
#endif
// Vendored verbatim from the SingleStepTests project (MIT, tests/vendor/).  Its
// Read<DATA>() compares an `int` loop counter against `sizeof(DATA)`, which is
// three -Wsign-compare warnings at the three instantiations.  Suppressed around
// the include rather than patched, so tests/vendor/mooreader.h stays a byte-for-
// byte copy of upstream and re-vendoring it needs no re-patching.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#include "mooreader.h"
#pragma GCC diagnostic pop

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>
#include <dirent.h>
#include <sys/stat.h>
#include <algorithm>

using Moo::REG32;

// ---- Test CPU: flat 16MB RAM, IO reads return 0xFF ----
class TestMem : public emu88_mem {
public:
  TestMem() : emu88_mem(0x1000000) { set_a20(true); }  // 16MB, A20 on
};

class TestCpu : public emu88 {
public:
  TestCpu(emu88_mem *m) : emu88(m) {}
  emu88_uint8 port_in(emu88_uint16) override { return 0xFF; }
  emu88_uint16 port_in16(emu88_uint16) override { return 0xFFFF; }
  void port_out(emu88_uint16, emu88_uint8) override {}
  void port_out16(emu88_uint16, emu88_uint16) override {}
  // Tests inject a HALT at exception ISRs; default real-mode dispatch is fine.
};

struct Options {
  bool summary = false;
  int show = 5;
  bool stop_on_fail = false;
  bool quiet = false;
  std::string only;
};

struct FileResult {
  std::string name;
  int total = 0, passed = 0, skipped = 0;
};

static const char *reg_name(REG32 r) { return Moo::GetRegister32Name(r); }

// Read the emulator's current value of a REG32.
static uint32_t cpu_get(const emu88 &c, REG32 r) {
  switch (r) {
    case REG32::CR0: return c.cr0;
    case REG32::CR3: return c.cr3;
    case REG32::EAX: return c.get_reg32(emu88::reg_AX);
    case REG32::EBX: return c.get_reg32(emu88::reg_BX);
    case REG32::ECX: return c.get_reg32(emu88::reg_CX);
    case REG32::EDX: return c.get_reg32(emu88::reg_DX);
    case REG32::ESI: return c.get_reg32(emu88::reg_SI);
    case REG32::EDI: return c.get_reg32(emu88::reg_DI);
    case REG32::EBP: return c.get_reg32(emu88::reg_BP);
    case REG32::ESP: return c.get_reg32(emu88::reg_SP);
    case REG32::CS:  return c.sregs[emu88::seg_CS];
    case REG32::DS:  return c.sregs[emu88::seg_DS];
    case REG32::ES:  return c.sregs[emu88::seg_ES];
    case REG32::FS:  return c.sregs[emu88::seg_FS];
    case REG32::GS:  return c.sregs[emu88::seg_GS];
    case REG32::SS:  return c.sregs[emu88::seg_SS];
    case REG32::EIP: return c.ip;
    case REG32::EFLAGS: return c.get_eflags();
    case REG32::DR6: return c.dr[6];
    case REG32::DR7: return c.dr[7];
    default: return 0;
  }
}

// Apply an initial register value to the emulator.
static void cpu_set_initial(emu88 &c, REG32 r, uint32_t v) {
  switch (r) {
    case REG32::CR0: c.cr0 = v; break;
    case REG32::CR3: c.cr3 = v; break;
    case REG32::EAX: c.set_reg32(emu88::reg_AX, v); break;
    case REG32::EBX: c.set_reg32(emu88::reg_BX, v); break;
    case REG32::ECX: c.set_reg32(emu88::reg_CX, v); break;
    case REG32::EDX: c.set_reg32(emu88::reg_DX, v); break;
    case REG32::ESI: c.set_reg32(emu88::reg_SI, v); break;
    case REG32::EDI: c.set_reg32(emu88::reg_DI, v); break;
    case REG32::EBP: c.set_reg32(emu88::reg_BP, v); break;
    case REG32::ESP: c.set_reg32(emu88::reg_SP, v); break;
    // Segments set via load_segment_real below (need base=sel<<4); skip here.
    case REG32::CS: case REG32::DS: case REG32::ES:
    case REG32::FS: case REG32::GS: case REG32::SS: break;
    case REG32::EIP: c.ip = v; break;
    case REG32::EFLAGS: c.set_eflags(v); break;
    case REG32::DR6: c.dr[6] = v; break;
    case REG32::DR7: c.dr[7] = v; break;
    default: break;
  }
}

static int seg_index_for(REG32 r) {
  switch (r) {
    case REG32::CS: return emu88::seg_CS;
    case REG32::DS: return emu88::seg_DS;
    case REG32::ES: return emu88::seg_ES;
    case REG32::FS: return emu88::seg_FS;
    case REG32::GS: return emu88::seg_GS;
    case REG32::SS: return emu88::seg_SS;
    default: return -1;
  }
}

// Run a single test against a reused cpu/mem. `dirty` carries the addresses
// touched by the previous test so they can be cleared cheaply (avoids zeroing
// all 16MB per test). Returns true on pass; on failure, appends a diff string.
static bool g_diag = false;  // dump initial state for failing tests

static bool run_test(TestCpu &cpu, TestMem &mem, std::vector<uint32_t> &dirty,
                     const Moo::Reader::Test &t, std::string &diag) {
  // Restore the bytes the previous test dirtied back to zero.
  for (uint32_t a : dirty) mem.store_mem(a, 0);
  dirty.clear();

  cpu.reset();
  cpu.cpu_type = emu88::CPU_386;
  cpu.lock_ud = true;
  mem.set_a20(true);

  // Set scalar / GP registers first (so CR0.PE is correct before segment loads).
  for (auto r : Moo::REG32Range())
    cpu_set_initial(cpu, r, t.GetInitialRegister(r));

  // Load segment registers as real-mode (base = sel<<4, limit 0xFFFF).
  for (auto r : {REG32::ES, REG32::CS, REG32::SS, REG32::DS, REG32::FS, REG32::GS}) {
    int si = seg_index_for(r);
    cpu.load_segment_real(si, (uint16_t)t.GetInitialRegister(r));
  }

  // Initial RAM.
  for (const auto &e : t.init_state.ram) {
    mem.store_mem(e.address, e.value);
    dirty.push_back(e.address);
  }
  // Make sure addresses the instruction is expected to write are tracked too,
  // so they get cleared before the next test.
  for (const auto &e : t.final_state.ram) dirty.push_back(e.address);

  // Execute until HALT (the suite appends a HALT after the instruction, and
  // injects one at the ISR entry when an exception is taken).
  cpu.halted = false;
  int guard = 0;
  const int GUARD_MAX = 5000;
  while (!cpu.halted && guard++ < GUARD_MAX)
    cpu.execute();
  if (guard >= GUARD_MAX) {
    char b[128];
    snprintf(b, sizeof b, "  did not halt within %d instructions (CS:IP=%04X:%08X)  test-expects-exc=%s(%d)  bytes=%02X %02X %02X\n",
             GUARD_MAX, cpu.sregs[emu88::seg_CS], cpu.ip,
             t.has_exception ? "YES" : "no", t.has_exception ? t.exception.number : -1,
             t.bytes.size()>0?t.bytes[0]:0, t.bytes.size()>1?t.bytes[1]:0, t.bytes.size()>2?t.bytes[2]:0);
    diag += b;
    return false;
  }

  bool ok = true;
  char b[256];

  // Optionally record the initial state for empirical flag-rule derivation.
  std::string init_dump;
  if (g_diag) {
    char ib[512];
    snprintf(ib, sizeof ib,
      "  bytes:");
    init_dump += ib;
    for (auto v : t.bytes) { snprintf(ib, sizeof ib, " %02X", v); init_dump += ib; }
    snprintf(ib, sizeof ib,
      "\n  init EAX=%08X EBX=%08X ECX=%08X EDX=%08X ESI=%08X EDI=%08X EBP=%08X ESP=%08X\n"
      "  init EFLAGS=%08X  expect EFLAGS=%08X\n",
      t.GetInitialRegister(REG32::EAX), t.GetInitialRegister(REG32::EBX),
      t.GetInitialRegister(REG32::ECX), t.GetInitialRegister(REG32::EDX),
      t.GetInitialRegister(REG32::ESI), t.GetInitialRegister(REG32::EDI),
      t.GetInitialRegister(REG32::EBP), t.GetInitialRegister(REG32::ESP),
      t.GetInitialRegister(REG32::EFLAGS), t.GetFinalRegister(REG32::EFLAGS, true));
    init_dump += ib;
    snprintf(ib, sizeof ib,
      "  expect ESP=%08X EIP=%08X CS=%04X EAX=%08X  exception=%s(%d)\n",
      t.GetFinalRegister(REG32::ESP, false), t.GetFinalRegister(REG32::EIP, false),
      (uint16_t)t.GetFinalRegister(REG32::CS, false), t.GetFinalRegister(REG32::EAX, false),
      t.has_exception ? "YES" : "no", t.has_exception ? t.exception.number : -1);
    init_dump += ib;
  }

  // Compare registers, masking undefined bits via the file's RM32 mask chunk.
  // The mask must apply whether or not the register is listed in the final state:
  // a register omitted from `final` means its DEFINED bits equal the initial value
  // (undefined bits are still don't-care), so compare (initial & mask) too.
  for (auto r : Moo::REG32Range()) {
    uint32_t mask = t.final_state.masks.HasRegister(r)
                      ? t.final_state.masks.GetRegister(r) : 0xFFFFFFFFu;
    uint32_t raw_exp = t.final_state.regs.HasRegister(r)
                         ? t.final_state.regs.GetRegister(r)
                         : t.GetInitialRegister(r);
    uint32_t exp = raw_exp & mask;
    uint32_t act = cpu_get(cpu, r) & mask;
    if (act != exp) {
      ok = false;
      snprintf(b, sizeof b, "  reg %-6s expected %08X got %08X (mask %08X)\n",
               reg_name(r), exp, act, mask);
      diag += b;
    }
  }

  // Compare final RAM. When an exception was taken, the FLAGS image pushed to
  // the stack may contain undefined bits; mask those two bytes with the EFLAGS
  // undefined mask if the test provides one.
  uint32_t eflags_mask = 0xFFFFFFFFu;
  if (t.final_state.masks.HasRegister(REG32::EFLAGS))
    eflags_mask = t.final_state.masks.GetRegister(REG32::EFLAGS);
  for (const auto &e : t.final_state.ram) {
    uint8_t exp = e.value;
    uint8_t act = mem.fetch_mem(e.address);
    uint8_t bmask = 0xFF;
    if (t.has_exception) {
      if (e.address == t.exception.flag_addr)     bmask = (uint8_t)(eflags_mask & 0xFF);
      else if (e.address == t.exception.flag_addr + 1) bmask = (uint8_t)((eflags_mask >> 8) & 0xFF);
    }
    if ((exp & bmask) != (act & bmask)) {
      ok = false;
      snprintf(b, sizeof b, "  ram [%06X] expected %02X got %02X (mask %02X)\n",
               e.address, exp, act, bmask);
      diag += b;
    }
  }
  if (!ok && g_diag) diag = init_dump + diag;
  return ok;
}

static FileResult run_file(TestCpu &cpu, TestMem &mem, const std::string &path,
                           const Options &opt, const std::string &revocation) {
  FileResult fr;
  // basename, stripping only the .MOO / .MOO.gz suffix (keep group-extension
  // and prefix dots, e.g. "F7.6", "66F7").
  size_t slash = path.find_last_of('/');
  std::string base = (slash == std::string::npos) ? path : path.substr(slash + 1);
  if (base.size() >= 7 && base.substr(base.size() - 7) == ".MOO.gz")
    base = base.substr(0, base.size() - 7);
  else if (base.size() >= 4 && base.substr(base.size() - 4) == ".MOO")
    base = base.substr(0, base.size() - 4);
  fr.name = base;

  Moo::Reader reader;
  try {
    reader.AddFromFile(path);
    if (!revocation.empty()) reader.AddRevocationList(revocation);
  } catch (const std::exception &ex) {
    fprintf(stderr, "ERROR reading %s: %s\n", path.c_str(), ex.what());
    return fr;
  }

  int shown = 0;
  std::vector<uint32_t> dirty;
  for (auto &t : reader) {
    if (reader.IsRevoked(t)) { fr.skipped++; continue; }
    fr.total++;
    std::string diag;
    if (run_test(cpu, mem, dirty, t, diag)) {
      fr.passed++;
    } else {
      if (!opt.summary && shown < opt.show) {
        printf("FAIL %s test #%u  %s\n%s", fr.name.c_str(), t.index, t.name.c_str(),
               diag.c_str());
        shown++;
      }
      if (opt.stop_on_fail) break;
    }
  }
  return fr;
}

static void collect(const std::string &p, std::vector<std::string> &out) {
  struct stat st;
  if (stat(p.c_str(), &st) != 0) return;
  if (S_ISDIR(st.st_mode)) {
    DIR *d = opendir(p.c_str());
    if (!d) return;
    std::vector<std::string> names;
    struct dirent *de;
    while ((de = readdir(d))) {
      std::string n = de->d_name;
      if (n == "." || n == "..") continue;
      if (n.size() >= 4 && n.substr(n.size() - 4) == ".MOO") names.push_back(n);
      else if (n.size() >= 7 && n.substr(n.size() - 7) == ".MOO.gz") names.push_back(n);
    }
    closedir(d);
    std::sort(names.begin(), names.end());
    for (auto &n : names) out.push_back(p + "/" + n);
  } else {
    out.push_back(p);
  }
}

int main(int argc, char **argv) {
  Options opt;
  std::vector<std::string> inputs;
  std::string revocation;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "--summary") opt.summary = true;
    else if (a == "--quiet") opt.quiet = true;
    else if (a == "--stop-on-fail") opt.stop_on_fail = true;
    else if (a == "--diag") g_diag = true;
    else if (a == "--show" && i + 1 < argc) opt.show = atoi(argv[++i]);
    else if (a == "--only" && i + 1 < argc) opt.only = argv[++i];
    else if (a == "--revoke" && i + 1 < argc) revocation = argv[++i];
    else inputs.push_back(a);
  }

  std::vector<std::string> files;
  for (auto &p : inputs) collect(p, files);
  if (files.empty()) {
    fprintf(stderr, "no .MOO[.gz] files found\n");
    return 2;
  }

  // One CPU + 16MB memory reused across all tests.
  TestMem mem;
  TestCpu cpu(&mem);

  long grand_total = 0, grand_pass = 0, grand_skip = 0;
  int files_with_fail = 0;
  for (auto &f : files) {
    // --only filter on file basename
    if (!opt.only.empty()) {
      size_t slash = f.find_last_of('/');
      std::string base = (slash == std::string::npos) ? f : f.substr(slash + 1);
      if (base.rfind(opt.only, 0) != 0) continue;
    }
    FileResult fr = run_file(cpu, mem, f, opt, revocation);
    grand_total += fr.total; grand_pass += fr.passed; grand_skip += fr.skipped;
    bool perfect = (fr.passed == fr.total);
    if (!perfect) files_with_fail++;
    if (!opt.quiet || !perfect) {
      printf("%-10s %6d/%-6d%s%s\n", fr.name.c_str(), fr.passed, fr.total,
             perfect ? " OK" : "  <-- FAIL",
             fr.skipped ? ("  (skipped " + std::to_string(fr.skipped) + ")").c_str() : "");
    }
    fflush(stdout);
  }
  printf("==== TOTAL %ld/%ld passed (%.4f%%), %ld skipped, %d/%zu files with failures ====\n",
         grand_pass, grand_total,
         grand_total ? 100.0 * grand_pass / grand_total : 0.0,
         grand_skip, files_with_fail, files.size());
  return (grand_pass == grand_total) ? 0 : 1;
}
