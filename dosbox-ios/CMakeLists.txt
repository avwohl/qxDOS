cmake_minimum_required(VERSION 3.21)
project(dosbox-ios LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 17)

# DOSBox source root
set(DB ${CMAKE_CURRENT_SOURCE_DIR}/../dosbox-staging/src)

# Our custom dosbox_config.h lives in dosbox-ios/
# It must be found BEFORE any DOSBox headers
set(CONFIG_DIR ${CMAKE_CURRENT_SOURCE_DIR})

# ============================================================================
# Find dependencies
# ============================================================================

# SDL2 — required
find_package(SDL2 REQUIRED)

# SDL2_image — required for image loading
find_package(SDL2_image QUIET)
if(NOT SDL2_image_FOUND)
    # Try pkg-config as fallback
    find_package(PkgConfig)
    if(PkgConfig_FOUND)
        pkg_check_modules(SDL2_IMAGE SDL2_image)
    endif()
endif()

# libpng — required for capture/screenshots
find_package(PNG QUIET)

# speexdsp — required for audio resampling
find_package(PkgConfig)
if(PkgConfig_FOUND)
    pkg_check_modules(SPEEXDSP speexdsp)
endif()

# iir — audio filtering (try pkg-config, cmake, or header-only)
if(PkgConfig_FOUND)
    pkg_check_modules(IIR iir)
endif()

# opusfile — for opus audio decoding
if(PkgConfig_FOUND)
    pkg_check_modules(OPUSFILE opusfile)
endif()

# ============================================================================
# Vendored libraries (from dosbox-staging/src/libs/)
# ============================================================================

# Common include directories that most vendored libs need
set(COMMON_INCLUDES
    ${CONFIG_DIR}             # dosbox_config.h
    ${CONFIG_DIR}/stubs       # stub headers for missing libraries
    ${DB}                     # dosbox.h and other top-level headers
)

# Loguru (logging)
add_library(loguru STATIC ${DB}/libs/loguru/loguru.cpp)
target_include_directories(loguru PUBLIC ${DB}/libs/loguru ${COMMON_INCLUDES})
target_compile_definitions(loguru PUBLIC LOGURU_USE_FMTLIB=0)

# Audio decoders
add_library(decoders STATIC
    ${DB}/libs/decoders/flac.c
    ${DB}/libs/decoders/mp3.cpp
    ${DB}/libs/decoders/mp3_seek_table.cpp
    ${DB}/libs/decoders/opus.cpp
    ${DB}/libs/decoders/SDL_sound.c
    ${DB}/libs/decoders/vorbis.c
    ${DB}/libs/decoders/wav.c
)
target_include_directories(decoders PUBLIC
    ${COMMON_INCLUDES}
    ${DB}/libs/decoders
    ${DB}/libs
)
target_link_libraries(decoders PRIVATE SDL2::SDL2)
if(OPUSFILE_FOUND)
    target_link_libraries(decoders PRIVATE ${OPUSFILE_LIBRARIES})
    target_include_directories(decoders PRIVATE ${OPUSFILE_INCLUDE_DIRS})
endif()

# Nuked OPL3
add_library(nuked STATIC ${DB}/libs/nuked/opl3.c)
target_include_directories(nuked PUBLIC ${DB}/libs/nuked)

# ESFMu
add_library(esfmu STATIC
    ${DB}/libs/ESFMu/esfm.c
    ${DB}/libs/ESFMu/esfm_registers.c
)
target_include_directories(esfmu PUBLIC ${DB}/libs/ESFMu)

# ReSIDfp (SID chip)
file(GLOB RESIDFP_SRC ${DB}/libs/residfp/*.cpp ${DB}/libs/residfp/resample/*.cpp)
add_library(residfp STATIC ${RESIDFP_SRC})
target_include_directories(residfp PUBLIC ${DB}/libs/residfp)

# TAL-Chorus
add_library(talchorus STATIC ${DB}/libs/tal-chorus/Lfo.cpp)
target_include_directories(talchorus PUBLIC ${DB}/libs/tal-chorus ${COMMON_INCLUDES})

# whereami
add_library(whereami STATIC ${DB}/libs/whereami/whereami.c)
target_include_directories(whereami PUBLIC ${DB}/libs/whereami)

# YM7128B
add_library(ym7128b STATIC ${DB}/libs/YM7128B_emu/YM7128B_emu.c)
target_include_directories(ym7128b PUBLIC ${DB}/libs/YM7128B_emu)

# ZMBV (needs zlib)
find_package(ZLIB REQUIRED)
add_library(zmbv STATIC ${DB}/libs/zmbv/zmbv.cpp)
target_include_directories(zmbv PUBLIC ${DB}/libs/zmbv ${COMMON_INCLUDES})
target_link_libraries(zmbv PRIVATE ZLIB::ZLIB)

# ============================================================================
# DOSBox core static library
# ============================================================================

add_library(dosbox-core STATIC
    # Top-level
    ${DB}/dosbox.cpp

    # Audio
    ${DB}/audio/clap/event_list.cpp
    ${DB}/audio/clap/library.cpp
    ${DB}/audio/clap/plugin.cpp
    ${DB}/audio/clap/plugin_manager.cpp
    ${DB}/audio/disk_noise.cpp
    ${DB}/audio/compressor.cpp
    ${DB}/audio/envelope.cpp
    ${DB}/audio/mixer.cpp
    ${DB}/audio/noise_gate.cpp
    ${DB}/audio/opl_capture.cpp

    # Capture
    ${DB}/capture/capture.cpp
    ${DB}/capture/capture_audio.cpp
    ${DB}/capture/capture_midi.cpp
    ${DB}/capture/capture_video.cpp
    ${DB}/capture/image/image_capturer.cpp
    ${DB}/capture/image/image_saver.cpp
    ${DB}/capture/image/image_scaler.cpp
    ${DB}/capture/image/png_writer.cpp

    # Config
    ${DB}/config/config.cpp
    ${DB}/config/setup.cpp

    # CPU
    ${DB}/cpu/callback.cpp
    ${DB}/cpu/core_dyn_x86.cpp
    ${DB}/cpu/core_dynrec.cpp
    ${DB}/cpu/core_full.cpp
    ${DB}/cpu/core_normal.cpp
    ${DB}/cpu/core_prefetch.cpp
    ${DB}/cpu/core_simple.cpp
    ${DB}/cpu/cpu.cpp
    ${DB}/cpu/flags.cpp
    ${DB}/cpu/mmx.cpp
    ${DB}/cpu/modrm.cpp
    ${DB}/cpu/paging.cpp

    # FPU
    ${DB}/fpu/fpu.cpp

    # DOS
    ${DB}/dos/cdrom.cpp
    ${DB}/dos/cdrom_image.cpp
    ${DB}/dos/dos.cpp
    ${DB}/dos/dos_classes.cpp
    ${DB}/dos/dos_code_page.cpp
    ${DB}/dos/dos_devices.cpp
    ${DB}/dos/dos_execute.cpp
    ${DB}/dos/dos_files.cpp
    ${DB}/dos/dos_ioctl.cpp
    ${DB}/dos/dos_keyboard_layout.cpp
    ${DB}/dos/dos_locale.cpp
    ${DB}/dos/dos_locale_data.cpp
    ${DB}/dos/dos_memory.cpp
    ${DB}/dos/dos_misc.cpp
    ${DB}/dos/dos_mscdex.cpp
    ${DB}/dos/dos_programs.cpp
    ${DB}/dos/dos_tables.cpp
    ${DB}/dos/dos_windows.cpp
    ${DB}/dos/drive_cache.cpp
    ${DB}/dos/drive_fat.cpp
    ${DB}/dos/drive_iso.cpp
    ${DB}/dos/drive_local.cpp
    ${DB}/dos/drive_overlay.cpp
    ${DB}/dos/drive_virtual.cpp
    ${DB}/dos/drives.cpp
    ${DB}/dos/programs.cpp
    ${DB}/dos/programs/attrib.cpp
    ${DB}/dos/programs/autotype.cpp
    ${DB}/dos/programs/biostest.cpp
    ${DB}/dos/programs/boot.cpp
    ${DB}/dos/programs/choice.cpp
    ${DB}/dos/programs/clip.cpp
    ${DB}/dos/programs/help.cpp
    ${DB}/dos/programs/makeimg.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/imgmount_fix.cpp
    ${DB}/dos/programs/intro.cpp
    ${DB}/dos/programs/keyb.cpp
    ${DB}/dos/programs/loadfix.cpp
    ${DB}/dos/programs/loadrom.cpp
    ${DB}/dos/programs/ls.cpp
    ${DB}/dos/programs/mem.cpp
    ${DB}/dos/programs/mixer.cpp
    ${DB}/dos/programs/mode.cpp
    ${DB}/dos/programs/more.cpp
    ${DB}/dos/programs/more_output.cpp
    ${DB}/dos/programs/mount.cpp
    ${DB}/dos/programs/mount_common.cpp
    ${DB}/dos/programs/mouse.cpp
    ${DB}/dos/programs/mousectl.cpp
    ${DB}/dos/programs/move.cpp
    ${DB}/dos/programs/rescan.cpp
    ${DB}/dos/programs/serial.cpp
    ${DB}/dos/programs/setver.cpp
    ${DB}/dos/programs/showpic.cpp
    ${DB}/dos/programs/subst.cpp
    ${DB}/dos/programs/tree.cpp

    # GUI
    ${DB}/gui/auto_image_adjustments.cpp
    ${DB}/gui/clipboard.cpp
    ${DB}/gui/mapper.cpp
    ${DB}/gui/sdl_gui.cpp
    ${DB}/gui/shader_manager.cpp
    ${DB}/gui/titlebar.cpp
    ${DB}/gui/render/deinterlacer.cpp
    ${DB}/gui/render/render.cpp
    ${DB}/gui/render/scaler/scalers.cpp
    ${DB}/gui/render/sdl_renderer.cpp

    # Hardware
    ${DB}/hardware/audio/adlib_gold.cpp
    ${DB}/hardware/audio/cms.cpp
    ${DB}/hardware/audio/covox.cpp
    ${DB}/hardware/audio/disney.cpp
    ${DB}/hardware/audio/gus.cpp
    ${DB}/hardware/audio/imfc.cpp
    ${DB}/hardware/audio/innovation.cpp
    ${DB}/hardware/audio/lpt_dac.cpp
    ${DB}/hardware/audio/mame/saa1099.cpp
    ${DB}/hardware/audio/mame/sn76496.cpp
    ${DB}/hardware/audio/mpu401.cpp
    ${DB}/hardware/audio/opl.cpp
    ${DB}/hardware/audio/pcspeaker.cpp
    ${DB}/hardware/audio/pcspeaker_discrete.cpp
    ${DB}/hardware/audio/pcspeaker_impulse.cpp
    ${DB}/hardware/audio/ps1audio.cpp
    ${DB}/hardware/audio/soundblaster.cpp
    ${DB}/hardware/audio/speaker.cpp
    ${DB}/hardware/audio/ston1_dac.cpp
    ${DB}/hardware/audio/tandy_sound.cpp
    ${DB}/hardware/input/intel8042.cpp
    ${DB}/hardware/input/intel8255.cpp
    ${DB}/hardware/input/joystick.cpp
    ${DB}/hardware/input/keyboard.cpp
    ${DB}/hardware/input/keyboard_scancodes.cpp
    ${DB}/hardware/input/mouse.cpp
    ${DB}/hardware/input/mouse_common.cpp
    ${DB}/hardware/input/mouse_config.cpp
    ${DB}/hardware/input/mouse_interfaces.cpp
    ${DB}/hardware/input/mouse_manymouse.cpp
    ${DB}/hardware/input/mouseif_dos_driver.cpp
    ${DB}/hardware/input/mouseif_dos_driver_state.cpp
    ${DB}/hardware/input/mouseif_ps2_bios.cpp
    ${DB}/hardware/input/mouseif_virtual_machines.cpp
    # Network — NE2000 + IPX included for DOS networking
    ${DB}/hardware/network/ipx.cpp
    ${DB}/hardware/network/ipxserver.cpp
    ${DB}/hardware/network/ne2000.cpp
    # Serial port — all included (enet.h clock_gettime fix via compile definition)
    ${DB}/hardware/serialport/misc_util.cpp
    ${DB}/hardware/serialport/nullmodem.cpp
    ${DB}/hardware/serialport/serialdummy.cpp
    ${DB}/hardware/serialport/serialmouse.cpp
    ${DB}/hardware/serialport/serialport.cpp
    ${DB}/hardware/serialport/softmodem.cpp
    ${DB}/hardware/video/reelmagic/driver.cpp
    ${DB}/hardware/video/reelmagic/player.cpp
    ${DB}/hardware/video/reelmagic/video_mixer.cpp
    ${DB}/hardware/video/vga.cpp
    ${DB}/hardware/video/vga_attr.cpp
    ${DB}/hardware/video/vga_crtc.cpp
    ${DB}/hardware/video/vga_dac.cpp
    ${DB}/hardware/video/vga_draw.cpp
    ${DB}/hardware/video/vga_gfx.cpp
    ${DB}/hardware/video/vga_memory.cpp
    ${DB}/hardware/video/vga_misc.cpp
    ${DB}/hardware/video/vga_other.cpp
    ${DB}/hardware/video/vga_paradise.cpp
    ${DB}/hardware/video/vga_s3.cpp
    ${DB}/hardware/video/vga_seq.cpp
    ${DB}/hardware/video/vga_tseng.cpp
    ${DB}/hardware/video/vga_xga.cpp
    ${DB}/hardware/video/voodoo.cpp
    ${DB}/hardware/cmos.cpp
    ${DB}/hardware/dma.cpp
    ${DB}/hardware/ide.cpp
    ${DB}/hardware/memory.cpp
    ${DB}/hardware/pci_bus.cpp
    ${DB}/hardware/pic.cpp
    ${DB}/hardware/port.cpp
    ${DB}/hardware/port_containers.cpp
    ${DB}/hardware/timer.cpp
    ${DB}/hardware/virtualbox.cpp
    ${DB}/hardware/vmware.cpp

    # Interrupts / BIOS
    ${DB}/ints/bios.cpp
    ${DB}/ints/bios_disk.cpp
    ${DB}/ints/bios_keyboard.cpp
    ${DB}/ints/bios_pci.cpp
    ${DB}/ints/ems.cpp
    ${DB}/ints/int10.cpp
    ${DB}/ints/int10_char.cpp
    ${DB}/ints/int10_memory.cpp
    ${DB}/ints/int10_misc.cpp
    ${DB}/ints/int10_modes.cpp
    ${DB}/ints/int10_pal.cpp
    ${DB}/ints/int10_put_pixel.cpp
    ${DB}/ints/int10_vesa.cpp
    ${DB}/ints/int10_video_state.cpp
    ${DB}/ints/int10_vptable.cpp
    ${DB}/ints/xms.cpp

    # MIDI (FluidSynth stubbed, CoreAudio/ALSA/Win32 disabled via config)
    ${CONFIG_DIR}/stubs/fluidsynth_stub.cpp
    ${DB}/midi/midi.cpp
    ${DB}/midi/mt32.cpp
    ${DB}/midi/mt32_lasynth_model.cpp
    ${DB}/midi/soundcanvas.cpp

    # Misc
    ${DB}/misc/ansi_code_markup.cpp
    ${DB}/misc/console.cpp
    ${DB}/misc/cross.cpp
    ${DB}/misc/dos_rwops.cpp
    ${DB}/misc/fs_utils.cpp
    ${DB}/misc/fs_utils_posix.cpp
    ${DB}/misc/help_util.cpp
    ${DB}/misc/host_locale.cpp
    ${DB}/misc/host_locale_posix.cpp
    ${DB}/misc/host_locale_macos.cpp
    ${DB}/misc/image_decoder.cpp
    ${DB}/misc/iso_locale_codes.cpp
    ${DB}/misc/messages.cpp
    ${DB}/misc/messages_adjust.cpp
    ${DB}/misc/messages_po_entry.cpp
    ${DB}/misc/rwqueue.cpp
    ${DB}/misc/support.cpp
    ${DB}/misc/unicode.cpp
    ${DB}/misc/unicode_encodings.cpp
    ${DB}/misc/video.cpp

    # Network — ethernet + NE2000 (SLIRP backend stubbed, no libslirp on iOS)
    ${DB}/network/ethernet.cpp
    # ${DB}/network/ethernet_slirp.cpp  # replaced by stub below
    ${CONFIG_DIR}/stubs/ethernet_slirp_stub.cpp
    ${CONFIG_DIR}/stubs/webserver_stub.cpp

    # Shell
    ${DB}/shell/autoexec.cpp
    ${DB}/shell/command_line.cpp
    ${DB}/shell/file_reader.cpp
    ${DB}/shell/shell.cpp
    ${DB}/shell/shell_batch.cpp
    ${DB}/shell/shell_cmds.cpp
    ${DB}/shell/shell_history.cpp
    ${DB}/shell/shell_misc.cpp

    # Utils
    ${DB}/utils/env_utils.cpp
    ${DB}/utils/string_utils.cpp
)

# Platform-specific exclusions: remove Windows/Linux-only files
# These are listed in add_library but we skip them by marking as header-only
set_source_files_properties(
    ${DB}/dos/cdrom_ioctl_linux.cpp
    ${DB}/dos/cdrom_win32.cpp
    ${DB}/misc/fs_utils_win32.cpp
    ${DB}/misc/host_locale_win32.cpp
    PROPERTIES HEADER_FILE_ONLY TRUE
)

# OpenGL renderer — excluded (C_OPENGL=0)
# ${DB}/gui/render/opengl_renderer.cpp is NOT in the source list

# Include directories — our config header comes FIRST
target_include_directories(dosbox-core PUBLIC
    ${CONFIG_DIR}                          # dosbox_config.h (ours)
    ${CONFIG_DIR}/stubs                    # stub headers for missing libs
    ${DB}                                  # dosbox.h and top-level headers
    ${DB}/..                               # for includes like "src/foo.h"
)

# DOSBox internal includes (the source tree uses relative paths)
target_include_directories(dosbox-core PRIVATE
    ${DB}/audio
    ${DB}/capture
    ${DB}/config
    ${DB}/cpu
    ${DB}/dos
    ${DB}/fpu
    ${DB}/gui
    ${DB}/gui/render
    ${DB}/hardware
    ${DB}/hardware/audio
    ${DB}/hardware/input
    ${DB}/hardware/network
    ${DB}/hardware/serialport
    ${DB}/hardware/video
    ${DB}/ints
    ${DB}/midi
    ${DB}/misc
    ${DB}/network
    ${DB}/shell
    ${DB}/utils
    ${DB}/libs
    ${DB}/libs/decoders
    ${DB}/libs/loguru
    ${DB}/libs/nuked
    ${DB}/libs/ESFMu
    ${DB}/libs/residfp
    ${DB}/libs/tal-chorus
    ${DB}/libs/whereami
    ${DB}/libs/YM7128B_emu
    ${DB}/libs/zmbv
    ${DB}/libs/simde
    ${DB}/libs/stb_truetype
    ${DB}/libs/json
    ${DB}/libs/base64
    ${DB}/libs/http
    ${DB}/libs/clap
    ${DB}/libs/PL_MPEG
)

# Compile definitions
target_compile_definitions(dosbox-core PRIVATE
    # Feature flags (must match dosbox_config.h)
    C_OPENGL=0
    C_DEBUGGER=0
    C_HEAVY_DEBUGGER=0
    C_MT32EMU=0
    C_MANYMOUSE=0
)

# Platform-specific settings
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    # Fix enet.h clock_gettime conflict on iOS:
    # enet.h defines its own clock_gettime when __APPLE__ &&
    # __MAC_OS_X_VERSION_MIN_REQUIRED < 101200.  On iOS this macro is
    # undefined (evaluates to 0), triggering the conflict.
    target_compile_definitions(dosbox-core PRIVATE
        __MAC_OS_X_VERSION_MIN_REQUIRED=101500
    )
    # iOS: no DYNREC — pthread_jit_write_protect_np is macOS-only.
    # Falls back to interpretive core (slower but correct).
    target_compile_definitions(dosbox-core PRIVATE C_DYNREC=0)
else()
    # macOS: DYNREC with pthread_jit_write_protect_np
    target_compile_definitions(dosbox-core PRIVATE
        HAVE_PTHREAD_WRITE_PROTECT_NP
    )
endif()

# C++20 for DOSBox, C17 for C files
set_target_properties(dosbox-core PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    C_STANDARD 17
)

# Link vendored libraries
target_link_libraries(dosbox-core PRIVATE
    loguru
    decoders
    nuked
    esfmu
    residfp
    talchorus
    whereami
    ym7128b
    zmbv
)

# Link SDL2
target_link_libraries(dosbox-core PUBLIC SDL2::SDL2)
if(TARGET SDL2_image::SDL2_image)
    target_link_libraries(dosbox-core PUBLIC SDL2_image::SDL2_image)
elseif(SDL2_IMAGE_FOUND)
    target_link_libraries(dosbox-core PUBLIC ${SDL2_IMAGE_LIBRARIES})
    target_include_directories(dosbox-core PUBLIC ${SDL2_IMAGE_INCLUDE_DIRS})
endif()

# Link PNG
if(PNG_FOUND)
    target_link_libraries(dosbox-core PRIVATE PNG::PNG)
endif()

# Link speexdsp
if(SPEEXDSP_FOUND)
    target_link_libraries(dosbox-core PRIVATE ${SPEEXDSP_LIBRARIES})
    target_include_directories(dosbox-core PRIVATE ${SPEEXDSP_INCLUDE_DIRS})
endif()

# Link iir
if(IIR_FOUND)
    target_link_libraries(dosbox-core PRIVATE ${IIR_LIBRARIES})
    target_include_directories(dosbox-core PRIVATE ${IIR_INCLUDE_DIRS})
endif()

# Apple frameworks (iOS)
if(APPLE)
    find_library(COREAUDIO_FW CoreAudio)
    find_library(AUDIOTOOLBOX_FW AudioToolbox)
    find_library(COREFOUNDATION_FW CoreFoundation)
    target_link_libraries(dosbox-core PUBLIC
        ${COREAUDIO_FW}
        ${AUDIOTOOLBOX_FW}
        ${COREFOUNDATION_FW}
    )
endif()

# Header-only libraries (asio for serial port networking)
# asio is header-only and platform-independent, so reuse host install
find_path(ASIO_INCLUDE_DIR asio.hpp
    HINTS /opt/homebrew/include
    NO_CMAKE_FIND_ROOT_PATH  # search host paths even when cross-compiling
)
if(ASIO_INCLUDE_DIR)
    target_include_directories(dosbox-core SYSTEM PRIVATE ${ASIO_INCLUDE_DIR})
endif()

# Suppress warnings in vendored code
target_compile_options(dosbox-core PRIVATE
    -Wno-unused-parameter
    -Wno-sign-compare
    -Wno-missing-field-initializers
)

# ============================================================================
# Bridge library (our iOS integration)
# ============================================================================

add_library(dosbox-bridge STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/dosbox_bridge.cpp
)

target_include_directories(dosbox-bridge PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(dosbox-bridge PUBLIC dosbox-core)

# ============================================================================
# macOS test harness (not built for iOS cross-compile)
# ============================================================================

if(NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
    add_executable(test_boot ${CMAKE_CURRENT_SOURCE_DIR}/test_boot.cpp)
    target_link_libraries(test_boot PRIVATE dosbox-bridge)
    target_include_directories(test_boot PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_directories(test_boot PRIVATE /opt/homebrew/lib)
endif()
