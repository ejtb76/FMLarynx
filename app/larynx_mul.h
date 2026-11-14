/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Presethead */
/* 64-bit multiplies, kept out of line (as Amenity's jungle_mul.h). On the FM-1 an inlined 32x32->64 multiply
 * among loads now and then leaves a register one bit off and the firmware crashes; in a function of its own the
 * product is used at once and it does not happen (felucca-bsp docs/CPU.md). Every 64-bit multiply in FMLarynx's
 * sound goes through these: the results are the same as the inline products', bit for bit. */
#ifndef LARYNX_MUL_H
#define LARYNX_MUL_H
#include <stdint.h>

#define LARYNX_MUL static __attribute__((noinline, unused))
/* (a * b) >> shift */
LARYNX_MUL int32_t mul_shr(int32_t a, int32_t b, unsigned shift) { return (int32_t)(((int64_t)a * b) >> shift); }
/* the whole product, for sums */
LARYNX_MUL int64_t mul64(int32_t a, int32_t b) { return (int64_t)a * b; }
#endif
