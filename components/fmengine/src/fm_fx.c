/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 cardputer-fm1 contributors.
 *
 * The six-slot chain. Integer only, no allocation, no libm.
 *
 * Design notes worth knowing before reading the code:
 *   - The filter is a topology-preserving state-variable filter. Its coefficients come
 *     from a small tan(pi*fc/fs) table generated at build time (tools/gen_tables.c), not
 *     from tanf(), because the firmware links no libm into the audio task.
 *   - Reverb is a four-line feedback delay network with damping. It is not a model of the
 *     FM-1's reverb (that is proprietary); it is a reverb with the same three controls
 *     (decay, mix, type) and the same musical range, which is what the panel exposes.
 *   - Delay stores 12 bits per sample: 96 dB of dynamic range on a line whose own noise
 *     floor sits well above that after a few repeats.
 *   - Distortion is tanh-shaped via the same exp2 table the engine uses, with a one-pole
 *     tone control and a DC blocker (an asymmetric shaper would otherwise ruin headroom).
 *   - Chorus and phaser are stereo; the mono speaker sums them, the jack does not. */
#include "fm_fx.h"
#include "fm_tables.h"

/* ---------------------------------------------------------------- helpers --- */
static int32_t clamp_i(int32_t v, int32_t lo, int32_t hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

/* tan(pi * fc / fs) in Q12 for the 0..127 cutoff scale, 30 Hz .. 16 kHz.
 * Generated values are in fm_fx_svf_g[] (see tools/gen_tables.c). */
extern const uint16_t fm_fx_svf_g[128];
/* ------------------------------------------------------------- the filter --- */
/* Topology-preserving SVF (Zavalishin). One multiply per state update, unconditionally
 * stable, and its low/band/high taps are all available, which is what the FM-1's
 * LPF/BPF/HPF switch needs. */
static void svf_process(int32_t *low, int32_t *band, int32_t g_q12, int32_t k_q12, int32_t x)
{
    int32_t g = g_q12;
    int32_t k = k_q12;
    int32_t a1 = 4096 - ((int32_t)((int64_t)g * k >> 12));
    int32_t a2 = (int32_t)((int64_t)g * a1 >> 12);
    int32_t a3 = (int32_t)((int64_t)g * a2 >> 12);
    int32_t v3 = x - *low;
    int32_t v1 = (int32_t)(((int64_t)a1 * *band) >> 12);
    int32_t v2 = v3 - v1;
    int32_t b1 = (int32_t)(((int64_t)a2 * *band) >> 12) + v2;
    *band = (int32_t)(((int64_t)a2 * *band >> 12) + (int32_t)((int64_t)g * v2 >> 12)) + b1;
    *low = (int32_t)(((int64_t)a3 * *band >> 12) + *low) + (int32_t)((int64_t)g * b1 >> 12);
    *band = clamp_i(*band, -(1 << 27), (1 << 27));
    *low = clamp_i(*low, -(1 << 27), (1 << 27));
}

/* The SVF yields low and band directly; high is x - low - k*band, which is the same
 * relation the state update enforces, so all three taps come from one structure. */
static int32_t svf_tap(int32_t low, int32_t band, int32_t x, int32_t k_q12, unsigned type)
{
    switch (type) {
    case 0: return low;                                        /* LPF */
    case 1: return band;                                       /* BPF */
    default: return x - low - (int32_t)(((int64_t)k_q12 * band) >> 12);   /* HPF */
    }
}

/* ---------------------------------------------------------------- reverb --- */
/* Four lines, feedback matrix 0.5 with alternating signs (a Hadamard-like mix that is
 * cheap and does not blow up), plus one-pole damping in the feedback path. */
static const uint16_t fm_rev_len[FM_FX_REV_LINES] = {
    FM_FX_REV_LEN, FM_FX_REV_LEN - 311, FM_FX_REV_LEN - 617, FM_FX_REV_LEN - 883
};

static void reverb_process(fm_fx_t *fx, int32_t *l, int32_t *r)
{
    const fm_fx_params_t *p = &fx->p;
    /* decay 0..100 -> feedback 0..0.93 in Q12 */
    int32_t fb = (int32_t)((int64_t)p->decay * 3809 >> 10);
    /* type picks the damping: room dark, hall medium, plate bright */
    int32_t damp_q12 = p->rev_type == 0 ? 3400 : p->rev_type == 1 ? 2500 : 1400;
    int32_t in_l = *l, in_r = *r;
    int32_t acc[2] = {0, 0};

    for (int i = 0; i < FM_FX_REV_LINES; i++) {
        uint16_t len = fm_rev_len[i];
        uint16_t pos = fx->rev_pos[i];
        int32_t dl = fx->rev_buf[i][0][pos];
        int32_t dr = fx->rev_buf[i][1][pos];
        acc[0] += dl;
        acc[1] += dr;
        /* damping: one-pole low-pass inside the loop */
        fx->rev_damp[0] = dl + (int32_t)((int64_t)(fx->rev_damp[0] - dl) * damp_q12 >> 12);
        fx->rev_damp[1] = dr + (int32_t)((int64_t)(fx->rev_damp[1] - dr) * damp_q12 >> 12);
        int32_t wl = (in_l >> 2) + (int32_t)((int64_t)fx->rev_damp[0] * fb >> 12);
        int32_t wr = (in_r >> 2) + (int32_t)((int64_t)fx->rev_damp[1] * fb >> 12);
        /* the sign flip per line decorrelates the four */
        if (i & 1) {
            wl = -wl;
            wr = -wr;
        }
        fx->rev_buf[i][0][pos] = (int16_t)clamp_i(wl, -32768, 32767);
        fx->rev_buf[i][1][pos] = (int16_t)clamp_i(wr, -32768, 32767);
        fx->rev_pos[i] = (uint16_t)((pos + 1 >= len) ? 0 : pos + 1);
    }
    int32_t mix = p->rev_mix;                /* 0..100 */
    int32_t wet_l = acc[0] >> 1, wet_r = acc[1] >> 1;
    *l = (int32_t)(((int64_t)in_l * (100 - mix) + (int64_t)wet_l * mix) / 100);
    *r = (int32_t)(((int64_t)in_r * (100 - mix) + (int64_t)wet_r * mix) / 100);
}

/* ----------------------------------------------------------------- delay --- */
/* 12-bit packed mono line: two samples in three bytes. */
static inline int16_t delay_get(const fm_fx_t *fx, uint32_t pos)
{
    uint32_t byte = (pos * 3) >> 1;
    uint16_t v;
    if (pos & 1)
        v = (uint16_t)((fx->delay_buf[byte] >> 4) | (fx->delay_buf[byte + 1] << 4));
    else
        v = (uint16_t)(fx->delay_buf[byte] | ((fx->delay_buf[byte + 1] & 0x0f) << 8));
    return (int16_t)(v << 4);                /* sign-extend the 12-bit value */
}

static inline void delay_put(fm_fx_t *fx, uint32_t pos, int16_t sample)
{
    uint32_t byte = (pos * 3) >> 1;
    uint16_t v = (uint16_t)((uint16_t)sample >> 4) & 0x0fff;
    if (pos & 1) {
        fx->delay_buf[byte] = (uint8_t)((fx->delay_buf[byte] & 0x0f) | ((v & 0x0f) << 4));
        fx->delay_buf[byte + 1] = (uint8_t)(v >> 4);
    } else {
        fx->delay_buf[byte] = (uint8_t)(v & 0xff);
        fx->delay_buf[byte + 1] = (uint8_t)((fx->delay_buf[byte + 1] & 0xf0) | (v >> 8));
    }
}

static void delay_process(fm_fx_t *fx, int32_t *l, int32_t *r)
{
    const fm_fx_params_t *p = &fx->p;
    /* rate 0..100 -> 30..1,000 ms, logarithmic enough to feel linear on a knob */
    uint32_t ms = 30 + (uint32_t)p->delay_rate * (FM_FX_DELAY_MS - 30) / 100;
    uint32_t samples = ms * 44100 / 1000;
    if (samples >= FM_FX_DELAY_SAMPLES)
        samples = FM_FX_DELAY_SAMPLES - 1;
    uint32_t read = (fx->delay_pos + FM_FX_DELAY_SAMPLES - samples) % FM_FX_DELAY_SAMPLES;
    int32_t mono = (*l + *r) >> 1;
    int32_t wet = delay_get(fx, read);
    /* one-pole smoothing: a 12-bit line is audibly gritty on repeats without it */
    fx->delay_lp += (wet - fx->delay_lp) >> 2;
    int32_t fb = (int32_t)((int64_t)p->delay_decay * 3500 >> 10);
    int32_t write = mono + (int32_t)((int64_t)fx->delay_lp * fb >> 12);
    delay_put(fx, fx->delay_pos, (int16_t)clamp_i(write >> 4, -2048, 2047));
    fx->delay_pos = (fx->delay_pos + 1) % FM_FX_DELAY_SAMPLES;
    int32_t mix = p->delay_mix;
    int32_t wl = fx->delay_lp;
    *l = (int32_t)(((int64_t)*l * (100 - mix) + (int64_t)wl * mix) / 100);
    *r = (int32_t)(((int64_t)*r * (100 - mix) + (int64_t)wl * mix) / 100);
}

/* ------------------------------------------------------------ distortion --- */
/* tanh(x) by the rational form x(27+x^2)/(27+9x^2): monotonic, odd, |error| < 0.02, and
 * no table or libm. Input and output Q16, saturating at |x| = 4 (where tanh is 0.999). */
static int32_t tanh_approx(int32_t x)
{
    int32_t neg = x < 0;
    int32_t ax = neg ? -x : x;
    if (ax > (4 << 16))
        ax = 4 << 16;
    int32_t x2 = (int32_t)(((int64_t)ax * ax) >> 16);
    int32_t num = (int32_t)(((int64_t)ax * ((27 << 16) + x2)) >> 16);
    int32_t den = (27 << 16) + 9 * x2;
    int32_t y = (int32_t)(((int64_t)num << 16) / den);
    return neg ? -y : y;
}

static void dist_process(fm_fx_t *fx, int32_t *l, int32_t *r)
{
    const fm_fx_params_t *p = &fx->p;
    /* gain 0..100 -> 1x .. 31x */
    int32_t gain = (1 << 16) + (int32_t)((int64_t)p->dist_gain * (30 << 16) / FM_PCT_MAX);
    /* tone 0..100 -> low-pass coefficient 1.0 .. 0.07 (dark .. bright) */
    int32_t tone_coef = 4096 - (int32_t)((int64_t)p->dist_tone * 3800 / FM_PCT_MAX);
    /* level 0..100 -> 0 .. 1.0 in Q12 */
    int32_t level = (int32_t)((int64_t)p->dist_level * 4096 / FM_PCT_MAX);
    int32_t in[2] = {*l, *r};
    int32_t out[2];
    for (int ch = 0; ch < 2; ch++) {
        int32_t x = (int32_t)(((int64_t)in[ch] * gain) >> 16);
        x = clamp_i(x, -(4 << 16), 4 << 16);
        int32_t shaped = tanh_approx(x);                       /* Q16, +-65536 */
        /* tone: one-pole low-pass after the shaper */
        fx->dist_dc[ch] += (int32_t)(((int64_t)(shaped - fx->dist_dc[ch]) * tone_coef) >> 12);
        /* the shaper is odd, so the residual DC is tiny; scale to the engine's domain */
        int32_t y = (int32_t)(((int64_t)fx->dist_dc[ch] * level) >> 12);
        out[ch] = y << 9;                                       /* Q16 -> ~int32 domain */
    }
    *l = out[0];
    *r = out[1];
}

/* ---------------------------------------------------------------- chorus --- */
static void chorus_process(fm_fx_t *fx, int32_t *l, int32_t *r)
{
    const fm_fx_params_t *p = &fx->p;
    /* freq 0..100 -> 0.1..8 Hz in Q32 phase increment */
    uint32_t inc = (uint32_t)((uint64_t)(1u << 24) * (uint32_t)(p->ch_freq * 8 + 10) / (100u * 44100u / 1000u));
    int32_t depth = 4 + (int32_t)((int64_t)p->ch_depth * 20 / 100);   /* 4..24 samples */
    int32_t mix = p->ch_mix;
    fx->ch_buf[0][fx->ch_pos] = (int16_t)clamp_i(*l >> 8, -32768, 32767);
    fx->ch_buf[1][fx->ch_pos] = (int16_t)clamp_i(*r >> 8, -32768, 32767);
    uint32_t phase = fx->ch_phase;
    /* two taps in quadrature, one per channel: the width comes from the phase offset */
    int32_t off0 = (int32_t)((int64_t)depth * (((phase >> 8) & 0xffff) - 32768) >> 15) + depth;
    int32_t off1 = (int32_t)((int64_t)depth * ((((phase + 0x40000000u) >> 8) & 0xffff) - 32768) >> 15) + depth;
    uint16_t r0 = (uint16_t)((fx->ch_pos + 1024 - (uint16_t)(off0 < 0 ? 0 : off0)) & 1023);
    uint16_t r1 = (uint16_t)((fx->ch_pos + 1024 - (uint16_t)(off1 < 0 ? 0 : off1)) & 1023);
    int32_t wet0 = (int32_t)fx->ch_buf[0][r0] << 8;
    int32_t wet1 = (int32_t)fx->ch_buf[1][r1] << 8;
    fx->ch_pos = (uint16_t)((fx->ch_pos + 1) & 1023);
    fx->ch_phase = phase + inc;
    *l = (int32_t)(((int64_t)*l * (100 - mix) + (int64_t)wet0 * mix) / 100);
    *r = (int32_t)(((int64_t)*r * (100 - mix) + (int64_t)wet1 * mix) / 100);
}

/* ---------------------------------------------------------------- phaser --- */
static void phaser_process(fm_fx_t *fx, int32_t *l, int32_t *r)
{
    const fm_fx_params_t *p = &fx->p;
    int32_t depth = p->ph_depth;
    int32_t mix = p->ph_mix;
    /* swept allpass coefficient: 0.25..0.75 as the LFO runs */
    fx->ph_phase += (uint32_t)((uint64_t)(1u << 24) * (uint32_t)(p->ph_freq * 4 + 5) / (100u * 44100u / 1000u));
    int32_t lfo = (int32_t)((fx->ph_phase >> 8) & 0xffff) - 32768;    /* -1..1 */
    int32_t coef = (int32_t)(2048 + (int64_t)depth * lfo * 16 / 32768);
    coef = clamp_i(coef, 512, 3600);
    int32_t in[2] = {*l, *r}, out[2];
    for (int ch = 0; ch < 2; ch++) {
        int32_t x = in[ch];
        for (int s = 0; s < 4; s++) {
            int32_t y = (int32_t)(((int64_t)-coef * x + fx->ph_state[ch][s]) >> 12);
            fx->ph_state[ch][s] = x + (int32_t)(((int64_t)coef * y) >> 12);
            x = y;
        }
        out[ch] = x;
    }
    *l = (int32_t)(((int64_t)in[0] * (100 - mix) + (int64_t)out[0] * mix) / 100);
    *r = (int32_t)(((int64_t)in[1] * (100 - mix) + (int64_t)out[1] * mix) / 100);
}

/* --------------------------------------------------------------- the API --- */
void fm_fx_init(fm_fx_t *fx)
{
    for (unsigned i = 0; i < sizeof(*fx); i++)
        ((uint8_t *)fx)[i] = 0;
    for (int s = 0; s < FM_FX_SLOTS; s++) {
        fx->order[s] = (uint8_t)s;
        fx->p.on[s] = 0;
    }
    /* The FM-1's effect defaults: reverb Room, distortion level and filter cutoff at
     * musically neutral positions, so switching a slot on does something audible. */
    fx->p.type = 0;
    fx->p.cutoff = 107;
    fx->p.q = 0;
    fx->p.rev_type = 0;
    fx->p.decay = 40;
    fx->p.rev_mix = 30;
    fx->p.delay_decay = 40;
    fx->p.delay_rate = 50;
    fx->p.delay_mix = 30;
    fx->p.dist_gain = 0;
    fx->p.dist_tone = 50;
    fx->p.dist_level = 50;
    fx->p.ch_freq = 20;
    fx->p.ch_depth = 50;
    fx->p.ch_mix = 40;
    fx->p.ph_freq = 20;
    fx->p.ph_depth = 50;
    fx->p.ph_mix = 40;
}

void fm_fx_set_cc(fm_fx_t *fx, uint8_t cc, uint8_t value)
{
    if (cc > 23)
        return;
    unsigned slot = cc / 4, which = cc % 4;
    fm_fx_params_t *p = &fx->p;
    if (which == 0) {
        /* CC (4*slot) is that slot's own Switch: 0 = off, >=1 = on */
        p->on[slot] = (uint8_t)(value > 0);
        return;
    }
    switch (slot) {
    case FM_FX_SLOT_FILTER:
        if (which == 1) p->type = value > FM_FILTER_TYPE_MAX ? FM_FILTER_TYPE_MAX : value;
        if (which == 2) p->cutoff = value > FM_FILTER_CUTOFF_MAX ? FM_FILTER_CUTOFF_MAX : value;
        if (which == 3) p->q = value > FM_FILTER_Q_MAX ? FM_FILTER_Q_MAX : value;
        break;
    case FM_FX_SLOT_REVERB:
        if (which == 1) p->rev_type = value > FM_REVERB_TYPE_MAX ? FM_REVERB_TYPE_MAX : value;
        if (which == 2) p->decay = value > FM_PCT_MAX ? FM_PCT_MAX : value;
        if (which == 3) p->rev_mix = value > FM_PCT_MAX ? FM_PCT_MAX : value;
        break;
    case FM_FX_SLOT_DELAY:
        if (which == 1) p->delay_decay = value > FM_PCT_MAX ? FM_PCT_MAX : value;
        if (which == 2) p->delay_rate = value > FM_PCT_MAX ? FM_PCT_MAX : value;
        if (which == 3) p->delay_mix = value > FM_PCT_MAX ? FM_PCT_MAX : value;
        break;
    case FM_FX_SLOT_DIST:
        if (which == 1) p->dist_gain = value > FM_PCT_MAX ? FM_PCT_MAX : value;
        if (which == 2) p->dist_tone = value > FM_PCT_MAX ? FM_PCT_MAX : value;
        if (which == 3) p->dist_level = value > FM_PCT_MAX ? FM_PCT_MAX : value;
        break;
    case FM_FX_SLOT_CHORUS:
        if (which == 1) p->ch_freq = value > FM_PCT_MAX ? FM_PCT_MAX : value;
        if (which == 2) p->ch_depth = value > FM_PCT_MAX ? FM_PCT_MAX : value;
        if (which == 3) p->ch_mix = value > FM_PCT_MAX ? FM_PCT_MAX : value;
        break;
    case FM_FX_SLOT_PHASER:
        if (which == 1) p->ph_freq = value > FM_PCT_MAX ? FM_PCT_MAX : value;
        if (which == 2) p->ph_depth = value > FM_PCT_MAX ? FM_PCT_MAX : value;
        if (which == 3) p->ph_mix = value > FM_PCT_MAX ? FM_PCT_MAX : value;
        break;
    }
}

void fm_fx_move(fm_fx_t *fx, unsigned from, unsigned to)
{
    if (from >= FM_FX_SLOTS || to >= FM_FX_SLOTS || from == to)
        return;
    uint8_t v = fx->order[from];
    if (from < to) {
        for (unsigned i = from; i < to; i++)
            fx->order[i] = fx->order[i + 1];
    } else {
        for (unsigned i = from; i > to; i--)
            fx->order[i] = fx->order[i - 1];
    }
    fx->order[to] = v;
}

int fm_fx_idle(const fm_fx_t *fx)
{
    for (int s = 0; s < FM_FX_SLOTS; s++)
        if (fx->p.on[s])
            return 0;
    return 1;
}

void fm_fx_process(fm_fx_t *fx, int32_t *l, int32_t *r)
{
    if (fm_fx_idle(fx))
        return;
    for (unsigned i = 0; i < FM_FX_SLOTS; i++) {
        unsigned slot = fx->order[i];
        switch (slot) {
        case FM_FX_SLOT_FILTER:
            if (fx->p.on[FM_FX_SLOT_FILTER]) {
                int32_t g = fm_fx_svf_g[fx->p.cutoff & 127];
                /* q 0..10 -> damping k from 2.0 down to 0.2 (Q 0.5 .. 5.0) in Q12 */
                int32_t k = 8192 - (int32_t)((int64_t)fx->p.q * 7372 / FM_FILTER_Q_MAX);
                int32_t inl = *l, inr = *r;
                svf_process(&fx->svf_low[0], &fx->svf_band[0], g, k, inl);
                svf_process(&fx->svf_low[1], &fx->svf_band[1], g, k, inr);
                *l = svf_tap(fx->svf_low[0], fx->svf_band[0], inl, k, fx->p.type);
                *r = svf_tap(fx->svf_low[1], fx->svf_band[1], inr, k, fx->p.type);
            }
            break;
        case FM_FX_SLOT_REVERB:
            if (fx->p.on[FM_FX_SLOT_REVERB]) reverb_process(fx, l, r);
            break;
        case FM_FX_SLOT_DELAY:
            if (fx->p.on[FM_FX_SLOT_DELAY]) delay_process(fx, l, r);
            break;
        case FM_FX_SLOT_DIST:
            if (fx->p.on[FM_FX_SLOT_DIST]) dist_process(fx, l, r);
            break;
        case FM_FX_SLOT_CHORUS:
            if (fx->p.on[FM_FX_SLOT_CHORUS]) chorus_process(fx, l, r);
            break;
        case FM_FX_SLOT_PHASER:
            if (fx->p.on[FM_FX_SLOT_PHASER]) phaser_process(fx, l, r);
            break;
        }
    }
}
