/*
 * dosbox_bridge.cpp - C bridge for launching DOSBox from iOS
 *
 * Implements the bridge functions by calling into DOSBox's initialization
 * and execution APIs.  This replaces DOSBox's main.cpp entry point.
 */

#include "dosbox_bridge.h"
#include "int_e0_hostio.h"

// Tell SDL we handle the application lifecycle ourselves (SwiftUI owns main)
#define SDL_MAIN_HANDLED
#include <SDL.h>

#include <TargetConditionals.h>
#include <dispatch/dispatch.h>
#include <pthread.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <cstring>
#include <string>
#include <vector>
#include <mach/mach_time.h>

// Simple debug log for the one-shot dosbox_start() path (used by test_boot)
#define BLOG(fmt, ...) fprintf(stderr, "[DOSBox Bridge] " fmt "\n", ##__VA_ARGS__)

// DOSBox headers
#include "dosbox.h"
#include "config/config.h"
#include "config/setup.h"
#include "gui/common.h"
#include "gui/mapper.h"
#include "gui/render/render.h"
#include "shell/shell.h"
#include "shell/command_line.h"
#include "dos/dos_locale.h"
#include "misc/cross.h"
#include "utils/checks.h"

CHECK_NARROWING();

// Global config pointer (declared extern in DOSBox)
extern std::unique_ptr<Config> control;

static std::atomic<bool> s_running{false};
static std::atomic<bool> s_atexit_registered{false};
static dosbox_frame_callback_t s_frame_cb = nullptr;
static void *s_frame_ctx = nullptr;

// Ensure DOSBox is torn down before C++ static destructors run.
// Without this, exit() triggers ~unique_ptr on module singletons
// whose dependencies (loguru, allocators) are already destroyed.
static void dosbox_atexit_cleanup()
{
    if (s_running.load()) {
        DOSBOX_RequestShutdown();
        // Give the background thread a moment to finish
        for (int i = 0; i < 100 && s_running.load(); ++i)
            usleep(10000); // 10ms, up to 1s total
    }
    // Always force-exit to prevent static destruction order crashes.
    // DOSBox's module unique_ptrs and global hash maps have no
    // guaranteed destruction order; letting them run causes crashes
    // (e.g. PIC destructor accesses already-destroyed IO maps).
    // The OS reclaims all process resources on exit anyway.
    _exit(0);
}

// Frame throttle — limit callbacks to ~30fps to avoid overwhelming the UI thread
static uint64_t s_last_frame_time = 0;
static constexpr uint64_t FRAME_MIN_INTERVAL_NS = 33000000; // ~30fps

// Called by DOSBox's SdlRenderer::PresentFrame() after each rendered frame.
// Converts ARGB8888 to RGBA8888 and delivers to the registered callback.
extern "C" void dosbox_on_frame_presented(
    const uint8_t *pixels, int width, int height, int pitch, uint32_t format)
{
    if (!s_frame_cb || !pixels || width <= 0 || height <= 0) return;

    // Throttle to avoid flooding the main thread
    uint64_t now = mach_absolute_time();
    static mach_timebase_info_data_t tbi = {};
    if (tbi.denom == 0) mach_timebase_info(&tbi);
    uint64_t elapsed_ns = (now - s_last_frame_time) * tbi.numer / tbi.denom;
    if (elapsed_ns < FRAME_MIN_INTERVAL_NS) return;
    s_last_frame_time = now;

    // Convert ARGB8888 → RGBA8888
    size_t rgba_size = static_cast<size_t>(width) * height * 4;
    std::vector<uint8_t> rgba(rgba_size);

    for (int y = 0; y < height; y++) {
        const uint32_t *src = reinterpret_cast<const uint32_t *>(pixels + y * pitch);
        uint8_t *dst = rgba.data() + y * width * 4;
        for (int x = 0; x < width; x++) {
            uint32_t argb = src[x];
            dst[x * 4 + 0] = (argb >> 16) & 0xFF; // R
            dst[x * 4 + 1] = (argb >> 8)  & 0xFF; // G
            dst[x * 4 + 2] = (argb)       & 0xFF; // B
            dst[x * 4 + 3] = 0xFF;                // A (always opaque)
        }
    }

    s_frame_cb(rgba.data(), width, height, s_frame_ctx);
}

// GFX_ShowMsg is referenced by DOSBox but defined in main.cpp which we don't include
void GFX_ShowMsg(const char* format, ...)
{
    char buf[512];
    va_list msg;
    va_start(msg, format);
    vsnprintf(buf, sizeof(buf), format, msg);
    va_end(msg);
    fprintf(stderr, "[DOSBox] %s\n", buf);
}

/* ---------- config file generation ---------- */

char *dosbox_write_config(const dosbox_config_t *cfg)
{
    if (!cfg || !cfg->working_dir) return nullptr;

    std::string path = std::string(cfg->working_dir) + "/dosbox-ios.conf";
    FILE *f = fopen(path.c_str(), "w");
    if (!f) return nullptr;

    // [sdl] — on iOS, fullscreen + explicit window_size avoids
    // get_desktop_size() assertion where SDL_GetDisplayBounds returns 0×0
    fprintf(f, "[sdl]\n");
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
    fprintf(f, "fullscreen=true\n");
    fprintf(f, "window_size=640x480\n");
#endif
    fprintf(f, "\n");

    // [dosbox]
    fprintf(f, "[dosbox]\n");
    fprintf(f, "machine=%s\n", cfg->machine ? cfg->machine : "svga_s3");
    fprintf(f, "memsize=%d\n", cfg->memsize > 0 ? cfg->memsize : 16);
    fprintf(f, "\n");

    // [cpu] — DOSBox-staging modern settings
    // cycles > 0: fixed N for real mode
    // cycles == 0: 3000 for real mode (classic "auto")
    // cycles < 0: max everywhere
    fprintf(f, "[cpu]\n");
    fprintf(f, "cputype=%s\n", cfg->cputype ? cfg->cputype : "auto");
    if (cfg->cycles > 0) {
        fprintf(f, "cpu_cycles=%d\n", cfg->cycles);
    } else if (cfg->cycles < 0) {
        fprintf(f, "cpu_cycles=max\n");
    } else {
        fprintf(f, "cpu_cycles=3000\n");
    }
    // Protected mode cycles
    if (cfg->cycles >= 0) {
        if (cfg->cycles_protected > 0)
            fprintf(f, "cpu_cycles_protected=%d\n", cfg->cycles_protected);
        else
            fprintf(f, "cpu_cycles_protected=max\n");
    }
    fprintf(f, "\n");

    // [render] — frameskip was removed; use host_rate instead
    // (omit render section or set host_rate if needed)
    fprintf(f, "[render]\n");
    fprintf(f, "\n");

    // [sblaster]
    fprintf(f, "[sblaster]\n");
    if (cfg->sb_enabled)
        fprintf(f, "sbtype=sb16\n");
    else
        fprintf(f, "sbtype=none\n");
    fprintf(f, "\n");

    // [gus]
    fprintf(f, "[gus]\n");
    fprintf(f, "gus=%s\n", cfg->gus_enabled ? "true" : "false");
    fprintf(f, "\n");

    // [speaker] — pcspeaker values: impulse, discrete, none (not true/false)
    fprintf(f, "[speaker]\n");
    fprintf(f, "pcspeaker=%s\n", cfg->speaker_enabled ? "impulse" : "none");
    fprintf(f, "\n");

    // [ethernet] — NE2000 NIC with SLIRP userspace networking
    fprintf(f, "[ethernet]\n");
    fprintf(f, "ne2000=true\n");
    fprintf(f, "nicbase=300\n");
    fprintf(f, "nicirq=3\n");
    fprintf(f, "macaddr=AC:DE:48:88:BB:AA\n");
    fprintf(f, "\n");

    // [autoexec] — mount disks and boot
    fprintf(f, "[autoexec]\n");

    if (cfg->floppy_a_path)
        fprintf(f, "imgmount a \"%s\" -t floppy\n", cfg->floppy_a_path);
    if (cfg->floppy_b_path)
        fprintf(f, "imgmount b \"%s\" -t floppy\n", cfg->floppy_b_path);

    if (cfg->hdd_c_path)
        fprintf(f, "imgmount c \"%s\" -t hdd -fs fat\n", cfg->hdd_c_path);
    if (cfg->hdd_d_path)
        fprintf(f, "imgmount d \"%s\" -t hdd -fs fat\n", cfg->hdd_d_path);
    if (cfg->iso_path)
        fprintf(f, "imgmount e \"%s\" -t iso\n", cfg->iso_path);

    // Additional autoexec commands (e.g., host-dir mounts for testing)
    if (cfg->autoexec) {
        for (int i = 0; cfg->autoexec[i]; i++)
            fprintf(f, "%s\n", cfg->autoexec[i]);
    }

    // Switch to C: and run its AUTOEXEC.BAT if present
    if (cfg->floppy_a_path) {
        fprintf(f, "boot a:\n");
    } else if (cfg->hdd_c_path) {
        fprintf(f, "c:\n");
        fprintf(f, "if exist c:\\autoexec.bat call c:\\autoexec.bat\n");
        fprintf(f, "SET DIRCMD=/ON\n");
    }

    fclose(f);
    return strdup(path.c_str());
}

/* ---------- iOS environment helpers ---------- */

// Set XDG_CONFIG_HOME to the app container so DOSBox creates its
// config directory inside the sandbox instead of ~/.config/
static void setup_ios_environment(const char *working_dir)
{
    if (working_dir) {
        setenv("XDG_CONFIG_HOME", working_dir, 1);
        setenv("XDG_DATA_HOME", working_dir, 1);
    }
    // Let SDL handle video rendering (it creates a Metal-backed view on iOS).
    // We capture frames via dosbox_on_frame_presented hook.
}

/* ---------- lifecycle ---------- */

int dosbox_start(const dosbox_config_t *cfg,
                 dosbox_frame_callback_t frame_cb,
                 void *context)
{
    if (s_running.load()) return -1;

    DOSBOX_ClearShutdownRequest();

    s_frame_cb = frame_cb;
    s_frame_ctx = context;

    setup_ios_environment(cfg ? cfg->working_dir : nullptr);

    // Tell SDL an external context exists (the app manages the UI).
    // This lets SDL create a real renderer (Metal/software) that actually
    // draws pixels, while avoiding UIWindow creation on iOS.
    SDL_SetHint(SDL_HINT_VIDEO_EXTERNAL_CONTEXT, "1");

    SDL_SetMainReady();

    char *conf_path = dosbox_write_config(cfg);
    if (!conf_path) return -1;

    s_running.store(true);
    int return_code = 0;

    try {
        std::vector<std::string> args = {
            "dosbox",
            "--conf", conf_path,
            "--noprimaryconf",
            "--nolocalconf"
        };

        int argc = static_cast<int>(args.size());
        std::vector<char*> argv;
        for (auto& a : args) argv.push_back(a.data());
        argv.push_back(nullptr);

        CommandLine command_line(argc, argv.data());
        control = std::make_unique<Config>(&command_line);

        init_config_dir();

        DOS_Locale_AddMessages();
        RENDER_AddMessages();
        GFX_AddConfigSection();
        DOSBOX_InitModuleConfigsAndMessages();

        control->ParseConfigFiles(get_config_dir());

        GFX_InitSdl();
        DOSBOX_InitModules();
        HOSTIO_Init(cfg ? cfg->host_dir : nullptr);
        GFX_InitAndStartGui();

        MAPPER_BindKeys(get_sdl_section());

        // Start the DOS shell — this blocks until exit
        SHELL_InitAndRun();

        // Cleanup
        HOSTIO_Destroy();
        DOSBOX_DestroyModules();
        GFX_Destroy();
        GFX_Quit();

    } catch (const std::exception& e) {
        BLOG("Error: %s", e.what());
        return_code = 1;
    } catch (...) {
        BLOG("Unknown error");
        return_code = 1;
    }

    free(conf_path);
    s_running.store(false);
    return return_code;
}

// Stored state for two-phase API (must outlive dosbox_init → dosbox_run)
static char *s_conf_path = nullptr;
static std::unique_ptr<CommandLine> s_cmdline;
static std::string s_host_dir;

int dosbox_init(const dosbox_config_t *cfg,
                dosbox_frame_callback_t frame_cb,
                void *context)
{
    if (s_running.load()) return -1;

    if (!s_atexit_registered.exchange(true))
        atexit(dosbox_atexit_cleanup);

    s_frame_cb = frame_cb;
    s_frame_ctx = context;
    s_last_frame_time = 0;

    setup_ios_environment(cfg ? cfg->working_dir : nullptr);

    SDL_SetHint(SDL_HINT_VIDEO_EXTERNAL_CONTEXT, "1");
    SDL_SetMainReady();

    s_conf_path = dosbox_write_config(cfg);
    if (!s_conf_path) return -1;

    s_running.store(true);

    try {
        std::vector<std::string> args = {
            "dosbox",
            "--conf", s_conf_path,
            "--noprimaryconf",
            "--nolocalconf"
        };

        int argc = static_cast<int>(args.size());
        std::vector<char*> argv;
        for (auto& a : args) argv.push_back(a.data());
        argv.push_back(nullptr);

        s_cmdline = std::make_unique<CommandLine>(argc, argv.data());
        control = std::make_unique<Config>(s_cmdline.get());

        init_config_dir();

        DOS_Locale_AddMessages();
        RENDER_AddMessages();
        GFX_AddConfigSection();
        DOSBOX_InitModuleConfigsAndMessages();

        control->ParseConfigFiles(get_config_dir());

        // These touch UIKit and MUST run on the main thread
        GFX_InitSdl();
        DOSBOX_InitModules();
        s_host_dir = (cfg && cfg->host_dir) ? cfg->host_dir : "";
        HOSTIO_Init(s_host_dir.empty() ? nullptr : s_host_dir.c_str());
        GFX_InitAndStartGui();
        MAPPER_BindKeys(get_sdl_section());

    } catch (const std::exception& e) {
        HOSTIO_Destroy();
        free(s_conf_path);
        s_conf_path = nullptr;
        s_cmdline.reset();
        s_running.store(false);
        return -1;
    } catch (...) {
        HOSTIO_Destroy();
        free(s_conf_path);
        s_conf_path = nullptr;
        s_cmdline.reset();
        s_running.store(false);
        return -1;
    }

    return 0;
}

static void dosbox_gfx_teardown_on_main(void *)
{
    GFX_Destroy();
    GFX_Quit();
}

int dosbox_run(void)
{
    if (!s_running.load()) return -1;

    int return_code = 0;
    try {
        // Start the DOS shell — this blocks until exit
        SHELL_InitAndRun();

        // Cleanup — GFX touches UIKit and must run on main thread
        HOSTIO_Destroy();
        DOSBOX_DestroyModules();
        if (pthread_main_np()) {
            GFX_Destroy();
            GFX_Quit();
        } else {
            dispatch_sync_f(dispatch_get_main_queue(), nullptr,
                            dosbox_gfx_teardown_on_main);
        }

    } catch (const std::exception& e) {
        return_code = 1;
    } catch (...) {
        return_code = 1;
    }

    free(s_conf_path);
    s_conf_path = nullptr;
    s_cmdline.reset();
    s_host_dir.clear();
    s_running.store(false);
    return return_code;
}

void dosbox_request_shutdown(void)
{
    // Only set the DOSBox shutdown flag — do NOT clear s_running.
    // s_running stays true until dosbox_run() finishes all cleanup.
    // This prevents the atexit handler from skipping protection
    // and prevents dosbox_init() from racing with ongoing cleanup.
    if (s_running.load()) {
        DOSBOX_RequestShutdown();
    }
}

int dosbox_is_running(void)
{
    return s_running.load() ? 1 : 0;
}

/* ---------- input injection ---------- */

void dosbox_inject_key(int sdl_scancode, int pressed)
{
    // Push SDL keyboard event into SDL's event queue
    // DOSBox polls SDL events in its main loop
    SDL_Event event = {};
    event.type = pressed ? SDL_KEYDOWN : SDL_KEYUP;
    event.key.keysym.scancode = static_cast<SDL_Scancode>(sdl_scancode);
    event.key.keysym.sym = SDL_GetKeyFromScancode(event.key.keysym.scancode);
    event.key.state = pressed ? SDL_PRESSED : SDL_RELEASED;
    SDL_PushEvent(&event);
}

// Map ASCII to SDL scancode + whether shift is needed
static bool ascii_to_sdl(uint16_t ch, SDL_Scancode &sc, bool &shift)
{
    shift = false;
    if (ch >= 'a' && ch <= 'z') { sc = (SDL_Scancode)(SDL_SCANCODE_A + (ch - 'a')); return true; }
    if (ch >= 'A' && ch <= 'Z') { sc = (SDL_Scancode)(SDL_SCANCODE_A + (ch - 'A')); shift = true; return true; }
    if (ch >= '1' && ch <= '9') { sc = (SDL_Scancode)(SDL_SCANCODE_1 + (ch - '1')); return true; }
    if (ch == '0') { sc = SDL_SCANCODE_0; return true; }
    switch (ch) {
        case '\r': case '\n': sc = SDL_SCANCODE_RETURN; return true;
        case ' ':  sc = SDL_SCANCODE_SPACE; return true;
        case '\t': sc = SDL_SCANCODE_TAB; return true;
        case 0x08: sc = SDL_SCANCODE_BACKSPACE; return true;
        case 0x1B: sc = SDL_SCANCODE_ESCAPE; return true;
        case '-':  sc = SDL_SCANCODE_MINUS; return true;
        case '=':  sc = SDL_SCANCODE_EQUALS; return true;
        case '[':  sc = SDL_SCANCODE_LEFTBRACKET; return true;
        case ']':  sc = SDL_SCANCODE_RIGHTBRACKET; return true;
        case '\\': sc = SDL_SCANCODE_BACKSLASH; return true;
        case ';':  sc = SDL_SCANCODE_SEMICOLON; return true;
        case '\'': sc = SDL_SCANCODE_APOSTROPHE; return true;
        case '`':  sc = SDL_SCANCODE_GRAVE; return true;
        case ',':  sc = SDL_SCANCODE_COMMA; return true;
        case '.':  sc = SDL_SCANCODE_PERIOD; return true;
        case '/':  sc = SDL_SCANCODE_SLASH; return true;
        // Shifted symbols
        case '!':  sc = SDL_SCANCODE_1; shift = true; return true;
        case '@':  sc = SDL_SCANCODE_2; shift = true; return true;
        case '#':  sc = SDL_SCANCODE_3; shift = true; return true;
        case '$':  sc = SDL_SCANCODE_4; shift = true; return true;
        case '%':  sc = SDL_SCANCODE_5; shift = true; return true;
        case '^':  sc = SDL_SCANCODE_6; shift = true; return true;
        case '&':  sc = SDL_SCANCODE_7; shift = true; return true;
        case '*':  sc = SDL_SCANCODE_8; shift = true; return true;
        case '(':  sc = SDL_SCANCODE_9; shift = true; return true;
        case ')':  sc = SDL_SCANCODE_0; shift = true; return true;
        case '_':  sc = SDL_SCANCODE_MINUS; shift = true; return true;
        case '+':  sc = SDL_SCANCODE_EQUALS; shift = true; return true;
        case '{':  sc = SDL_SCANCODE_LEFTBRACKET; shift = true; return true;
        case '}':  sc = SDL_SCANCODE_RIGHTBRACKET; shift = true; return true;
        case '|':  sc = SDL_SCANCODE_BACKSLASH; shift = true; return true;
        case ':':  sc = SDL_SCANCODE_SEMICOLON; shift = true; return true;
        case '"':  sc = SDL_SCANCODE_APOSTROPHE; shift = true; return true;
        case '~':  sc = SDL_SCANCODE_GRAVE; shift = true; return true;
        case '<':  sc = SDL_SCANCODE_COMMA; shift = true; return true;
        case '>':  sc = SDL_SCANCODE_PERIOD; shift = true; return true;
        case '?':  sc = SDL_SCANCODE_SLASH; shift = true; return true;
        default: return false;
    }
}

void dosbox_inject_char(uint16_t unicode_char)
{
    SDL_Scancode sc;
    bool shift;
    if (!ascii_to_sdl(unicode_char, sc, shift)) return;

    if (shift) {
        SDL_Event se = {};
        se.type = SDL_KEYDOWN;
        se.key.keysym.scancode = SDL_SCANCODE_LSHIFT;
        se.key.keysym.sym = SDLK_LSHIFT;
        se.key.state = SDL_PRESSED;
        SDL_PushEvent(&se);
    }

    SDL_Event down = {};
    down.type = SDL_KEYDOWN;
    down.key.keysym.scancode = sc;
    down.key.keysym.sym = SDL_GetKeyFromScancode(sc);
    down.key.state = SDL_PRESSED;
    SDL_PushEvent(&down);

    SDL_Event up = {};
    up.type = SDL_KEYUP;
    up.key.keysym.scancode = sc;
    up.key.keysym.sym = SDL_GetKeyFromScancode(sc);
    up.key.state = SDL_RELEASED;
    SDL_PushEvent(&up);

    if (shift) {
        SDL_Event se = {};
        se.type = SDL_KEYUP;
        se.key.keysym.scancode = SDL_SCANCODE_LSHIFT;
        se.key.keysym.sym = SDLK_LSHIFT;
        se.key.state = SDL_RELEASED;
        SDL_PushEvent(&se);
    }
}

void dosbox_inject_mouse(int dx, int dy, int buttons)
{
    SDL_Event event = {};
    event.type = SDL_MOUSEMOTION;
    event.motion.xrel = dx;
    event.motion.yrel = dy;
    event.motion.state = static_cast<uint32_t>(buttons);
    SDL_PushEvent(&event);
}

void dosbox_inject_mouse_abs(int x, int y, int buttons)
{
    SDL_Event event = {};
    event.type = SDL_MOUSEMOTION;
    event.motion.x = x;
    event.motion.y = y;
    event.motion.state = static_cast<uint32_t>(buttons);
    SDL_PushEvent(&event);
}
