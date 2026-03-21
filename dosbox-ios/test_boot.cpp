/*
 * test_boot.cpp - macOS CLI harness to test DOSBox boot
 *
 * Usage: ./test_boot [disk_image.img]
 * Boots DOSBox with the given HDD image (or freedos_hd.img by default).
 * Opens an SDL window on macOS so you can see the DOS screen.
 */

#define SDL_MAIN_HANDLED
#include <SDL.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>

#include "dosbox_bridge.h"

static void frame_callback(const uint8_t *pixels, int width, int height, void *ctx)
{
    (void)ctx;
    static int frame_count = 0;
    frame_count++;
    if (frame_count % 30 == 1) {
        // Sample pixels: top-left corner (text area) and center
        const uint8_t *p1 = pixels + (10 * width + 10) * 4; // near top-left
        int cx = width / 2, cy = height / 2;
        const uint8_t *p2 = pixels + (cy * width + cx) * 4;
        // Count non-black pixels to verify content
        int nonblack = 0;
        for (int i = 0; i < width * height; i++) {
            if (pixels[i*4] || pixels[i*4+1] || pixels[i*4+2]) { nonblack++; break; }
        }
        fprintf(stderr, "[test_boot] Frame %d: %dx%d  top=(%d,%d,%d) center=(%d,%d,%d) content=%s\n",
                frame_count, width, height,
                p1[0], p1[1], p1[2], p2[0], p2[1], p2[2],
                nonblack ? "YES" : "empty");
    }
}

int main(int argc, char *argv[])
{
    SDL_SetMainReady();

    const char *hdd_path = nullptr;
    const char *floppy_path = nullptr;

    // Parse args
    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (strstr(arg, ".img") || strstr(arg, ".IMG")) {
            // Guess type by size
            FILE *f = fopen(arg, "rb");
            if (!f) {
                fprintf(stderr, "Cannot open: %s\n", arg);
                return 1;
            }
            fseek(f, 0, SEEK_END);
            long sz = ftell(f);
            fclose(f);

            if (sz <= 1474560) {
                floppy_path = arg;
                fprintf(stderr, "Floppy: %s (%ld bytes)\n", arg, sz);
            } else {
                hdd_path = arg;
                fprintf(stderr, "HDD: %s (%ld bytes)\n", arg, sz);
            }
        }
    }

    // Default to freedos_hd.img
    if (!hdd_path && !floppy_path) {
        hdd_path = "fd/freedos_hd.img";
        fprintf(stderr, "Using default: %s\n", hdd_path);
    }

    // Working dir for config file
    const char *workdir = "/tmp/dosbox-test";
    mkdir(workdir, 0755);

    // Set up environment
    setenv("XDG_CONFIG_HOME", workdir, 1);
    setenv("XDG_DATA_HOME", workdir, 1);

    // Check for a second disk image for D:
    const char *hdd_d_path = nullptr;
    if (hdd_path) {
        // Auto-detect games disk
        std::string games = std::string(hdd_path);
        auto pos = games.rfind('/');
        if (pos != std::string::npos) {
            std::string games_path = games.substr(0, pos + 1) + "freedos_games.img";
            FILE *gf = fopen(games_path.c_str(), "rb");
            if (gf) {
                fclose(gf);
                static std::string games_str = games_path;
                hdd_d_path = games_str.c_str();
                fprintf(stderr, "Games disk D: %s\n", hdd_d_path);
            }
        }
    }

    dosbox_config_t cfg = {};
    cfg.machine = "svga_s3";
    cfg.cycles = 0;  // auto (0=auto, >0=fixed, <0=max)
    cfg.cycles_protected = 50000;  // ~486DX2/66 for DOOM
    cfg.memsize = 16;
    cfg.sb_enabled = 1;
    cfg.gus_enabled = 0;
    cfg.speaker_enabled = 1;
    cfg.mouse_enabled = 1;
    // Use imgmount path (same as iOS app)
    cfg.hdd_c_path = hdd_path;
    cfg.hdd_d_path = hdd_d_path;
    cfg.floppy_a_path = floppy_path;
    cfg.working_dir = workdir;

    fprintf(stderr, "Starting DOSBox...\n");
    fprintf(stderr, "  HDD C: %s\n", hdd_path ? hdd_path : "(none)");
    fprintf(stderr, "  HDD D: %s\n", hdd_d_path ? hdd_d_path : "(none)");
    fprintf(stderr, "  Floppy A: %s\n", floppy_path ? floppy_path : "(none)");

    int rc = dosbox_start(&cfg, frame_callback, nullptr);

    fprintf(stderr, "DOSBox exited with code %d\n", rc);
    return rc;
}
