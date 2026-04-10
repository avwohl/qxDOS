/*
 * Emu88Emulator.mm - Objective-C++ bridge for the emu88 hardware backend
 *
 * Adapted from /Users/wohl/src/iosFreeDOS/iosFreeDOS/Bridge/DOSEmulator.mm.
 * Notable changes vs the original:
 *   1. All public symbols renamed Emu88* to coexist with DOSBox's DOSEmulator.
 *   2. Frame delivery composed to RGBA in C++ and sent through the same
 *      `emulatorFrameReady:width:height:` selector qxDOS already uses for
 *      DOSBox, so EmulatorViewModel needs no separate render path.
 *   3. Disk storage replaced with mmap(MAP_SHARED) for writable host files
 *      and mmap(MAP_PRIVATE) when the file is read-only. The classic heap
 *      path is kept only for `loadDisk:fromData:` (in-memory blobs).
 *   4. NE2000 networking wired up via Emu88SlirpNet so the guest gets a
 *      working DHCP/NAT TCP/IP stack on top of the same libslirp instance
 *      DOSBox uses.
 *   5. dos_io subclass moved into an anonymous namespace.
 *   6. Stop is a clean in-process teardown — no _exit(0) — so the user can
 *      restart the emu88 backend without killing the app.
 *   7. emulatorDidExit dispatched after the run loop drains.
 */

#import "Emu88Emulator.h"
#import "Emu88SlirpNet.h"

#include "../../emu88/dos_machine.h"
#include "../../emu88/vga_font_8x16.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <mach/mach_time.h>
#include <sched.h>

//=============================================================================
// CGA 16-color palette (used for text-mode attribute decoding)
//=============================================================================

namespace {

struct RGBA { uint8_t r, g, b, a; };

constexpr RGBA kCgaPalette[16] = {
    {0x00, 0x00, 0x00, 0xFF}, // 0  black
    {0x00, 0x00, 0xAA, 0xFF}, // 1  blue
    {0x00, 0xAA, 0x00, 0xFF}, // 2  green
    {0x00, 0xAA, 0xAA, 0xFF}, // 3  cyan
    {0xAA, 0x00, 0x00, 0xFF}, // 4  red
    {0xAA, 0x00, 0xAA, 0xFF}, // 5  magenta
    {0xAA, 0x55, 0x00, 0xFF}, // 6  brown
    {0xAA, 0xAA, 0xAA, 0xFF}, // 7  light gray
    {0x55, 0x55, 0x55, 0xFF}, // 8  dark gray
    {0x55, 0x55, 0xFF, 0xFF}, // 9  light blue
    {0x55, 0xFF, 0x55, 0xFF}, // 10 light green
    {0x55, 0xFF, 0xFF, 0xFF}, // 11 light cyan
    {0xFF, 0x55, 0x55, 0xFF}, // 12 light red
    {0xFF, 0x55, 0xFF, 0xFF}, // 13 light magenta
    {0xFF, 0xFF, 0x55, 0xFF}, // 14 yellow
    {0xFF, 0xFF, 0xFF, 0xFF}, // 15 white
};

inline RGBA expand_dac(uint8_t r6, uint8_t g6, uint8_t b6) {
    // VGA DAC values are 6-bit (0..63). Scale up to 8-bit.
    return RGBA{
        (uint8_t)((r6 * 255) / 63),
        (uint8_t)((g6 * 255) / 63),
        (uint8_t)((b6 * 255) / 63),
        0xFF
    };
}

//=============================================================================
// mmap-backed disk store
//=============================================================================

struct DiskBacking {
    uint8_t *base = nullptr;
    uint64_t size = 0;
    int      fd   = -1;     // -1 if heap-allocated
    bool     is_mmap = false;
    bool     manifest = false;
};

static constexpr int kMaxDrives = 5;

inline int drive_index(int drive) {
    if (drive >= 0 && drive < 2) return drive;        // A, B
    if (drive >= 0x80 && drive < 0x82) return drive - 0x80 + 2;  // C, D
    if (drive == 0xE0) return 4;                       // CD-ROM
    return -1;
}

//=============================================================================
// dos_io implementation for qxDOS / emu88
//=============================================================================

class dos_io_qxdos : public dos_io {
public:
    dos_io_qxdos() {
        for (int i = 0; i < kMaxDrives; ++i) disks[i] = DiskBacking{};
        mach_timebase_info(&timebase_);
        last_video_ns_ = 0;
        slirp_ = std::make_unique<Emu88SlirpNet>();
        slirp_->start();
    }

    ~dos_io_qxdos() override {
        for (int i = 0; i < kMaxDrives; ++i) free_disk(i);
        if (slirp_) slirp_->stop();
    }

    // ---- delegate plumbing ----
    __weak id<Emu88EmulatorDelegate> delegate;

    std::atomic<int> mouse_x{320};
    std::atomic<int> mouse_y{100};
    std::atomic<int> mouse_btn{0};
    bool has_mouse = true;

    Emu88SlirpNet *net() { return slirp_.get(); }

    // ---- disk loading ----

    bool load_disk_path(int drive, const char *path) {
        int idx = drive_index(drive);
        if (idx < 0 || !path) return false;
        free_disk(idx);

        int fd = ::open(path, O_RDWR);
        bool shared = true;
        int prot = PROT_READ | PROT_WRITE;
        int flags = MAP_SHARED;
        if (fd < 0) {
            // Fall back to read-only / private map for catalog or sandboxed files.
            fd = ::open(path, O_RDONLY);
            if (fd < 0) {
                NSLog(@"[emu88] open(%s) failed: %s", path, strerror(errno));
                return false;
            }
            shared = false;
            prot = PROT_READ | PROT_WRITE;  // PROT_WRITE on a MAP_PRIVATE region
            flags = MAP_PRIVATE;             // → COW pages, never written back
        }

        struct stat st;
        if (::fstat(fd, &st) != 0 || st.st_size == 0) {
            NSLog(@"[emu88] fstat(%s) failed", path);
            ::close(fd);
            return false;
        }

        void *map = ::mmap(nullptr, (size_t)st.st_size, prot, flags, fd, 0);
        if (map == MAP_FAILED) {
            NSLog(@"[emu88] mmap(%s, %lld) failed: %s",
                  path, (long long)st.st_size, strerror(errno));
            ::close(fd);
            return false;
        }
        ::madvise(map, (size_t)st.st_size, MADV_RANDOM);

        DiskBacking &d = disks[idx];
        d.base = (uint8_t *)map;
        d.size = (uint64_t)st.st_size;
        d.fd   = fd;
        d.is_mmap = true;
        d.manifest = false;
        NSLog(@"[emu88] mounted drive 0x%02X (%s, %lld bytes, %s)",
              drive, path, (long long)st.st_size,
              shared ? "writable" : "read-only");
        return true;
    }

    bool load_disk_data(int drive, const uint8_t *data, size_t size) {
        int idx = drive_index(drive);
        if (idx < 0 || !data) return false;
        free_disk(idx);
        auto *buf = new uint8_t[size];
        std::memcpy(buf, data, size);
        disks[idx].base = buf;
        disks[idx].size = size;
        disks[idx].fd   = -1;
        disks[idx].is_mmap = false;
        return true;
    }

    bool load_iso_path(const char *path) {
        int idx = drive_index(0xE0);
        if (idx < 0 || !path) return false;
        free_disk(idx);
        int fd = ::open(path, O_RDONLY);
        if (fd < 0) return false;
        struct stat st;
        if (::fstat(fd, &st) != 0 || st.st_size == 0) {
            ::close(fd);
            return false;
        }
        void *map = ::mmap(nullptr, (size_t)st.st_size,
                           PROT_READ, MAP_PRIVATE, fd, 0);
        if (map == MAP_FAILED) { ::close(fd); return false; }
        ::madvise(map, (size_t)st.st_size, MADV_RANDOM);
        DiskBacking &d = disks[idx];
        d.base = (uint8_t *)map;
        d.size = (uint64_t)st.st_size;
        d.fd   = fd;
        d.is_mmap = true;
        d.manifest = false;
        return true;
    }

    bool is_loaded(int drive) {
        int idx = drive_index(drive);
        return idx >= 0 && disks[idx].base != nullptr;
    }

    uint64_t get_disk_size(int drive) {
        int idx = drive_index(drive);
        return idx >= 0 ? disks[idx].size : 0;
    }

    NSData *get_disk_data(int drive) {
        int idx = drive_index(drive);
        if (idx < 0 || !disks[idx].base) return nil;
        return [NSData dataWithBytes:disks[idx].base length:(NSUInteger)disks[idx].size];
    }

    void sync_all_disks() {
        for (int i = 0; i < kMaxDrives; ++i) {
            DiskBacking &d = disks[i];
            if (d.is_mmap && d.base) {
                ::msync(d.base, (size_t)d.size, MS_ASYNC);
            }
        }
    }

    void set_manifest(int drive, bool manifest) {
        int idx = drive_index(drive);
        if (idx >= 0) disks[idx].manifest = manifest;
    }

    bool poll_manifest_write_warning() {
        return manifest_write_fired_.exchange(false);
    }

    // ---- dos_io overrides ----

    void console_write(uint8_t ch) override { (void)ch; }
    bool console_has_input() override { return false; }
    int  console_read() override { return -1; }

    void video_mode_changed(int mode, int cols, int rows) override {
        (void)mode; (void)cols; (void)rows;
        // Mode changes are reflected at the next refresh; no need to notify
        // the UI separately because the framebuffer dimensions are encoded
        // in each emulatorFrameReady: dispatch.
    }

    void video_refresh(const uint8_t *vram, int cols, int rows) override {
        if (!should_refresh()) return;
        if (!vram || cols <= 0 || rows <= 0) return;

        const int cell_w = 8, cell_h = 16;
        const int width  = cols * cell_w;
        const int height = rows * cell_h;

        NSMutableData *out = [NSMutableData dataWithLength:(NSUInteger)(width * height * 4)];
        if (!out) return;
        RGBA *pixels = (RGBA *)out.mutableBytes;

        const int cur_row = cursor_row_;
        const int cur_col = cursor_col_;

        for (int row = 0; row < rows; ++row) {
            for (int col = 0; col < cols; ++col) {
                int cell_idx = (row * cols + col) * 2;
                uint8_t ch = vram[cell_idx];
                uint8_t attr = vram[cell_idx + 1];
                RGBA fg = kCgaPalette[attr & 0x0F];
                RGBA bg = kCgaPalette[(attr >> 4) & 0x07];

                const uint8_t *glyph = vga_font_8x16[ch];
                int px = col * cell_w;
                int py = row * cell_h;
                for (int gy = 0; gy < cell_h; ++gy) {
                    uint8_t bits = glyph[gy];
                    int line_off = (py + gy) * width + px;
                    for (int gx = 0; gx < cell_w; ++gx) {
                        bool on = (bits >> (7 - gx)) & 1;
                        pixels[line_off + gx] = on ? fg : bg;
                    }
                }

                // Cursor: 2-pixel underline at bottom of cell.
                if (row == cur_row && col == cur_col) {
                    int line_off = (py + cell_h - 2) * width + px;
                    for (int gx = 0; gx < cell_w; ++gx) pixels[line_off + gx] = fg;
                    line_off = (py + cell_h - 1) * width + px;
                    for (int gx = 0; gx < cell_w; ++gx) pixels[line_off + gx] = fg;
                }
            }
        }

        dispatch_to_delegate(out, width, height);
    }

    void video_refresh_gfx(const uint8_t *framebuf, int width, int height,
                           const uint8_t palette[][3]) override {
        if (!should_refresh()) return;
        if (!framebuf || width <= 0 || height <= 0) return;

        NSMutableData *out = [NSMutableData dataWithLength:(NSUInteger)(width * height * 4)];
        if (!out) return;
        RGBA *pixels = (RGBA *)out.mutableBytes;

        // Pre-expand the 256-entry DAC palette.
        RGBA pal[256];
        for (int i = 0; i < 256; ++i) {
            pal[i] = expand_dac(palette[i][0], palette[i][1], palette[i][2]);
        }

        int n = width * height;
        for (int i = 0; i < n; ++i) pixels[i] = pal[framebuf[i]];

        dispatch_to_delegate(out, width, height);
    }

    void video_set_cursor(int row, int col) override {
        cursor_row_ = row;
        cursor_col_ = col;
    }

    bool disk_present(int drive) override {
        int idx = drive_index(drive);
        return idx >= 0 && disks[idx].base != nullptr;
    }

    size_t disk_read(int drive, uint64_t offset, uint8_t *buf, size_t count) override {
        int idx = drive_index(drive);
        if (idx < 0 || !disks[idx].base) return 0;
        if (offset >= disks[idx].size) return 0;
        if (offset + count > disks[idx].size) count = disks[idx].size - offset;
        std::memcpy(buf, disks[idx].base + offset, count);
        return count;
    }

    size_t disk_write(int drive, uint64_t offset, const uint8_t *buf, size_t count) override {
        int idx = drive_index(drive);
        if (idx < 0 || !disks[idx].base) return 0;
        if (offset >= disks[idx].size) return 0;
        if (offset + count > disks[idx].size) count = disks[idx].size - offset;
        std::memcpy(disks[idx].base + offset, buf, count);
        if (disks[idx].manifest) manifest_write_fired_.store(true);
        return count;
    }

    uint64_t disk_size(int drive) override {
        int idx = drive_index(drive);
        return idx >= 0 ? disks[idx].size : 0;
    }

    void get_time(int &hour, int &minute, int &sec, int &hundredths) override {
        NSDate *now = [NSDate date];
        NSCalendar *cal = [NSCalendar currentCalendar];
        NSDateComponents *c = [cal components:(NSCalendarUnitHour |
                                                NSCalendarUnitMinute |
                                                NSCalendarUnitSecond)
                                     fromDate:now];
        hour = (int)c.hour;
        minute = (int)c.minute;
        sec = (int)c.second;
        hundredths = 0;
    }

    void get_date(int &year, int &month, int &day, int &weekday) override {
        NSDate *now = [NSDate date];
        NSCalendar *cal = [NSCalendar currentCalendar];
        NSDateComponents *c = [cal components:(NSCalendarUnitYear |
                                                NSCalendarUnitMonth |
                                                NSCalendarUnitDay |
                                                NSCalendarUnitWeekday)
                                     fromDate:now];
        year = (int)c.year;
        month = (int)c.month;
        day = (int)c.day;
        weekday = ((int)c.weekday + 5) % 7;
    }

    void mouse_get_state(int &x, int &y, int &buttons) override {
        x = mouse_x.load();
        y = mouse_y.load();
        buttons = mouse_btn.load();
    }

    bool mouse_present() override { return has_mouse; }

    // ---- networking (NE2000 → libslirp) ----

    void net_send(const uint8_t *data, int len) override {
        if (slirp_) slirp_->send_packet(data, len);
    }

    int net_receive(uint8_t *buf, int max_len) override {
        return slirp_ ? slirp_->receive_packet(buf, max_len) : 0;
    }

    bool net_available() override {
        return slirp_ ? slirp_->packet_available() : false;
    }

private:
    DiskBacking disks[kMaxDrives];
    std::atomic<bool> manifest_write_fired_{false};

    mach_timebase_info_data_t timebase_;
    uint64_t last_video_ns_;
    int cursor_row_ = -1;
    int cursor_col_ = -1;

    std::unique_ptr<Emu88SlirpNet> slirp_;

    void free_disk(int idx) {
        DiskBacking &d = disks[idx];
        if (!d.base) { d = DiskBacking{}; return; }
        if (d.is_mmap) {
            ::munmap(d.base, (size_t)d.size);
            if (d.fd >= 0) ::close(d.fd);
        } else {
            delete[] d.base;
        }
        d = DiskBacking{};
    }

    bool should_refresh() {
        uint64_t now_ns = mach_absolute_time() * timebase_.numer / timebase_.denom;
        if (now_ns - last_video_ns_ < 16000000ULL) return false;  // ~60 fps cap
        last_video_ns_ = now_ns;
        return true;
    }

    void dispatch_to_delegate(NSData *frame, int w, int h) {
        id<Emu88EmulatorDelegate> d = delegate;
        if (!d) return;
        if (![d respondsToSelector:@selector(emulatorFrameReady:width:height:)]) return;
        dispatch_async(dispatch_get_main_queue(), ^{
            [d emulatorFrameReady:frame width:w height:h];
        });
    }
};

//=============================================================================
// ASCII → BIOS scancode lookup
//=============================================================================

uint8_t ascii_to_scancode(uint8_t ascii) {
    if (ascii >= 1 && ascii <= 26) {
        return ascii_to_scancode((uint8_t)('a' + ascii - 1));
    }
    if (ascii >= 'a' && ascii <= 'z') {
        static const uint8_t sc[] = {
            0x1E,0x30,0x2E,0x20,0x12,0x21,0x22,0x23,0x17,0x24,
            0x25,0x26,0x32,0x31,0x18,0x19,0x10,0x13,0x1F,0x14,
            0x16,0x2F,0x11,0x2D,0x15,0x2C
        };
        return sc[ascii - 'a'];
    }
    if (ascii >= 'A' && ascii <= 'Z') return ascii_to_scancode((uint8_t)(ascii - 'A' + 'a'));
    if (ascii >= '1' && ascii <= '9') return 0x02 + (ascii - '1');
    if (ascii == '0') return 0x0B;
    switch (ascii) {
        case '\r': case '\n': return 0x1C;
        case '\t': return 0x0F;
        case 0x1B: return 0x01;
        case ' ':  return 0x39;
        case '-':  return 0x0C;
        case '=':  return 0x0D;
        case '[':  return 0x1A;
        case ']':  return 0x1B;
        case '\\': return 0x2B;
        case ';':  return 0x27;
        case '\'': return 0x28;
        case '`':  return 0x29;
        case ',':  return 0x33;
        case '.':  return 0x34;
        case '/':  return 0x35;
        case 0x08: return 0x0E;
        default:   return 0x00;
    }
}

} // anonymous namespace

//=============================================================================
// Emu88Emulator
//=============================================================================

@implementation Emu88Emulator {
    std::unique_ptr<emu88_mem>   _mem;
    std::unique_ptr<dos_io_qxdos> _io;
    std::unique_ptr<dos_machine> _machine;

    dispatch_queue_t       _emulatorQueue;
    dispatch_semaphore_t   _keySemaphore;
    BOOL                   _shouldRun;
    Emu88ControlifyMode    _controlifyMode;

    dos_machine::Config    _config;
}

- (instancetype)init {
    self = [super init];
    if (self) {
        _emulatorQueue  = dispatch_queue_create("com.awohl.qxDOS.emu88", DISPATCH_QUEUE_SERIAL);
        _keySemaphore   = dispatch_semaphore_create(0);
        _shouldRun      = NO;
        _controlifyMode = Emu88ControlifyOff;
    }
    return self;
}

- (void)dealloc { [self stop]; }

// ---- configuration ----

- (void)setCpuType:(Emu88CpuType)type {
    switch (type) {
        case Emu88Cpu8088: _config.cpu = emu88::CPU_8088; break;
        case Emu88Cpu286:  _config.cpu = emu88::CPU_286;  break;
        case Emu88Cpu386:  _config.cpu = emu88::CPU_386;  break;
    }
}

- (void)setDisplayAdapter:(Emu88DisplayAdapter)adapter {
    _config.display = static_cast<dos_machine::DisplayAdapter>(adapter);
}

- (void)setMouseEnabled:(BOOL)enabled {
    _config.mouse_enabled = enabled;
    if (_io) _io->has_mouse = enabled;
}

- (void)setSpeakerEnabled:(BOOL)enabled {
    _config.speaker_enabled = enabled;
}

// ---- disks ----

- (BOOL)loadDisk:(int)drive fromPath:(NSString *)path {
    if (!_io) _io = std::make_unique<dos_io_qxdos>();
    return _io->load_disk_path(drive, [path UTF8String]);
}

- (BOOL)loadDisk:(int)drive fromData:(NSData *)data {
    if (!_io) _io = std::make_unique<dos_io_qxdos>();
    return _io->load_disk_data(drive, (const uint8_t *)data.bytes, (size_t)data.length);
}

- (nullable NSData *)getDiskData:(int)drive {
    return _io ? _io->get_disk_data(drive) : nil;
}

- (BOOL)saveDisk:(int)drive toPath:(NSString *)path {
    NSData *d = [self getDiskData:drive];
    if (!d) return NO;
    return [d writeToFile:path atomically:YES];
}

- (BOOL)isDiskLoaded:(int)drive { return _io && _io->is_loaded(drive); }
- (uint64_t)diskSize:(int)drive { return _io ? _io->get_disk_size(drive) : 0; }

- (int)loadISO:(NSString *)path {
    if (!_io) _io = std::make_unique<dos_io_qxdos>();
    return _io->load_iso_path([path UTF8String]) ? 0xE0 : -1;
}

- (void)syncDisks {
    if (_io) _io->sync_all_disks();
}

// ---- execution ----

- (BOOL)isRunning          { return _shouldRun; }
- (BOOL)isWaitingForInput  { return _machine && _machine->is_waiting_for_key(); }

- (void)startWithBootDrive:(int)drive {
    if (_shouldRun) return;
    if (!_io) _io = std::make_unique<dos_io_qxdos>();
    _io->delegate = self.delegate;
    _io->has_mouse = _config.mouse_enabled;

    // Networking is on by default for both backends, mirroring DOSBox.
    _config.ne2000_enabled = true;

    // Scale physical RAM by CPU type.
    uint32_t ram_size;
    switch (_config.cpu) {
        case emu88::CPU_8088:
        case emu88::CPU_186:  ram_size = 0x100000;   break;  // 1 MB
        case emu88::CPU_286:  ram_size = 0x1000000;  break;  // 16 MB
        default:              ram_size = 0x4000000;  break;  // 64 MB
    }
    _mem     = std::make_unique<emu88_mem>(ram_size);
    _machine = std::make_unique<dos_machine>(_mem.get(), _io.get());
    _machine->configure(_config);

    if (!_machine->boot(drive)) {
        NSLog(@"[emu88] boot failed for drive 0x%02X", drive);
        return;
    }
    NSLog(@"[emu88] booted drive 0x%02X (cpu=%d display=%d speed=%d ne2000=%d)",
          drive, (int)_config.cpu, (int)_config.display,
          (int)_machine->get_speed(), _config.ne2000_enabled);

    _shouldRun = YES;
    dispatch_async(_emulatorQueue, ^{ [self runLoop]; });
}

- (void)stop {
    if (!_shouldRun && !_machine) return;
    if (_io) _io->delegate = nil;
    _shouldRun = NO;
    // Wake the run loop if it's parked on the key semaphore so it can exit.
    dispatch_semaphore_signal(_keySemaphore);
    // Wait for the emulator queue to drain so the unique_ptrs below are not
    // freed while runLoop is still touching them.
    dispatch_sync(_emulatorQueue, ^{});
    _machine.reset();
    _mem.reset();
    _io.reset();
}

- (void)reset {
    [self stop];
    // Caller is expected to call startWithBootDrive: again to restart.
}

- (void)runLoop {
    mach_timebase_info_data_t timebase;
    mach_timebase_info(&timebase);

    uint64_t batch_wall_start = mach_absolute_time();
    unsigned long long batch_cycle_start = _machine->cycles;
    int batch_count = 0;
    int net_pump_count = 0;

    while (_shouldRun) {
        bool ok = _machine->run_batch(10000);
        batch_count++;

        // Drive the libslirp poll loop on the emulator thread (same pattern
        // as DOSBox's SlirpEthernetConnection).  Pump every batch with a
        // zero-timeout poll — cheap when there are no fds, and we don't want
        // to introduce extra latency on the keypress path.
        if (_io && _io->net()) {
            if ((++net_pump_count & 3) == 0) _io->net()->pump(0);
        }

        if (!ok) {
            NSLog(@"[emu88] CPU halted at %04X:%08X (cycles=%llu, batches=%d)",
                  _machine->sregs[dos_machine::seg_CS], _machine->ip,
                  _machine->cycles, batch_count);
            break;
        }

        if (batch_count == 1000 || (batch_count % 50000 == 0)) {
            NSLog(@"[emu88] batch=%d cycles=%llu speed=%d wfk=%d halted=%d",
                  batch_count, _machine->cycles, (int)_machine->get_speed(),
                  _machine->is_waiting_for_key(), _machine->halted);
        }

        // Speed throttling: per-batch with drift cap.
        uint32_t cps = 0;
        switch (_machine->get_speed()) {
            case dos_machine::SPEED_FULL:      cps = 0;          break;
            case dos_machine::SPEED_PC_4_77:   cps = 4770000;    break;
            case dos_machine::SPEED_AT_8:      cps = 8000000;    break;
            case dos_machine::SPEED_386SX_16:  cps = 48000000;   break;
            case dos_machine::SPEED_386DX_33:  cps = 100000000;  break;
            case dos_machine::SPEED_486DX2_66: cps = 260000000;  break;
        }

        if (cps > 0) {
            unsigned long long elapsed_cycles = _machine->cycles - batch_cycle_start;
            uint64_t wall_now = mach_absolute_time();
            uint64_t wall_elapsed_ns = (wall_now - batch_wall_start) * timebase.numer / timebase.denom;
            uint64_t target_ns = (uint64_t)elapsed_cycles * 1000000000ULL / cps;

            if (target_ns > wall_elapsed_ns) {
                uint64_t sleep_ns = target_ns - wall_elapsed_ns;
                if (sleep_ns > 50000000) sleep_ns = 50000000;
                if (sleep_ns > 100000) usleep((unsigned)(sleep_ns / 1000));
            } else if (wall_elapsed_ns - target_ns > 100000000) {
                batch_wall_start = wall_now;
                batch_cycle_start = _machine->cycles;
            }
        }

        if (_machine->is_waiting_for_key()) {
            id<Emu88EmulatorDelegate> d = _io->delegate;
            if (d && [d respondsToSelector:@selector(emulatorDidRequestInput)]) {
                dispatch_async(dispatch_get_main_queue(), ^{
                    [d emulatorDidRequestInput];
                });
            }
            dispatch_semaphore_wait(_keySemaphore,
                dispatch_time(DISPATCH_TIME_NOW, 55 * NSEC_PER_MSEC));
            batch_wall_start = mach_absolute_time();
            batch_cycle_start = _machine->cycles;
        } else if (cps == 0 && (batch_count & 63) == 0) {
            sched_yield();
        }
    }

    _shouldRun = NO;

    id<Emu88EmulatorDelegate> d = _io ? _io->delegate : nil;
    if (d && [d respondsToSelector:@selector(emulatorDidExit)]) {
        dispatch_async(dispatch_get_main_queue(), ^{
            [d emulatorDidExit];
        });
    }
}

// ---- input ----

- (void)sendCharacter:(unichar)ch {
    if (!_machine) return;
    uint8_t ascii = (uint8_t)ch;
    if (_controlifyMode != Emu88ControlifyOff) {
        if (ascii >= 'a' && ascii <= 'z') ascii = (uint8_t)(ascii - 'a' + 1);
        else if (ascii >= 'A' && ascii <= 'Z') ascii = (uint8_t)(ascii - 'A' + 1);
        if (_controlifyMode == Emu88ControlifyOneChar) _controlifyMode = Emu88ControlifyOff;
    }
    if (ascii == '\n') ascii = '\r';
    _machine->queue_key(ascii, ascii_to_scancode(ascii));
    dispatch_semaphore_signal(_keySemaphore);
}

- (void)sendScancode:(uint8_t)ascii scancode:(uint8_t)scancode {
    if (!_machine) return;
    _machine->queue_key(ascii, scancode);
    dispatch_semaphore_signal(_keySemaphore);
}

- (void)sendScancodePress:(uint8_t)scancode {
    if (!_machine) return;
    _machine->queue_key(0, scancode);
    dispatch_semaphore_signal(_keySemaphore);
}

- (void)sendScancodeRelease:(uint8_t)scancode {
    // emu88's BIOS keyboard model is press-only; the queued key already
    // satisfies INT 16h. We accept the call so the input router on the
    // Swift side can call it without branching, but there is nothing to do.
    (void)scancode;
}

- (void)updateMouseX:(int)x y:(int)y buttons:(int)buttons {
    if (_io) {
        _io->mouse_x.store(x);
        _io->mouse_y.store(y);
        _io->mouse_btn.store(buttons);
    }
}

- (void)setControlify:(Emu88ControlifyMode)mode { _controlifyMode = mode; }
- (Emu88ControlifyMode)getControlify             { return _controlifyMode; }

- (void)setSpeed:(Emu88SpeedMode)mode {
    _config.speed = static_cast<dos_machine::SpeedMode>(mode);
    if (_machine) _machine->set_speed(_config.speed);
}

- (Emu88SpeedMode)getSpeed {
    if (!_machine) return Emu88SpeedFull;
    return static_cast<Emu88SpeedMode>(_machine->get_speed());
}

- (void)setDiskIsManifest:(int)drive isManifest:(BOOL)manifest {
    if (_io) _io->set_manifest(drive, manifest);
}

- (BOOL)pollManifestWriteWarning {
    return _io ? _io->poll_manifest_write_warning() : NO;
}

@end
