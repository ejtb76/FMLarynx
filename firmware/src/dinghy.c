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
#include "platform.c"

/* ---- the sound (runs in the audio ISR: keep it short, no flash, no screen)
 * FormantSynth prototype: our real plugin (JUCE, see Source/PluginProcessor.h) is monophonic
 * -- one oscillator through one formant filter, last-note-priority -- so NV=1 here matches it
 * exactly; play()/stop()/quiet()/app_main() below are unchanged and already generic over NV. */
#define NV 1u                                   /* voices: monophonic, like the real plugin */
static struct {
    volatile uint32_t inc;                      /* phase step per frame: f * 2^32 / 44100 */
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

/* knob-driven parameters -- pm = permille (0..1000 == the float plugin's 0.0..1.0), chosen so
 * every encoder click is a plain add/sub with no extra scaling at the UI layer. */
static struct {
    int32_t x, y;                               /* articulator plane, pm */
    int32_t size;                                /* 500..2000 == 0.5x..2.0x */
    int32_t resx10;                              /* Q*10, 10..500 == Q 1.0..50.0 */
    int32_t voice;                               /* pm, 0 = male .. 1000 = female/child */
    int32_t level;                                /* pm */
    int32_t lfoXRate, lfoXDepth, lfoYRate, lfoYDepth;   /* rate: pm (0..8 Hz); depth: pm of the x/y range */
} par = {500, 500, 1000, 80, 0, 1000, 0, 0, 0, 0};

static void formant_update_coeffs(int32_t cb0[NB], int32_t cb2[NB], int32_t ca1[NB], int32_t ca2[NB])
{
    int32_t lfoX = (sine(lfoXPh) * par.lfoXDepth) / 32767;    /* -depth..depth, pm */
    int32_t lfoY = (sine(lfoYPh) * par.lfoYDepth) / 32767;
    int32_t x = par.x + lfoX, y = par.y + lfoY, target[NB];
    uint32_t b;
    x = x < 0 ? 0 : x > 1000 ? 1000 : x;
    y = y < 0 ? 0 : y > 1000 ? 1000 : y;
    lfoXPh += par.lfoXRate * 199458u;           /* rate pm -> phase step per block, 0..8 Hz */
    lfoYPh += par.lfoYRate * 199458u;

    vowel_freqs(x, y, par.voice, target);
    for (b = 0; b < NB; b++) {
        int32_t f = (target[b] * par.size) / 1000, fc, turns_q16, sw, cw, alpha, a0;
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

        alpha = (sw * 5) / par.resx10;                         /* sin(w0) / (2*Q), Q15 */
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
    uint32_t ph = voice[0].ph, step = voice[0].inc;
    int32_t amp = voice[0].amp, g = voice[0].gate;
    int32_t cb0[NB], cb2[NB], ca1[NB], ca2[NB];
    int32_t resGain;
#ifndef DINGHY_HOST
    uint32_t t0 = fm1_ticks();
#endif
    if (!amp && !g)
        return;                                 /* silent: costs nothing (filter state just holds,
                                                   * LFOs and coefficients just don't update) */
    formant_update_coeffs(cb0, cb2, ca1, ca2);   /* control rate: once per 256-frame block */
    resGain = res_gain_q8[par.resx10 / 10];      /* resonance volume compensation, see the table above */
    for (i = 0; i < n; i++) {
        int32_t saw, s;
        if (amp < g) { amp += 48; if (amp > g) amp = g; }        /* attack ~5 ms */
        else if (amp > g) { amp -= 6; if (amp < g) amp = g; }    /* release ~45 ms */
        ph += step;
        saw = (int32_t)ph >> 16;                /* naive sawtooth, -32768..32767: harmonically rich
                                                   * source for the filter to shape -- a sine alone
                                                   * wouldn't show off formant filtering at all */
        saw = (saw * amp) >> 15;                /* envelope the excitation, then resonate it */
        s = formant(saw, cb0, cb2, ca1, ca2);
        s = (s * resGain) >> 8;                 /* resonance compensation */
        s = (s * par.level) / 1000;             /* the Level knob, pm */
        out[2u * i] += s;                       /* L */
        out[2u * i + 1u] += s;                  /* R */
    }
    voice[0].ph = ph;
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
    voice[pick].inc = INC_C9[note % 12u] >> (10u - note / 12u);
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

/* ---- the screen: the name, the last note big, the keyboard with what sounds, the octave */
#define C_ACC RGB(180, 140, 255)                /* lilac, as the logo */
#define C_KEY RGB(210, 210, 210)
#define C_OFF RGB(90, 90, 90)
#define KEY0 53u                                /* key 0 = F3 */
static int black(uint32_t note) { return (0x54Au >> (note % 12u)) & 1u; }

static void draw_note(void)
{
    static const char *const NAME[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    char s[8] = "-";
    if (last_note < 128u) {
        const char *p = NAME[last_note % 12u];
        uint32_t i = 0;
        while (*p) s[i++] = *p++;
        if (last_note >= 12u) s[i++] = (char)('0' + last_note / 12u - 1u);
        s[i] = 0;
    }
    draw_text_box(0, 82, 240, &FONT_L, s, C_WHITE, 1);
}

static void draw_keys(uint32_t lit)             /* bit k: key k sounds */
{
    uint32_t k, w = 0;
    cv_begin(240, 56, C_BLACK);
    for (k = 0; k < 27u; k++)                   /* 16 white keys of 14 px from x = 8 */
        if (!black(KEY0 + k)) {
            cv_rect(8 + (int32_t)w * 14, 0, 13, 56, lit >> k & 1u ? C_ACC : C_KEY);
            w++;
        }
    for (k = 0, w = 0; k < 27u; k++)            /* the black ones on the line between two white */
        if (black(KEY0 + k))
            cv_rect(8 + (int32_t)w * 14 - 5, 0, 9, 34, lit >> k & 1u ? C_ACC : C_BLACK);
        else
            w++;
    cv_blit(0, 140);
}

/* ---- 16-step sequencer: BTN_REC toggles record (ENC_SELECT browses the 16 steps; a key records
 * its note into the cursor step and advances; hold BTN_GLO while pressing the key to mark that
 * step a slide -- no hard retrigger, just an immediate pitch change under a continuing envelope,
 * same idea as the real plugin's legato). BTN_PLAY runs the pattern; hold BTN_SEQ + ENC_SELECT
 * sets the tempo. The pattern and tempo live in `set` (app_main()) so they survive a power-off,
 * same as the octave. A step with no note recorded into it plays as a rest (silence). */
#define NSTEP 16u

struct seq_step_s { uint8_t note, flags; };     /* flags: bit0 active, bit1 slide */

static void seq_trigger(const struct seq_step_s *s)   /* shared by the internal clock and MIDI clock */
{
    if (s->flags & 1u) {
        if (!(s->flags & 2u)) voice[0].amp = 0;        /* not a slide: hard retrigger */
        play(s->note, 100);                             /* a slide just retunes under the still-running
                                                          * envelope -- legato */
        midi_send(0x90, s->note, 100);
    } else {
        stop((uint32_t)voice[0].note);                   /* an empty step is a rest */
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

#ifndef DINGHY_HOST                             /* fmt_int is in libc.c, only built for real hardware
                                                  * (tests/host.c stands in for platform.c on a computer
                                                  * and doesn't pull it in -- and the host build's
                                                  * fm1_enc_take() stub always returns 0 anyway, so
                                                  * touched_id would never leave its sentinel there) */
static const char *const PNAME[] = {"SIZE", "RES", "VOICE", "LEVEL", "X", "Y",
                                     "X RATE", "X DEPTH", "Y RATE", "Y DEPTH", "VOWEL", "BPM", "STEP"};
static void draw_touched(uint32_t id, int32_t val)    /* last knob moved: name + value, replaces the
                                                        * static subtitle once anything has been touched */
{
    char s[24], v[8];
    const char *p = PNAME[id];
    uint32_t i = 0, j;
    while (*p) s[i++] = (uint8_t)*p++;
    s[i++] = ' ';
    fmt_int(v, val);
    for (j = 0; v[j]; j++) s[i++] = v[j];
    s[i] = 0;
    draw_text_box(0, 52, 240, &FONT_S, s, C_OFF, 1);
}
#endif

static void draw_status(int32_t oct)
{
    char s[12] = "OCTAVE +0";
    s[7] = oct < 0 ? '-' : '+';
    s[8] = (char)('0' + (oct < 0 ? -oct : oct));
    draw_text_box(0, 212, 240, &FONT_S, s, C_OFF, 1);
}

#ifndef DINGHY_HOST
static void draw_isr(uint32_t now, uint32_t max)    /* app_audio() time, now/worst since boot, vs the
                                                      * 2.9 ms safe half-budget from docs/RULES.md */
{
    char s[24] = "ISR ", a[6], b[6];
    uint32_t i = 4, j;
    fmt_int(a, (int32_t)now);
    fmt_int(b, (int32_t)max);
    for (j = 0; a[j]; j++) s[i++] = a[j];
    s[i++] = '/'; for (j = 0; b[j]; j++) s[i++] = b[j];
    s[i++] = 'u'; s[i++] = 's'; s[i] = 0;
    draw_text_box(0, 224, 240, &FONT_S, s, max > 2900u ? C_ACC : C_OFF, 1);   /* lilac past the 2.9 ms safe half-budget */
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

/* ---- controls: 7 encoders, no menus -- ENC_KNOB1..4 do size/res/voice/level normally and the
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
    } set = {0};
    uint32_t prev = 0, k, t_change = 0, dirty = 0, lit_drawn = ~0u, note_drawn = ~0u;
    int32_t oct_drawn = 99;
    int32_t rec_mode = 0, rec_step = 0, seq_running = 0, seq_step = 0;
    uint32_t seq_next_ms = 0;
    uint32_t midiclk_last_ms = 0u - 1000u, midiclk_count = 0;   /* "ages ago": starts on the internal clock */
#ifndef DINGHY_HOST
    uint32_t touched_id_drawn = ~0u;
    int32_t touched_val_drawn = 0;
    int32_t rec_step_drawn = -1;
#endif
#ifndef DINGHY_HOST
    uint32_t isr_now_drawn = ~0u, isr_max_drawn = ~0u;
#endif
    uint8_t held[27] = {0};                     /* the note each key plays */
    settings_load(&set, sizeof set);
    if (set.oct < -2 || set.oct > 2) set.oct = 0;
    if (set.bpm < 40 || set.bpm > 300) set.bpm = 120;

    lcd_fill(0, 0, 240, 240, C_BLACK);
    draw_text_box(0, 18, 240, &FONT_L, APP_NAME, C_ACC, 1);
    draw_text_box(0, 52, 240, &FONT_S, "formantsynth prototype  v" APP_VER, C_OFF, 1);
#ifndef DINGHY_HOST
    led_dim(BTN_LFO, 1);                        /* soft glow, always -- fm1_led_key(BTN_LFO, ...) above
                                                  * goes full bright on top of it while held */
    led_dim(BTN_GLO, 1);                        /* same: a landmark showing it's an available modifier;
                                                  * goes full bright per-step while browsing in record
                                                  * mode, see the rec_step tracking below */
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
                if (rec_mode) {                     /* record into the cursor step, then advance it */
                    set.step[rec_step].note = held[k];
                    set.step[rec_step].flags = 1u | (fm1_in.buttons & (1u << BTN_GLO) ? 2u : 0u);
                    rec_step = (int32_t)((rec_step + 1) % (int32_t)NSTEP);
                    dirty = 1; t_change = fm1_ms;
                    touched_id = 12; touched_val = rec_step;
                }
            }
            if (up >> k & 1u) { stop(held[k]); midi_send(0x80, held[k], 0); }
        }

        b = fm1_input_edges(&rel);              /* buttons pressed since the last call (bit = button id) */
        if (b & (1u << BTN_OCT_DOWN) && set.oct > -2) { set.oct--; dirty = 1; t_change = fm1_ms; }
        if (b & (1u << BTN_OCT_UP) && set.oct < 2) { set.oct++; dirty = 1; t_change = fm1_ms; }
        if (b & (1u << BTN_REC)) {              /* toggle record mode: stop playback, reset the cursor */
            rec_mode = !rec_mode;
            if (rec_mode) { seq_running = 0; rec_step = 0; stop((uint32_t)voice[0].note); }
#ifndef DINGHY_HOST
            else if (rec_step_drawn >= 0) { led_dim(KEY_LED(white_key_id((uint32_t)rec_step_drawn)), 0); rec_step_drawn = -1; }
#endif
        }
        if (b & (1u << BTN_PLAY) && !rec_mode) {   /* toggle the sequencer; can't run while recording */
            seq_running = !seq_running;
            if (seq_running) { seq_step = 0; seq_next_ms = fm1_ms; }
            else stop((uint32_t)voice[0].note);
        }

        {                                        /* 7 knobs, no menus (see the comment above knob()) */
            int32_t lfo = (int32_t)(fm1_in.buttons & (1u << BTN_LFO));
            int32_t seqShift = (int32_t)(fm1_in.buttons & (1u << BTN_SEQ));
            int32_t d;
            d = fm1_enc_take(ENC_KNOB1);
            if (lfo) knob(&par.lfoXRate,  d, 20, 0, 1000, 6); else knob(&par.size,   d, 20,  500, 2000, 0);
            d = fm1_enc_take(ENC_KNOB2);
            if (lfo) knob(&par.lfoXDepth, d, 20, 0, 1000, 7); else knob(&par.resx10, d, 10,   10,  500, 1);
            d = fm1_enc_take(ENC_KNOB3);
            if (lfo) knob(&par.lfoYRate,  d, 20, 0, 1000, 8); else knob(&par.voice,  d, 20,    0, 1000, 2);
            d = fm1_enc_take(ENC_KNOB4);
            if (lfo) knob(&par.lfoYDepth, d, 20, 0, 1000, 9); else knob(&par.level,  d, 20,    0, 1000, 3);
            knob(&par.x, fm1_enc_take(ENC_ALGORITHM), 20, 0, 1000, 4);
            d = fm1_enc_take(ENC_SELECT);        /* Y normally; steps while recording; tempo under SEQ */
            if (d) {
                if (rec_mode) {
                    rec_step += d;
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
            d = fm1_enc_take(ENC_PRESETS);
            if (d) {
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
                }
            } else if (st == 0xFAu) { seq_running = 1; seq_step = 0; midiclk_count = 0; }   /* start */
            else if (st == 0xFBu) { seq_running = 1; }                                       /* continue */
            else if (st == 0xFCu) { seq_running = 0; stop((uint32_t)voice[0].note); }        /* stop */
        }

        /* one sequencer step at the internal tempo -- only when no external MIDI clock has ticked in
         * the last 500 ms; while a DAW/clock is actively sending 0xF8, that drives stepping instead
         * (above), so the two never double-trigger */
        if (seq_running && !rec_mode && fm1_ms - midiclk_last_ms > 500u && fm1_ms >= seq_next_ms) {
            seq_next_ms += (uint32_t)(15000 / set.bpm);   /* 60000 / bpm / 4; no drift: += not = */
            seq_trigger(&set.step[seq_step]);
            seq_step = (seq_step + 1) % (int32_t)NSTEP;
        }

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
#ifndef DINGHY_HOST
        if (rec_mode && rec_step != rec_step_drawn) {   /* the cursor's own soft glow among the step LEDs */
            if (rec_step_drawn >= 0) led_dim(KEY_LED(white_key_id((uint32_t)rec_step_drawn)), 0);
            led_dim(KEY_LED(white_key_id((uint32_t)rec_step)), 1);
            rec_step_drawn = rec_step;
        }
        fm1_led_key(BTN_GLO, (fm1_in.buttons & (1u << BTN_GLO)) != 0          /* full bright while held */
                           || (rec_mode && (set.step[rec_step].flags & 2u) != 0));  /* or browsing a slide step */
#endif
#ifndef DINGHY_HOST
        fm1_led_key(BTN_LFO, (fm1_in.buttons & (1u << BTN_LFO)) != 0);   /* full bright while held; the
                                                                          * constant soft glow (set once,
                                                                          * below) shows through otherwise */
#endif

        if (lit != lit_drawn) { lit_drawn = lit; draw_keys(lit); }   /* redraw only what changed */
        if (last_note != note_drawn) { note_drawn = last_note; draw_note(); }
        if (set.oct != oct_drawn) { oct_drawn = set.oct; draw_status(set.oct); }
#ifndef DINGHY_HOST
        if (touched_id != ~0u && (touched_id != touched_id_drawn || touched_val != touched_val_drawn)) {
            touched_id_drawn = touched_id; touched_val_drawn = touched_val;
            draw_touched(touched_id, touched_val);
        }
#endif
#ifndef DINGHY_HOST
        if (dbg_us_now != isr_now_drawn || dbg_us_max != isr_max_drawn) {
            isr_now_drawn = dbg_us_now; isr_max_drawn = dbg_us_max;
            draw_isr(isr_now_drawn, isr_max_drawn);
        }
#endif

        if (dirty && quiet() && fm1_ms - t_change > 2000u) {   /* 2 s after the change, when silent (an erase stalls the sound) */
            settings_save(&set, sizeof set);
            dirty = 0;
        }
    }
}
