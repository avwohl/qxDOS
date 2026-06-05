// Standalone unit test for the 16550 UART module (emu88/uart16550.{h,cc}).
//
// Build & run:
//   clang++ -std=c++20 -I emu88 tests/uart_unit.cc emu88/uart16550.cc \
//       -o /tmp/uart_unit && /tmp/uart_unit
//
// Drives the UART directly via write_port/read_port/poll() and the tx()/rx()
// host callbacks, asserting the documented register/interrupt behavior.

#include "uart16550.h"

#include <cstdint>
#include <cstdio>
#include <vector>

static int g_failures = 0;
static int g_checks = 0;

static void check(bool cond, const char *what) {
  g_checks++;
  if (!cond) {
    g_failures++;
    std::printf("  FAIL: %s\n", what);
  }
}

// Register offsets, for readability in the test.
enum {
  R_RBR = 0, R_THR = 0, R_DLL = 0,
  R_IER = 1, R_DLM = 1,
  R_IIR = 2, R_FCR = 2,
  R_LCR = 3,
  R_MCR = 4,
  R_LSR = 5,
  R_MSR = 6,
  R_SCR = 7,
};

int main() {
  // --- Test 1: divisor latch (DLAB) round-trip --------------------------
  {
    UART16550 u(0x3F8, 4);
    // Set DLAB in LCR.
    u.write_port(R_LCR, 0x83);            // 8N1 + DLAB
    check(u.dlab(), "DLAB set after LCR=0x83");
    u.write_port(R_DLL, 0x01);            // divisor low  = 0x01
    u.write_port(R_DLM, 0x00);            // divisor high = 0x00 -> 115200 baud
    check(u.read_port(R_DLL) == 0x01, "DLL reads back 0x01");
    check(u.read_port(R_DLM) == 0x00, "DLM reads back 0x00");
    check(u.divisor() == 0x0001, "divisor() == 1");

    // A different divisor.
    u.write_port(R_DLL, 0x0C);
    u.write_port(R_DLM, 0x01);            // 0x010C
    check(u.read_port(R_DLL) == 0x0C, "DLL reads back 0x0C");
    check(u.read_port(R_DLM) == 0x01, "DLM reads back 0x01");
    check(u.divisor() == 0x010C, "divisor() == 0x010C");

    // Clear DLAB; now reg 0/1 are RBR/THR and IER, not the latch.
    u.write_port(R_LCR, 0x03);
    check(!u.dlab(), "DLAB clear after LCR=0x03");
    u.write_port(R_IER, 0x00);
    check(u.read_port(R_IER) == 0x00, "IER reads 0 (not DLM) when DLAB=0");
    // The latch value is retained behind the bank.
    u.write_port(R_LCR, 0x83);
    check(u.divisor() == 0x010C, "divisor retained behind DLAB bank");
    u.write_port(R_LCR, 0x03);
  }

  // --- Test 2: TX path -- write THR, tx() fires, THRE asserts -----------
  {
    UART16550 u(0x3F8, 4);
    std::vector<uint8_t> sent;
    u.tx = [&](uint8_t b) { sent.push_back(b); };

    // THRE should be set out of reset (transmitter idle).
    check((u.read_port(R_LSR) & 0x20) != 0, "LSR THRE set at reset");
    check((u.read_port(R_LSR) & 0x40) != 0, "LSR TEMT set at reset");

    u.write_port(R_THR, 'A');
    check(sent.size() == 1 && sent[0] == 'A', "tx() received 'A'");
    check((u.read_port(R_LSR) & 0x20) != 0, "THRE re-asserts after TX");

    u.write_port(R_THR, 0x55);
    u.write_port(R_THR, 0xAA);
    check(sent.size() == 3, "tx() received all three bytes");
    check(sent[1] == 0x55 && sent[2] == 0xAA, "tx() byte values correct");
  }

  // --- Test 3: RX path -- rx()/poll() fills FIFO, RBR drains it ----------
  {
    UART16550 u(0x3F8, 4);
    std::vector<uint8_t> incoming = {'H', 'i', '!'};
    size_t idx = 0;
    u.rx = [&]() -> int {
      if (idx < incoming.size()) return incoming[idx++];
      return -1;
    };

    check((u.read_port(R_LSR) & 0x01) == 0, "DR clear before poll");
    u.poll();
    check((u.read_port(R_LSR) & 0x01) != 0, "DR set after poll() pulls data");

    check(u.read_port(R_RBR) == 'H', "RBR yields 'H'");
    check(u.read_port(R_RBR) == 'i', "RBR yields 'i'");
    check((u.read_port(R_LSR) & 0x01) != 0, "DR still set (one byte left)");
    check(u.read_port(R_RBR) == '!', "RBR yields '!'");
    check((u.read_port(R_LSR) & 0x01) == 0, "DR clear after draining FIFO");
  }

  // --- Test 4: RX interrupt (Received Data Available) -------------------
  {
    UART16550 u(0x3F8, 4);
    std::vector<uint8_t> incoming = {0x42};
    size_t idx = 0;
    u.rx = [&]() -> int {
      if (idx < incoming.size()) return incoming[idx++];
      return -1;
    };

    check(!u.irq_pending(), "no IRQ pending at reset");
    check((u.read_port(R_IIR) & 0x01) != 0, "IIR bit0=1 (no int) at reset");

    // Enable Received Data Available interrupt.
    u.write_port(R_IER, 0x01);  // IER_RDA
    u.poll();                   // pulls 0x42 into RX FIFO (trigger=1)
    check(u.irq_pending(), "IRQ pending after RX byte with RDA enabled");
    check(u.irq_number() == 4, "irq_number() == 4 for COM1");
    check((u.read_port(R_IIR) & 0x0F) == 0x04, "IIR reports RDA (0x04)");

    // Reading the byte clears DR and therefore the RDA interrupt.
    check(u.read_port(R_RBR) == 0x42, "RBR yields the queued byte");
    check(!u.irq_pending(), "IRQ clears after draining RX FIFO");
    check((u.read_port(R_IIR) & 0x01) != 0, "IIR no-int after RX drained");
  }

  // --- Test 5: THR-empty interrupt --------------------------------------
  {
    UART16550 u(0x3F8, 4);
    std::vector<uint8_t> sent;
    u.tx = [&](uint8_t b) { sent.push_back(b); };

    // Enabling THRE interrupt while THR is empty should raise an interrupt.
    u.write_port(R_IER, 0x02);  // IER_THRE
    check(u.irq_pending(), "IRQ pending when THRE enabled and THR empty");
    check((u.read_port(R_IIR) & 0x0F) == 0x02, "IIR reports THRE (0x02)");

    // Reading IIR identifying THRE clears that interrupt source.
    check(!u.irq_pending(), "THRE int cleared after reading IIR");

    // Writing then re-emptying THR re-arms the THRE interrupt.
    u.write_port(R_THR, 'Z');
    check(sent.size() == 1 && sent[0] == 'Z', "TX byte delivered");
    check(u.irq_pending(), "THRE int re-arms after THR write/empty");
    check((u.read_port(R_IIR) & 0x0F) == 0x02, "IIR reports THRE again");

    // Disabling the THRE interrupt drops the IRQ line.
    u.write_port(R_IER, 0x00);
    check(!u.irq_pending(), "IRQ drops when THRE int disabled");
  }

  // --- Test 6: FIFO control + scratch register --------------------------
  {
    UART16550 u(0x3F8, 4);
    u.write_port(R_SCR, 0x5A);
    check(u.read_port(R_SCR) == 0x5A, "scratch register round-trips");

    // Enable FIFOs; IIR top bits should report FIFO-enabled (0xC0).
    u.write_port(R_FCR, 0x01);
    check((u.read_port(R_IIR) & 0xC0) == 0xC0, "IIR FIFO bits set when enabled");
  }

  // --- Test 7: loopback (MCR_LOOP) feeds TX back into RX -----------------
  {
    UART16550 u(0x3F8, 4);
    bool tx_called = false;
    u.tx = [&](uint8_t) { tx_called = true; };
    u.write_port(R_MCR, 0x10);   // enable LOOP
    u.write_port(R_THR, 0x7E);
    check(!tx_called, "tx() not called in loopback");
    u.poll();
    check((u.read_port(R_LSR) & 0x01) != 0, "DR set from looped byte");
    check(u.read_port(R_RBR) == 0x7E, "looped byte read back from RBR");
  }

  if (g_failures == 0) {
    std::printf("ALL UART TESTS PASS (%d checks)\n", g_checks);
    return 0;
  } else {
    std::printf("%d FAILURES (out of %d checks)\n", g_failures, g_checks);
    return 1;
  }
}
