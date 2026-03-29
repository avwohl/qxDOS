/*
 * DOSEmulator.mm - Objective-C++ bridge for DOSBox emulator
 *
 * Wraps the C dosbox_bridge API in an Objective-C class that the
 * SwiftUI layer can use.  Manages disk image storage, config generation,
 * and frame delivery to the delegate.
 */

#import "DOSEmulator.h"
#include "dosbox_bridge.h"
#include <atomic>
#include <mutex>
#include <string>
#include <mach/mach_time.h>

//=============================================================================
// Disk storage
//=============================================================================

static constexpr int MAX_DRIVES = 5;

/// Map drive numbers to slot indices.
///  0=A, 1=B, 0x80=C, 0x81=D, 0xE0=CD-ROM
static int drive_index(int drive) {
    if (drive >= 0 && drive < 2) return drive;
    if (drive >= 0x80 && drive < 0x82) return drive - 0x80 + 2;
    if (drive == 0xE0) return 4;
    return -1;
}

//=============================================================================
// Frame callback (C trampoline → Objective-C delegate)
//=============================================================================

// Frame coalescing: drop frames if main thread hasn't consumed the previous one
static std::atomic<bool> s_frame_pending{false};

static void frame_callback(const uint8_t *pixels, int width, int height, void *ctx)
{
    if (s_frame_pending.load(std::memory_order_relaxed)) return;

    DOSEmulator *emu = (__bridge DOSEmulator *)ctx;
    id<DOSEmulatorDelegate> d = emu.delegate;
    if (d && [d respondsToSelector:@selector(emulatorFrameReady:width:height:)]) {
        NSData *data = [NSData dataWithBytes:pixels length:width * height * 4];
        int w = width, h = height;
        s_frame_pending.store(true, std::memory_order_relaxed);
        dispatch_async(dispatch_get_main_queue(), ^{
            [d emulatorFrameReady:data width:w height:h];
            s_frame_pending.store(false, std::memory_order_relaxed);
        });
    }
}

//=============================================================================
// DOSEmulator Implementation
//=============================================================================

@implementation DOSEmulator {
    // Disk images held in memory (for catalog disks loaded from NSData)
    uint8_t *_diskData[MAX_DRIVES];
    uint64_t _diskSize[MAX_DRIVES];
    bool     _diskIsManifest[MAX_DRIVES];
    std::atomic<bool> _manifestWriteFired;

    // Disk image paths on disk (for file-backed disks)
    NSString *_diskPath[MAX_DRIVES];

    // Temp directory for writing memory-backed disks to files
    NSString *_tmpDir;

    // Configuration
    DOSMachineType _machineType;
    int _memoryMB;
    BOOL _mouseEnabled;
    BOOL _speakerEnabled;
    BOOL _sbEnabled;
    DOSSpeedMode _speedMode;
    int _customCycles;
    NSString *_cpuType;

    dispatch_queue_t _emulatorQueue;
    BOOL _shouldRun;
}

- (instancetype)init {
    self = [super init];
    if (self) {
        for (int i = 0; i < MAX_DRIVES; i++) {
            _diskData[i] = nullptr;
            _diskSize[i] = 0;
            _diskIsManifest[i] = false;
            _diskPath[i] = nil;
        }
        _manifestWriteFired.store(false);
        _machineType = DOSMachineSVGA;
        _memoryMB = 16;
        _mouseEnabled = YES;
        _speakerEnabled = YES;
        _sbEnabled = YES;
        _speedMode = DOSSpeedMax;
        _customCycles = 0;
        _cpuType = @"auto";
        _emulatorQueue = dispatch_queue_create("com.awohl.qxDOS.dosbox", DISPATCH_QUEUE_SERIAL);
        _shouldRun = NO;

        // Create temp directory for disk image files
        _tmpDir = [NSTemporaryDirectory() stringByAppendingPathComponent:@"dosbox-disks"];
        [[NSFileManager defaultManager] createDirectoryAtPath:_tmpDir
                                  withIntermediateDirectories:YES
                                                   attributes:nil
                                                        error:nil];
    }
    return self;
}

- (void)dealloc {
    [self stop];
    for (int i = 0; i < MAX_DRIVES; i++)
        delete[] _diskData[i];
}

#pragma mark - Configuration

- (void)setMachineType:(DOSMachineType)type { _machineType = type; }
- (void)setCpuType:(NSString*)cpuType { _cpuType = [cpuType copy]; }
- (void)setMemoryMB:(int)mb { _memoryMB = mb; }
- (void)setMouseEnabled:(BOOL)enabled { _mouseEnabled = enabled; }
- (void)setSpeakerEnabled:(BOOL)enabled { _speakerEnabled = enabled; }
- (void)setSoundBlasterEnabled:(BOOL)enabled { _sbEnabled = enabled; }

#pragma mark - Disk Management

- (BOOL)loadDisk:(int)drive fromPath:(NSString*)path {
    int idx = drive_index(drive);
    if (idx < 0) return NO;

    // Store the path — DOSBox will mount directly from the file
    _diskPath[idx] = [path copy];

    // Also read into memory for getDiskData / saveDisk
    NSData *data = [NSData dataWithContentsOfFile:path];
    if (!data) return NO;

    delete[] _diskData[idx];
    _diskData[idx] = new uint8_t[data.length];
    memcpy(_diskData[idx], data.bytes, data.length);
    _diskSize[idx] = data.length;
    return YES;
}

- (BOOL)loadDisk:(int)drive fromData:(NSData*)data {
    int idx = drive_index(drive);
    if (idx < 0) return NO;

    delete[] _diskData[idx];
    _diskData[idx] = new uint8_t[data.length];
    memcpy(_diskData[idx], data.bytes, data.length);
    _diskSize[idx] = data.length;

    // Write to a temp file so DOSBox can mount it
    NSString *filename = [NSString stringWithFormat:@"drive_%d.img", drive];
    NSString *tmpPath = [_tmpDir stringByAppendingPathComponent:filename];
    [data writeToFile:tmpPath atomically:YES];
    _diskPath[idx] = tmpPath;

    return YES;
}

- (BOOL)isDiskLoaded:(int)drive {
    int idx = drive_index(drive);
    return idx >= 0 && _diskData[idx] != nullptr;
}

- (nullable NSData*)getDiskData:(int)drive {
    int idx = drive_index(drive);
    if (idx < 0 || !_diskData[idx]) return nil;

    // If disk is file-backed and DOSBox may have written to it, re-read
    if (_diskPath[idx] && _shouldRun) {
        NSData *fresh = [NSData dataWithContentsOfFile:_diskPath[idx]];
        if (fresh) return fresh;
    }

    return [NSData dataWithBytes:_diskData[idx] length:(NSUInteger)_diskSize[idx]];
}

- (BOOL)saveDisk:(int)drive toPath:(NSString*)path {
    NSData *data = [self getDiskData:drive];
    if (!data) return NO;
    return [data writeToFile:path atomically:YES];
}

- (uint64_t)diskSize:(int)drive {
    int idx = drive_index(drive);
    return (idx >= 0) ? _diskSize[idx] : 0;
}

- (int)loadISO:(NSString*)path {
    int idx = drive_index(0xE0);
    if (idx < 0) return -1;
    _diskPath[idx] = [path copy];

    NSData *data = [NSData dataWithContentsOfFile:path];
    if (!data) return -1;

    delete[] _diskData[idx];
    _diskData[idx] = new uint8_t[data.length];
    memcpy(_diskData[idx], data.bytes, data.length);
    _diskSize[idx] = data.length;
    return 0xE0;
}

#pragma mark - Execution

- (BOOL)isRunning { return _shouldRun; }

- (void)startWithBootDrive:(int)drive dosType:(int)dosType {
    if (_shouldRun) return;
    _shouldRun = YES;

    // Build DOSBox config
    dosbox_config_t cfg = {};

    // Machine type
    switch (_machineType) {
        case DOSMachineVGA:      cfg.machine = "vgaonly"; break;
        case DOSMachineEGA:      cfg.machine = "ega"; break;
        case DOSMachineCGA:      cfg.machine = "cga"; break;
        case DOSMachineTandy:    cfg.machine = "tandy"; break;
        case DOSMachineHercules: cfg.machine = "hercules"; break;
        case DOSMachineSVGA:     cfg.machine = "svga_s3"; break;
    }

    cfg.cputype = [_cpuType UTF8String];
    cfg.memsize = _memoryMB;
    cfg.sb_enabled = _sbEnabled ? 1 : 0;
    cfg.speaker_enabled = _speakerEnabled ? 1 : 0;
    cfg.mouse_enabled = _mouseEnabled ? 1 : 0;

    // Cycles — real mode speed (protected mode uses max for game compat)
    switch (_speedMode) {
        case DOSSpeedMax:   cfg.cycles = -1; break;   // max everywhere
        case DOSSpeed3000:  cfg.cycles = DOSBOX_SPEED_8088; break;
        case DOSSpeed8000:  cfg.cycles = DOSBOX_SPEED_286; break;
        case DOSSpeed20000: cfg.cycles = DOSBOX_SPEED_386SX; break;
        case DOSSpeed50000: cfg.cycles = DOSBOX_SPEED_486DX2; break;
        case DOSSpeedFixed: cfg.cycles = _customCycles; break;
    }
    cfg.cycles_protected = 0; // auto (max for protected mode games)

    // Disk paths
    if (_diskPath[0]) cfg.floppy_a_path = [_diskPath[0] UTF8String];
    if (_diskPath[1]) cfg.floppy_b_path = [_diskPath[1] UTF8String];
    if (_diskPath[2]) cfg.hdd_c_path    = [_diskPath[2] UTF8String];
    if (_diskPath[3]) cfg.hdd_d_path    = [_diskPath[3] UTF8String];
    if (_diskPath[4]) cfg.iso_path      = [_diskPath[4] UTF8String];

    cfg.working_dir = [_tmpDir UTF8String];

    // Host file I/O root for R.COM/W.COM — app's Documents directory
    NSString *docsDir = NSSearchPathForDirectoriesInDomains(
        NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
    cfg.host_dir = docsDir ? [docsDir UTF8String] : nullptr;

    // Boot drive and DOS type
    cfg.boot_drive = drive;
    cfg.dos_type = dosType;

    // Phase 1: init on main thread (SDL/UIKit requires it)
    int initResult = dosbox_init(&cfg, frame_callback, (__bridge void *)self);
    if (initResult != 0) {
        _shouldRun = NO;
        return;
    }

    // Phase 2: run loop on background thread (blocks until exit)
    dispatch_async(_emulatorQueue, ^{
        dosbox_run();
        self->_shouldRun = NO;
        // Notify delegate on main thread
        dispatch_async(dispatch_get_main_queue(), ^{
            id<DOSEmulatorDelegate> d = self.delegate;
            if (d && [d respondsToSelector:@selector(emulatorDidExit)]) {
                [d emulatorDidExit];
            }
        });
    });
}

- (void)stop {
    if (!_shouldRun) return;
    dosbox_request_shutdown();
    _shouldRun = NO;
    // Do NOT dispatch_sync here — dosbox_run() needs to dispatch_sync
    // back to the main thread for GFX_Destroy, which would deadlock.
    // The emulatorDidExit callback fires when shutdown completes.
}

- (void)reset {
    [self stop];
}

#pragma mark - Input

- (void)sendCharacter:(unichar)ch {
    dosbox_inject_char(ch);
}

/// Map PC AT scancodes to SDL scancodes.
/// Maps PC AT scancodes (set 1) to SDL scancodes for DOSBox input injection.
static int pc_to_sdl_scancode(uint8_t pc) {
    // Common PC AT scancode → SDL_Scancode mapping
    static const int map[128] = {
        [0x01] = 41,  // Esc
        [0x02] = 30,  // 1
        [0x03] = 31,  // 2
        [0x04] = 32,  // 3
        [0x05] = 33,  // 4
        [0x06] = 34,  // 5
        [0x07] = 35,  // 6
        [0x08] = 36,  // 7
        [0x09] = 37,  // 8
        [0x0A] = 38,  // 9
        [0x0B] = 39,  // 0
        [0x0C] = 45,  // -
        [0x0D] = 46,  // =
        [0x0E] = 42,  // Backspace
        [0x0F] = 43,  // Tab
        [0x10] = 20,  // Q
        [0x11] = 26,  // W
        [0x12] = 8,   // E
        [0x13] = 21,  // R
        [0x14] = 23,  // T
        [0x15] = 28,  // Y
        [0x16] = 24,  // U
        [0x17] = 12,  // I
        [0x18] = 18,  // O
        [0x19] = 19,  // P
        [0x1A] = 47,  // [
        [0x1B] = 48,  // ]
        [0x1C] = 40,  // Enter
        [0x1D] = 224, // LCtrl
        [0x1E] = 4,   // A
        [0x1F] = 22,  // S
        [0x20] = 7,   // D
        [0x21] = 9,   // F
        [0x22] = 10,  // G
        [0x23] = 11,  // H
        [0x24] = 13,  // J
        [0x25] = 14,  // K
        [0x26] = 15,  // L
        [0x27] = 51,  // ;
        [0x28] = 52,  // '
        [0x29] = 53,  // `
        [0x2A] = 225, // LShift
        [0x2B] = 49,  // backslash
        [0x2C] = 29,  // Z
        [0x2D] = 27,  // X
        [0x2E] = 6,   // C
        [0x2F] = 25,  // V
        [0x30] = 5,   // B
        [0x31] = 17,  // N
        [0x32] = 16,  // M
        [0x33] = 54,  // ,
        [0x34] = 55,  // .
        [0x35] = 56,  // /
        [0x36] = 229, // RShift
        [0x38] = 226, // LAlt
        [0x39] = 44,  // Space
        [0x3A] = 57,  // CapsLock
        [0x3B] = 58,  // F1
        [0x3C] = 59,  // F2
        [0x3D] = 60,  // F3
        [0x3E] = 61,  // F4
        [0x3F] = 62,  // F5
        [0x40] = 63,  // F6
        [0x41] = 64,  // F7
        [0x42] = 65,  // F8
        [0x43] = 66,  // F9
        [0x44] = 67,  // F10
        [0x47] = 74,  // Home
        [0x48] = 82,  // Up
        [0x49] = 75,  // PgUp
        [0x4B] = 80,  // Left
        [0x4D] = 79,  // Right
        [0x4F] = 77,  // End
        [0x50] = 81,  // Down
        [0x51] = 78,  // PgDn
        [0x52] = 73,  // Insert
        [0x53] = 76,  // Delete
        [0x57] = 68,  // F11
        [0x58] = 69,  // F12
    };
    if (pc < 128 && map[pc]) return map[pc];
    return pc; // fallback
}

- (void)sendScancode:(uint8_t)ascii scancode:(uint8_t)scancode {
    int sdl_sc = pc_to_sdl_scancode(scancode);
    dosbox_inject_key(sdl_sc, 1);  // press
    dosbox_inject_key(sdl_sc, 0);  // release
    (void)ascii;
}

- (void)sendScancodePress:(uint8_t)scancode {
    int sdl_sc = pc_to_sdl_scancode(scancode);
    dosbox_inject_key(sdl_sc, 1);
}

- (void)sendScancodeRelease:(uint8_t)scancode {
    int sdl_sc = pc_to_sdl_scancode(scancode);
    dosbox_inject_key(sdl_sc, 0);
}

- (void)updateMouseX:(int)x y:(int)y buttons:(int)buttons {
    dosbox_inject_mouse_abs(x, y, buttons);
}

- (void)updateMouseDX:(int)dx dy:(int)dy buttons:(int)buttons {
    dosbox_inject_mouse(dx, dy, buttons);
}

#pragma mark - Speed

- (void)setSpeed:(DOSSpeedMode)mode {
    _speedMode = mode;
    // TODO: if running, dynamically change DOSBox cycles
}

- (void)setCustomCycles:(int)cycles {
    _customCycles = cycles;
}

- (DOSSpeedMode)getSpeed {
    return _speedMode;
}

#pragma mark - Manifest tracking

- (void)setDiskIsManifest:(int)drive isManifest:(BOOL)manifest {
    int idx = drive_index(drive);
    if (idx >= 0) _diskIsManifest[idx] = manifest;
}

- (BOOL)pollManifestWriteWarning {
    return _manifestWriteFired.exchange(false);
}

@end
