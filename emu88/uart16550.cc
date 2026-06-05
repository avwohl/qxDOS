#include "uart16550.h"

UART16550::UART16550(uint16_t base, int irq) : base_(base), irq_(irq) {
  reset();
}

void UART16550::reset() {
  dll_ = 0x0C;
  dlm_ = 0x00;
  ier_ = 0x00;
  lcr_ = 0x03;  // 8N1, DLAB clear
  mcr_ = 0x00;
  msr_ = 0x00;
  scr_ = 0x00;
  fcr_ = 0x00;
  fifo_enabled_ = false;
  rx_fifo_.clear();
  tx_fifo_.clear();
  thre_int_pending_ = false;
  lsr_ = LSR_THRE | LSR_TEMT;  // transmitter idle, no data
  iir_ = IIR_NONE;
  irq_line_ = false;
  update_status();
}

int UART16550::rx_trigger() const {
  if (!fifo_enabled_) return 1;
  switch ((fcr_ >> 6) & 0x03) {
    case 0: return 1;
    case 1: return 4;
    case 2: return 8;
    default: return 14;
  }
}

void UART16550::transmit(uint8_t b) {
  if (mcr_ & MCR_LOOP) {
    // Local loopback: the byte is fed back into the receiver and the modem
    // control lines drive the modem status lines internally.
    if (!rx_fifo_.full()) {
      rx_fifo_.push(b);
    } else {
      lsr_ |= LSR_OE;  // overrun
    }
  } else if (tx) {
    tx(b);
  }
  // We model the shift register as instantaneous: as soon as the byte leaves
  // THR, both THR-empty and transmitter-empty are true again.
}

void UART16550::update_status() {
  // Data-ready reflects the RX FIFO.
  if (!rx_fifo_.empty())
    lsr_ |= LSR_DR;
  else
    lsr_ &= ~LSR_DR;

  // THR is always immediately empty in this model (instantaneous transmit),
  // so LSR_THRE/LSR_TEMT stay set except transiently inside write_port.

  // In loopback mode the lower MSR bits mirror the modem control outputs and
  // their delta bits are set when they change. We recompute the static mirror
  // here (delta bits are managed where MCR is written / MSR is read).
  if (mcr_ & MCR_LOOP) {
    uint8_t mirror = 0;
    if (mcr_ & MCR_RTS) mirror |= MSR_CTS;
    if (mcr_ & MCR_DTR) mirror |= MSR_DSR;
    if (mcr_ & MCR_OUT1) mirror |= MSR_RI;
    if (mcr_ & MCR_OUT2) mirror |= MSR_DCD;
    msr_ = (uint8_t)((msr_ & 0x0F) | mirror);
  }

  // --- Determine the highest-priority pending interrupt, gated by IER. ---
  // Priority: RLS > RDA > THRE > MS.
  uint8_t id = IIR_NONE;
  bool pending = false;

  bool rls = (ier_ & IER_RLS) &&
             (lsr_ & (LSR_OE | LSR_PE | LSR_FE | LSR_BI));
  bool rda = (ier_ & IER_RDA) && (rx_fifo_.size() >= rx_trigger());
  bool thre = (ier_ & IER_THRE) && thre_int_pending_;
  bool ms = (ier_ & IER_MS) && (msr_ & 0x0F);

  if (rls) {
    id = IIR_RLS;
    pending = true;
  } else if (rda) {
    id = IIR_RDA;
    pending = true;
  } else if (thre) {
    id = IIR_THRE;
    pending = true;
  } else if (ms) {
    id = IIR_MS;
    pending = true;
  }

  uint8_t fifo_bits = fifo_enabled_ ? IIR_FIFO : 0x00;
  if (pending)
    iir_ = (uint8_t)(id | fifo_bits);
  else
    iir_ = (uint8_t)(IIR_NONE | fifo_bits);

  irq_line_ = pending;
}

void UART16550::poll() {
  if (rx) {
    // Drain as many host bytes as the FIFO will hold this tick.
    int b;
    while (!rx_fifo_.full() && (b = rx()) >= 0) {
      rx_fifo_.push((uint8_t)(b & 0xFF));
    }
    // If a byte arrived while the FIFO was full, the next pop'd-in byte was
    // already lost above (rx() not called past full); mark overrun only if the
    // FIFO is full and host still has data is indeterminate — we conservatively
    // do not set OE here, matching "FIFO absorbs to capacity".
  }
  update_status();
}

void UART16550::write_port(uint16_t rel, uint8_t val) {
  switch (rel & 0x07) {
    case REG_RBR_THR:
      if (dlab()) {
        dll_ = val;  // Divisor Latch LSB
      } else {
        // Write to THR. THRE briefly drops then immediately re-asserts because
        // the byte is shifted out instantaneously in this model.
        lsr_ &= ~(LSR_THRE | LSR_TEMT);
        thre_int_pending_ = false;  // writing THR clears a pending THRE int
        transmit(val);
        lsr_ |= (LSR_THRE | LSR_TEMT);
        // Re-arm a THRE interrupt: empty THR raises THRE int if enabled.
        thre_int_pending_ = true;
      }
      break;

    case REG_IER_DLM:
      if (dlab()) {
        dlm_ = val;  // Divisor Latch MSB
      } else {
        ier_ = (uint8_t)(val & 0x0F);
        // Enabling THRE while THR is empty arms the THRE interrupt.
        if (ier_ & IER_THRE) thre_int_pending_ = true;
      }
      break;

    case REG_IIR_FCR: {
      // Writing reg 2 hits the FIFO Control Register.
      fcr_ = val;
      bool en = (val & 0x01) != 0;
      if (en && !fifo_enabled_) {
        // Turning FIFOs on resets them.
        rx_fifo_.clear();
        tx_fifo_.clear();
      }
      fifo_enabled_ = en;
      if (val & 0x02) rx_fifo_.clear();  // RX FIFO reset
      if (val & 0x04) tx_fifo_.clear();  // TX FIFO reset
      break;
    }

    case REG_LCR:
      lcr_ = val;  // includes DLAB (bit7), word length, stop, parity
      break;

    case REG_MCR: {
      uint8_t old_mirror_in = msr_;
      mcr_ = (uint8_t)(val & 0x1F);
      // In loopback, changes to the control outputs set MSR delta bits.
      if (mcr_ & MCR_LOOP) {
        uint8_t before = old_mirror_in & 0xF0;
        update_status();  // recompute the static mirror into msr_
        uint8_t after = msr_ & 0xF0;
        uint8_t changed = (uint8_t)(before ^ after);
        if (changed & MSR_CTS) msr_ |= MSR_DCTS;
        if (changed & MSR_DSR) msr_ |= MSR_DDSR;
        if (changed & MSR_RI)  msr_ |= MSR_TERI;  // RI delta is trailing-edge,
                                                  // but model as any change.
        if (changed & MSR_DCD) msr_ |= MSR_DDCD;
      }
      break;
    }

    case REG_LSR:
      // LSR is effectively read-only on real hardware; writes ignored.
      break;

    case REG_MSR:
      // MSR is read-only; writes ignored.
      break;

    case REG_SCR:
      scr_ = val;
      break;
  }
  update_status();
}

uint8_t UART16550::read_port(uint16_t rel) {
  uint8_t out = 0xFF;
  switch (rel & 0x07) {
    case REG_RBR_THR:
      if (dlab()) {
        out = dll_;
      } else {
        // Read Receiver Buffer: pop one byte from the RX FIFO.
        int b = rx_fifo_.pop();
        out = (b < 0) ? 0x00 : (uint8_t)b;
        // Reading data clears DR when the FIFO drains; status updated below.
      }
      break;

    case REG_IER_DLM:
      out = dlab() ? dlm_ : ier_;
      break;

    case REG_IIR_FCR: {
      out = iir_;
      // Reading IIR while THRE is the active source clears that source.
      if ((iir_ & 0x0F) == IIR_THRE) {
        thre_int_pending_ = false;
      }
      break;
    }

    case REG_LCR:
      out = lcr_;
      break;

    case REG_MCR:
      out = mcr_;
      break;

    case REG_LSR:
      // Reading LSR returns and then clears the error/break sticky bits.
      out = lsr_;
      lsr_ &= ~(LSR_OE | LSR_PE | LSR_FE | LSR_BI | LSR_FIFOE);
      break;

    case REG_MSR:
      // Reading MSR returns status and clears the delta (lower nibble) bits.
      out = msr_;
      msr_ &= 0xF0;
      break;

    case REG_SCR:
      out = scr_;
      break;
  }
  update_status();
  return out;
}
