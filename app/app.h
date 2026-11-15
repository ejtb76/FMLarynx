/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Charles Vestal; FMLarynx's changes (C) 2026 Presethead */
/* FMLarynx's half of the contract with the platform (fm1-base: device/unity.c, host/plat_host.c), from fm1-base's
 * template. */
#pragma once
#include <stdint.h>
#include "plat.h"

#define APP_NAME "FMLARYNX"
#define APP_SPLASH_RGB RGB(232, 64, 88)
#if __has_include("../build/splash.inc")
#define APP_SPLASH                       /* the picture (assets/splash.png) and the version: app_splash, app.c */
void app_splash(void);
#ifdef PLAT_HOST
#define APP_HOST_CMD                     /* the host's "splash" (its boot skips it): for a screenshot */
int app_host_cmd(const char *cmd, const char *a, const char *b, const char *c, const char *d);
#endif
#endif
#define APP_BLOCK 256u                   /* frames per audio block: 5.8 ms at 44.1 kHz */
#define APP_CANVAS_PX (240u * 114u)      /* the biggest strip drawn at once: the throat, 240 x 114 */
#define APP_TICK                         /* app_tick: the internal MIDI clock, out to USB and Bluetooth, 1 kHz */
#define APP_BTDEV                        /* Bluetooth MIDI devices (lib/btdev.h): the connections page (EDIT) */
#define APP_LOG                          /* the clock's readout (fm1_remote.py log): app_log, app.c */
void app_log(void (*line)(const char *t));
#define APP_USB_HOST_NAME               /* the USB device hosted, by its own name: HOST TO AMENITY */

/* the store: the app's objects, the panel calibration (the platform's) and Bluetooth's address and pairing among
 * them, and their flash offsets */
enum { OBJ_SETTINGS, OBJ_PANEL, OBJ_BLE, OBJ_NOBJ };
#define APP_STORE_MAP {0xFC000u, 0xDC000u, 0xDE000u}

/* the contract: the platform calls these */
void app_init(void);                     /* boot: the saved settings (or defaults) */
void app_render(int32_t *lr, uint32_t n, uint32_t master_q12);   /* audio interrupt: n stereo frames, 24-bit */
void app_tick(void);                     /* 1 kHz, in the timer interrupt that takes in USB and TRS MIDI */
void app_panic(void);                    /* silence now (before an update rewrites the flash) */
void app_ui_init(void);                  /* the first screen, after the splash */
void app_frame(void);                    /* the UI: input and a redraw, about 60 times a second */
void app_input_only(void);               /* between frames: input only */
void app_say(const char *a, const char *b);   /* a short message on the screen and to a screen reader */

/* the host simulator's checks (host/plat_host.c "expect"): note (-1 none), octave, rec, playing, step, dirty, bpm,
 * used, page */
#define APP_HOST_EXPECT
int app_expect(const char *what, int *got);
