/*
 * DOSEmulator.h - Objective-C bridge for DOSBox emulator
 *
 * Same public API as iosFreeDOS v1, but backed by DOSBox-staging
 * instead of the custom emu88 CPU emulator.
 */

#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

// Machine type (maps to DOSBox "machine" config)
typedef NS_ENUM(NSInteger, DOSMachineType) {
    DOSMachineVGA    NS_SWIFT_NAME(vga) = 0,
    DOSMachineEGA    NS_SWIFT_NAME(ega) = 1,
    DOSMachineCGA    NS_SWIFT_NAME(cga) = 2,
    DOSMachineTandy  NS_SWIFT_NAME(tandy) = 3,
    DOSMachineHercules NS_SWIFT_NAME(hercules) = 4,
    DOSMachineSVGA   NS_SWIFT_NAME(svga) = 5
};

// Speed / cycles mode
typedef NS_ENUM(NSInteger, DOSSpeedMode) {
    DOSSpeedMax     NS_SWIFT_NAME(max) = 0,     // cycles=max
    DOSSpeed3000    NS_SWIFT_NAME(slow) = 1,     // 3000 cycles (XT-class)
    DOSSpeed8000    NS_SWIFT_NAME(medium) = 2,   // 8000 cycles (AT-class)
    DOSSpeed20000   NS_SWIFT_NAME(fast) = 3,     // 20000 cycles (386-class)
    DOSSpeed50000   NS_SWIFT_NAME(faster) = 4,   // 50000 cycles (486-class)
    DOSSpeedFixed   NS_SWIFT_NAME(fixed) = 5     // custom cycle count
};

@protocol DOSEmulatorDelegate <NSObject>
@optional
/// DOSBox rendered a new video frame (RGBA pixels, width x height)
- (void)emulatorFrameReady:(NSData*)pixels width:(int)width height:(int)height;
/// DOSBox is waiting for keyboard input (can animate cursor, etc.)
- (void)emulatorDidRequestInput;
/// DOSBox has exited (e.g., user typed EXIT or game quit)
- (void)emulatorDidExit;
@end

@interface DOSEmulator : NSObject

@property (weak, nonatomic) id<DOSEmulatorDelegate> delegate;
@property (readonly, nonatomic) BOOL isRunning;

- (instancetype)init;

// Configuration (call before start)
- (void)setMachineType:(DOSMachineType)type;
- (void)setMemoryMB:(int)mb;
- (void)setMouseEnabled:(BOOL)enabled;
- (void)setSpeakerEnabled:(BOOL)enabled;
- (void)setSoundBlasterEnabled:(BOOL)enabled;

// Disk management — DOSBox mounts images directly from paths
- (BOOL)loadDisk:(int)drive fromPath:(NSString*)path;
- (BOOL)loadDisk:(int)drive fromData:(NSData*)data;
- (BOOL)isDiskLoaded:(int)drive;
- (nullable NSData*)getDiskData:(int)drive;
- (BOOL)saveDisk:(int)drive toPath:(NSString*)path;
- (uint64_t)diskSize:(int)drive;
- (int)loadISO:(NSString*)path;

// Execution
- (void)startWithBootDrive:(int)drive;
- (void)stop;
- (void)reset;

// Input
- (void)sendCharacter:(unichar)ch;
- (void)sendScancode:(uint8_t)ascii scancode:(uint8_t)scancode;
- (void)updateMouseX:(int)x y:(int)y buttons:(int)buttons;

// Speed
- (void)setSpeed:(DOSSpeedMode)mode;
- (void)setCustomCycles:(int)cycles;
- (DOSSpeedMode)getSpeed;

// Manifest disk tracking (for catalog disk write warnings)
- (void)setDiskIsManifest:(int)drive isManifest:(BOOL)manifest;
- (BOOL)pollManifestWriteWarning;

@end

NS_ASSUME_NONNULL_END
