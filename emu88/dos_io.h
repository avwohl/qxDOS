#ifndef DOS_IO_H
#define DOS_IO_H

#include <cstdint>
#include <cstddef>

// Abstract I/O interface for the DOS machine.
// Platform code (iOS, macOS, CLI) implements this.

class dos_io {
public:
  virtual ~dos_io() = default;

  // Console I/O
  virtual void console_write(uint8_t ch) = 0;
  virtual bool console_has_input() = 0;
  virtual int console_read() = 0;  // returns -1 if no input

  // Video display
  virtual void video_mode_changed(int mode, int cols, int rows) = 0;
  // vram points to character/attribute pairs (2 bytes per cell)
  virtual void video_refresh(const uint8_t *vram, int cols, int rows) = 0;
  // Graphics mode: framebuffer is width*height bytes of palette indices,
  // palette is 256*3 bytes of RGB (0-63 VGA DAC range)
  virtual void video_refresh_gfx(const uint8_t *framebuf, int width, int height,
                                  const uint8_t palette[][3]) {
    (void)framebuf; (void)width; (void)height; (void)palette;
  }
  virtual void video_set_cursor(int row, int col) = 0;

  // Disk I/O (drive: 0=A, 1=B, 0x80=C, 0x81=D, 0xE0=CD-ROM)
  virtual bool disk_present(int drive) = 0;
  virtual size_t disk_read(int drive, uint64_t offset,
                           uint8_t *buf, size_t count) = 0;
  virtual size_t disk_write(int drive, uint64_t offset,
                            const uint8_t *buf, size_t count) = 0;
  virtual uint64_t disk_size(int drive) = 0;
  virtual int disk_sector_size(int drive) { (void)drive; return 512; }

  // Time
  virtual void get_time(int &hour, int &min, int &sec, int &hundredths) = 0;
  virtual void get_date(int &year, int &month, int &day, int &weekday) = 0;

  // Speaker (optional)
  virtual void speaker_beep(int freq_hz, int duration_ms) {
    (void)freq_hz; (void)duration_ms;
  }

  // Mouse input from host (optional)
  // Returns current mouse state: x (0-639), y (0-199), buttons (bit0=L, bit1=R, bit2=M)
  virtual void mouse_get_state(int &x, int &y, int &buttons) {
    x = 0; y = 0; buttons = 0;
  }
  virtual bool mouse_present() { return false; }

  // Host file transfer (for R.COM/W.COM guest programs)
  virtual bool host_file_open_read(const char *path) { (void)path; return false; }
  virtual bool host_file_open_write(const char *path) { (void)path; return false; }
  virtual int host_file_read_byte() { return -1; }     // -1 = EOF/error
  virtual bool host_file_write_byte(uint8_t byte) { (void)byte; return false; }
  virtual void host_file_close_read() {}
  virtual void host_file_close_write() {}

  // Network I/O (for NE2000 emulation)
  virtual void net_send(const uint8_t *data, int len) { (void)data; (void)len; }
  virtual int net_receive(uint8_t *buf, int maxlen) { (void)buf; (void)maxlen; return 0; }
  virtual bool net_available() { return false; }
};

#endif // DOS_IO_H
