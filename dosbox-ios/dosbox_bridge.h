/*
 * dosbox_bridge.h - C interface for launching DOSBox from iOS
 *
 * This is the thin C bridge between the Objective-C++ DOSEmulator wrapper
 * and DOSBox-staging internals.  The iOS app never calls DOSBox headers
 * directly; everything goes through these functions.
 */

#ifndef DOSBOX_BRIDGE_H
#define DOSBOX_BRIDGE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- configuration ---------- */

typedef struct {
    const char *machine;       /* "svga_s3", "vgaonly", "ega", "cga", etc. */
    int         cycles;        /* >0=fixed, 0=auto(3000/max), <0=max */
    int         cycles_protected; /* protected mode cycles (0=same as cycles) */
    int         memsize;       /* MB of RAM (default 16) */

    /* Sound */
    int         sb_enabled;    /* Sound Blaster: 0=off, 1=sb16 */
    int         gus_enabled;
    int         speaker_enabled;

    /* Input */
    int         mouse_enabled;

    /* Disk images: NULL = not mounted */
    const char *floppy_a_path;
    const char *floppy_b_path;
    const char *hdd_c_path;     /* FAT image or directory to mount as C: */
    const char *hdd_d_path;
    const char *iso_path;       /* CD-ROM ISO image for D: or E: */

    /* Autoexec commands (NULL-terminated array, or NULL for none) */
    const char **autoexec;

    /* Working directory for DOSBox config files */
    const char *working_dir;

    /* Boot drive: 0=A, 0x80=C, 0xE0=CD-ROM (default: auto-detect) */
    int         boot_drive;
} dosbox_config_t;

/* CPU speed presets (DOSBox cycle values) */
enum {
    DOSBOX_SPEED_AUTO     =     0,  /* 3000 real mode, max protected */
    DOSBOX_SPEED_8088     =  3000,  /* IBM PC 4.77 MHz */
    DOSBOX_SPEED_286      =  7000,  /* IBM AT 8 MHz */
    DOSBOX_SPEED_386SX    = 15000,  /* 386SX 16 MHz */
    DOSBOX_SPEED_386DX    = 26000,  /* 386DX 33 MHz */
    DOSBOX_SPEED_486DX2   = 50000,  /* 486DX2 66 MHz */
    DOSBOX_SPEED_PENTIUM  = 77000,  /* Pentium 60-66 MHz */
    DOSBOX_SPEED_MAX      =    -1,  /* unlimited */
};

/* ---------- frame callback ---------- */

/*
 * Called by the DOSBox render path each time a frame is ready.
 * pixels:  RGBA8888 pixel data (width * height * 4 bytes)
 * width, height: frame dimensions
 * context: opaque pointer passed at startup
 */
typedef void (*dosbox_frame_callback_t)(const uint8_t *pixels,
                                        int width, int height,
                                        void *context);

/* ---------- lifecycle ---------- */

/*
 * Write a DOSBox .conf file from the given config struct.
 * Returns the path to the written file (caller must free).
 */
char *dosbox_write_config(const dosbox_config_t *cfg);

/*
 * Start DOSBox with the given config.  Blocks until DOSBox exits.
 * Call from a background thread.
 *
 * frame_cb:  called on every rendered frame (may be NULL)
 * context:   passed through to frame_cb
 *
 * Returns 0 on success, non-zero on error.
 */
int dosbox_start(const dosbox_config_t *cfg,
                 dosbox_frame_callback_t frame_cb,
                 void *context);

/*
 * Two-phase startup for iOS.
 *
 * dosbox_init: initializes SDL, GFX, and DOSBox modules.
 *              MUST be called on the main thread (SDL_VideoInit touches UIKit).
 *              Returns 0 on success, non-zero on error.
 *
 * dosbox_run:  starts the DOS shell (SHELL_InitAndRun). Blocks until DOSBox
 *              exits. Call from a background thread after dosbox_init succeeds.
 *              Returns 0 on success, non-zero on error.
 */
int dosbox_init(const dosbox_config_t *cfg,
                dosbox_frame_callback_t frame_cb,
                void *context);
int dosbox_run(void);

/*
 * Request DOSBox to shut down gracefully.
 * Safe to call from any thread.
 */
void dosbox_request_shutdown(void);

/*
 * Check if DOSBox is currently running.
 */
int dosbox_is_running(void);

/* ---------- input injection ---------- */

/* Inject a key press/release.  scancode is the SDL scancode. */
void dosbox_inject_key(int sdl_scancode, int pressed);

/* Inject a character (for text input that bypasses scancodes). */
void dosbox_inject_char(uint16_t unicode_char);

/* Inject mouse motion (relative) and button state. */
void dosbox_inject_mouse(int dx, int dy, int buttons);

/* Inject absolute mouse position (0-639, 0-399 typical). */
void dosbox_inject_mouse_abs(int x, int y, int buttons);

#ifdef __cplusplus
}
#endif

#endif /* DOSBOX_BRIDGE_H */
