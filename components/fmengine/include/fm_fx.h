/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 cardputer-fm1 contributors.
 *
 * The FM-1's six-slot effect chain, as portable integer DSP.
 *
 * Slot order is the instrument's: FILTER, REVERB, DELAY, DISTORTION, CHORUS, PHASER,
 * reorderable. Each slot has an on/off and three or four parameters, addressed exactly as
 * the official MIDI document maps them (CC 0..23, four per slot).
 *
 * Everything is fixed point and allocation-free: buffers are static inside fm_fx_t, sized
 * for the mono 44.1 kHz path. That is what makes 1 second of delay fit in 64 KB by storing
 * 12-bit samples, which is the deviation from the FM-1's 2,040 ms documented in the plan.
 *
 * Stereo note: the engine renders stereo internally but the Cardputer's ES8311 is mono, so
 * the chain runs stereo and the caller sums. Chorus and phaser produce their width from
 * that stereo pair; on the speaker it collapses, on the 3.5 mm jack it does not.
 */
#ifndef FM_FX_H
#define FM_FX_H

#include <stdint.h>

#define FM_FX_SLOTS 6
#define FM_FX_STEREO 2

/* Slot indices, in the FM-1's default chain order. */
enum {
    FM_FX_SLOT_FILTER, FM_FX_SLOT_REVERB, FM_FX_SLOT_DELAY,
    FM_FX_SLOT_DIST, FM_FX_SLOT_CHORUS, FM_FX_SLOT_PHASER
};

/* Parameter ranges straight from the MIDI document. */
enum {
    FM_FILTER_TYPE_MAX = 2,      /* LPF / BPF / HPF */
    FM_FILTER_CUTOFF_MAX = 107,
    FM_FILTER_Q_MAX = 10,
    FM_REVERB_TYPE_MAX = 2,      /* Room / Hall / Plate */
    FM_PCT_MAX = 100
};

/* tan(pi*fc/fs) in Q12 for the 0..127 cutoff scale (30 Hz .. 16 kHz), generated at build
 * time by tools/gen_tables.c: the audio path links no libm. */
extern const uint16_t fm_fx_svf_g[128];

/* Delay is 1,000 ms at 44.1 kHz, mono, 12-bit packed: 66,150 bytes. */
#define FM_FX_DELAY_MS 1000
#define FM_FX_DELAY_SAMPLES (44100 * FM_FX_DELAY_MS / 1000)

/* Reverb: a four-line FDN, the shortest musically usable size at this RAM budget. */
#define FM_FX_REV_LINES 4
#define FM_FX_REV_LEN 1789       /* ~40.6 ms, prime-ish so the lines do not align */
#define FM_FX_REV_BYTES (FM_FX_REV_LINES * FM_FX_REV_LEN * 2)

typedef struct {
    uint8_t on[FM_FX_SLOTS];          /* per slot: the FM-1's "Switch" CC, one per slot */
    /* FILTER */
    uint8_t type, cutoff, q;
    /* REVERB */
    uint8_t rev_type, decay, rev_mix;
    /* DELAY */
    uint8_t delay_decay, delay_rate, delay_mix;
    /* DISTORTION */
    uint8_t dist_gain, dist_tone, dist_level;
    /* CHORUS */
    uint8_t ch_freq, ch_depth, ch_mix;
    /* PHASER */
    uint8_t ph_freq, ph_depth, ph_mix;
} fm_fx_params_t;

typedef struct {
    fm_fx_params_t p;
    uint8_t order[FM_FX_SLOTS];      /* slot indices, the chain order */

    /* filter state: two-pole state-variable, stereo */
    int32_t svf_low[FM_FX_STEREO], svf_band[FM_FX_STEREO];

    /* reverb: four delay lines per channel, int16 */
    int16_t rev_buf[FM_FX_REV_LINES][FM_FX_STEREO][FM_FX_REV_LEN];
    uint16_t rev_pos[FM_FX_REV_LINES];
    int32_t rev_damp[FM_FX_STEREO];

    /* delay: 12-bit packed, interleaved stereo-free (mono send) */
    uint8_t delay_buf[(FM_FX_DELAY_SAMPLES * 3 + 1) / 2];
    uint32_t delay_pos;
    int32_t delay_lp;

    /* chorus: 20 ms max, stereo, one LFO */
    int16_t ch_buf[FM_FX_STEREO][1024];
    uint16_t ch_pos;
    uint32_t ch_phase;

    /* phaser: four allpass stages per channel */
    int32_t ph_state[FM_FX_STEREO][4];
    uint32_t ph_phase;

    /* distortion: DC blocker */
    int32_t dist_dc[FM_FX_STEREO];
} fm_fx_t;

void fm_fx_init(fm_fx_t *fx);

/* The chain: filter -> reverb -> delay -> distortion -> chorus -> phaser, or the order the
 * caller has arranged. `l` and `r` are one sample each, in the engine's int32 domain. */
void fm_fx_process(fm_fx_t *fx, int32_t *l, int32_t *r);

/* Parameter write, in the CC layout: cc 0..23 -> slot cc/4, parameter cc%4. */
void fm_fx_set_cc(fm_fx_t *fx, uint8_t cc, uint8_t value);

/* Move a slot to another position in the chain (the FM-1's SEL-drag behaviour). */
void fm_fx_move(fm_fx_t *fx, unsigned from, unsigned to);

/* True when no slot is enabled and the chain can be skipped entirely. */
int fm_fx_idle(const fm_fx_t *fx);

#endif /* FM_FX_H */
