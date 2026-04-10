#ifndef NE2000_H
#define NE2000_H

#include <cstdint>
#include <cstring>
#include <functional>

// NE2000 (DP8390-based) network interface card emulation
// Provides 32 I/O ports (base+0x00 to base+0x1F) and 32KB on-card buffer RAM.
// Compatible with standard NE2000.COM packet driver.

class ne2000 {
public:
  ne2000();

  void reset();
  void set_mac(const uint8_t mac[6]);

  // I/O port access (offset 0x00-0x1F from base address)
  void iowrite(int offset, uint8_t value);
  uint8_t ioread(int offset);
  void iowrite16(int offset, uint16_t value);
  uint16_t ioread16(int offset);

  // Receive a packet from the network (Ethernet frame, no FCS)
  void receive(const uint8_t *data, int len);

  // Check if IRQ line is asserted
  bool irq_active() const { return (isr & imr) != 0; }

  // Transmit callback: called when the guest sends a packet
  std::function<void(const uint8_t *, int)> on_transmit;

private:
  // DP8390 Command Register
  uint8_t cr;

  // Page 0 write registers
  uint8_t pstart;   // Receive ring start page
  uint8_t pstop;    // Receive ring stop page
  uint8_t bnry;     // Boundary pointer (last page read by host)
  uint8_t tpsr;     // Transmit page start
  uint16_t tbcr;    // Transmit byte count
  uint8_t isr;      // Interrupt Status Register
  uint16_t rsar;    // Remote Start Address Register
  uint16_t rbcr;    // Remote Byte Count Register
  uint8_t rcr;      // Receive Configuration Register
  uint8_t tcr;      // Transmit Configuration Register
  uint8_t dcr;      // Data Configuration Register
  uint8_t imr;      // Interrupt Mask Register

  // Page 0 read-only registers
  uint8_t tsr;      // Transmit Status Register
  uint8_t rsr;      // Receive Status Register

  // Page 1 registers
  uint8_t par[6];   // Physical Address (MAC)
  uint8_t curr;     // Current page (next RX write position)
  uint8_t mar[8];   // Multicast Address Register

  // Remote DMA tracking
  uint16_t rdar;    // Current Remote DMA Address

  // NE2000 on-card memory (PROM area + 32KB buffer)
  // PROM: addresses 0x0000-0x001F (MAC doubled for 16-bit, + signature)
  // Buffer: addresses 0x4000-0xBFFF (pages 0x40-0xBF, 32KB)
  static constexpr int MEM_TOTAL = 0xC000;
  static constexpr int BUF_START = 0x4000;   // Page 0x40
  static constexpr int BUF_SIZE  = 0x8000;   // 32KB
  uint8_t mem[MEM_TOTAL];

  // Internal helpers
  int page() const { return (cr >> 6) & 3; }
  void do_transmit();
  uint8_t dma_read_byte();
  void dma_write_byte(uint8_t val);
};

#endif // NE2000_H
