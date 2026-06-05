#ifndef UART16550_H
#define UART16550_H

#include <cstdint>
#include <functional>

// 16550 UART (PC COM1-style serial port).
//
// Models a National Semiconductor 16550A-compatible UART as seen by DOS
// software: 8 I/O registers at a base (default 0x3F8 = COM1), a DLAB-banked
// 16-bit divisor latch, programmable line/modem control, line/modem status,
// and 16550 transmit/receive FIFOs (modeled as simple byte queues). The IRQ
// line (default 4 for COM1) is asserted per the Interrupt Enable Register and
// reported through the Interrupt Identification Register.
//
// This is NOT an AudioDevice. The host transports bytes via two callbacks:
//   tx(byte)  -- a byte the guest wrote to THR; send it to the host link.
//   rx() -> int -- poll the host for one incoming byte (-1 if none available).
// The main agent calls poll() periodically to drain rx() into the RX FIFO and
// to refresh LSR/IIR, then checks irq_pending()/irq_number() to deliver an IRQ.
//
// All state is contained in the class. No global state, no iostream.
class UART16550 {
public:
  // base = absolute base I/O port (COM1 = 0x3F8, COM2 = 0x2F8, ...).
  // irq  = the IRQ line this UART drives (COM1 = 4, COM2 = 3).
  explicit UART16550(uint16_t base = 0x3F8, int irq = 4);

  void reset();

  // Port access. rel = absolute_port - base, must be 0..7.
  void write_port(uint16_t rel, uint8_t val);
  uint8_t read_port(uint16_t rel);

  // Host transport callbacks (set by the main agent).
  std::function<void(uint8_t)> tx;  // guest wrote THR -> send byte to host
  std::function<int()> rx;          // poll host for a byte (-1 == none)

  // Pull any pending host bytes via rx() into the RX FIFO, then recompute the
  // line status and interrupt identification. Call once per emulation batch.
  void poll();

  // IRQ reporting for the main agent's PIC.
  bool irq_pending() const { return irq_line_; }
  int irq_number() const { return irq_; }

  // Convenience accessors (useful for tests / introspection).
  uint16_t base() const { return base_; }
  uint16_t divisor() const { return (uint16_t)((dlm_ << 8) | dll_); }
  bool dlab() const { return (lcr_ & 0x80) != 0; }

private:
  // --- Register offsets (relative to base) ---
  enum {
    REG_RBR_THR = 0,  // DLAB=0: RBR (read) / THR (write); DLAB=1: DLL
    REG_IER_DLM = 1,  // DLAB=0: IER; DLAB=1: DLM
    REG_IIR_FCR = 2,  // read: IIR; write: FCR
    REG_LCR = 3,      // Line Control (bit 7 = DLAB)
    REG_MCR = 4,      // Modem Control
    REG_LSR = 5,      // Line Status (read mostly)
    REG_MSR = 6,      // Modem Status
    REG_SCR = 7,      // Scratch
  };

  // --- Line Status Register bits ---
  enum {
    LSR_DR = 0x01,    // Data Ready (RX FIFO non-empty)
    LSR_OE = 0x02,    // Overrun Error
    LSR_PE = 0x04,    // Parity Error
    LSR_FE = 0x08,    // Framing Error
    LSR_BI = 0x10,    // Break Interrupt
    LSR_THRE = 0x20,  // Transmit Holding Register Empty
    LSR_TEMT = 0x40,  // Transmitter Empty (THR + shift both empty)
    LSR_FIFOE = 0x80, // Error in RX FIFO
  };

  // --- Interrupt Enable Register bits ---
  enum {
    IER_RDA = 0x01,   // Received Data Available
    IER_THRE = 0x02,  // THR Empty
    IER_RLS = 0x04,   // Receiver Line Status
    IER_MS = 0x08,    // Modem Status
  };

  // --- Interrupt Identification (read) ---
  enum {
    IIR_NONE = 0x01,    // bit0=1 => no interrupt pending
    IIR_RLS = 0x06,     // Receiver line status (highest)
    IIR_RDA = 0x04,     // Received data available
    IIR_THRE = 0x02,    // THR empty
    IIR_MS = 0x00,      // Modem status (lowest)
    IIR_FIFO = 0xC0,    // FIFOs enabled (bits 6-7) when FCR0=1
  };

  // --- Modem Control bits ---
  enum {
    MCR_DTR = 0x01,
    MCR_RTS = 0x02,
    MCR_OUT1 = 0x04,
    MCR_OUT2 = 0x08,   // gates the IRQ onto the bus on real HW
    MCR_LOOP = 0x10,   // local loopback
  };

  // --- Modem Status bits ---
  enum {
    MSR_DCTS = 0x01,
    MSR_DDSR = 0x02,
    MSR_TERI = 0x04,
    MSR_DDCD = 0x08,
    MSR_CTS = 0x10,
    MSR_DSR = 0x20,
    MSR_RI = 0x40,
    MSR_DCD = 0x80,
  };

  static constexpr int FIFO_CAP = 64;  // 16550 FIFO depth

  struct ByteFifo {
    uint8_t buf[FIFO_CAP];
    int head = 0;
    int count = 0;
    void clear() { head = 0; count = 0; }
    bool empty() const { return count == 0; }
    bool full() const { return count >= FIFO_CAP; }
    int size() const { return count; }
    bool push(uint8_t b) {
      if (full()) return false;
      buf[(head + count) % FIFO_CAP] = b;
      count++;
      return true;
    }
    int pop() {  // -1 if empty
      if (empty()) return -1;
      int b = buf[head];
      head = (head + 1) % FIFO_CAP;
      count--;
      return b;
    }
    int peek() const { return empty() ? -1 : buf[head]; }
  };

  // Recompute LSR data-ready/error bits and the cached IIR/IRQ line.
  void update_status();
  // Deliver one THR byte to the host (or loopback into RX).
  void transmit(uint8_t b);
  // FIFO trigger level (in bytes) selected by FCR bits 6-7.
  int rx_trigger() const;

  uint16_t base_;
  int irq_;

  // Divisor latch (DLAB-banked behind reg 0/1).
  uint8_t dll_ = 0x0C;  // power-on divisor low (1200 baud-ish default; arbitrary)
  uint8_t dlm_ = 0x00;

  uint8_t ier_ = 0x00;  // Interrupt Enable
  uint8_t lcr_ = 0x03;  // Line Control (8N1 by default, DLAB clear)
  uint8_t mcr_ = 0x00;  // Modem Control
  uint8_t lsr_ = LSR_THRE | LSR_TEMT;  // Line Status
  uint8_t msr_ = 0x00;  // Modem Status
  uint8_t scr_ = 0x00;  // Scratch

  uint8_t fcr_ = 0x00;     // last FCR written
  bool fifo_enabled_ = false;

  ByteFifo rx_fifo_;
  ByteFifo tx_fifo_;  // present for fidelity; we transmit immediately

  // Pending "THRE caused an interrupt" latch: cleared on THR write or IIR read.
  bool thre_int_pending_ = false;

  uint8_t iir_ = IIR_NONE;  // cached identification
  bool irq_line_ = false;   // cached asserted IRQ
};

#endif // UART16550_H
