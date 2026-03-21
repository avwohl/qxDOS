/*
 * dosbox_bridge.cpp - C bridge for launching DOSBox from iOS
 *
 * Implements the bridge functions by calling into DOSBox's initialization
 * and execution APIs.  This replaces DOSBox's main.cpp entry point.
 */

#include "dosbox_bridge.h"

// Tell SDL we handle the application lifecycle ourselves (SwiftUI owns main)
#define SDL_MAIN_HANDLED
#include <SDL.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <mach/mach_time.h>
#include <dispatch/dispatch.h>

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
static dosbox_frame_callback_t s_frame_cb = nullptr;
static void *s_frame_ctx = nullptr;

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

    // [autoexec] — mount disks and set up boot
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

    // Boot from floppy if present, otherwise switch to C:
    // Floppy images need `boot` to execute the boot sector.
    // HDD FAT images are already accessible via mount — just switch to C:.
    if (cfg->floppy_a_path) {
        fprintf(f, "boot a:\n");
    } else if (cfg->hdd_c_path) {
        fprintf(f, "c:\n");
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
        GFX_InitAndStartGui();

        MAPPER_BindKeys(get_sdl_section());

        // Start the DOS shell — this blocks until exit
        SHELL_InitAndRun();

        // Cleanup
        DOSBOX_DestroyModules();
        GFX_Destroy();

    } catch (const std::exception& e) {
        fprintf(stderr, "[DOSBox Bridge] Error: %s\n", e.what());
        return_code = 1;
    } catch (...) {
        fprintf(stderr, "[DOSBox Bridge] Unknown error\n");
        return_code = 1;
    }

    free(conf_path);
    s_running.store(false);
    return return_code;
}

// Stored config path for two-phase API
static char *s_conf_path = nullptr;

int dosbox_init(const dosbox_config_t *cfg,
                dosbox_frame_callback_t frame_cb,
                void *context)
{
    if (s_running.load()) return -1;

    s_frame_cb = frame_cb;
    s_frame_ctx = context;

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

        CommandLine command_line(argc, argv.data());
        control = std::make_unique<Config>(&command_line);

        init_config_dir();

        DOS_Locale_AddMessages();
        RENDER_AddMessages();
        GFX_AddConfigSection();
        DOSBOX_InitModuleConfigsAndMessages();

        control->ParseConfigFiles(get_config_dir());

        // These touch UIKit and MUST run on the main thread
        GFX_InitSdl();
        DOSBOX_InitModules();
        GFX_InitAndStartGui();
        MAPPER_BindKeys(get_sdl_section());

    } catch (const std::exception& e) {
        fprintf(stderr, "[DOSBox Bridge] Init error: %s\n", e.what());
        free(s_conf_path);
        s_conf_path = nullptr;
        s_running.store(false);
        return -1;
    } catch (...) {
        fprintf(stderr, "[DOSBox Bridge] Init unknown error\n");
        free(s_conf_path);
        s_conf_path = nullptr;
        s_running.store(false);
        return -1;
    }

    return 0;
}

int dosbox_run(void)
{
    if (!s_running.load()) return -1;

    int return_code = 0;
    try {
        // Start the DOS shell — this blocks until exit
        SHELL_InitAndRun();

        // Cleanup
        DOSBOX_DestroyModules();
        GFX_Destroy();

    } catch (const std::exception& e) {
        fprintf(stderr, "[DOSBox Bridge] Run error: %s\n", e.what());
        return_code = 1;
    } catch (...) {
        fprintf(stderr, "[DOSBox Bridge] Run unknown error\n");
        return_code = 1;
    }

    free(s_conf_path);
    s_conf_path = nullptr;
    s_running.store(false);
    return return_code;
}

void dosbox_request_shutdown(void)
{
    if (s_running.load()) {
        DOSBOX_RequestShutdown();
    }
    s_running.store(false);
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

void dosbox_inject_char(uint16_t unicode_char)
{
    // Push SDL text input event
    SDL_Event event = {};
    event.type = SDL_TEXTINPUT;
    // UTF-8 encode the character
    if (unicode_char < 0x80) {
        event.text.text[0] = static_cast<char>(unicode_char);
        event.text.text[1] = '\0';
    } else if (unicode_char < 0x800) {
        event.text.text[0] = static_cast<char>(0xC0 | (unicode_char >> 6));
        event.text.text[1] = static_cast<char>(0x80 | (unicode_char & 0x3F));
        event.text.text[2] = '\0';
    }
    SDL_PushEvent(&event);
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
