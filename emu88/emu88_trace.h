#ifndef EMU88_TRACE_H
#define EMU88_TRACE_H

#include "emu88_types.h"

class emu88_trace {
public:
  virtual ~emu88_trace() = default;

  virtual void comment(const char *fmt, ...) {
    (void)fmt;
  }

  virtual void asm_op(const char *fmt, ...) {
    (void)fmt;
  }

  virtual void flush(void) {
  }

  virtual void fetch(emu88_uint8 opstream_byte, emu88_uint32 addr) {
    (void)opstream_byte;
    (void)addr;
  }
};

#endif // EMU88_TRACE_H
