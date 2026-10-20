/* SPDX-License-Identifier: GPL-3.0-only */
/* Dinghy, modified into a FormantSynth prototype: a monophonic sawtooth through a 3-band vowel
 * formant filter (vowel "a" fixed for now), in place of the stock 4-voice sine. See the formant
 * filter block below for the why. This is the file to change. Everything below it (platform.c) is
 * done: updates, the USB rescue, USB-MIDI, USB audio, keys, knobs, LEDs, the screen, a settings sector.
 *
 *   keys       play one formant-filtered voice; the notes go out over USB-MIDI too
 *   OCT- OCT+  the octave (kept in flash)
 *   MASTER     the volume (platform.c)
 *   USB-MIDI   notes in play the voice, with their velocity
 *
 * Rules of the road (the chip has no FPU and no libgcc): integers only, no float, no 64-bit division.
 * tests/host.c runs this file on a computer: the screen as a PNG, the sound as a WAV.
 * What platform.c offers: docs/API.md. The chip's rules: docs/RULES.md. */
#define APP_NAME "FMLARYNX"                      /* the screen, the rescue and the crash screen say this */
#define LCD_BAUD 2u                             /* LCD SPI at 60/(2+1) = 20 MHz, not the platform's 12: the
                                                  * throat is ~55 KB a frame (the ST7789 takes up to ~60) */
#include "platform.c"

/* ---- the sound (runs in the audio ISR: keep it short, no flash, no screen)
 * FormantSynth prototype: our real plugin (JUCE, see Source/PluginProcessor.h) is monophonic
 * -- one oscillator through one formant filter, last-note-priority -- so NV=1 here matches it
 * exactly; play()/stop()/quiet()/app_main() below are unchanged and already generic over NV. */
#define NV 1u                                   /* voices: monophonic, like the real plugin */
static struct {
    volatile uint32_t inc;                      /* phase step per frame: f * 2^32 / 44100 -- written by
                                                  * app_audio() only, which moves it to tgt */
    volatile uint32_t tgt;                      /* the pitch play() asks for, same units */
    volatile int32_t glide;                     /* 0: inc jumps to tgt; 1: glides there (a 303 slide) */
    volatile int32_t gate;                      /* target level, Q15: 0 = released */
    volatile int32_t amp;                       /* the level now: glides to gate, no clicks -- volatile
                                                  * since the sequencer (app_main) now also resets it
                                                  * for a hard retrigger on a non-slide step */
    uint32_t ph, age;
    uint8_t note;
} voice[NV];

/* ---- formant filter: 3 parallel RBJ "constant 0 dB peak" bandpass biquads tuned to F1/F2/F3,
 * ported from the float design in Source/dsp/FormantFilter.h -- including the X/Y vowel-plane
 * blend, Size/Resonance/Voice, and the X/Y LFOs. All of it is fixed-point: no float, no 64-bit
 * division anywhere (docs/RULES.md). Coefficients are recomputed once per 256-frame block (control
 * rate, like the original), not per sample -- this is the only part of app_audio() with any real
 * cost, and measured headroom (fm1_ticks()) is ~19x the safe budget even with it, see draw_isr().
 * b1 is always 0 for this biquad type, so each band costs 4 multiplies, not 5. */
#define NB   3u                                 /* formant bands: F1, F2, F3 */
#define NVOW 5u                                 /* reference vowels: i, e, a, o, u */
#define CQ   15                                 /* biquad coefficient format: Q15 (was Q13; see the
                                                  * precision note in formant_update_coeffs()) */

static int32_t sine(uint32_t ph)                /* a full cycle over 2^32 -> -32767..32767 (a refined
                                                   * parabola, ~0.1%); reused below as sin()/cos() for
                                                   * an arbitrary angle, not just an oscillator phase */
{
    int32_t x = (int32_t)ph >> 16, ax = x < 0 ? -x : x, y, ay;
    y = (x * (32768 - ax)) >> 13;               /* 4x(1-|x|), Q15 */
    ay = y < 0 ? -y : y;
    y += ((((y * ay) >> 15) - y) * 7373) >> 15; /* + 0.225 (y|y| - y) */
    return y > 32767 ? 32767 : y < -32767 ? -32767 : y;
}

/* log2(Hz) in Q8, F1/F2/F3 rows x i/e/a/o/u columns (adult male / female-child), from
 * FormantFilter.h's kVowelF/kVowelFf -- precomputed offline (log2, not part of the firmware)
 * since these 30 numbers never change; only the *blend* of them needs to happen on-device. */
static const int32_t fc_lnf_m[NB][NVOW] = {
    {2068, 2317, 2435, 2344, 2107},
    {2857, 2776, 2583, 2487, 2500},
    {2958, 2887, 2881, 2876, 2849},
};
static const int32_t fc_lnf_f[NB][NVOW] = {
    {2119, 2369, 2491, 2356, 2184},
    {2930, 2864, 2625, 2520, 2532},
    {2993, 2956, 2933, 2919, 2914},
};
static const int32_t vowel_x[NVOW] = {0, 0, 500, 1000, 1000};      /* pm (0..1000 == 0.0..1.0) */
static const int32_t vowel_y[NVOW] = {0, 500, 1000, 500, 0};
static const int32_t band_gain_q8[NB] = {256, 205, 154};           /* kBandGain 1.0, 0.8, 0.6 */

/* Resonance volume compensation: a narrow (high-Q) band passes much less of a sawtooth's energy
 * unless a harmonic happens to land right on it -- real behaviour of this filter design (confirmed
 * by measuring it offline), not a bug, but it reads as "gets quieter" when sweeping Resonance. This
 * boosts output as Q rises to flatten that out, deliberately diverging from the float plugin (which
 * has no such compensation). Index = res_x10/10 = Q; gain = sqrt(min(Q,20)) in Q8, ramp capped at
 * Q=20 so a harmonic that *is* aligned doesn't get boosted into hard clipping at high Q. */
static const int32_t res_gain_q8[51] = {        /* exactly 51 entries (index 0..50) -- a short
                                                  * initializer here zero-pads the rest, which is
                                                  * exactly what silenced Resonance=500 the first time */
    0, 256, 362, 443, 512, 572, 627, 677, 724, 768,
    810, 849, 887, 923, 958, 991, 1024, 1056, 1086, 1116,
    1145, 1145, 1145, 1145, 1145, 1145, 1145, 1145, 1145, 1145,
    1145, 1145, 1145, 1145, 1145, 1145, 1145, 1145, 1145, 1145,
    1145, 1145, 1145, 1145, 1145, 1145, 1145, 1145, 1145, 1145,
    1145,
};

static int32_t exp2_fixed(int32_t log2_q8)      /* Q8 log2(Hz) -> Hz; quadratic 2^f approximation,
                                                  * < 0.6% error at the reference-table round trip --
                                                  * inaudible for a resonance this wide */
{
    int32_t ip = log2_q8 >> 8, frac = log2_q8 & 255;          /* frac: Q8, 0..255 == f in [0,1) */
    int32_t poly = 45412 + ((15834 * frac) >> 8);             /* Q16: 0.6930 + 0.2416f */
    int32_t mant = 65536 + ((frac * poly) >> 8);              /* Q16: 1 + f*(0.6930+0.2416f) */
    return ((int32_t)1 << ip) * (mant >> 8) >> 8;             /* (1<<ip) * 2^f */
}

static void vowel_freqs(int32_t x, int32_t y, int32_t voice_pm, int32_t outHz[NB])
{                                                /* inverse-distance-squared blend in log2 domain,
                                                   * matching FormantFilter::vowelFreqs exactly */
    int32_t wsum = 0, logsum[NB] = {0, 0, 0};
    uint32_t v, b;
    for (v = 0; v < NVOW; v++) {
        int32_t dx = x - vowel_x[v], dy = y - vowel_y[v];
        int32_t denom = dx * dx + dy * dy + 100;              /* pm^2, +epsilon so an exact hit stays finite */
        int32_t w = (1 << 20) / denom;
        wsum += w;
        for (b = 0; b < NB; b++) {
            int32_t lf = (fc_lnf_m[b][v] * (1000 - voice_pm) + fc_lnf_f[b][v] * voice_pm) / 1000;
            logsum[b] += w * lf;
        }
    }
    if (wsum < 1) wsum = 1;                                   /* guard: unreachable for this vowel layout */
    for (b = 0; b < NB; b++)
        outHz[b] = exp2_fixed(logsum[b] / wsum);
}

static int32_t mFreq[NB] = {-1, -1, -1};        /* smoothed centre freq per band, Hz; -1 = snap on first block */
static int32_t fz1x[NB], fz2x[NB], fz1y[NB], fz2y[NB];   /* x[n-1], x[n-2], y[n-1], y[n-2] per band */
static uint32_t lfoXPh, lfoYPh;                 /* LFO phase, advanced once per block (control rate) */
static volatile int32_t vow_lock;               /* 0, or 1..5: a step's vowel (i e a o u) in place of the
                                                  * X/Y knobs -- the playing step's, or in record mode the
                                                  * cursor step's; set by the main loop, read by the ISR */

/* Accent (TB-303 / MC-202 style, per sequencer step): seq_trigger() sets accent_env to full, and it
 * decays once per block (~200 ms). While it is up it raises the resonance (up to 2x Q) and pulls the
 * vowel halfway towards "a" (the most open one), on top of the louder velocity -- a short "quack".
 * Written by the main loop, read and decayed by the audio ISR: a plain int32 store is atomic here. */
static volatile int32_t accent_env;             /* Q15: 32767 = just triggered, 0 = no accent */
static int32_t res_eff = 80;                    /* resx10 with the accent applied; set per block by
                                                  * formant_update_coeffs(), read by app_audio() for the
                                                  * resonance compensation */

/* knob-driven parameters -- pm = permille (0..1000 == the float plugin's 0.0..1.0), chosen so
 * every encoder click is a plain add/sub with no extra scaling at the UI layer. */
static struct {
    int32_t x, y;                               /* articulator plane, pm */
    int32_t size;                                /* 500..2000 == 0.5x..2.0x */
    int32_t resx10;                              /* Q*10, 10..500 == Q 1.0..50.0 */
    int32_t voice;                               /* pm, 0 = male .. 1000 = female/child */
    int32_t cutoff;                               /* pm: the low-pass after the formants, 60 Hz .. open
                                                   * (1000 = off); Env Mod opens it further */
    int32_t lfoXRate, lfoXDepth, lfoYRate, lfoYDepth;   /* rate: pm (0..8 Hz); depth: pm of the x/y range */
    int32_t decay, envmod, accent, ampdec;      /* the 303's envelope knobs (under BTN_ENV), all pm:
                                                  * decay 30 ms..2 s; envmod how far the envelope opens
                                                  * Size (up to 2x); accent scales the accent; ampdec the
                                                  * volume's decay, 1000 = none (holds, as before) */
} par = {500, 500, 1000, 80, 0, 1000, 0, 0, 0, 0, 500, 0, 1000, 1000};

/* The envelope (TB-303 style): every new note (not a slide, which carries on) restarts it at full;
 * it decays once per block, mod_env opening Size by envmod, amp_env fading the volume. play()
 * raises env_trig, app_audio() restarts both: the ISR alone writes the envelopes, no race. */
static volatile int32_t env_trig;
static int32_t mod_env, amp_env = 32767;        /* Q15 */
static const int32_t decay_q15[21] = {          /* per-block (5.8 ms) factor, Q15, for time constants
                                                  * 30 ms * (2000/30)^(i/20): 30 ms .. 2 s, exponential
                                                  * so the knob feels even -- precomputed offline */
    27003, 28011, 28856, 29559, 30142, 30622, 31018, 31342, 31607, 31824,
    32001, 32145, 32262, 32357, 32434, 32497, 32548, 32590, 32624, 32651, 32673,
};
static int32_t decay_coef(int32_t pm)           /* pm 0..1000 -> the factor, interpolated */
{
    int32_t i = pm / 50, f = pm % 50;
    if (i >= 20) return decay_q15[20];
    return decay_q15[i] + (decay_q15[i + 1] - decay_q15[i]) * f / 50;
}

static void formant_update_coeffs(int32_t cb0[NB], int32_t cb2[NB], int32_t ca1[NB], int32_t ca2[NB])
{
    int32_t lfoX = (sine(lfoXPh) * par.lfoXDepth) / 32767;    /* -depth..depth, pm */
    int32_t lfoY = (sine(lfoYPh) * par.lfoYDepth) / 32767;
    int32_t vl = vow_lock;                      /* a step's vowel replaces the knobs; the LFOs stay on top */
    int32_t x = (vl ? vowel_x[vl - 1] : par.x) + lfoX, y = (vl ? vowel_y[vl - 1] : par.y) + lfoY, target[NB];
    int32_t acc = accent_env * par.accent / 1000;          /* the Accent knob scales all of it */
    int32_t sz = par.size + ((par.size * par.envmod / 1000) * mod_env >> 15);   /* Env Mod: up to 2x */
    uint32_t b;
    x = x < 0 ? 0 : x > 1000 ? 1000 : x;
    y = y < 0 ? 0 : y > 1000 ? 1000 : y;
    lfoXPh += par.lfoXRate * 199458u;           /* rate pm -> phase step per block, 0..8 Hz */
    lfoYPh += par.lfoYRate * 199458u;

    x += ((vowel_x[2] - x) * acc) >> 16;        /* accent: halfway to "a" at full (>> 16 = * acc/2) */
    y += ((vowel_y[2] - y) * acc) >> 16;
    res_eff = par.resx10 + ((par.resx10 * acc) >> 15);        /* accent: up to 2x Q */
    if (res_eff > 500) res_eff = 500;
    accent_env = (accent_env * 31830) >> 15;    /* decay per 5.8 ms block: e^(-5.8/200) -> ~200 ms */
    mod_env = (mod_env * decay_coef(par.decay)) >> 15;

    vowel_freqs(x, y, par.voice, target);
    for (b = 0; b < NB; b++) {
        int32_t f = (target[b] * sz) / 1000, fc, turns_q16, sw, cw, alpha, a0;
        uint32_t ph0;
        if (f < 20) f = 20; else if (f > 19845) f = 19845;    /* Nyquist guard, 0.45 * 44100 */
        if (mFreq[b] < 0) mFreq[b] = f;                        /* snap on the very first block */
        else mFreq[b] += ((f - mFreq[b]) * 8260) >> 15;        /* one-pole smooth, ~20 ms (kSmoothMs) */
        fc = mFreq[b];

        turns_q16 = (fc << 16) / 44100;                        /* fc/fs as a Q16 fraction of a turn;
                                                                 * fc<<16 fits int32 up to ~32 kHz, no
                                                                 * 64-bit needed */
        ph0 = (uint32_t)turns_q16 << 16;                       /* -> the sine()/oscillator phase domain */
        sw = sine(ph0);                                        /* sin(w0), Q15 */
        cw = sine(ph0 + (1u << 30));                           /* cos(w0): a quarter turn ahead */

        alpha = (sw * 5) / res_eff;                            /* sin(w0) / (2*Q), Q15 */
        a0 = 32768 + alpha;                                    /* 1 + alpha, Q15 */
        {
            int32_t ratio = (alpha << 15) / a0;                /* alpha/a0, Q15 -- one precise division;
                                                                 * dividing straight into a Q13-ish result
                                                                 * (the old code) truncated alpha/a0 itself
                                                                 * to a handful of bits at high Q (e.g. a
                                                                 * true b0 of 8.5 rounded to 8, a ~6% error
                                                                 * that the resonant feedback then amplifies
                                                                 * into an audible ~27% volume loss at Q=50
                                                                 * -- confirmed by measuring the fixed-point
                                                                 * filter's own gain at resonance offline) */
            cb0[b] = (ratio * band_gain_q8[b]) >> 8;           /* * gain, Q15 */
        }
        cb2[b] = -cb0[b];
        ca1[b] = (-65536 * cw) / a0;                            /* -2*cos(w0)/a0, Q15 */
        ca2[b] = ((32768 - alpha) * 32768) / a0;                /* (1-alpha)/a0, Q15 */
    }
}

static int32_t formant(int32_t x, const int32_t cb0[NB], const int32_t cb2[NB],
                        const int32_t ca1[NB], const int32_t ca2[NB])
{
    uint32_t b;
    int32_t out = 0;
    for (b = 0; b < NB; b++) {
        int32_t x1 = fz1x[b], x2 = fz2x[b], y1 = fz1y[b], y2 = fz2y[b];
        /* Q15 coefficients: the pre-shift sum can exceed INT32_MAX (e.g. |a1|+|a2| alone can approach
         * 3*32768*32767), so it needs int64 here -- the shift itself is still free (no 64-bit division
         * anywhere, docs/RULES.md). Each band's own output narrows back to the Q15 sample range right
         * after, so the per-band sum below (out) never needs to be anything but int32. */
        int32_t y = (int32_t)(((int64_t)cb0[b] * x + (int64_t)cb2[b] * x2
                               - (int64_t)ca1[b] * y1 - (int64_t)ca2[b] * y2) >> CQ);
        fz2x[b] = x1; fz1x[b] = x;
        fz2y[b] = y1; fz1y[b] = y;
        out += y;
    }
    return out > 32767 ? 32767 : out < -32767 ? -32767 : out;
}

/* ---- CPU budget measurement: docs/RULES.md says app_audio() has 5.8 ms per 256-frame block (less
 * in practice) and to stay under half (2.9 ms); the way to know is to measure, not calculate, with
 * fm1_ticks() (a free-running 24 MHz counter, docs/API.md). Host-only build has neither the counter
 * nor a real ISR budget, so this is hardware-only. dbg_us_max is the worst block seen since boot;
 * shown on screen by draw_isr() below. */
#ifndef DINGHY_HOST
static volatile uint32_t dbg_us_now, dbg_us_max;
#endif

static void app_audio(int32_t *out, uint32_t n)
{
    uint32_t i;
    uint32_t ph = voice[0].ph, step = voice[0].inc, tgt = voice[0].tgt;
    int32_t amp = voice[0].amp, g = voice[0].gate;
    int32_t cb0[NB], cb2[NB], ca1[NB], ca2[NB];
    int32_t resGain, rg, rg_step, ae, ae_step;
    static int32_t rg_last = 256;               /* last block's resonance gain, Q8 */
    static int32_t lp1, lp2, lk_last = 16384;   /* the Cutoff low-pass: state, last block's factor */
    int32_t lk, lk_step;
#ifndef DINGHY_HOST
    uint32_t t0 = fm1_ticks();
#endif
    if (!amp && !g) {                           /* released and faded out -- but the resonant filter
                                                   * may still ring: run on until it has died away, or
                                                   * freezing it mid-ring would cut the sound off with a
                                                   * click, and the next note would restart that frozen
                                                   * ring with a second one */
        uint32_t b;
        int32_t ring = 0;
        for (b = 0; b < NB; b++) {
            int32_t y1 = fz1y[b] < 0 ? -fz1y[b] : fz1y[b], y2 = fz2y[b] < 0 ? -fz2y[b] : fz2y[b];
            if (y1 > 2 || y2 > 2) ring = 1;
        }
        if (lp2 > 2 || lp2 < -2) ring = 1;
        if (!ring) {                            /* silent: costs nothing; start the next note from rest */
            for (b = 0; b < NB; b++) fz1x[b] = fz2x[b] = fz1y[b] = fz2y[b] = 0;
            lp1 = lp2 = 0;
            return;
        }
    }
    ae = amp_env;                                /* Amp Decay: a ramp across the block from where it
                                                   * was, so a restart fades up instead of jumping */
    if (env_trig) { env_trig = 0; mod_env = 32767; amp_env = 32767; }   /* a new note: restart */
    if (par.ampdec < 1000) amp_env = (amp_env * decay_coef(par.ampdec)) >> 15;
    ae_step = (amp_env - ae) / (int32_t)n;
    formant_update_coeffs(cb0, cb2, ca1, ca2);   /* control rate: once per 256-frame block */
    resGain = res_gain_q8[res_eff / 10];         /* resonance volume compensation, see the table above */
    rg = rg_last << 8;                           /* ramped across the block too (Q16): an accent's Q
                                                   * jump would otherwise step the output gain at once */
    rg_step = ((resGain - rg_last) << 8) / (int32_t)n;
    rg_last = resGain;
    {                                            /* Cutoff (TB-303 style): log2 Hz in Q8, 60 Hz (1512)
                                                   * .. 18 kHz across the knob, Env Mod adding up to 4
                                                   * octaves (1024); from ~16 kHz up the filter is off */
        int32_t l2 = 1512 + par.cutoff * 2107 / 1000 + ((par.envmod * 1024 / 1000) * mod_env >> 15);
        int32_t k = 16384;                       /* Q14: 1 = passes everything */
        if (l2 < 3584) {
            int32_t w = exp2_fixed(l2) * 4669 / 1000;   /* 2 pi fc / fs, Q15 */
            k = (w << 14) / (32768 + w);         /* one-pole factor w / (1 + w), stays below 1 */
        }
        lk = lk_last << 8;                       /* ramped across the block like rg */
        lk_step = ((k - lk_last) << 8) / (int32_t)n;
        lk_last = k;
    }
    if (!voice[0].glide) step = tgt;
    for (i = 0; i < n; i++) {
        int32_t saw, s;
        if (step != tgt) {                       /* slide: one-pole towards tgt, ~12 ms time constant
                                                   * (2^9 frames), on pitch in ~50 ms like a 303; the
                                                   * difference of two pitches here always fits an int32 */
            int32_t d = (int32_t)(tgt - step) >> 9;
            step = d ? step + (uint32_t)d : tgt;
        }
        if (amp < g) { amp += 48; if (amp > g) amp = g; }        /* attack ~5 ms */
        else if (amp > g) { amp -= 6; if (amp < g) amp = g; }    /* release ~45 ms */
        ph += step;
        saw = (int32_t)ph >> 16;                /* naive sawtooth, -32768..32767: harmonically rich
                                                   * source for the filter to shape -- a sine alone
                                                   * wouldn't show off formant filtering at all */
        saw = (saw * amp) >> 15;                /* envelope the excitation, then resonate it */
        s = formant(saw, cb0, cb2, ca1, ca2);
        lp1 += ((s - lp1) * (lk >> 8)) >> 14;   /* Cutoff: two one-poles, 12 dB/octave; |s| <= 32767
                                                   * here, so the products fit an int32 in Q14 */
        lp2 += ((lp1 - lp2) * (lk >> 8)) >> 14;
        lk += lk_step;
        s = lp2;
        s = (s * (rg >> 8)) >> 8;               /* resonance compensation */
        rg += rg_step;
        s = (s * (ae >> 3)) >> 12;              /* Amp Decay; ae in Q12 here: s can reach ~150000 after
                                                  * the resonance gain, and that times Q15 overflows */
        ae += ae_step;
        out[2u * i] += s;                       /* L */
        out[2u * i + 1u] += s;                  /* R */
    }
    voice[0].ph = ph;
    voice[0].inc = step;
    voice[0].amp = amp;
#ifndef DINGHY_HOST
    dbg_us_now = (fm1_ticks() - t0) / FM1_TICKS_PER_US;
    if (dbg_us_now > dbg_us_max) dbg_us_max = dbg_us_now;
#endif
}

/* ---- notes (main loop) */
static const uint32_t INC_C9[12] = {            /* MIDI 120..131 (C9..B9); lower octaves shift right */
    815363807u, 863847862u, 915214929u, 969636441u, 1027294024u, 1088380105u,
    1153098554u, 1221665363u, 1294309365u, 1371273005u, 1452813141u, 1539201906u};
static uint32_t ages, last_note = 255;
static int32_t glide_next;                     /* set around a play() call by the sequencer: glide to
                                                  * the new note instead of jumping (a 303 slide) */

static void play(uint32_t note, uint32_t vel)
{
    uint32_t v, pick = 0, best = 0xFFFFFFFFu;
    if (note > 127u || !vel) return;
    for (v = 0; v < NV; v++) {                  /* the same note, else a free voice, else the oldest */
        uint32_t score = voice[v].note == note && voice[v].gate ? 0u
                       : voice[v].gate ? 0x80000000u + voice[v].age : (uint32_t)voice[v].amp + 1u;
        if (score < best) { best = score; pick = v; }
    }
    voice[pick].note = (uint8_t)note;
    voice[pick].age = ++ages;
    voice[pick].glide = glide_next;              /* before tgt: the ISR never glides to a jump */
    if (!glide_next) env_trig = 1;               /* a new note restarts the envelope; a slide carries on */
    voice[pick].tgt = INC_C9[note % 12u] >> (10u - note / 12u);
    voice[pick].gate = (int32_t)(vel * 90u);    /* 127 -> 11430: conservative headroom before the formant filter */
    last_note = note;
}
static void stop(uint32_t note)
{
    uint32_t v;
    for (v = 0; v < NV; v++)
        if (voice[v].note == note)
            voice[v].gate = 0;
}
static int quiet(void)                          /* nothing sounds, not even a release */
{
    uint32_t v;
    for (v = 0; v < NV; v++)
        if (voice[v].gate || voice[v].amp) return 0;
    return 1;
}

/* ---- the screen: the inside of a throat on top -- its opening follows the vowel (X/Y), the note
 * sounding in it -- then the step strip, the keyboard, and the last knob moved and the octave. One
 * palette of flesh tones for all of it. */
#define C_BG     RGB(28, 6, 12)                 /* behind everything: almost black, a bit of red */
#define C_KEY    RGB(246, 205, 196)             /* white keys, notes in the strip: pale flesh */
#define C_BLK    RGB(92, 18, 36)                /* black keys: deep red */
#define C_LIT    RGB(232, 64, 88)               /* a key that sounds: raw red */
#define C_EMPTY  RGB(66, 22, 32)                /* a rest in the strip */
#define C_ACC    RGB(240, 120, 150)             /* an accented step: hot pink */
#define C_TEXT   RGB(232, 150, 160)
#define C_DIMT   RGB(140, 70, 82)
#define C_HOLE   RGB(15, 0, 5)
#define C_TONGUE RGB(220, 80, 100)
#define C_GROOVE RGB(190, 60, 85)
#define C_UVULA  RGB(210, 85, 105)
#define C_UVTIP  RGB(225, 95, 115)
#define C_TOOTH  RGB(245, 240, 225)
#define C_SHINE  RGB(255, 190, 200)
#define C_DEEP   RGB(90, 10, 30)                /* the throat's walls: deep down .. */
#define C_NEAR   RGB(232, 120, 135)             /* .. and at the edge */
#define KEY0 53u                                /* key 0 = F3 */
#define THROAT_H 114u                           /* screen rows 0..113; the step strip starts at 116 */
static int black(uint32_t note) { return (0x54Au >> (note % 12u)) & 1u; }

static uint16_t mix565(uint16_t a, uint16_t b, uint32_t t, uint32_t n)   /* colour a..b, t of n */
{
    int32_t ar = a >> 11, ag = (a >> 5) & 63, ab = a & 31, br = b >> 11, bg = (b >> 5) & 63, bb = b & 31;
    int32_t r = ar + (br - ar) * (int32_t)t / (int32_t)n, g = ag + (bg - ag) * (int32_t)t / (int32_t)n;
    int32_t bl = ab + (bb - ab) * (int32_t)t / (int32_t)n;
    return (uint16_t)(((uint32_t)r << 11) | ((uint32_t)g << 5) | (uint32_t)bl);
}

/* text blended onto whatever is already in the canvas (cv_text() blends onto black only), so it
 * can sit on the throat or on C_BG without dark fringes */
static int32_t cv_text_over(int32_t x, int32_t y, const felucca_font_t *f, const char *s, uint16_t c)
{
    for (; *s; s++) {
        uint32_t gi = glyph(f, (uint8_t)*s), gx, gy, w = f->bw[gi], bpr = (w + 1u) / 2u;
        const uint8_t *gd = f->data + f->off[gi];
        for (gy = 0; gy < f->h; gy++)
            for (gx = 0; gx < w; gx++) {
                uint32_t v = gd[gy * bpr + gx / 2u];
                int32_t px = x - f->pad + (int32_t)gx, py = y + (int32_t)gy + cv_oy;
                v = (gx & 1u) ? (v & 15u) : (v >> 4);
                if (v && (uint32_t)px < cv_w && (uint32_t)py < cv_h) {
                    uint16_t *d = &cv_px[(uint32_t)py * cv_w + (uint32_t)px];
                    *d = swap16(mix565(swap16(*d), c, v, 15u));
                }
            }
        x += f->adv[gi];
    }
    return x;
}

#ifndef DINGHY_HOST
static int lcd_idle(void) { return !lcd_busy || fm1_lcd_done(); }   /* the last blit has gone out */
#else
static int lcd_idle(void) { return 1; }        /* the host's screen is instant */
#endif

static void text_box(uint32_t x, uint32_t y, uint32_t w, const felucca_font_t *f, const char *s, uint16_t c)
{                                               /* like draw_text_box(), centred, on C_BG */
    cv_begin(w, f->h, C_BG);
    cv_oy = 0;
    cv_text_over(((int32_t)w - text_w(f, s)) / 2, 0, f, s, c);
    cv_blit(x, y);
}

static int32_t isqrt(int32_t n)                 /* floor(sqrt(n)), n >= 0 */
{
    int32_t r = 0, b = 1 << 30;
    while (b > n) b >>= 2;
    while (b) {
        if (n >= r + b) { n -= r + b; r = (r >> 1) + b; } else r >>= 1;
        b >>= 2;
    }
    return r;
}

static void fill_ellipse(int32_t cx, int32_t cy, int32_t rx, int32_t ry, uint16_t c)
{                                               /* one cv_rect() per canvas row it covers */
    int32_t y0 = cy - ry, y1 = cy + ry, y;
    if (ry < 1 || rx < 1) return;
    if (y0 < -cv_oy) y0 = -cv_oy;               /* only the rows of this strip */
    if (y1 > (int32_t)cv_h - 1 - cv_oy) y1 = (int32_t)cv_h - 1 - cv_oy;
    for (y = y0; y <= y1; y++) {
        int32_t dy = y - cy, hw = rx * isqrt((ry * ry - dy * dy) << 8) / (ry << 4);
        cv_rect(cx - hw, y, 2 * hw + 1, 1, c);
    }
}

static void note_name(char *s, uint32_t note)   /* "A#3", or "-" for none */
{
    static const char *const NAME[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    uint32_t i = 0;
    if (note < 128u) {
        const char *p = NAME[note % 12u];
        while (*p) s[i++] = *p++;
        if (note >= 12u) s[i++] = (char)('0' + note / 12u - 1u);
    } else
        s[i++] = '-';
    s[i] = 0;
}

/* A piece of the throat (screen columns x0..x0+w-1, rows row0..row0+h-1): a tunnel of rings from
 * pink at the edge to dark red deep down, its opening shaped by the vowel -- Y opens it (i/u low and
 * narrow, a high and wide), X rounds it (front i/e wide and flat, back o/u round) -- with the uvula
 * hanging over it (its tip swayed sideways by sway px), the tongue under it, teeth along the top and
 * the note in the opening. x, y in pm. */
static void draw_throat(uint32_t x0, uint32_t w, uint32_t row0, uint32_t h, int32_t x, int32_t y,
                        uint32_t note, int32_t sway)
{
    static const int8_t TOOTH_INSET[5] = {0, 0, 1, 2, 3};
    const int32_t cx = 120 - (int32_t)x0, cy = 60, RINGS = 7;   /* cx: in canvas columns */
    int32_t hh = 10 + 34 * y / 1000;            /* the opening: half height .. */
    int32_t hw = hh + (1000 - x) * (54 - hh > 0 ? 54 - hh : 0) / 1000;   /* .. and half width */
    int32_t k, ut, tt, r;
    char nm[8];
    cv_begin(w, h, C_BG);
    cv_oy = -(int32_t)row0;                     /* draw in screen rows */
    for (k = RINGS; k > 0; k--)                 /* outside in: the tunnel */
        fill_ellipse(cx, cy, hw + k * (150 - hw) / RINGS, hh + k * (80 - hh) / RINGS,
                     mix565(C_DEEP, C_NEAR, (uint32_t)k, (uint32_t)RINGS));
    for (k = 3; k <= 5; k += 2) {               /* wet shine: short thin arcs, upper left of two rings,
                                                  * only a quarter of the way from the ring's own colour
                                                  * to C_SHINE -- a hint, not a highlight */
        int32_t rw = hw + k * (150 - hw) / RINGS, rh = hh + k * (80 - hh) / RINGS, a;
        uint16_t c = mix565(mix565(C_DEEP, C_NEAR, (uint32_t)k, (uint32_t)RINGS), C_SHINE, 1u, 4u);
        for (a = 210; a <= 240; a++) {
            uint32_t ph = (uint32_t)a * 11930465u;          /* degrees -> the sine() phase */
            int32_t px = cx + rw * sine(ph + (1u << 30)) / 32767, py = cy + rh * sine(ph) / 32767;
            cv_rect(px, py, 1, 1, c);
        }
    }
    fill_ellipse(cx, cy, hw, hh, C_HOLE);       /* the opening */
    tt = cy + hh - 2;                           /* the tongue, its top just under the opening */
    fill_ellipse(cx, tt + 45, 70, 45, C_TONGUE);
    fill_ellipse(cx, tt + 33, 4, 27, C_GROOVE);
    ut = cy - hh - 2;                           /* the uvula, its tip just over the opening, or in front
                                                  * of it, halfway down, when it is wide open */
    if (ut < 30) ut = 30;
    for (r = 0; r < ut - 10; r++) {             /* hung at the top, swaying more towards the tip */
        int32_t half = 9 - 3 * r / (ut - 10), off = sway * r / (ut - 10);
        cv_rect(cx + off - half, r, 2 * half + 1, 1, C_UVULA);
    }
    fill_ellipse(cx + sway, ut - 10, 9, 10, C_UVTIP);
    for (k = 0; k < 10; k++)                    /* teeth along the top edge */
        for (r = 0; r < 11; r++) {
            int32_t in = r < 6 ? 0 : TOOTH_INSET[r - 6];
            cv_rect(4 + k * 47 / 2 - (int32_t)x0 + in, r, 20 - 2 * in, 1, C_TOOTH);
        }
    note_name(nm, note);                        /* the note, in the opening, with a dark shadow */
    {
        int32_t tx = cx - text_w(&FONT_L, nm) / 2, ty = cy - (int32_t)FONT_L.h / 2;
        cv_text_over(tx + 1, ty + 1, &FONT_L, nm, C_HOLE);
        cv_text_over(tx, ty, &FONT_L, nm, C_WHITE);
    }
    cv_blit(x0, row0);
    cv_oy = 0;
}

static void draw_keys(uint32_t lit)             /* bit k: key k sounds */
{
    uint32_t k, w = 0;
    cv_begin(240, 56, C_BG);
    for (k = 0; k < 27u; k++)                   /* 16 white keys of 14 px from x = 8 */
        if (!black(KEY0 + k)) {
            cv_rect(8 + (int32_t)w * 14, 0, 13, 56, lit >> k & 1u ? C_LIT : C_KEY);
            w++;
        }
    for (k = 0, w = 0; k < 27u; k++)            /* the black ones on the line between two white */
        if (black(KEY0 + k))
            cv_rect(8 + (int32_t)w * 14 - 5, 0, 9, 34, lit >> k & 1u ? C_LIT : C_BLK);
        else
            w++;
    cv_blit(0, 140);
}

/* ---- 16-step sequencer: BTN_REC toggles record (ENC_SELECT browses the 16 steps; a key records
 * its note into the cursor step and advances; hold BTN_GLO while pressing the key to mark that
 * step a slide -- it ties into the next step, which glides there, see seq_trigger()); hold BTN_ENV
 * the same way to accent it (louder, plus the accent_env sweep above); a tap on either alone
 * toggles it on the cursor step; BTN_SEL enters a rest (clears the step, moves on). BTN_PLAY runs the pattern
 * (BTN_ARP + BTN_PLAY: a random one, see seq_randomize()); hold BTN_SEQ + ENC_SELECT
 * sets the tempo. The pattern and tempo live in `set` (app_main()) so they survive a power-off,
 * same as the octave. A step with no note recorded into it plays as a rest (silence). */
#define NSTEP 16u

#ifndef DINGHY_HOST
#define RNG_SEED fm1_ticks()
#else
#define RNG_SEED 12345u
#endif
struct seq_step_s { uint8_t note, flags; };     /* flags: bit0 active, bit1 slide, bit2 accent, bits 3-5
                                                  * a vowel lock (0 none, 1..5 i e a o u) -- new bits in the
                                                  * same byte, so `set` keeps its size and old patterns load
                                                  * as they were (no accents, no vowels) */
#define STEP_VOW(f) ((uint32_t)(f) >> 3 & 7u)

/* As on a 303: a note sounds for half its step, then lets go (seq_release()); a slide step holds its
 * note into the next step instead, which then glides there in pitch without restarting the envelope.
 * So the slide bit on step N ties N to N+1 -- the bar draw_steps() runs into the next cell. */
static uint32_t seq_note = 255;                 /* the note the sequencer holds, 255 = none */
static int32_t seq_tie;                         /* that note's step was a slide: hold it into the next */

static void seq_release(void)                   /* half a step after a note, unless it ties */
{
    if (seq_note < 128u && !seq_tie) {
        stop(seq_note);                         /* stop() only touches the voice if it still plays
                                                  * seq_note, so a key played live meanwhile stays */
        midi_send(0x80, seq_note, 0);
        seq_note = 255;
    }
}

static void seq_halt(void)                      /* the sequencer stops: let go of everything */
{
    seq_tie = 0;
    seq_release();
}

static uint32_t uv_kick_ms = 0u - 1000u;       /* the last accent, for the uvula's wobble */

static void seq_trigger(const struct seq_step_s *s)   /* shared by the internal clock and MIDI clock */
{
    uint32_t prev = seq_note;
    if (s->flags & 1u) {
        uint32_t vel = s->flags & 4u ? 100u + 27u * (uint32_t)par.accent / 1000u : 100u;   /* accent: louder */
        int32_t slide = seq_tie && prev < 128u;        /* the step before tied into this one */
        if (!slide) voice[0].amp = 0;                  /* not a slide: hard retrigger */
        if (s->flags & 4u) { accent_env = 32767; uv_kick_ms = fm1_ms; }   /* the uvula wobbles too */
        glide_next = slide;
        play(s->note, vel);
        glide_next = 0;
        if (prev != s->note) {
            midi_send(0x90, s->note, vel);
            if (prev < 128u) midi_send(0x80, prev, 0);  /* after the new note-on: legato */
        } else if (!slide) {
            midi_send(0x80, prev, 0);                    /* same note again, retriggered */
            midi_send(0x90, s->note, vel);
        }                                                /* same note tied: it just goes on */
        seq_note = s->note;
        vow_lock = (int32_t)STEP_VOW(s->flags);        /* the step's vowel, or back to the knobs */
        seq_tie = (s->flags & 2u) != 0;
    } else {
        seq_halt();                                    /* an empty step is a rest, and ends a tie */
    }
}

/* ARP + PLAY: a random pattern, acid style -- notes from the chosen scale over the chosen root, two
 * octaves up, the root and its octave the most likely; step 1 always the root, so the groove has an
 * anchor; about a quarter rests, a fifth slides, a quarter accents. ARP + a key picks the root (and
 * makes a pattern on it), ARP + SELECT the scale. (ARP, not SEQ: SEQ sits right next to PLAY.) Plain xorshift, seeded by the free-running tick
 * counter at the first press, so every power-on gives other patterns. */
#define NSCALE 6u
static const uint16_t SCALE_MASK[NSCALE] = {    /* bit n: n semitones over the root is in the scale */
    0x0000u | 1u << 0 | 1u << 3 | 1u << 5 | 1u << 7 | 1u << 10,                       /* minor pentatonic */
    0x0000u | 1u << 0 | 1u << 2 | 1u << 3 | 1u << 5 | 1u << 7 | 1u << 8 | 1u << 10,   /* natural minor */
    0x0000u | 1u << 0 | 1u << 1 | 1u << 3 | 1u << 5 | 1u << 7 | 1u << 8 | 1u << 10,   /* phrygian */
    0x0000u | 1u << 0 | 1u << 2 | 1u << 3 | 1u << 5 | 1u << 7 | 1u << 9 | 1u << 10,   /* dorian */
    0x0000u | 1u << 0 | 1u << 2 | 1u << 4 | 1u << 5 | 1u << 7 | 1u << 9 | 1u << 11,   /* major */
    0x0FFFu,                                                                          /* chromatic */
};
static uint32_t seq_rng;                        /* own state: libc.c's rng() isn't in the host build */
static uint32_t rnd(uint32_t n)                 /* 0 .. n-1 */
{
    if (!seq_rng) seq_rng = RNG_SEED | 1u;
    seq_rng ^= seq_rng << 13; seq_rng ^= seq_rng >> 17; seq_rng ^= seq_rng << 5;
    return seq_rng % n;
}
static void seq_randomize(struct seq_step_s *step, uint32_t root, uint32_t scale)
{
    uint32_t k;
    for (k = 0; k < NSTEP; k++) {
        uint32_t f = 0;
        if (k == 0 || rnd(100) >= 25u) {        /* a note (else a rest) */
            uint32_t r = rnd(100), d = 0, tries;
            f = 1u;
            if (rnd(100) < 20u) f |= 2u;        /* slide */
            if (rnd(100) < 25u) f |= 4u;        /* accent */
            if (k == 0 || r < 30u) d = 0;       /* the root .. */
            else if (r < 45u) d = 12;           /* .. its octave .. */
            else                                /* .. or any note of the scale in the two octaves */
                for (tries = 0; tries < 32u; tries++) {
                    d = rnd(25);
                    if (SCALE_MASK[scale] >> (d % 12u) & 1u) break;
                }
            step[k].note = (uint8_t)(root + d > 127u ? root : root + d);
        } else
            step[k].note = 0;
        step[k].flags = (uint8_t)f;
    }
}

static uint32_t white_key_id(uint32_t n)        /* physical key index of the n-th white key (0..15) --
                                                  * same white keys, same count, as draw_keys() lights */
{
    uint32_t k, w = 0;
    for (k = 0; k < 27u; k++)
        if (!black(KEY0 + k)) {
            if (w == n) return k;
            w++;
        }
    return 0;
}

/* The pattern as a strip of 16 cells right above the keyboard drawing, each over its own white key
 * (same x as draw_keys()): dark = rest, pale = a note, pink = an accented note; a bar under a cell
 * running into the next one = a slide (legato into the next step); a red tick under it = the
 * record cursor, or the step sounding while the pattern plays (the two never show at once). */
static const uint16_t TINY_VOW[NVOW] = {       /* i e a o u, 3 x 5 px, rows top down, 3 bits each */
    0x2u << 12 | 0x0u << 9 | 0x2u << 6 | 0x2u << 3 | 0x2u,                               /* i */
    0x2u << 12 | 0x5u << 9 | 0x7u << 6 | 0x4u << 3 | 0x3u,                               /* e */
    0x6u << 12 | 0x1u << 9 | 0x3u << 6 | 0x5u << 3 | 0x3u,                               /* a */
    0x0u << 12 | 0x2u << 9 | 0x5u << 6 | 0x5u << 3 | 0x2u,                               /* o */
    0x0u << 12 | 0x5u << 9 | 0x5u << 6 | 0x5u << 3 | 0x3u,                               /* u */
};
static void draw_steps(const struct seq_step_s *step, int32_t mark)   /* mark: -1 = none */
{
    uint32_t n;
    cv_begin(240, 24, C_BG);
    for (n = 0; n < NSTEP; n++) {
        int32_t x = 8 + (int32_t)n * 14;
        uint32_t f = step[n].flags;
        cv_rect(x, 0, 13, 15, !(f & 1u) ? C_EMPTY : f & 4u ? C_ACC : C_KEY);
        if (STEP_VOW(f)) {                      /* its vowel, as a 3 x 5 pixel letter */
            uint32_t r, c, g = TINY_VOW[STEP_VOW(f) - 1u];
            for (r = 0; r < 5u; r++)
                for (c = 0; c < 3u; c++)
                    if (g >> (14u - r * 3u - c) & 1u)
                        cv_rect(x + 5 + (int32_t)c, 5 + (int32_t)r, 1, 1, f & 1u ? C_BLK : C_KEY);
        }
        if ((f & 3u) == 3u)
            cv_rect(x, 17, n + 1u < NSTEP ? 14 : 13, 3, C_KEY);
        if ((int32_t)n == mark)
            cv_rect(x + 3, 21, 7, 3, C_LIT);
    }
    cv_blit(0, 116);
}

#ifndef DINGHY_HOST                             /* fmt_int is in libc.c, only built for real hardware
                                                  * (tests/host.c stands in for platform.c on a computer
                                                  * and doesn't pull it in -- and the host build's
                                                  * fm1_enc_take() stub always returns 0 anyway, so
                                                  * touched_id would never leave its sentinel there) */
static const char *const PNAME[] = {"SIZE", "RES", "VOICE", "CUTOFF", "X", "Y",
                                     "X RATE", "X DEPTH", "Y RATE", "Y DEPTH", "VOWEL", "BPM", "STEP",
                                     "DECAY", "ENV MOD", "ACCENT", "AMP DECAY", "RANDOM", "SCALE", "ROOT", "STEP VOWEL"};
static void draw_touched(uint32_t id, int32_t val)    /* last knob moved: name + value, replaces the
                                                        * name shown at power-on */
{
    char s[24], v[8];
    const char *p = PNAME[id];
    uint32_t i = 0, j;
    static const char *const SCALE_NAME[NSCALE] = {"MIN PENTA", "MINOR", "PHRYGIAN", "DORIAN", "MAJOR", "CHROMATIC"};
    while (*p) s[i++] = (uint8_t)*p++;
    s[i++] = ' ';
    if (id == 18) {                             /* a scale by name, a root by note */
        for (p = SCALE_NAME[val]; *p; ) s[i++] = *p++;
    } else if (id == 19) {
        note_name(v, (uint32_t)val);
        for (j = 0; v[j]; j++) s[i++] = v[j];
    } else if (id == 10 || id == 20) {          /* a vowel by letter; a step's: 0 = none, shown as - */
        s[i++] = id == 20 && !val ? '-' : "ieaou"[id == 20 ? val - 1 : val];
    } else {
        fmt_int(v, val);
        for (j = 0; v[j]; j++) s[i++] = v[j];
    }
    s[i] = 0;
    text_box(0, 199, 240, &FONT_S, s, C_TEXT);
}
#endif

static void draw_status(int32_t oct)
{
    char s[12] = "OCTAVE +0";
    s[7] = oct < 0 ? '-' : '+';
    s[8] = (char)('0' + (oct < 0 ? -oct : oct));
    text_box(0, 220, 120, &FONT_S, s, C_DIMT);     /* the left half of the bottom line */
}

#ifndef DINGHY_HOST
static void draw_isr(uint32_t now, uint32_t max, uint32_t draw_ms)   /* app_audio() time in us, now/worst
                                                      * since boot, vs the 2.9 ms safe half-budget from
                                                      * docs/RULES.md; D = the slowest throat strip, ms */
{
    char s[28] = "ISR ", a[6], b[6], c[6];
    uint32_t i = 4, j;
    fmt_int(a, (int32_t)now);
    fmt_int(b, (int32_t)max);
    fmt_int(c, (int32_t)draw_ms);
    for (j = 0; a[j]; j++) s[i++] = a[j];
    s[i++] = '/'; for (j = 0; b[j]; j++) s[i++] = b[j];
    s[i++] = ' '; s[i++] = 'D'; for (j = 0; c[j]; j++) s[i++] = c[j];
    s[i] = 0;
    text_box(120, 220, 120, &FONT_S, s, max > 2900u ? C_LIT : C_DIMT);   /* right half; red past the 2.9 ms safe half-budget */
}
#endif

#ifndef DINGHY_HOST
static void led_dim(uint32_t id, int on)        /* like fm1_led_key(), but the glow layer (~1/24
                                                  * brightness, docs/API.md): a soft always-on landmark
                                                  * for BTN_LFO, with the normal fm1_led_key() on top
                                                  * while held -- "an LED in several layers takes the
                                                  * brightest" (fm1_input.h), so held always wins. */
{
    uint32_t p, r;
    for (p = 0; p < FM1_NCOL; p++)
        for (r = 1; r < 5u; r++)
            if (FM1_KEYMAP[r][p] == (int8_t)id) {
                if (on) fm1_led_dim[p] |= (uint8_t)(1u << r);
                else    fm1_led_dim[p] &= (uint8_t)~(1u << r);
            }
}

static void led_bg(uint32_t id, int on)         /* the third brightness layer (fm1_led_bg[], docs/API.md):
                                                  * its one shared pulse width is set once, in app_main(),
                                                  * to land visibly between led_dim()'s glow and a full
                                                  * fm1_led_key() -- a middle tier for "this step has a
                                                  * note" vs the cursor's dim glow vs a key actually
                                                  * sounding right now at full brightness; same "brightest
                                                  * layer wins" compositing as led_dim() */
{
    uint32_t p, r;
    for (p = 0; p < FM1_NCOL; p++)
        for (r = 1; r < 5u; r++)
            if (FM1_KEYMAP[r][p] == (int8_t)id) {
                if (on) fm1_led_bg[p] |= (uint8_t)(1u << r);
                else    fm1_led_bg[p] &= (uint8_t)~(1u << r);
            }
}
#endif

/* ---- controls: 7 encoders, no menus -- ENC_KNOB1..4 do size/res/voice/cutoff normally and the
 * 4 LFO params while BTN_LFO is held (a plain if on the button state every time a knob is read,
 * not a separate mode to enter/leave: release the button and the same encoder is back to its
 * normal job, same block). ENC_ALGORITHM/ENC_SELECT are direct X/Y; ENC_PRESETS jumps to a
 * reference vowel. touched_id/touched_val remember the last knob moved, for draw_touched(). */
static uint32_t touched_id = ~0u;
static int32_t touched_val;
static int32_t vowel_sel = 2;                   /* starts on "a", matching the old fixed-vowel prototype */

static void knob(int32_t *p, int32_t d, int32_t step, int32_t lo, int32_t hi, uint32_t id)
{
    if (!d) return;
    *p += d * step;
    if (*p < lo) *p = lo; else if (*p > hi) *p = hi;
    touched_id = id;
    touched_val = *p;
}

static void app_main(void)
{
    struct {                                    /* what survives a power-off (up to 256 bytes) */
        int8_t oct;
        int16_t bpm;
        struct seq_step_s step[NSTEP];
        uint8_t root, scale;                    /* the randomizer's: a MIDI note, an index into SCALE_MASK */
    } set = {0};
    uint32_t prev = 0, k, t_change = 0, dirty = 0, lit_drawn = ~0u, note_drawn = ~0u;
    uint32_t tap_pending = 0;                   /* GLO/ENV pressed, not yet used as a modifier */
    uint32_t randoms = 0;                       /* ARP + PLAY presses, shown as RANDOM n */
    int32_t thr_x = -100, thr_y = -100;         /* the throat as last drawn .. */
    uint32_t thr_ms = 0, thr_row = THROAT_H;    /* .. when, and the next row of a frame being drawn */
    int32_t thr_sway = 0;                       /* .. and the uvula's sway in it */
    uint32_t uv_ms = 0;                         /* the uvula last redrawn on its own */
    int32_t oct_drawn = 99, mark_drawn = -2;
    uint8_t flags_drawn[NSTEP] = {0};           /* what draw_steps() last showed; mark_drawn -2 forces
                                                  * the first draw */
    int32_t rec_mode = 0, rec_step = 0, seq_running = 0, seq_step = 0;
    uint32_t seq_next_ms = 0, seq_off_ms = 0;  /* next step; when the note of this one lets go */
    uint32_t midiclk_last_ms = 0u - 1000u, midiclk_count = 0;   /* "ages ago": starts on the internal clock */
#ifndef DINGHY_HOST
    uint32_t touched_id_drawn = ~0u;
    int32_t touched_val_drawn = 0;
    int32_t rec_step_drawn = -1;
#endif
#ifndef DINGHY_HOST
    uint32_t isr_now_drawn = ~0u, isr_max_drawn = ~0u, thr_max_ms = 0, thr_max_drawn = ~0u, thr_t0 = 0;
#define THR_T0 (thr_t0 = fm1_ticks())         /* time one throat strip, CPU and the wait for the last blit */
#define THR_T1 do { uint32_t ms_ = (fm1_ticks() - thr_t0) / (FM1_TICKS_PER_US * 1000u); \
                    if (ms_ > thr_max_ms) thr_max_ms = ms_; } while (0)
#else
#define THR_T0 ((void)0)
#define THR_T1 ((void)0)
#endif
    uint8_t held[27] = {0};                     /* the note each key plays */
    settings_load(&set, sizeof set);
    if (set.oct < -2 || set.oct > 2) set.oct = 0;
    if (set.bpm < 40 || set.bpm > 300) set.bpm = 120;
    if (set.root < 24 || set.root > 108) set.root = 48;      /* C3 */
    if (set.scale >= NSCALE) set.scale = 0;

    lcd_fill(0, 0, 240, 240, C_BG);
    text_box(0, 199, 240, &FONT_S, APP_NAME "  v" APP_VER, C_TEXT);   /* until a knob is touched */
#ifndef DINGHY_HOST
    led_dim(BTN_LFO, 1);                        /* soft glow, always -- fm1_led_key(BTN_LFO, ...) above
                                                  * goes full bright on top of it while held */
    led_dim(BTN_ENV, 1);                        /* same: always does something (the envelope knobs) */
    led_dim(BTN_SEQ, 1);                        /* and SEQ (+ SELECT: the tempo) .. */
    led_dim(BTN_ARP, 1);                        /* .. and ARP: the randomizer's shift (random pattern,
                                                  * root, scale) */
                                                /* BTN_GLO glows only in record mode, where it does
                                                  * something: set per pass in the main loop below */
    fm1_led_bg_ns = 16000u;                     /* the shared pulse width for led_bg()'s middle tier --
                                                  * between led_dim()'s ~1/24 glow and a full LED; tune
                                                  * by eye if "filled step" isn't clearly distinct */
#endif

    for (;;) {
        uint32_t keys, down, up, rel, b, lit = 0, base = (uint32_t)(KEY0 + 12 * set.oct);
        uint8_t st, d1, d2;
        platform_poll();

        keys = fm1_in.notes;                    /* bit k: key k (0 = F3 .. 26 = G5) */
        down = keys & ~prev;
        up = prev & ~keys;
        prev = keys;
        for (k = 0; k < 27u; k++) {                 /* (a key stops the note it started: the octave may change meanwhile) */
            if (down >> k & 1u) {
                held[k] = (uint8_t)(base + k);
                play(held[k], 100);                 /* always sounds live, recording or not -- you hear
                                                      * what you're entering */
                midi_send(0x90, held[k], 100);
                if (fm1_in.buttons & (1u << BTN_ARP)) {  /* ARP + a key: the randomizer's root, and a
                                                      * pattern on it right away (not recorded) */
                    set.root = held[k];
                    seq_randomize(set.step, set.root, set.scale);
                    dirty = 1; t_change = fm1_ms;
                    touched_id = 19; touched_val = set.root;
                } else if (rec_mode) {              /* record into the cursor step, then advance it */
                    set.step[rec_step].note = held[k];
                    set.step[rec_step].flags = (uint8_t)((set.step[rec_step].flags & 0x38u)   /* keeps its vowel */
                                                            | 1u | (fm1_in.buttons & (1u << BTN_GLO) ? 2u : 0u)
                                                            | (fm1_in.buttons & (1u << BTN_ENV) ? 4u : 0u));
                    rec_step = (int32_t)((rec_step + 1) % (int32_t)NSTEP);
                    tap_pending = 0;                /* GLO/ENV held through this: a modifier, not a tap */
                    dirty = 1; t_change = fm1_ms;
                    touched_id = 12; touched_val = rec_step;
                }
            }
            if (up >> k & 1u) { stop(held[k]); midi_send(0x80, held[k], 0); }
        }

        b = fm1_input_edges(&rel);              /* buttons pressed since the last call (bit = button id) */
        if (b & (1u << BTN_OCT_DOWN) && set.oct > -2) { set.oct--; dirty = 1; t_change = fm1_ms; }
        if (b & (1u << BTN_OCT_UP) && set.oct < 2) { set.oct++; dirty = 1; t_change = fm1_ms; }
        /* GLO/ENV tapped on their own in record mode toggle slide/accent on the cursor step (note and
         * cursor unchanged). On release, not press: held while a key records or while SELECT moves the
         * cursor, they are a modifier instead, and recording/moving cancels the toggle (tap_pending) */
        tap_pending |= b & (1u << BTN_GLO | 1u << BTN_ENV);
        if (rec_mode && (set.step[rec_step].flags & 1u) && (rel & tap_pending)) {
            if (rel & tap_pending & (1u << BTN_GLO)) set.step[rec_step].flags ^= 2u;
            if (rel & tap_pending & (1u << BTN_ENV)) set.step[rec_step].flags ^= 4u;
            dirty = 1; t_change = fm1_ms;
        }
        tap_pending &= ~rel;
        if (rec_mode && (b & (1u << BTN_SEL))) {   /* SEL in record mode: a rest -- clears the cursor
                                                   * step (note, slide, accent) and moves on, the same
                                                   * way a key records a note and moves on */
            set.step[rec_step].note = 0;
            set.step[rec_step].flags = 0;
            rec_step = (int32_t)((rec_step + 1) % (int32_t)NSTEP);
            tap_pending = 0;
            dirty = 1; t_change = fm1_ms;
            touched_id = 12; touched_val = rec_step;
        }
        if (b & (1u << BTN_REC)) {              /* toggle record mode: stop playback, reset the cursor */
            rec_mode = !rec_mode;
            if (rec_mode) { seq_running = 0; rec_step = 0; stop((uint32_t)voice[0].note); seq_halt(); }
#ifndef DINGHY_HOST
            else if (rec_step_drawn >= 0) { led_dim(KEY_LED(white_key_id((uint32_t)rec_step_drawn)), 0); rec_step_drawn = -1; }
#endif
        }
        if (b & (1u << BTN_PLAY) && (fm1_in.buttons & (1u << BTN_ARP))) {  /* ARP + PLAY: a random pattern,
                                                   * playing on as it was -- heard from the next step */
            seq_randomize(set.step, set.root, set.scale);
            dirty = 1; t_change = fm1_ms;
            touched_id = 17; touched_val = (int32_t)++randoms;
        } else if (b & (1u << BTN_PLAY) && !rec_mode) {   /* toggle the sequencer; can't run while recording */
            seq_running = !seq_running;
            if (seq_running) { seq_step = 0; seq_next_ms = fm1_ms; }
            else seq_halt();
        }

        {                                        /* 7 knobs, no menus (see the comment above knob()) */
            int32_t lfo = (int32_t)(fm1_in.buttons & (1u << BTN_LFO));
            int32_t env = (int32_t)(fm1_in.buttons & (1u << BTN_ENV));   /* BTN_ENV held: the envelope,
                                                                       * same idea as BTN_LFO */
            int32_t seqShift = (int32_t)(fm1_in.buttons & (1u << BTN_SEQ));
            int32_t d;
            d = fm1_enc_take(ENC_KNOB1);
            if (env && d) tap_pending = 0;      /* ENV used as a shift: not a tap (record mode) */
            if (lfo) knob(&par.lfoXRate,  d, 20, 0, 1000, 6); else if (env) knob(&par.decay,  d, 20, 0, 1000, 13);
            else knob(&par.size,   d, 20,  500, 2000, 0);
            d = fm1_enc_take(ENC_KNOB2);
            if (env && d) tap_pending = 0;
            if (lfo) knob(&par.lfoXDepth, d, 20, 0, 1000, 7); else if (env) knob(&par.envmod, d, 20, 0, 1000, 14);
            else knob(&par.resx10, d, 10,   10,  500, 1);
            d = fm1_enc_take(ENC_KNOB3);
            if (env && d) tap_pending = 0;
            if (lfo) knob(&par.lfoYRate,  d, 20, 0, 1000, 8); else if (env) knob(&par.accent, d, 20, 0, 1000, 15);
            else knob(&par.voice,  d, 20,    0, 1000, 2);
            d = fm1_enc_take(ENC_KNOB4);
            if (env && d) tap_pending = 0;
            if (lfo) knob(&par.lfoYDepth, d, 20, 0, 1000, 9); else if (env) knob(&par.ampdec, d, 20, 0, 1000, 16);
            else knob(&par.cutoff, d, 20,    0, 1000, 3);
            knob(&par.x, fm1_enc_take(ENC_ALGORITHM), 20, 0, 1000, 4);
            d = fm1_enc_take(ENC_SELECT);        /* Y normally; steps while recording; tempo under SEQ;
                                                   * the randomizer's scale under ARP */
            if (d) {
                if (fm1_in.buttons & (1u << BTN_ARP)) {
                    int32_t sc = (int32_t)set.scale + (d > 0 ? 1 : -1);
                    set.scale = (uint8_t)(sc < 0 ? 0 : sc >= (int32_t)NSCALE ? (int32_t)NSCALE - 1 : sc);
                    dirty = 1; t_change = fm1_ms;
                    touched_id = 18; touched_val = set.scale;
                } else if (rec_mode) {
                    rec_step += d;
                    tap_pending = 0;
                    if (rec_step < 0) rec_step = 0; else if (rec_step >= (int32_t)NSTEP) rec_step = (int32_t)NSTEP - 1;
                    touched_id = 12; touched_val = rec_step;
                } else if (seqShift) {
                    set.bpm = (int16_t)(set.bpm + d * 2);
                    if (set.bpm < 40) set.bpm = 40; else if (set.bpm > 300) set.bpm = 300;
                    dirty = 1; t_change = fm1_ms;
                    touched_id = 11; touched_val = set.bpm;
                } else {
                    knob(&par.y, d, 20, 0, 1000, 5);
                }
            }
            d = fm1_enc_take(ENC_PRESETS);       /* a reference vowel; in record mode the cursor step's */
            if (d && rec_mode) {
                int32_t v = (int32_t)STEP_VOW(set.step[rec_step].flags) + (d > 0 ? 1 : -1);
                v = v < 0 ? 0 : v > (int32_t)NVOW ? (int32_t)NVOW : v;
                set.step[rec_step].flags = (uint8_t)((set.step[rec_step].flags & ~0x38u) | (uint32_t)v << 3);
                tap_pending = 0;
                dirty = 1; t_change = fm1_ms;
                touched_id = 20; touched_val = v;
            } else if (d) {
                vowel_sel += d;
                if (vowel_sel < 0) vowel_sel = 0; else if (vowel_sel >= (int32_t)NVOW) vowel_sel = (int32_t)NVOW - 1;
                par.x = vowel_x[vowel_sel]; par.y = vowel_y[vowel_sel];
                touched_id = 10; touched_val = vowel_sel;
            }
        }

        while (midi_get(&st, &d1, &d2)) {       /* USB-MIDI in, any channel */
            if ((st & 0xF0u) == 0x90u && d2) play(d1, d2);
            else if ((st & 0xF0u) == 0x80u || (st & 0xF0u) == 0x90u) stop(d1);
            else if (st == 0xF8u) {             /* MIDI clock, 24 per quarter note: 6 ticks = one 16th */
                midiclk_last_ms = fm1_ms;
                seq_next_ms = fm1_ms + (uint32_t)(15000 / set.bpm);   /* keep the fallback timer from
                                                                       * firing a catch-up burst if the
                                                                       * external clock later drops out */
                if (seq_running && !rec_mode && ++midiclk_count >= 6u) {
                    midiclk_count = 0;
                    seq_trigger(&set.step[seq_step]);
                    seq_step = (seq_step + 1) % (int32_t)NSTEP;
                } else if (seq_running && !rec_mode && midiclk_count == 3u) {
                    seq_release();              /* half a 16th: the note lets go unless it ties */
                }
            } else if (st == 0xFAu) { seq_running = 1; seq_step = 0; midiclk_count = 0; }   /* start */
            else if (st == 0xFBu) { seq_running = 1; }                                       /* continue */
            else if (st == 0xFCu) { seq_running = 0; seq_halt(); }                            /* stop */
        }

        /* one sequencer step at the internal tempo -- only when no external MIDI clock has ticked in
         * the last 500 ms; while a DAW/clock is actively sending 0xF8, that drives stepping instead
         * (above), so the two never double-trigger */
        if (seq_running && !rec_mode && fm1_ms - midiclk_last_ms > 500u && fm1_ms >= seq_next_ms) {
            seq_next_ms += (uint32_t)(15000 / set.bpm);   /* 60000 / bpm / 4; no drift: += not = */
            seq_off_ms = fm1_ms + (uint32_t)(7500 / set.bpm);   /* half a step */
            seq_trigger(&set.step[seq_step]);
            seq_step = (seq_step + 1) % (int32_t)NSTEP;
        }
        if (seq_running && fm1_ms - midiclk_last_ms > 500u && (int32_t)(fm1_ms - seq_off_ms) >= 0)
            seq_release();                       /* internal clock: the note lets go unless it ties */

        base = (uint32_t)(KEY0 + 12 * set.oct);
        for (k = 0; k < NV; k++)                 /* what actually sounds, on the keys of this octave --
                                                   * always full fm1_led_key() brightness, in both modes:
                                                   * recording still monitors live */
            if (voice[k].gate && voice[k].note >= base && voice[k].note < base + 27u)
                lit |= 1u << (voice[k].note - base);
        for (k = 0; k < 27u; k++)
            fm1_led_key(KEY_LED(k), lit >> k & 1u);
#ifndef DINGHY_HOST
        if (rec_mode)                            /* layer "has a note" underneath: led_bg's middle tier
                                                   * loses to a sounding key's full brightness above, so
                                                   * a step that is both filled and sounding still reads
                                                   * as "sounding", same "brightest wins" compositing */
            for (k = 0; k < NSTEP; k++)
                led_bg(KEY_LED(white_key_id(k)), (set.step[k].flags & 1u) != 0);
#endif
        fm1_led_key(BTN_OCT_DOWN, set.oct < 0);
        fm1_led_key(BTN_OCT_UP, set.oct > 0);
        fm1_led_key(BTN_REC, rec_mode);
        fm1_led_key(BTN_PLAY, seq_running);
        if (rec_mode) vow_lock = (int32_t)STEP_VOW(set.step[rec_step].flags);   /* hear and see the step */
        else if (!seq_running) vow_lock = 0;
#ifndef DINGHY_HOST
        if (rec_mode && rec_step != rec_step_drawn) {   /* the cursor's own soft glow among the step LEDs */
            if (rec_step_drawn >= 0) led_dim(KEY_LED(white_key_id((uint32_t)rec_step_drawn)), 0);
            led_dim(KEY_LED(white_key_id((uint32_t)rec_step)), 1);
            rec_step_drawn = rec_step;
        }
        {                                        /* GLO = slide, ENV = accent: GLO glows softly in record
                                                   * mode (ENV always, see above); full while held (GLO:
                                                   * in record mode), or when the step under the cursor
                                                   * has it; while playing, full on the step that sounds
                                                   * if it has it */
            uint32_t f = rec_mode ? set.step[rec_step].flags
                       : seq_running ? set.step[(seq_step + (int32_t)NSTEP - 1) % (int32_t)NSTEP].flags : 0u;
            led_dim(BTN_GLO, rec_mode != 0);
            led_dim(BTN_SEL, rec_mode != 0);     /* SEL = rest, likewise only in record mode */
            fm1_led_key(BTN_SEL, rec_mode && (fm1_in.buttons & (1u << BTN_SEL)));
            fm1_led_key(BTN_GLO, (rec_mode && (fm1_in.buttons & (1u << BTN_GLO))) || (f & 2u));
            fm1_led_key(BTN_ENV, (fm1_in.buttons & (1u << BTN_ENV)) || (f & 4u));
        }
#endif
#ifndef DINGHY_HOST
        fm1_led_key(BTN_SEQ, (fm1_in.buttons & (1u << BTN_SEQ)) != 0);
        fm1_led_key(BTN_ARP, (fm1_in.buttons & (1u << BTN_ARP)) != 0);
        fm1_led_key(BTN_LFO, (fm1_in.buttons & (1u << BTN_LFO)) != 0);   /* full bright while held; the
                                                                          * constant soft glow (set once,
                                                                          * below) shows through otherwise */
#endif

        if (lit != lit_drawn) { lit_drawn = lit; draw_keys(lit); }   /* redraw only what changed */
        {                                        /* the throat: redrawn when the vowel (with the LFOs)
                                                   * or the note changes, at most ~20 times a second (a
                                                   * frame is ~22 ms of SPI at 20 MHz), in 16-row chunks,
                                                   * each started only once the LCD has sent the last
                                                   * one -- so the main loop (and the sequencer in it)
                                                   * never waits for the throat */
            int32_t vl = vow_lock;
            int32_t ex = (vl ? vowel_x[vl - 1] : par.x) + sine(*(volatile uint32_t *)&lfoXPh) * par.lfoXDepth / 32767;
            int32_t ey = (vl ? vowel_y[vl - 1] : par.y) + sine(*(volatile uint32_t *)&lfoYPh) * par.lfoYDepth / 32767;
            uint32_t ut_ = fm1_ms - uv_kick_ms;
            int32_t sway = 0;                    /* after an accent: ~2.5 swings at 8 Hz, 5 px at first,
                                                   * dying away over 300 ms */
            ex = ex < 0 ? 0 : ex > 1000 ? 1000 : ex;
            ey = ey < 0 ? 0 : ey > 1000 ? 1000 : ey;
            if (ut_ < 300u)
                sway = 5 * sine(ut_ * 34359738u) * (int32_t)(300u - ut_) / (32767 * 300);
            if (thr_row >= THROAT_H && (ex - thr_x > 7 || thr_x - ex > 7 || ey - thr_y > 7 || thr_y - ey > 7
                                        || last_note != note_drawn) && fm1_ms - thr_ms >= 50u) {
                thr_x = ex; thr_y = ey; note_drawn = last_note; thr_ms = fm1_ms;   /* a new frame */
                thr_sway = sway;
                thr_row = 0;
            }
            if (thr_row < THROAT_H && lcd_idle()) {
                uint32_t h = THROAT_H - thr_row < 16u ? THROAT_H - thr_row : 16u;
                THR_T0;
                draw_throat(0, 240, thr_row, h, thr_x, thr_y, note_drawn, thr_sway);
                THR_T1;
                thr_row += h;
            } else if (thr_row >= THROAT_H && sway != thr_sway && fm1_ms - uv_ms >= 30u && lcd_idle()) {
                thr_sway = sway;                 /* between frames: only the uvula's corner, 48 x 56 px */
                uv_ms = fm1_ms;
                draw_throat(96, 48, 0, 56, thr_x, thr_y, note_drawn, thr_sway);
            }
        }
        if (set.oct != oct_drawn) { oct_drawn = set.oct; draw_status(set.oct); }
        {
            int32_t mark = rec_mode ? rec_step
                         : seq_running ? (seq_step + (int32_t)NSTEP - 1) % (int32_t)NSTEP : -1;
            uint32_t changed = mark != mark_drawn;
            for (k = 0; k < NSTEP; k++)
                if (flags_drawn[k] != set.step[k].flags) { flags_drawn[k] = set.step[k].flags; changed = 1; }
            if (changed) { mark_drawn = mark; draw_steps(set.step, mark); }
        }
#ifndef DINGHY_HOST
        if (touched_id != ~0u && (touched_id != touched_id_drawn || touched_val != touched_val_drawn)) {
            touched_id_drawn = touched_id; touched_val_drawn = touched_val;
            draw_touched(touched_id, touched_val);
        }
#endif
#ifndef DINGHY_HOST
        if (dbg_us_now != isr_now_drawn || dbg_us_max != isr_max_drawn || thr_max_ms != thr_max_drawn) {
            thr_max_drawn = thr_max_ms;
            isr_now_drawn = dbg_us_now; isr_max_drawn = dbg_us_max;
            draw_isr(isr_now_drawn, isr_max_drawn, thr_max_drawn);
        }
#endif

        if (dirty && !seq_running && quiet() && fm1_ms - t_change > 2000u) {   /* 2 s after the change, when silent
                                                   * (an erase stalls the sound) and not playing: the gaps
                                                   * between a pattern's notes are silent too, and a stall
                                                   * there would break the groove */
            settings_save(&set, sizeof set);
            dirty = 0;
        }
    }
}
