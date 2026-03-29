# DOSBox In-Process Restart: Static State Audit

DOSBox-staging was designed as a run-once-and-exit process. We run it
multiple times in the same iOS/Mac app process. This document tracks
all static/global state that must be managed across restart cycles.

Audited 2026-03-26. Covers all ~150 source files in our build.

## Architecture

DOSBox modules follow a consistent pattern:
- Static `std::unique_ptr<Module>` singleton
- `MODULE_Init()` creates via `std::make_unique`
- `MODULE_Destroy()` resets via `= {}`

`DOSBOX_InitModules()` calls all Init functions in order.
`DOSBOX_DestroyModules()` calls all Destroy functions in reverse order.
The callback system (`CALLBACK_Init()`) fully resets the handler table,
and all modules re-allocate fresh callback numbers in their Init().

This means **most** statics are safe. The problems are:
1. One-time init guards that prevent re-init
2. File-scope statics not owned by a module singleton
3. State that Init/Destroy don't touch

## Teardown Sequence (dosbox_bridge.cpp)

  dosbox_run() cleanup:
    1. HOSTIO_Destroy()         -- deletes HostIO instance
    2. DOSBOX_DestroyModules()  -- destroys all module singletons
    3. GFX_Destroy()            -- stops rendering, destroys mapper
    4. GFX_Quit()               -- destroys SDL renderer/window, SDL_Quit

  dosbox_init() re-init:
    1. DOSBOX_ClearShutdownRequest()
    2. Reset s_last_frame_time
    3. SDL_SetMainReady() + SDL_SetHint()
    4. Config creation
    5. GFX_InitSdl()            -- SDL_InitSubSystem()
    6. DOSBOX_InitModules()     -- recreates all module singletons
    7. HOSTIO_Init()            -- allocates new HostIO
    8. GFX_InitAndStartGui()    -- creates renderer/window


## Bridge Layer (our code in dosbox-ios/ and qxDOS/Bridge/)

### dosbox-ios/dosbox_bridge.cpp

  s_running (atomic<bool>)      Reset to false at end of dosbox_run()
                                and in all error paths. OK.

  s_frame_cb (function ptr)     Set fresh at top of dosbox_init(). OK.

  s_frame_ctx (void*)           Set fresh at top of dosbox_init(). OK.

  s_last_frame_time (uint64_t)  Reset to 0 at top of dosbox_init(). OK.

  tbi (mach_timebase_info)      Function-local, immutable OS constant. OK.

  s_conf_path (char*)           Freed in dosbox_run() and error paths. OK.

  s_cmdline (unique_ptr)        Reset in dosbox_run() and error paths. OK.

  s_host_dir (string)           Cleared in dosbox_run() and error paths. OK.

### dosbox-ios/int_e0_hostio.cpp

  s_hostio (HostIO*)            Heap-allocated in HOSTIO_Init(), deleted
                                in HOSTIO_Destroy(). Destructor cleans up
                                CALLBACK_HandlerObject and file handles. OK.

### qxDOS/Bridge/DOSEmulator.mm

  s_frame_pending (atomic<bool>)  May be stale on restart. Minor -- at
                                  worst drops one frame. Low priority.


## DOSBox Submodule -- FIXED (patched in our fork)

### dosbox.cpp: is_shutdown_requested (CRASH)
  Set true on quit, never cleared. Main loop exits immediately.
  Fix: Added DOSBOX_ClearShutdownRequest(), called in dosbox_init().

### cpu/cpu.cpp: Cpu::initialised (CRASH)
  One-time init guard. Skips all CPU initialization on restart.
  Fix: Reset to false in CPU_Destroy().

### shell/shell.cpp: is_shell_running (WRONG)
  Set true when shell starts, never cleared.
  Fix: Set false when shell loop exits.

### shell/autoexec.cpp: 6 file-scope statics (CRASH)
  autoexec_bat_utf8, autoexec_bat_bin, vfile_code_page,
  autoexec_has_echo_off, autoexec_variables, autoexec_lines.
  Never cleared. assert(!autoexec_bat_utf8) crashes on restart.
  Fix: All reset at top of AUTOEXEC_Init().

### misc/cross.cpp: cached_config_dir (WRONG)
  One-time guard caches config dir path, never re-reads.
  Fix: Clear at top of init_config_dir().

### gui/mapper.cpp: caps_lock_event, num_lock_event (CRASH)
  Raw pointers into events vector. Become dangling after
  MAPPER_Destroy() clears events. Dereferenced in next init.
  Fix: Set to nullptr before events.clear() in MAPPER_Destroy().


## DOSBox Submodule -- Safe (handled by Init/Destroy cycle)

### Module singletons (unique_ptr)
  All properly reset by their Destroy() functions:
  cpu_instance, memory_module, pic, timer, io_module, cmos_module,
  pci_interface, sblaster, gus, imfc, innovation, lpt_dac, cms,
  opl, ps1_synth, mpu401, ne2000, ipx, joystick, serial_ports,
  bios, ems_module, xms_module, dos_module, midi_instance,
  voodoo state, ide_controllers, autoexec_module, capture state.

### callback_number_t statics
  CALLBACK_Init() fully resets the Callback_Handlers[] array.
  CALLBACK_Allocate() scans from index 1, finds fresh slots.
  All modules re-allocate in their Init() functions, overwriting
  the old static values. SAFE.
  Files: bios_keyboard.cpp, int10.cpp, dos_misc.cpp, dos_tables.cpp,
  mouse.cpp, mouseif_dos_driver.cpp, reelmagic/driver.cpp, etc.

### Hardware state structs
  Owned by module singletons, reset when singleton is destroyed
  and recreated: SbInfo, Mpu, VGA state, DMA channels, PIC state,
  timer channels, keyboard buffer, mouse state, etc.

### Mapper data structures
  events, buttons, bindgroups, handlergroup, all_binds, holdlist
  all properly cleared by MAPPER_Destroy(). Re-populated by
  MAPPER_BindKeys() during next init.

### first_shell (shell.cpp)
  Set to nullptr after delete in SHELL_InitAndRun(). SAFE.

### control (global Config pointer)
  Reset to {} at end of DOSBOX_DestroyModules(). SAFE.


## DOSBox Submodule -- Known Minor Issues (not fixed)

### Log/warning deduplication (COSMETIC)
  ~30 `static bool already_warned` and `static bool first_time`
  flags across keyboard.cpp, mouse*.cpp, intel8042.cpp, xms.cpp,
  bios_pci.cpp, virtualbox.cpp, vga_gfx.cpp, etc.
  Effect: Some first-run log messages suppressed on restart.

### sdl_gui.cpp caching statics (COSMETIC)
  last_presentation_mode, last_vsync_enabled (line 428-429)
  last_width, last_height (line 2225)
  Video mode/refresh rate caches in is_changed() (line 474-480)
  Effect: May skip first reconfiguration log or setting update.
  Cleared implicitly when renderer is recreated.

### titlebar.cpp state (COSMETIC)
  config struct, state struct, last_title_str cache.
  Effect: Stale titlebar text for first frame, then overwritten.

### render.cpp caches (COSMETIC)
  curr_image_adjustment_settings, current_stretch_axis.
  Effect: Stale for first frame, then overwritten by render reset.

### mapper.cpp timing (COSMETIC)
  joystick_subsystem_init_timestamp, prev_titlebar cache.
  Effect: Joystick init may use stale timestamp. Harmless.

### misc/messages.cpp: dictionary_english (COSMETIC)
  Never cleared. MSG_Add finds existing keys and logs
  "Duplicate text" warnings. No crash, just log spam.
  Could clear in a future messages reset function.

### dos_windows.cpp flags (WRONG, minor)
  is_windows_started, is_enhanced_mode. Not reset.
  Effect: DOSBox thinks Windows was running. Unlikely to
  matter since no Windows programs run in FreeDOS.

### dos_execute.cpp: psp_to_canonical_map (WRONG, minor)
  Maps PSP segments to program names. Never cleared.
  Effect: Stale program names in titlebar. Cosmetic.

### soundblaster.cpp: asp_init_in_progress (WRONG, minor)
  Not reset in SBLASTER_Destroy(). Only matters if
  destroyed mid-ASP-init. Extremely unlikely.


## DOSBox Submodule -- Not Applicable

### loguru atexit registration
  Only called once from DOSBox's main.cpp which we don't use.
  Our bridge doesn't call loguru::init(). SAFE.

### PDCurses atexit registration
  PDCurses not used (C_DEBUGGER=0). SAFE.

### SDL_Quit / reinit
  GFX_Quit() calls SDL_Quit(). GFX_InitSdl() calls
  SDL_InitSubSystem() which works after SDL_Quit(). SAFE.
