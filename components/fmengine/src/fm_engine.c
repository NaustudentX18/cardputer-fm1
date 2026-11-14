/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2012 Google Inc.  (msfa: env.cc, fm_core.cc, fm_op_kernel.cc,
 *                              dx7note.cc, lfo.cc, pitchenv.cc, patch.cc)
 * Copyright 2026 cardputer-fm1 contributors.
 *
 * Port notes, so a reviewer can diff this against upstream line by line:
 *   - Classes became plain functions taking a pointer (Env::getsample -> fm_env_getsample(&e)).
 *   - `min`/`max` from synth.h became fm_min_i/fm_max_i (no std headers on the device).
 *   - Table lookups stay 64-bit where upstream multiplies 32x32 into int64; that is what
 *     makes the result bit-identical. See FM_USE_Q15_GAIN for the optional fast path.
 *   - Accumulation order, block size (64), block-relative gain ramp and the 64-sample
 *     control rate are untouched.
 *   - Everything upstream keeps in globals (sintab/exp2tab/lut) is const in fm_tables.c.
 *   - Controllers::values_[128] (pitch bend) became one int32 in fm_synth_t; the other
 *     controller slots in msfa are unused by Dx7Note::compute. */
#include "fm_engine.h"

static inline int32_t fm_min_i(int32_t a, int32_t b) { return a < b ? a : b; }
static inline int32_t fm_max_i(int32_t a, int32_t b) { return a > b ? a : b; }

/* ---------------------------------------------------------------- tables --- */
/* Sin::lookup, SIN_DELTA branch (sin.h) */
static inline int32_t fm_sin_lookup(int32_t phase)
{
    const int SHIFT = 24 - FM_SIN_LG_N_SAMPLES;
    int32_t lowbits = phase & ((1 << SHIFT) - 1);
    int32_t phase_int = (phase >> (SHIFT - 1)) & ((FM_SIN_N_SAMPLES - 1) << 1);
    int32_t dy = fm_sin_tab[phase_int];
    int32_t y0 = fm_sin_tab[phase_int + 1];
    return y0 + (int32_t)(((int64_t)dy * (int64_t)lowbits) >> SHIFT);
}

/* Exp2::lookup (exp2.h) */
static inline int32_t fm_exp2_lookup(int32_t x)
{
    const int SHIFT = 24 - FM_EXP2_LG_N_SAMPLES;
    int32_t lowbits = x & ((1 << SHIFT) - 1);
    int32_t x_int = (x >> (SHIFT - 1)) & ((FM_EXP2_N_SAMPLES - 1) << 1);
    int32_t dy = fm_exp2_tab[x_int];
    int32_t y0 = fm_exp2_tab[x_int + 1];
    int32_t y = y0 + (int32_t)(((int64_t)dy * (int64_t)lowbits) >> SHIFT);
    return y >> (6 - (x >> 24));
}

/* Freqlut::lookup (freqlut.h) */
static inline int32_t fm_freq_lookup(int32_t logfreq)
{
    int32_t ix = (logfreq & 0xffffff) >> FM_FREQ_SAMPLE_SHIFT;
    int32_t y0 = fm_freq_tab[ix];
    int32_t y1 = fm_freq_tab[ix + 1];
    int32_t lowbits = logfreq & ((1 << FM_FREQ_SAMPLE_SHIFT) - 1);
    int32_t y = y0 + (int32_t)((((int64_t)(y1 - y0) * (int64_t)lowbits)) >> FM_FREQ_SAMPLE_SHIFT);
    int32_t hibits = logfreq >> 24;
    return y >> (FM_FREQ_MAX_LOGFREQ_INT - hibits);
}

/* ---------------------------------------------------------------- envelope -- */
static const int32_t fm_levellut[20] = {
    0, 5, 9, 13, 17, 20, 23, 25, 27, 29, 31, 33, 35, 37, 39, 41, 42, 43, 45, 46
};

int32_t fm_scale_outlevel(int32_t outlevel)
{
    return outlevel >= 20 ? 28 + outlevel : fm_levellut[outlevel < 0 ? 0 : outlevel];
}

static void fm_env_advance(fm_env_t *e, int newix)
{
    e->ix = (int8_t)newix;
    if (newix < 4) {
        int32_t newlevel = e->levels[newix];
        int32_t actuallevel = fm_scale_outlevel(newlevel) >> 1;
        actuallevel = (actuallevel << 6) + e->outlevel - 4256;
        actuallevel = actuallevel < 16 ? 16 : actuallevel;
        e->targetlevel = actuallevel << 16;
        e->rising = (e->targetlevel > e->level);

        int32_t qrate = (e->rates[newix] * 41) >> 6;
        qrate += e->rate_scaling;
        qrate = fm_min_i(qrate, 63);
        e->inc = (4 + (qrate & 3)) << (2 + FM_LG_N + (qrate >> 2));
    }
}

static void fm_env_init(fm_env_t *e, const int r[4], const int l[4], int32_t ol, int rate_scaling)
{
    for (int i = 0; i < 4; i++) {
        e->rates[i] = r[i];
        e->levels[i] = l[i];
    }
    e->outlevel = ol;
    e->rate_scaling = rate_scaling;
    e->level = 0;
    e->down = 1;
    fm_env_advance(e, 0);
}

static int32_t fm_env_getsample(fm_env_t *e)
{
    if (e->ix < 3 || (e->ix < 4 && !e->down)) {
        if (e->rising) {
            const int32_t jumptarget = 1716;
            if (e->level < (jumptarget << 16))
                e->level = jumptarget << 16;
            e->level += (((17 << 24) - e->level) >> 24) * e->inc;
            if (e->level >= e->targetlevel) {
                e->level = e->targetlevel;
                fm_env_advance(e, e->ix + 1);
            }
        } else {
            e->level -= e->inc;
            if (e->level <= e->targetlevel) {
                e->level = e->targetlevel;
                fm_env_advance(e, e->ix + 1);
            }
        }
    }
    return e->level;
}

static void fm_env_keydown(fm_env_t *e, int down)
{
    if (e->down != down) {
        e->down = (int8_t)down;
        fm_env_advance(e, down ? 0 : 3);
    }
}

/* ------------------------------------------------------------ pitch env ---- */
static const uint8_t fm_penv_ratetab[100] = {
    1, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12,
    12, 13, 13, 14, 14, 15, 16, 16, 17, 18, 18, 19, 20, 21, 22, 23, 24,
    25, 26, 27, 28, 30, 31, 33, 34, 36, 37, 38, 39, 41, 42, 44, 46, 47,
    49, 51, 53, 54, 56, 58, 60, 62, 64, 66, 68, 70, 72, 74, 76, 79, 82,
    85, 88, 91, 94, 98, 102, 106, 110, 115, 120, 125, 130, 135, 141, 147,
    153, 159, 165, 171, 178, 185, 193, 202, 211, 232, 243, 254, 255
};

static const int8_t fm_penv_pitchtab[100] = {
    -128, -116, -104, -95, -85, -76, -68, -61, -56, -52, -49, -46, -43,
    -41, -39, -37, -35, -33, -32, -31, -30, -29, -28, -27, -26, -25, -24,
    -23, -22, -21, -20, -19, -18, -17, -16, -15, -14, -13, -12, -11, -10,
    -9, -8, -7, -6, -5, -4, -3, -2, -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10,
    11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27,
    28, 29, 30, 31, 32, 33, 34, 35, 38, 40, 43, 46, 49, 53, 58, 65, 73,
    82, 92, 103, 115, 127
};

/* PitchEnv::unit_: generated from msfa's own expression in fm_tables.c */
#define FM_PENV_UNIT fm_penv_unit

static void fm_penv_advance(fm_penv_t *e, int newix)
{
    e->ix = (int8_t)newix;
    if (newix < 4) {
        int32_t newlevel = e->levels[newix] % 100;
        int32_t t = fm_penv_pitchtab[newlevel];
        e->targetlevel = t << 19;
        e->rising = (e->targetlevel > e->level);
        e->inc = fm_penv_ratetab[e->rates[newix] % 100] * FM_PENV_UNIT;
    }
}

static void fm_penv_set(fm_penv_t *e, const int r[4], const int l[4])
{
    for (int i = 0; i < 4; i++) {
        e->rates[i] = r[i];
        e->levels[i] = l[i];
    }
    e->level = fm_penv_pitchtab[l[3] % 100] << 19;
    e->down = 1;
    fm_penv_advance(e, 0);
}

static int32_t fm_penv_getsample(fm_penv_t *e)
{
    if (e->ix < 3 || (e->ix < 4 && !e->down)) {
        if (e->rising) {
            e->level += e->inc;
            if (e->level >= e->targetlevel) {
                e->level = e->targetlevel;
                fm_penv_advance(e, e->ix + 1);
            }
        } else {
            e->level -= e->inc;
            if (e->level <= e->targetlevel) {
                e->level = e->targetlevel;
                fm_penv_advance(e, e->ix + 1);
            }
        }
    }
    return e->level;
}

static void fm_penv_keydown(fm_penv_t *e, int down)
{
    if (e->down != down) {
        e->down = (int8_t)down;
        fm_penv_advance(e, down ? 0 : 3);
    }
}

/* ------------------------------------------------------------------- LFO --- */
/* msfa Lfo::unit_: generated from msfa's own expression in fm_tables.c */
#define FM_LFO_UNIT fm_lfo_unit

static void fm_lfo_reset(fm_lfo_t *l, const uint8_t params[6])
{
    int32_t rate = params[0];
    int32_t sr = rate == 0 ? 1 : (165 * rate) >> 6;
    sr *= sr < 160 ? 11 : (11 + ((sr - 160) >> 4));
    l->delta = FM_LFO_UNIT * (uint32_t)sr;
    int32_t a = 99 - params[1];
    if (a == 99) {
        l->delayinc = ~0u;
        l->delayinc2 = ~0u;
    } else {
        a = (16 + (a & 15)) << (1 + (a >> 4));
        l->delayinc = FM_LFO_UNIT * (uint32_t)a;
        a &= 0xff80;
        a = fm_max_i(0x80, a);
        l->delayinc2 = FM_LFO_UNIT * (uint32_t)a;
    }
    l->waveform = params[5];
    l->sync = params[4] != 0;
}

static int32_t fm_lfo_getsample(fm_lfo_t *l)
{
    l->phase += l->delta;
    int32_t x;
    switch (l->waveform) {
    case 0:                                     /* triangle */
        x = (int32_t)(l->phase >> 7);
        x ^= -(int32_t)(l->phase >> 31);
        x &= (1 << 24) - 1;
        return x;
    case 1:                                     /* sawtooth down */
        return (int32_t)((~l->phase ^ (1u << 31)) >> 8);
    case 2:                                     /* sawtooth up */
        return (int32_t)((l->phase ^ (1u << 31)) >> 8);
    case 3:                                     /* square */
        return (int32_t)(((~l->phase) >> 7) & (1u << 24));
    case 4:                                     /* sine */
        return (1 << 23) + (fm_sin_lookup((int32_t)(l->phase >> 8)) >> 1);
    case 5:                                     /* sample & hold */
        if (l->phase < l->delta)
            l->randstate = (uint8_t)((l->randstate * 179 + 17) & 0xff);
        x = l->randstate ^ 0x80;
        return (x + 1) << 16;
    }
    return 1 << 23;
}

static int32_t fm_lfo_getdelay(fm_lfo_t *l)
{
    uint32_t delta = l->delaystate < (1u << 31) ? l->delayinc : l->delayinc2;
    uint32_t d = l->delaystate + delta;
    if (d < l->delayinc)
        return 1 << 24;
    l->delaystate = d;
    if (d < (1u << 31))
        return 0;
    return (int32_t)((d >> 7) & ((1u << 24) - 1));
}

static void fm_lfo_keydown(fm_lfo_t *l)
{
    if (l->sync)
        l->phase = (1u << 31) - 1u;
    l->delaystate = 0;
}

/* -------------------------------------------------------------- operators -- */
/* fm_op_kernel.cc, all three entry points. gain1/gain2 ramp linearly over the
 * block: gain at sample i is gain1 + (1+i)/64 * (gain2 - gain1). */
static void fm_op_compute(int32_t *output, const int32_t *input, int32_t phase0, int32_t freq,
                          int32_t gain1, int32_t gain2, int add)
{
    int32_t dgain = (gain2 - gain1 + (FM_N >> 1)) >> FM_LG_N;
    int32_t gain = gain1;
    int32_t phase = phase0;
    if (add) {
        for (int i = 0; i < FM_N; i++) {
            gain += dgain;
            int32_t y = fm_sin_lookup(phase + input[i]);
            output[i] += (int32_t)(((int64_t)y * (int64_t)gain) >> 24);
            phase += freq;
        }
    } else {
        for (int i = 0; i < FM_N; i++) {
            gain += dgain;
            int32_t y = fm_sin_lookup(phase + input[i]);
            output[i] = (int32_t)(((int64_t)y * (int64_t)gain) >> 24);
            phase += freq;
        }
    }
}

static void fm_op_compute_pure(int32_t *output, int32_t phase0, int32_t freq,
                               int32_t gain1, int32_t gain2, int add)
{
    int32_t dgain = (gain2 - gain1 + (FM_N >> 1)) >> FM_LG_N;
    int32_t gain = gain1;
    int32_t phase = phase0;
    if (add) {
        for (int i = 0; i < FM_N; i++) {
            gain += dgain;
            int32_t y = fm_sin_lookup(phase);
            output[i] += (int32_t)(((int64_t)y * (int64_t)gain) >> 24);
            phase += freq;
        }
    } else {
        for (int i = 0; i < FM_N; i++) {
            gain += dgain;
            int32_t y = fm_sin_lookup(phase);
            output[i] = (int32_t)(((int64_t)y * (int64_t)gain) >> 24);
            phase += freq;
        }
    }
}

static void fm_op_compute_fb(int32_t *output, int32_t phase0, int32_t freq,
                             int32_t gain1, int32_t gain2, int32_t *fb_buf, int fb_shift, int add)
{
    int32_t dgain = (gain2 - gain1 + (FM_N >> 1)) >> FM_LG_N;
    int32_t gain = gain1;
    int32_t phase = phase0;
    int32_t y0 = fb_buf[0];
    int32_t y = fb_buf[1];
    if (add) {
        for (int i = 0; i < FM_N; i++) {
            gain += dgain;
            int32_t scaled_fb = (y0 + y) >> (fb_shift + 1);
            y0 = y;
            y = fm_sin_lookup(phase + scaled_fb);
            y = (int32_t)(((int64_t)y * (int64_t)gain) >> 24);
            output[i] += y;
            phase += freq;
        }
    } else {
        for (int i = 0; i < FM_N; i++) {
            gain += dgain;
            int32_t scaled_fb = (y0 + y) >> (fb_shift + 1);
            y0 = y;
            y = fm_sin_lookup(phase + scaled_fb);
            y = (int32_t)(((int64_t)y * (int64_t)gain) >> 24);
            output[i] = y;
            phase += freq;
        }
    }
    fb_buf[0] = y0;
    fb_buf[1] = y;
}

/* ------------------------------------------------------------- algorithms -- */
/* msfa's table, verbatim. Operator slot 0 is OP6, as in the DX7 voice layout. */
enum {
    FM_OUT_BUS_ONE = 1 << 0, FM_OUT_BUS_TWO = 1 << 1, FM_OUT_BUS_ADD = 1 << 2,
    FM_IN_BUS_ONE = 1 << 4, FM_IN_BUS_TWO = 1 << 5, FM_FB_IN = 1 << 6, FM_FB_OUT = 1 << 7
};

static const uint8_t fm_algorithms[32][6] = {
    {0xc1, 0x11, 0x11, 0x14, 0x01, 0x14},  /* 1 */
    {0x01, 0x11, 0x11, 0x14, 0xc1, 0x14},  /* 2 */
    {0xc1, 0x11, 0x14, 0x01, 0x11, 0x14},  /* 3 */
    {0x41, 0x11, 0x94, 0x01, 0x11, 0x14},  /* 4 */
    {0xc1, 0x14, 0x01, 0x14, 0x01, 0x14},  /* 5 */
    {0x41, 0x94, 0x01, 0x14, 0x01, 0x14},  /* 6 */
    {0xc1, 0x11, 0x05, 0x14, 0x01, 0x14},  /* 7 */
    {0x01, 0x11, 0xc5, 0x14, 0x01, 0x14},  /* 8 */
    {0x01, 0x11, 0x05, 0x14, 0xc1, 0x14},  /* 9 */
    {0x01, 0x05, 0x14, 0xc1, 0x11, 0x14},  /* 10 */
    {0xc1, 0x05, 0x14, 0x01, 0x11, 0x14},  /* 11 */
    {0x01, 0x05, 0x05, 0x14, 0xc1, 0x14},  /* 12 */
    {0xc1, 0x05, 0x05, 0x14, 0x01, 0x14},  /* 13 */
    {0xc1, 0x05, 0x11, 0x14, 0x01, 0x14},  /* 14 */
    {0x01, 0x05, 0x11, 0x14, 0xc1, 0x14},  /* 15 */
    {0xc1, 0x11, 0x02, 0x25, 0x05, 0x14},  /* 16 */
    {0x01, 0x11, 0x02, 0x25, 0xc5, 0x14},  /* 17 */
    {0x01, 0x11, 0x11, 0xc5, 0x05, 0x14},  /* 18 */
    {0xc1, 0x14, 0x14, 0x01, 0x11, 0x14},  /* 19 */
    {0x01, 0x05, 0x14, 0xc1, 0x14, 0x14},  /* 20 */
    {0x01, 0x14, 0x14, 0xc1, 0x14, 0x14},  /* 21 */
    {0xc1, 0x14, 0x14, 0x14, 0x01, 0x14},  /* 22 */
    {0xc1, 0x14, 0x14, 0x01, 0x14, 0x04},  /* 23 */
    {0xc1, 0x14, 0x14, 0x14, 0x04, 0x04},  /* 24 */
    {0xc1, 0x14, 0x14, 0x04, 0x04, 0x04},  /* 25 */
    {0xc1, 0x05, 0x14, 0x01, 0x14, 0x04},  /* 26 */
    {0x01, 0x05, 0x14, 0xc1, 0x14, 0x04},  /* 27 */
    {0x04, 0xc1, 0x11, 0x14, 0x01, 0x14},  /* 28 */
    {0xc1, 0x14, 0x01, 0x14, 0x04, 0x04},  /* 29 */
    {0x04, 0xc1, 0x11, 0x14, 0x04, 0x04},  /* 30 */
    {0xc1, 0x14, 0x04, 0x04, 0x04, 0x04},  /* 31 */
    {0xc4, 0x04, 0x04, 0x04, 0x04, 0x04},  /* 32 */
};

uint8_t fm_algorithm_op_flags(uint8_t algorithm, int op)
{
    return fm_algorithms[algorithm & 31][op];
}

int fm_algorithm_carriers(uint8_t algorithm)
{
    int count = 0;
    for (int i = 0; i < 6; i++)
        if ((fm_algorithms[algorithm & 31][i] & 7) == FM_OUT_BUS_ADD)
            count++;
    return count;
}

/* FmCore::compute */
static void fm_core_compute(fm_core_t *c, int32_t *output, fm_op_params_t *params,
                            int algorithm, int32_t *fb_buf, int feedback_shift)
{
    const int32_t kLevelThresh = 1120;
    const uint8_t *alg = fm_algorithms[algorithm & 31];
    int has_contents[3] = {1, 0, 0};
    for (int op = 0; op < 6; op++) {
        int flags = alg[op];
        int add = (flags & FM_OUT_BUS_ADD) != 0;
        fm_op_params_t *param = &params[op];
        int inbus = (flags >> 4) & 3;
        int outbus = flags & 3;
        int32_t *outptr = (outbus == 0) ? output : c->bus[outbus - 1];
        int32_t gain1 = param->gain[0];
        int32_t gain2 = param->gain[1];
        if (gain1 >= kLevelThresh || gain2 >= kLevelThresh) {
            if (!has_contents[outbus])
                add = 0;
            if (inbus == 0 || !has_contents[inbus]) {
                if ((flags & 0xc0) == 0xc0 && feedback_shift < 16)
                    fm_op_compute_fb(outptr, param->phase, param->freq, gain1, gain2,
                                     fb_buf, feedback_shift, add);
                else
                    fm_op_compute_pure(outptr, param->phase, param->freq, gain1, gain2, add);
            } else {
                fm_op_compute(outptr, c->bus[inbus - 1], param->phase, param->freq,
                              gain1, gain2, add);
            }
            has_contents[outbus] = 1;
        } else if (!add) {
            has_contents[outbus] = 0;
        }
        param->phase += param->freq << FM_LG_N;
    }
}

/* -------------------------------------------------------------- note setup -- */
static inline int32_t fm_midinote_to_logfreq(int32_t midinote)
{
    const int32_t base = 50857777;              /* (1<<24) * (log(440)/log(2) - 69/12) */
    const int32_t step = (1 << 24) / 12;
    return base + step * midinote;
}

static const int32_t fm_coarsemul[32] = {
    -16777216, 0, 16777216, 26591258, 33554432, 38955489, 43368474, 47099600,
    50331648, 53182516, 55732705, 58039632, 60145690, 62083076, 63876816,
    65546747, 67108864, 68576247, 69959732, 71268397, 72509921, 73690858,
    74816848, 75892776, 76922906, 77910978, 78860292, 79773775, 80654032,
    81503396, 82323963, 83117622
};

/* fm_fine[] comes from fm_tables.c: generated from msfa's own expression, not typed. */

static int32_t fm_osc_freq(int32_t midinote, int mode, int coarse, int fine, int detune)
{
    int32_t logfreq;
    if (mode == 0) {
        logfreq = fm_midinote_to_logfreq(midinote);
        logfreq += fm_coarsemul[coarse & 31];
        if (fine)
            logfreq += fm_fine[fine % 100];
        /* msfa's constant step. (Felucca replaces it with a note-dependent curve to match
         * a real DX7 more closely; that would break bit-exactness against msfa, so it is
         * not used here. At detune 7 both are 0.) */
        logfreq += 12606 * (detune - 7);
    } else {
        logfreq = (4458616 * ((coarse & 3) * 100 + fine)) >> 3;
        logfreq += detune > 7 ? 13457 * (detune - 7) : 0;
    }
    return logfreq;
}

static const uint8_t fm_velocity_data[64] = {
    0, 70, 86, 97, 106, 114, 121, 126, 132, 138, 142, 148, 152, 156, 160, 163,
    166, 170, 173, 174, 178, 181, 184, 186, 189, 190, 194, 196, 198, 200, 202,
    205, 206, 209, 211, 214, 216, 218, 220, 222, 224, 225, 227, 229, 230, 232,
    233, 235, 237, 238, 240, 241, 242, 243, 244, 246, 246, 248, 249, 250, 251,
    252, 253, 254
};

static int32_t fm_scale_velocity(int32_t velocity, int32_t sensitivity)
{
    int32_t clamped = fm_max_i(0, fm_min_i(127, velocity));
    int32_t vel_value = fm_velocity_data[clamped >> 1] - 239;
    return ((sensitivity * vel_value + 7) >> 3) << 4;
}

static int32_t fm_scale_rate(int32_t midinote, int32_t sensitivity)
{
    int32_t x = fm_min_i(31, fm_max_i(0, midinote / 3 - 7));
    return (sensitivity * x) >> 3;
}

static const uint8_t fm_exp_scale_data[33] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 11, 14, 16, 19, 23, 27, 33, 39, 47, 56, 66,
    80, 94, 110, 126, 142, 158, 174, 190, 206, 222, 238, 250
};

static int32_t fm_scale_curve(int32_t group, int32_t depth, int32_t curve)
{
    int32_t scale;
    if (curve == 0 || curve == 3) {
        scale = (group * depth * 329) >> 12;
    } else {
        int32_t idx = fm_min_i(group, 32);
        scale = (fm_exp_scale_data[idx] * depth * 329) >> 15;
    }
    if (curve < 2)
        scale = -scale;
    return scale;
}

static int32_t fm_scale_level(int32_t midinote, int32_t break_pt, int32_t left_depth,
                              int32_t right_depth, int32_t left_curve, int32_t right_curve)
{
    int32_t offset = midinote - break_pt - 17;
    if (offset >= 0)
        return fm_scale_curve(offset / 3, right_depth, right_curve);
    return fm_scale_curve((-offset) / 3, left_depth, left_curve);
}

static const uint8_t fm_pitchmodsenstab[8] = {0, 10, 20, 33, 55, 92, 153, 255};

/* Dx7Note::init */
static void fm_voice_init(fm_voice_t *v, const uint8_t *patch, int32_t midinote, int32_t velocity)
{
    int rates[4];
    int levels[4];
    for (int op = 0; op < 6; op++) {
        int off = op * FM_OP;
        for (int i = 0; i < 4; i++) {
            rates[i] = patch[off + i];
            levels[i] = patch[off + 4 + i];
        }
        int32_t outlevel = patch[off + 16];
        outlevel = fm_scale_outlevel(outlevel);
        int32_t level_scaling = fm_scale_level(midinote, patch[off + 8], patch[off + 9],
                                              patch[off + 10], patch[off + 11], patch[off + 12]);
        outlevel += level_scaling;
        outlevel = fm_min_i(127, outlevel);
        outlevel = outlevel << 5;
        outlevel += fm_scale_velocity(velocity, patch[off + 15]);
        outlevel = fm_max_i(0, outlevel);
        int32_t rate_scaling = fm_scale_rate(midinote, patch[off + 13]);
        fm_env_init(&v->env[op], rates, levels, outlevel, rate_scaling);

        int mode = patch[off + 17];
        int coarse = patch[off + 18];
        int fine = patch[off + 19];
        int detune = patch[off + 20];
        v->basepitch[op] = fm_osc_freq(midinote, mode, coarse, fine, detune);
        v->op[op].phase = 0;
        v->op[op].gain[1] = 0;
    }
    for (int i = 0; i < 4; i++) {
        rates[i] = patch[FM_PR1 + i];
        levels[i] = patch[FM_PL1 + i];
    }
    fm_penv_set(&v->penv, rates, levels);
    v->algorithm = (int8_t)(patch[FM_ALG] & 31);
    int feedback = patch[FM_FB];
    v->fb_shift = feedback != 0 ? 8 - feedback : 16;
    v->pitchmoddepth = (patch[FM_LPMD] * 165) >> 6;
    v->pitchmodsens = fm_pitchmodsenstab[patch[FM_LPMS] & 7];
    v->fb_buf[0] = 0;
    v->fb_buf[1] = 0;
    v->live = 1;
    v->down = 1;
}

/* Dx7Note::compute, plus the per-voice operator-on mask that the FM-1's patch
 * byte 155 carries (msfa sets it to 0x3f = all six on). */
static void fm_voice_compute(fm_voice_t *v, fm_core_t *core, int32_t *buf,
                             int32_t lfo_val, int32_t lfo_delay, int32_t pitchbend,
                             uint8_t op_mask)
{
    int32_t pitchmod = fm_penv_getsample(&v->penv);
    uint32_t pmd = (uint32_t)v->pitchmoddepth * (uint32_t)lfo_delay;   /* Q32 */
    int32_t senslfo = v->pitchmodsens * (lfo_val - (1 << 23));
    pitchmod += (int32_t)(((int64_t)pmd * (int64_t)senslfo) >> 39);
    pitchmod += (pitchbend - 0x2000) << 9;

    for (int op = 0; op < 6; op++) {
        v->op[op].gain[0] = v->op[op].gain[1];
        int32_t level = fm_env_getsample(&v->env[op]);
        int32_t gain = fm_exp2_lookup(level - (14 * (1 << 24)));
        if (!(op_mask & (1u << op)))
            gain = 0;                       /* operator switched off: silent, still advanced */
        v->op[op].freq = fm_freq_lookup(v->basepitch[op] + pitchmod);
        v->op[op].gain[1] = gain;
    }
    fm_core_compute(core, buf, v->op, v->algorithm, v->fb_buf, v->fb_shift);
}

/* --------------------------------------------------------------- DX7 patch -- */
/* patch.cc UnpackPatch: 128-byte bank record -> 156-byte voice. */
void fm_unpack_patch(uint8_t patch[FM_PATCH_BYTES], const uint8_t bulk[FM_PACKED_BYTES])
{
    for (int op = 0; op < 6; op++) {
        uint8_t *p = patch + op * FM_OP;
        const uint8_t *b = bulk + op * 17;
        for (int i = 0; i < 11; i++)
            p[i] = b[i];
        uint8_t leftrightcurves = b[11];
        p[11] = leftrightcurves & 3;
        p[12] = (leftrightcurves >> 2) & 3;
        uint8_t detune_rs = b[12];
        p[13] = detune_rs & 7;
        p[20] = detune_rs >> 3;
        uint8_t kvs_ams = b[13];
        p[14] = kvs_ams & 3;
        p[15] = kvs_ams >> 2;
        p[16] = b[14];
        uint8_t fcoarse_mode = b[15];
        p[17] = fcoarse_mode & 1;
        p[18] = fcoarse_mode >> 1;
        p[19] = b[16];
    }
    for (int i = 0; i < 9; i++)
        patch[FM_PR1 + i] = bulk[102 + i];
    patch[FM_ALG] &= 31;
    uint8_t oks_fb = bulk[111];
    patch[135] = oks_fb & 7;
    patch[136] = oks_fb >> 3;
    for (int i = 0; i < 4; i++)
        patch[FM_LFS + i] = bulk[112 + i];
    uint8_t lpms_lfw_lks = bulk[116];
    patch[141] = lpms_lfw_lks & 1;
    patch[142] = (lpms_lfw_lks >> 1) & 7;
    patch[143] = lpms_lfw_lks >> 4;
    for (int i = 0; i < 11; i++)
        patch[144 + i] = bulk[117 + i];
    patch[FM_OPS_ON] = 0x3f;
}

/* ------------------------------------------------------------------- synth -- */
void fm_engine_init(void) {}

void fm_synth_init(fm_synth_t *s, uint8_t voices)
{
    for (unsigned i = 0; i < sizeof(*s); i++)
        ((uint8_t *)s)[i] = 0;
    s->voices = voices < 1 ? 1 : voices > FM_MAX_VOICES ? FM_MAX_VOICES : voices;
    s->pitchbend = 0x2000;
    s->alloc_age = 0;
    for (int v = 0; v < FM_MAX_VOICES; v++) {
        s->voice[v].age = 0xFFFF;
        s->voice[v].note = 0xFF;
    }
}

void fm_synth_set_patch(fm_synth_t *s, const uint8_t patch[FM_PATCH_BYTES])
{
    for (int i = 0; i < FM_PATCH_BYTES; i++)
        s->patch[i] = patch[i];
    s->algorithm = patch[FM_ALG] & 31;
    s->feedback = patch[FM_FB];
    /* The LFO belongs to the patch: resetting it here is what msfa's ProgramChange does. */
    fm_lfo_reset(&s->lfo, patch + FM_LFS);
}

void fm_synth_set_algorithm(fm_synth_t *s, uint8_t algorithm)
{
    s->algorithm = algorithm & 31;
    s->patch[FM_ALG] = s->algorithm;
}

void fm_synth_set_feedback(fm_synth_t *s, uint8_t feedback)
{
    s->feedback = feedback & 7;
    s->patch[FM_FB] = s->feedback;
}

/* Voice allocation: free voice, else the oldest released one, else the oldest held.
 * Deterministic and allocation-free, which is what a 64-sample deadline needs. */
static fm_voice_t *fm_alloc_voice(fm_synth_t *s)
{
    fm_voice_t *oldest = 0;
    for (int i = 0; i < s->voices; i++) {
        fm_voice_t *v = &s->voice[i];
        if (!v->live || (v->age == 0xFFFF))
            return v;
    }
    /* steal: prefer a released voice, then the oldest */
    for (int i = 0; i < s->voices; i++) {
        fm_voice_t *v = &s->voice[i];
        if (!v->held) {
            if (!oldest || v->age < oldest->age)
                oldest = v;
        }
    }
    if (!oldest) {
        for (int i = 0; i < s->voices; i++) {
            fm_voice_t *v = &s->voice[i];
            if (!oldest || v->age < oldest->age)
                oldest = v;
        }
    }
    return oldest;
}

void fm_synth_note_on(fm_synth_t *s, uint8_t note, uint8_t velocity)
{
    if (velocity == 0) {
        fm_synth_note_off(s, note);
        return;
    }
    /* msfa's SynthUnit does this on every note-on (synth_unit.cc: lfo_.keydown()),
     * before Dx7Note::init. It is what OSC KEY SYNC acts on. */
    fm_lfo_keydown(&s->lfo);
    /* A key played again retriggers its own voice, as msfa does. */
    for (int i = 0; i < s->voices; i++) {
        fm_voice_t *v = &s->voice[i];
        if (v->live && v->note == note) {
            fm_voice_init(v, s->patch, note, velocity);
            v->note = note;
            v->held = 1;
            v->age = ++s->alloc_age;
            return;
        }
    }
    fm_voice_t *v = fm_alloc_voice(s);
    fm_voice_init(v, s->patch, note, velocity);
    v->note = note;
    v->held = 1;
    v->age = ++s->alloc_age;
}

void fm_synth_note_off(fm_synth_t *s, uint8_t note)
{
    for (int i = 0; i < s->voices; i++) {
        fm_voice_t *v = &s->voice[i];
        if (v->live && v->note == note && v->held) {
            for (int op = 0; op < 6; op++)
                fm_env_keydown(&v->env[op], 0);
            fm_penv_keydown(&v->penv, 0);
            v->held = 0;
            v->down = 0;
        }
    }
}

void fm_synth_all_notes_off(fm_synth_t *s)
{
    for (int i = 0; i < s->voices; i++)
        if (s->voice[i].live && s->voice[i].note != 0xFF)
            fm_synth_note_off(s, s->voice[i].note);
}

void fm_synth_pitch_bend(fm_synth_t *s, uint16_t value)
{
    s->pitchbend = value > 16383 ? 16383 : value;
}

/* Every carrier's gain below the operator threshold means the voice can no longer
 * make sound: msfa keeps such a voice allocated and the caller decides. */
static int fm_voice_silent(const fm_voice_t *v)
{
    const uint8_t *alg = fm_algorithms[v->algorithm & 31];
    for (int op = 0; op < 6; op++) {
        if ((alg[op] & 7) == FM_OUT_BUS_ADD && v->op[op].gain[1] >= 1120)
            return 0;
    }
    return 1;
}

void fm_synth_render(fm_synth_t *s, int32_t *out)
{
    for (int i = 0; i < FM_N; i++)
        out[i] = 0;
    int32_t lfovalue = fm_lfo_getsample(&s->lfo);
    int32_t lfodelay = fm_lfo_getdelay(&s->lfo);
    uint8_t op_mask = s->patch[FM_OPS_ON];
    for (int i = 0; i < s->voices; i++) {
        fm_voice_t *v = &s->voice[i];
        if (!v->live)
            continue;
        fm_voice_compute(v, &s->core, out, lfovalue, lfodelay, s->pitchbend, op_mask);
        if (!v->held && fm_voice_silent(v)) {
            v->live = 0;
            v->age = 0xFFFF;
            v->note = 0xFF;
        }
    }
}

int fm_synth_active(const fm_synth_t *s)
{
    for (int i = 0; i < s->voices; i++)
        if (s->voice[i].live && s->voice[i].note != 0xFF)
            return 1;
    return 0;
}

int fm_synth_voice_count(const fm_synth_t *s)
{
    int n = 0;
    for (int i = 0; i < s->voices; i++)
        if (s->voice[i].live && s->voice[i].note != 0xFF)
            n++;
    return n;
}
