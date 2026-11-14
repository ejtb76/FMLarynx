/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Charles Vestal; FMLarynx's changes (C) 2026 Presethead */
/* FMLarynx's half of the contract with the platform (fm1-base: device/unity.c, host/plat_host.c), from fm1-base's
 * template. */
#pragma once
#include <stdint.h>
#include "plat.h"

#define APP_NAME "FMLARYNX"
#define APP_SPLASH_RGB RGB(232, 64, 88)
#define APP_BLOCK 256u                   /* frames per audio block: 5.8 ms at 44.1 kHz */
#define APP_CANVAS_PX (240u * 64u)       /* the biggest strip drawn at once: the keys, 240 x 56 */

/* the store: the app's objects, the panel calibration (the platform's) and Bluetooth's address and pairing among
 * them, and their flash offsets */
enum { OBJ_SETTINGS, OBJ_PANEL, OBJ_BLE, OBJ_NOBJ };
#define APP_STORE_MAP {0xFC000u, 0xDC000u, 0xDE000u}

/* the contract: the platform calls these */
void app_init(void);                     /* boot: the saved settings (or defaults) */
void app_render(int32_t *lr, uint32_t n, uint32_t master_q12);   /* audio interrupt: n stereo frames, 24-bit */
void app_panic(void);                    /* silence now (before an update rewrites the flash) */
void app_ui_init(void);                  /* the first screen, after the splash */
void app_frame(void);                    /* the UI: input and a redraw, about 60 times a second */
void app_input_only(void);               /* between frames: input only */
void app_say(const char *a, const char *b);   /* a short message on the screen and to a screen reader */

/* the host simulator's checks (host/plat_host.c "expect"): note (-1 none), octave, rec, playing, step, dirty, bpm,
 * used */
#define APP_HOST_EXPECT
int app_expect(const char *what, int *got);
