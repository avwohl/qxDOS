/*
 * Emu88Emulator.h - Objective-C bridge for the emu88 hardware backend
 *
 * emu88 is the custom 8088/286/386+FPU+DPMI interpreter vendored from the
 * archived iosFreeDOS project. This bridge mirrors the surface area of
 * DOSEmulator.h so EmulatorViewModel can drive both backends with a single
 * conformance and a single frame-rendering pipeline:
 *
 *   - Frames are composited to RGBA in C++ inside Emu88Emulator.mm and
 *     delivered through the same `emulatorFrameReady:width:height:` selector
 *     that DOSEmulator already uses.
 *   - Networking flows through Emu88SlirpNet, which sits on top of the same
 *     statically-linked libslirp NAT instance that DOSBox uses today.
 *   - Disks are mmap-backed for large HDD images.
 */

#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

// CPU type
typedef NS_ENUM(NSInteger, Emu88CpuType) {
    Emu88Cpu8088 NS_SWIFT_NAME(i8088) = 0,
    Emu88Cpu286  NS_SWIFT_NAME(i286)  = 1,
    Emu88Cpu386  NS_SWIFT_NAME(i386)  = 2,
};

// Display adapter
typedef NS_ENUM(NSInteger, Emu88DisplayAdapter) {
    Emu88DisplayCGA      NS_SWIFT_NAME(cga)      = 0,
    Emu88DisplayMDA      NS_SWIFT_NAME(mda)      = 1,
    Emu88DisplayHercules NS_SWIFT_NAME(hercules) = 2,
    Emu88DisplayCGAMDA   NS_SWIFT_NAME(cgaMDA)   = 3,
    Emu88DisplayEGA      NS_SWIFT_NAME(ega)      = 4,
    Emu88DisplayVGA      NS_SWIFT_NAME(vga)      = 5,
};

// Speed modes
typedef NS_ENUM(NSInteger, Emu88SpeedMode) {
    Emu88SpeedFull    NS_SWIFT_NAME(full)    = 0,
    Emu88SpeedPC      NS_SWIFT_NAME(pc)      = 1,
    Emu88SpeedAT      NS_SWIFT_NAME(at)      = 2,
    Emu88Speed386SX   NS_SWIFT_NAME(i386sx)  = 3,
    Emu88Speed386DX   NS_SWIFT_NAME(i386dx)  = 4,
    Emu88Speed486DX2  NS_SWIFT_NAME(i486dx2) = 5,
};

// Controlify mode
typedef NS_ENUM(NSInteger, Emu88ControlifyMode) {
    Emu88ControlifyOff      NS_SWIFT_NAME(off)      = 0,
    Emu88ControlifyOneChar  NS_SWIFT_NAME(oneChar)  = 1,
    Emu88ControlifySticky   NS_SWIFT_NAME(sticky)   = 2,
};

@protocol Emu88EmulatorDelegate <NSObject>
@optional
/// Composited RGBA frame ready (same shape as DOSEmulatorDelegate so
/// EmulatorViewModel can implement the rendering path once.)
- (void)emulatorFrameReady:(NSData *)pixels width:(int)width height:(int)height;
/// Emulator is waiting for keyboard input.
- (void)emulatorDidRequestInput;
/// Emulator stopped (CPU halted, stop() called, etc.).
- (void)emulatorDidExit;
@end

@interface Emu88Emulator : NSObject

@property (weak, nonatomic) id<Emu88EmulatorDelegate> delegate;
@property (readonly, nonatomic) BOOL isRunning;
@property (readonly, nonatomic) BOOL isWaitingForInput;

- (instancetype)init;

// Configuration (call before start)
- (void)setCpuType:(Emu88CpuType)type;
- (void)setDisplayAdapter:(Emu88DisplayAdapter)adapter;
- (void)setMouseEnabled:(BOOL)enabled;
- (void)setSpeakerEnabled:(BOOL)enabled;

// Disk management — mmap-backed when loaded from a file path
- (BOOL)loadDisk:(int)drive fromPath:(NSString *)path;
- (BOOL)loadDisk:(int)drive fromData:(NSData *)data;
- (BOOL)isDiskLoaded:(int)drive;
- (nullable NSData *)getDiskData:(int)drive;
- (BOOL)saveDisk:(int)drive toPath:(NSString *)path;
- (uint64_t)diskSize:(int)drive;
- (int)loadISO:(NSString *)path;

/// Flush mmap-backed disks via msync(MS_ASYNC). No-op for heap-backed disks.
- (void)syncDisks;

// Execution
- (void)startWithBootDrive:(int)drive;
- (void)stop;
- (void)reset;

// Input
- (void)sendCharacter:(unichar)ch;
- (void)sendScancode:(uint8_t)ascii scancode:(uint8_t)scancode;
- (void)sendScancodePress:(uint8_t)scancode;
- (void)sendScancodeRelease:(uint8_t)scancode;
- (void)updateMouseX:(int)x y:(int)y buttons:(int)buttons;

// Speed
- (void)setSpeed:(Emu88SpeedMode)mode;
- (Emu88SpeedMode)getSpeed;

// Controlify
- (void)setControlify:(Emu88ControlifyMode)mode;
- (Emu88ControlifyMode)getControlify;

// Manifest disk tracking
- (void)setDiskIsManifest:(int)drive isManifest:(BOOL)manifest;
- (BOOL)pollManifestWriteWarning;

@end

NS_ASSUME_NONNULL_END
