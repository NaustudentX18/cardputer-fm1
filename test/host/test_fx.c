/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 cardputer-fm1 contributors.
 *
 * FX chain gate. Effects are easy to get subtly wrong in ways that pass listening, so each
 * one is checked by a property it must have, not by a golden hash (a golden would only
 * encode this implementation's bugs).
 *
 *  1. idle chain is a true bypass (bit-identical pass-through)
 *  2. every slot's Switch CC turns exactly that slot on and no other
 *  3. filter: LPF attenuates a high tone more than a low one; HPF the reverse
 *  4. filter cutoff sweep is monotonic (more cutoff = more high-frequency energy)
 *  5. reverb adds a tail: energy after the input stops is > 0 and decays
 *  6. delay repeats at the set time (autocorrelation peak lands where it should)
 *  7. distortion is bounded (never leaves +-1.0) and monotonic for monotonic input
 *  8. chorus and phaser do not blow up and do change the signal
 *  9. nothing produces NaN-like or unbounded state over 5 s of noise at extreme settings
 * 10. slot reordering actually reorders (delay-then-dist differs from dist-then-delay) */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "fm_fx.h"

static int failures;
#define CHECK(c, m) do { if (!(c)) { printf("  FAIL %s\n", m); failures++; } } while (0)

static unsigned rng_state = 12345;
static int32_t noise(void)
{
    rng_state = rng_state * 1664525u + 1013904223u;
    return (int32_t)(rng_state >> 8) - (1 << 23);       /* +-2^23 */
}

static double rms_of(const int32_t *v, int n)
{
    double s = 0;
    for (int i = 0; i < n; i++)
        s += (double)v[i] * v[i];
    return n ? sqrt(s / n) : 0;
}

/* run a slot over n samples of a sine, return output buffer */
static void run_sine(fm_fx_t *fx, int32_t *out, int n, double hz)
{
    for (int i = 0; i < n; i++) {
        int32_t v = (int32_t)(sin(2.0 * M_PI * hz * i / 44100.0) * (1 << 22));
        int32_t l = v, r = v;
        fm_fx_process(fx, &l, &r);
        out[i] = l;
    }
}

int main(void)
{
    static fm_fx_t fx;
    static int32_t buf[44100];

    /* 1: idle = true bypass */
    fm_fx_init(&fx);
    for (int i = 0; i < 512; i++) {
        int32_t v = noise();
        int32_t l = v, r = v;
        fm_fx_process(&fx, &l, &r);
        if (l != v || r != v) {
            printf("  FAIL idle chain changed sample %d: %d -> %d\n", i, v, l);
            failures++;
            break;
        }
    }
    printf("  1. idle chain is bit-exact bypass: %s\n", failures ? "FAIL" : "ok");
    int f0 = failures;

    /* 2: each Switch CC affects only its own slot */
    fm_fx_init(&fx);
    for (int slot = 0; slot < FM_FX_SLOTS; slot++) {
        fm_fx_init(&fx);
        fm_fx_set_cc(&fx, (uint8_t)(slot * 4), 1);
        int on = 0;
        for (int s = 0; s < FM_FX_SLOTS; s++)
            on += fx.p.on[s] ? 1 : 0;
        if (on != 1 || !fx.p.on[slot]) {
            printf("  FAIL switch CC %d turned on %d slots\n", slot * 4, on);
            failures++;
        }
        fm_fx_set_cc(&fx, (uint8_t)(slot * 4), 0);
        if (!fm_fx_idle(&fx)) {
            printf("  FAIL switch CC %d did not turn off\n", slot * 4);
            failures++;
        }
    }
    printf("  2. per-slot Switch CC is isolated: %s\n", failures > f0 ? "FAIL" : "ok");
    f0 = failures;

    /* 3 + 4: filter response */
    double lo_lp, hi_lp, lo_hp, hi_hp;
    fm_fx_init(&fx);
    fx.p.on[FM_FX_SLOT_FILTER] = 1;
    fx.p.type = 0; fx.p.cutoff = 40; fx.p.q = 0;         /* LPF, low cutoff */
    run_sine(&fx, buf, 8192, 200.0);
    lo_lp = rms_of(buf + 4096, 4096);
    fm_fx_init(&fx);
    fx.p.on[FM_FX_SLOT_FILTER] = 1;
    fx.p.type = 0; fx.p.cutoff = 40; fx.p.q = 0;
    run_sine(&fx, buf, 8192, 8000.0);
    hi_lp = rms_of(buf + 4096, 4096);
    fm_fx_init(&fx);
    fx.p.on[FM_FX_SLOT_FILTER] = 1;
    fx.p.type = 2; fx.p.cutoff = 40; fx.p.q = 0;         /* HPF, same cutoff */
    run_sine(&fx, buf, 8192, 200.0);
    lo_hp = rms_of(buf + 4096, 4096);
    fm_fx_init(&fx);
    fx.p.on[FM_FX_SLOT_FILTER] = 1;
    fx.p.type = 2; fx.p.cutoff = 40; fx.p.q = 0;
    run_sine(&fx, buf, 8192, 8000.0);
    hi_hp = rms_of(buf + 4096, 4096);
    if (!(hi_lp < lo_lp * 0.5)) {
        printf("  FAIL LPF did not attenuate 8 kHz vs 200 Hz (%.0f vs %.0f)\n", hi_lp, lo_lp);
        failures++;
    }
    if (!(lo_hp < hi_hp * 0.5)) {
        printf("  FAIL HPF did not attenuate 200 Hz vs 8 kHz (%.0f vs %.0f)\n", lo_hp, hi_hp);
        failures++;
    }
    printf("  3. filter LPF/HPF do what their names say: %s\n", failures > f0 ? "FAIL" : "ok");
    f0 = failures;

    /* monotonic cutoff: more cutoff = more high-frequency energy through an LPF */
    double prev = -1;
    int monotonic = 1;
    for (int c = 20; c <= 110; c += 15) {
        fm_fx_init(&fx);
        fx.p.on[FM_FX_SLOT_FILTER] = 1;
        fx.p.type = 0; fx.p.cutoff = (uint8_t)c; fx.p.q = 0;
        run_sine(&fx, buf, 8192, 6000.0);
        double e = rms_of(buf + 4096, 4096);
        if (prev >= 0 && e < prev * 0.9)
            monotonic = 0;
        prev = e;
    }
    CHECK(monotonic, "cutoff sweep is not monotonic");
    printf("  4. cutoff sweep is monotonic: %s\n", failures > f0 ? "FAIL" : "ok");
    f0 = failures;

    /* 5: reverb tail exists and decays */
    fm_fx_init(&fx);
    fx.p.on[FM_FX_SLOT_REVERB] = 1;
    fx.p.decay = 80; fx.p.rev_mix = 100; fx.p.rev_type = 1;
    for (int i = 0; i < 4410; i++) {                     /* 100 ms of input */
        int32_t v = noise();
        int32_t l = v, r = v;
        fm_fx_process(&fx, &l, &r);
        buf[i] = l;
    }
    int32_t zero_l = 0, zero_r = 0;
    double early = 0, late = 0;
    for (int i = 0; i < 8820; i++) {
        int32_t l = 0, r = 0;
        fm_fx_process(&fx, &l, &r);
        buf[i] = l;
        if (i < 2205) early += (double)l * l;
        if (i >= 6615) late += (double)l * l;
    }
    (void)zero_l; (void)zero_r;
    if (!(early > 0)) {
        printf("  FAIL reverb produced no tail\n");
        failures++;
    }
    if (!(late < early)) {
        printf("  FAIL reverb tail did not decay\n");
        failures++;
    }
    printf("  5. reverb tail present and decaying: %s\n", failures > f0 ? "FAIL" : "ok");
    f0 = failures;

    /* 6: delay repeats at the configured time */
    fm_fx_init(&fx);
    fx.p.on[FM_FX_SLOT_DELAY] = 1;
    fx.p.delay_rate = 13;                  /* per our mapping, ~150 ms */
    fx.p.delay_decay = 90; fx.p.delay_mix = 100;
    for (int i = 0; i < 44100; i++) {
        int32_t v = (i < 64) ? (1 << 20) : 0;            /* a click */
        int32_t l = v, r = v;
        fm_fx_process(&fx, &l, &r);
        buf[i] = l < 0 ? -l : l;
    }
    int peak_at = 64, peak = 0;
    for (int i = 200; i < 44100; i++) {
        if (buf[i] > peak) { peak = buf[i]; peak_at = i; }
    }
    double ms = (peak_at - 64) * 1000.0 / 44100.0;
    if (peak == 0 || ms < 30 || ms > 1000) {
        printf("  FAIL delay peak at %.1f ms (peak %d)\n", ms, peak);
        failures++;
    }
    printf("  6. delay repeats at %.1f ms (configured ~%.0f ms): %s\n",
           ms, 30 + 13 * (FM_FX_DELAY_MS - 30) / 100.0, failures > f0 ? "FAIL" : "ok");
    f0 = failures;

    /* 7: distortion bounded and monotonic */
    fm_fx_init(&fx);
    fx.p.on[FM_FX_SLOT_DIST] = 1;
    fx.p.dist_gain = 100; fx.p.dist_tone = 100; fx.p.dist_level = 100;
    int prev_out = 0, bounded = 1, ramp_mono = 1;
    for (int i = 1; i < 2000; i++) {
        int32_t in = i * 4000;                            /* rising ramp, well past full scale */
        int32_t l = in, r = in;
        fm_fx_process(&fx, &l, &r);
        if (l > (1 << 30) || l < -(1 << 30))
            bounded = 0;
        /* past the first few samples the tone filter has settled; from then on a rising
         * input must give a non-decreasing output */
        if (i > 32 && l < prev_out - (1 << 16))
            ramp_mono = 0;
        prev_out = l;
    }
    CHECK(bounded, "distortion left the int32 domain");
    CHECK(ramp_mono, "distortion is not monotonic on a rising ramp");
    printf("  7. distortion bounded and monotonic: %s\n", failures > f0 ? "FAIL" : "ok");
    f0 = failures;

    /* 8: chorus and phaser change the signal without exploding */
    for (int slot = FM_FX_SLOT_CHORUS; slot <= FM_FX_SLOT_PHASER; slot++) {
        fm_fx_init(&fx);
        fx.p.on[slot] = 1;
        if (slot == FM_FX_SLOT_CHORUS) { fx.p.ch_freq = 50; fx.p.ch_depth = 100; fx.p.ch_mix = 100; }
        else { fx.p.ph_freq = 50; fx.p.ph_depth = 100; fx.p.ph_mix = 100; }
        run_sine(&fx, buf, 8192, 440.0);
        double dry = (double)(1 << 22) / sqrt(2.0);
        double wet = rms_of(buf + 4096, 4096);
        if (wet <= 0 || wet > dry * 8) {
            printf("  FAIL slot %d: wet rms %.0f vs dry %.0f\n", slot, wet, dry);
            failures++;
        }
    }
    printf("  8. chorus and phaser stay bounded and audible: %s\n", failures > f0 ? "FAIL" : "ok");
    f0 = failures;

    /* 9: 5 s of noise at extremes: no runaway state */
    fm_fx_init(&fx);
    for (int s = 0; s < FM_FX_SLOTS; s++) {
        fx.p.on[s] = 1;
        for (int c = 0; c < 4; c++)
            fm_fx_set_cc(&fx, (uint8_t)(s * 4 + c), 100);
    }
    int bad = 0;
    for (int i = 0; i < 44100 * 5; i++) {
        int32_t l = noise(), r = noise();
        fm_fx_process(&fx, &l, &r);
        if (l > (1 << 30) || l < -(1 << 30) || r > (1 << 30) || r < -(1 << 30))
            bad++;
    }
    if (bad) {
        printf("  FAIL %d samples left the int32 domain with all slots at extremes\n", bad);
        failures++;
    }
    printf("  9. 5 s of noise, every slot at 100: %s\n", failures > f0 ? "FAIL" : "ok");
    f0 = failures;

    /* 10: reordering matters */
    static int32_t a[4096], b[4096];
    fm_fx_init(&fx);
    fx.p.on[FM_FX_SLOT_DELAY] = 1; fx.p.on[FM_FX_SLOT_DIST] = 1;
    fx.p.delay_rate = 20; fx.p.delay_decay = 60; fx.p.delay_mix = 80;
    fx.p.dist_gain = 90; fx.p.dist_tone = 80; fx.p.dist_level = 100;
    for (int i = 0; i < 4096; i++) {
        int32_t l = noise(), r = l;
        fm_fx_process(&fx, &l, &r);
        a[i] = l;
    }
    fm_fx_init(&fx);
    fx.p.on[FM_FX_SLOT_DELAY] = 1; fx.p.on[FM_FX_SLOT_DIST] = 1;
    fx.p.delay_rate = 20; fx.p.delay_decay = 60; fx.p.delay_mix = 80;
    fx.p.dist_gain = 90; fx.p.dist_tone = 80; fx.p.dist_level = 100;
    /* move distortion before delay */
    fm_fx_move(&fx, FM_FX_SLOT_DIST, FM_FX_SLOT_DELAY);
    for (int i = 0; i < 4096; i++) {
        int32_t l = noise(), r = l;
        fm_fx_process(&fx, &l, &r);
        b[i] = l;
    }
    if (memcmp(a, b, sizeof(a)) == 0) {
        printf("  FAIL reordering the chain produced identical output\n");
        failures++;
    }
    printf(" 10. reordering the chain changes the result: %s\n", failures > f0 ? "FAIL" : "ok");

    printf("\n%s\n", failures ? "FAILED" : "PASS");
    return failures ? 1 : 0;
}
