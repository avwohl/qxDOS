/*
 * dosbox_config.h - Hand-crafted configuration for iOS ARM64 build
 *
 * This replaces the CMake-generated dosbox_config.h.in.cmake.
 * Configured for: iOS ARM64, minimal dependencies, no debugger.
 */

#ifndef DOSBOX_DOSBOX_CONFIG_H
#define DOSBOX_DOSBOX_CONFIG_H

// Version
#define DOSBOX_VERSION "0.83.0-ios"
#define BUILD_GIT_HASH "ios"

// Operating System — iOS is Darwin-based like macOS
// We don't define MACOSX because some code paths assume desktop macOS
// #define MACOSX

// CPU and FPU emulation
#define C_TARGET_CPU_ARM  1
#define C_TARGET_CPU_X86  0
#define C_UNALIGNED_MEMORY 0  // ARM needs aligned access
#define C_PER_PAGE_W_OR_X 1  // Apple Silicon requires W^X

// Dynamic recompiler
// On macOS: DYNREC=1 with pthread_jit_write_protect_np (macOS 11+)
// On iOS: DYNREC=0 — pthread_jit_write_protect_np is macOS-only.
//   iOS 17.4+ has pthread_jit_write_with_callback_np but needs refactoring.
//   C_DYNREC is overridden to 0 by CMakeLists.txt for iOS builds.
#define C_DYNAMIC_X86 0
#ifndef C_DYNREC
#define C_DYNREC      1
#endif
#define C_FPU_X86     0
#define C_CORE_INLINE 1

// Features — minimal for iOS
#define C_OPENGL         0  // Use SDL renderer, not OpenGL (simpler)
#define C_DEBUGGER       0
#define C_HEAVY_DEBUGGER 0
#define C_MT32EMU        0  // No MT-32 (reduces dependencies)
#define C_MANYMOUSE      0  // No multi-mouse on iOS
#define SUPPORT_XINPUT2  0

// Apple frameworks — disable CoreAudio MIDI for now (simplifies build)
#define C_COREAUDIO      0
#define C_COREMIDI       0
#define C_COREFOUNDATION 1
#define C_CORESERVICES   0

// Data directory fallback
#define CUSTOM_DATADIR "/usr/local/share"

// No ALSA on iOS
#define C_ALSA 0

// System zlib — do NOT define C_SYSTEM_ZLIB_NG; use regular zlib
// #define C_SYSTEM_ZLIB_NG 0

// POSIX functions available on iOS (Darwin)
#define HAVE_FD_ZERO
#define HAVE_CLOCK_GETTIME
#define HAVE_BUILTIN_AVAILABLE
// Use sys_icache_invalidate on iOS instead of __builtin___clear_cache
// (the builtin emits ___clear_cache which isn't in the iOS runtime)
// #define HAVE_BUILTIN_CLEAR_CACHE
#define HAVE_MPROTECT
#define HAVE_MMAP
#define HAVE_MAP_JIT
// pthread_jit_write_protect_np: macOS 11+ only (unavailable on iOS)
// HAVE_PTHREAD_WRITE_PROTECT_NP defined by CMakeLists.txt for macOS only
#define HAVE_SYS_ICACHE_INVALIDATE
#define HAVE_PTHREAD_SETNAME_NP
#define HAVE_SETPRIORITY
#define HAVE_STRNLEN
#define HAVE_STRUCT_DIRENT_D_TYPE

// Headers available on iOS
#define HAVE_LIBGEN_H
#define HAVE_NETINET_IN_H
#define HAVE_STDLIB_H
#define HAVE_STRINGS_H
#define HAVE_SYS_SOCKET_H
#define HAVE_SYS_TYPES_H
// #define HAVE_SYS_XATTR_H  // available but not needed

#endif // DOSBOX_DOSBOX_CONFIG_H
