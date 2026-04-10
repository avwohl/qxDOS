#include "ne2000.h"

// DP8390 Command Register bits
enum {
  CR_STP      = 0x01,   // Stop
  CR_STA      = 0x02,   // Start
  CR_TXP      = 0x04,   // Transmit Packet
  CR_RD_MASK  = 0x38,   // Remote DMA command field
  CR_RD_READ  = 0x08,   // Remote read
  CR_RD_WRITE = 0x10,   // Remote write
  CR_RD_SEND  = 0x18,   // Send packet
  CR_RD_ABORT = 0x20,   // Abort/complete remote DMA
  CR_PS_MASK  = 0xC0,   // Page select
};

// ISR bits
enum {
  ISR_PRX = 0x01,   // Packet Received
  ISR_PTX = 0x02,   // Packet Transmitted
  ISR_RXE = 0x04,   // Receive Error
  ISR_TXE = 0x08,   // Transmit Error
  ISR_OVW = 0x10,   // Overwrite Warning (ring buffer full)
  ISR_CNT = 0x20,   // Counter Overflow
  ISR_RDC = 0x40,   // Remote DMA Complete
  ISR_RST = 0x80,   // Reset Status
};

// TCR bits
enum {
  TCR_LB_MASK = 0x06,  // Loopback mode
};

// RCR bits
enum {
  RCR_AB  = 0x04,   // Accept Broadcast
  RCR_AM  = 0x08,   // Accept Multicast
  RCR_PRO = 0x10,   // Promiscuous
  RCR_MON = 0x20,   // Monitor mode (don't store packets)
};

//=============================================================================
// Constructor / Reset
//=============================================================================

ne2000::ne2000() {
  memset(mem, 0xFF, sizeof(mem));
  memset(par, 0, sizeof(par));
  memset(mar, 0, sizeof(mar));
  on_transmit = nullptr;
  reset();
}

void ne2000::set_mac(const uint8_t mac[6]) {
  memcpy(par, mac, 6);
  // Set up PROM: MAC bytes doubled (for 16-bit word reads) + NE2000 signature
  for (int i = 0; i < 6; i++) {
    mem[i * 2]     = mac[i];
    mem[i * 2 + 1] = mac[i];
  }
  // NE2000 signature bytes
  mem[12] = 0x57; mem[13] = 0x57;
  mem[14] = 0x57; mem[15] = 0x57;
  for (int i = 16; i < 32; i++)
    mem[i] = 0x57;
}

void ne2000::reset() {
  cr = CR_STP | CR_RD_ABORT;  // Stopped, DMA abort
  isr = ISR_RST;              // Reset flag set
  imr = 0;
  dcr = 0;
  tcr = 0;
  rcr = 0;
  tsr = 0;
  rsr = 0;
  pstart = 0;
  pstop = 0;
  bnry = 0;
  curr = 0;
  tpsr = 0;
  tbcr = 0;
  rsar = 0;
  rbcr = 0;
  rdar = 0;
}

//=============================================================================
// I/O Write (8-bit)
//=============================================================================

void ne2000::iowrite(int offset, uint8_t value) {
  offset &= 0x1F;

  // Data port: Remote DMA write
  if (offset == 0x10) {
    dma_write_byte(value);
    return;
  }

  // Reset port
  if (offset == 0x1F) {
    reset();
    return;
  }

  if (offset > 0x0F) return;  // Ports 0x11-0x1E unused

  // Command Register: shared across all pages
  if (offset == 0x00) {
    // Preserve TXP if transmit is in progress (auto-clears)
    cr = value & ~CR_TXP;

    // Initiate transmit if TXP set and NIC is started
    if ((value & CR_TXP) && (value & CR_STA)) {
      do_transmit();
    }

    // Clear reset status when NIC is started
    if ((value & CR_STA) && !(value & CR_STP)) {
      isr &= ~ISR_RST;
    }

    // Set up remote DMA address when starting a DMA operation
    if ((value & CR_RD_MASK) == CR_RD_READ ||
        (value & CR_RD_MASK) == CR_RD_WRITE) {
      rdar = rsar;
    }
    return;
  }

  int pg = page();

  if (pg == 0) {
    switch (offset) {
      case 0x01: pstart = value; break;
      case 0x02: pstop = value; break;
      case 0x03: bnry = value; break;
      case 0x04: tpsr = value; break;
      case 0x05: tbcr = (tbcr & 0xFF00) | value; break;
      case 0x06: tbcr = (tbcr & 0x00FF) | ((uint16_t)value << 8); break;
      case 0x07: isr &= ~value; break;   // Write 1 to clear bits
      case 0x08: rsar = (rsar & 0xFF00) | value; break;
      case 0x09: rsar = (rsar & 0x00FF) | ((uint16_t)value << 8); break;
      case 0x0A: rbcr = (rbcr & 0xFF00) | value; break;
      case 0x0B: rbcr = (rbcr & 0x00FF) | ((uint16_t)value << 8); break;
      case 0x0C: rcr = value; break;
      case 0x0D: tcr = value; break;
      case 0x0E: dcr = value; break;
      case 0x0F: imr = value; break;
    }
  } else if (pg == 1) {
    switch (offset) {
      case 0x01: case 0x02: case 0x03:
      case 0x04: case 0x05: case 0x06:
        par[offset - 1] = value;
        break;
      case 0x07: curr = value; break;
      case 0x08: case 0x09: case 0x0A: case 0x0B:
      case 0x0C: case 0x0D: case 0x0E: case 0x0F:
        mar[offset - 8] = value;
        break;
    }
  }
  // Page 2 writes are ignored (read-only mirrors of page 0)
  // Page 3 is not used on NE2000
}

//=============================================================================
// I/O Read (8-bit)
//=============================================================================

uint8_t ne2000::ioread(int offset) {
  offset &= 0x1F;

  // Data port: Remote DMA read
  if (offset == 0x10) {
    return dma_read_byte();
  }

  // Reset port read also triggers reset
  if (offset == 0x1F) {
    reset();
    return 0;
  }

  if (offset > 0x0F) return 0xFF;

  // Command Register: shared across all pages
  if (offset == 0x00) return cr;

  int pg = page();

  if (pg == 0) {
    switch (offset) {
      case 0x01: return rdar & 0xFF;            // CLDA0
      case 0x02: return (rdar >> 8) & 0xFF;     // CLDA1
      case 0x03: return bnry;
      case 0x04: return tsr;
      case 0x05: return 0;                       // NCR (collision count)
      case 0x06: return 0;                       // FIFO
      case 0x07: return isr;
      case 0x08: return rdar & 0xFF;            // CRDA0
      case 0x09: return (rdar >> 8) & 0xFF;     // CRDA1
      case 0x0A: return 0;                       // Reserved
      case 0x0B: return 0;                       // Reserved
      case 0x0C: return rsr;
      case 0x0D: return 0;                       // CNTR0
      case 0x0E: return 0;                       // CNTR1
      case 0x0F: return 0;                       // CNTR2
    }
  } else if (pg == 1) {
    switch (offset) {
      case 0x01: case 0x02: case 0x03:
      case 0x04: case 0x05: case 0x06:
        return par[offset - 1];
      case 0x07: return curr;
      case 0x08: case 0x09: case 0x0A: case 0x0B:
      case 0x0C: case 0x0D: case 0x0E: case 0x0F:
        return mar[offset - 8];
    }
  } else if (pg == 2) {
    // Page 2: read-only mirrors of page 0 write registers
    switch (offset) {
      case 0x01: return pstart;
      case 0x02: return pstop;
      case 0x03: return bnry;
      case 0x04: return tpsr;
      case 0x05: return tbcr & 0xFF;
      case 0x06: return (tbcr >> 8) & 0xFF;
      case 0x07: return isr;
      case 0x08: return rsar & 0xFF;
      case 0x09: return (rsar >> 8) & 0xFF;
      case 0x0A: return rbcr & 0xFF;
      case 0x0B: return (rbcr >> 8) & 0xFF;
      case 0x0C: return rcr;
      case 0x0D: return tcr;
      case 0x0E: return dcr;
      case 0x0F: return imr;
    }
  }

  return 0xFF;
}

//=============================================================================
// I/O Write/Read (16-bit) - for INW/OUTW on the data port
//=============================================================================

void ne2000::iowrite16(int offset, uint16_t value) {
  if ((offset & 0x1F) == 0x10) {
    // Data port: two sequential byte DMA writes
    dma_write_byte(value & 0xFF);
    dma_write_byte((value >> 8) & 0xFF);
  } else {
    // Register ports: split into two byte writes
    iowrite(offset, value & 0xFF);
    int next = (offset & 0x1F) + 1;
    if (next < 0x10)
      iowrite((offset & ~0x1F) | next, (value >> 8) & 0xFF);
  }
}

uint16_t ne2000::ioread16(int offset) {
  if ((offset & 0x1F) == 0x10) {
    // Data port: two sequential byte DMA reads
    uint16_t lo = dma_read_byte();
    uint16_t hi = dma_read_byte();
    return lo | (hi << 8);
  }
  return ioread(offset);
}

//=============================================================================
// Remote DMA
//=============================================================================

uint8_t ne2000::dma_read_byte() {
  if (rbcr == 0) return 0xFF;

  uint8_t val = (rdar < MEM_TOTAL) ? mem[rdar] : 0xFF;

  rdar++;
  if (pstop && rdar == (uint16_t)(pstop << 8))
    rdar = (uint16_t)(pstart << 8);

  rbcr--;
  if (rbcr == 0)
    isr |= ISR_RDC;

  return val;
}

void ne2000::dma_write_byte(uint8_t val) {
  if (rbcr == 0) return;

  if (rdar >= BUF_START && rdar < MEM_TOTAL)
    mem[rdar] = val;

  rdar++;
  if (pstop && rdar == (uint16_t)(pstop << 8))
    rdar = (uint16_t)(pstart << 8);

  rbcr--;
  if (rbcr == 0)
    isr |= ISR_RDC;
}

//=============================================================================
// Transmit
//=============================================================================

void ne2000::do_transmit() {
  uint16_t addr = (uint16_t)tpsr << 8;
  uint16_t len = tbcr;

  if (len == 0 || len > 1600) {
    tsr = 0;
    isr |= ISR_TXE;
    return;
  }

  // In loopback mode, complete without actually sending
  if (tcr & TCR_LB_MASK) {
    tsr = 0x01;  // Packet transmitted OK
    isr |= ISR_PTX;
    return;
  }

  // Extract packet from NIC buffer
  uint8_t pkt[1600];
  uint16_t stop = pstop ? (uint16_t)(pstop << 8) : MEM_TOTAL;
  uint16_t start = pstart ? (uint16_t)(pstart << 8) : BUF_START;

  for (uint16_t i = 0; i < len; i++) {
    uint16_t a = addr + i;
    if (a >= stop)
      a = start + (a - stop);
    pkt[i] = (a < MEM_TOTAL) ? mem[a] : 0;
  }

  // Deliver to host
  if (on_transmit)
    on_transmit(pkt, len);

  tsr = 0x01;  // Packet transmitted OK
  isr |= ISR_PTX;
}

//=============================================================================
// Receive
//=============================================================================

void ne2000::receive(const uint8_t *data, int len) {
  // NIC must be started and not in monitor mode
  if (cr & CR_STP) return;
  if (rcr & RCR_MON) return;
  if (len < 14) return;  // Need at least an Ethernet header

  // Destination address filtering
  bool broadcast = true;
  for (int i = 0; i < 6; i++)
    if (data[i] != 0xFF) { broadcast = false; break; }

  bool unicast = true;
  for (int i = 0; i < 6; i++)
    if (data[i] != par[i]) { unicast = false; break; }

  bool multicast = (data[0] & 0x01) && !broadcast;

  if (!(rcr & RCR_PRO)) {
    if (!unicast) {
      if (broadcast && !(rcr & RCR_AB)) return;
      if (multicast && !(rcr & RCR_AM)) return;
      if (!broadcast && !multicast) return;
    }
  }

  // Total size: 4-byte receive header + packet data
  int total = len + 4;
  int pages_needed = (total + 255) / 256;

  // Check ring buffer space
  uint16_t stop = pstop ? pstop : 0xC0;
  uint16_t start = pstart ? pstart : 0x46;
  int avail;
  if (curr >= bnry)
    avail = (stop - curr) + (bnry - start);
  else
    avail = bnry - curr;

  if (pages_needed >= avail) {
    isr |= ISR_OVW;
    return;
  }

  // Next page pointer (wraps at pstop -> pstart)
  uint8_t next = curr + pages_needed;
  if (next >= stop) next = start + (next - stop);

  // Write 4-byte receive header + packet data into ring buffer
  uint16_t addr = (uint16_t)curr << 8;
  uint16_t stop_addr = (uint16_t)stop << 8;
  uint16_t start_addr = (uint16_t)start << 8;

  auto write_ring = [&](int idx, uint8_t v) {
    uint16_t a = addr + idx;
    if (a >= stop_addr)
      a = start_addr + (a - stop_addr);
    if (a < MEM_TOTAL)
      mem[a] = v;
  };

  write_ring(0, 0x01);              // RSR: packet received OK
  write_ring(1, next);              // Next page pointer
  write_ring(2, total & 0xFF);      // Length low
  write_ring(3, (total >> 8) & 0xFF); // Length high

  for (int i = 0; i < len; i++)
    write_ring(4 + i, data[i]);

  // Update current page and set interrupt
  curr = next;
  rsr = 0x01;
  isr |= ISR_PRX;
}
