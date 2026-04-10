#ifndef EMU88_TYPES_H
#define EMU88_TYPES_H

#include <cstdint>

typedef uint8_t emu88_uint8;
typedef int8_t emu88_int8;
typedef uint16_t emu88_uint16;
typedef int16_t emu88_int16;
typedef uint32_t emu88_uint32;
typedef int32_t emu88_int32;
typedef uint64_t emu88_uint64;
typedef int64_t emu88_int64;

#define EMU88_MK32(lo16, hi16) ((emu88_uint32(hi16) << 16) | emu88_uint32(lo16))

#define EMU88_GET_LOW8(x) ((x) & 0xFF)
#define EMU88_GET_HIGH8(x) (((x) >> 8) & 0xFF)
#define EMU88_MK16(low, high) ((emu88_uint16(high) << 8) | emu88_uint16(low))
#define EMU88_MK20(seg, off) ((emu88_uint32(seg) << 4) + emu88_uint32(off))

void emu88_fatal(const char *fmt, ...);

#endif // EMU88_TYPES_H
