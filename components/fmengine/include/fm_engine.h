/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2012 Google Inc.  (msfa, music-synthesizer-for-android)
 * Copyright 2026 cardputer-fm1 contributors.
 *
 * Portable 6-operator FM engine: the DX7 voice model and its renderer, as integer C.
 *
 * Provenance: a direct port of msfa's env.cc, pitchenv.cc, lfo.cc, fm_core.cc,
 * fm_op_kernel.cc, dx7note.cc, patch.cc and the inline sin.h / exp2.h / freqlut.h
 * lookups. Arithmetic, table values, block size, accumulation order and rounding
 * are unchanged, so output is bit-identical to upstream at 44100 Hz. The upstream
 * sources are vendored under reference/msfa (Apache-2.0) and test/host/test_engine.c
 * diffs this port against them sample for sample.
 *
 * What this file is NOT: it is not the FM-1's binary engine. M-VAVE's engine is
 * stripped pi32v2 assembly in a proprietary SDK; this is the same published
 * algorithm (the stock image carries msfa's 32-row table verbatim at 0x8BE8C). */
#ifndef FM_ENGINE_H
#define FM_ENGINE_H

#include <stdint.h>
#include "fm_tables.h"

/* ---- limits ------------------------------------------------------------- */
#define FM_OPS 6
#define FM_VOICES 12                  /* FM-1 parity; FM_MAX_VOICES is the hard cap */
#define FM_MAX_VOICES 16
#define FM_PATCH_BYTES 156            /* unpacked voice (msfa layout) */
#define FM_PACKED_BYTES 128           /* DX7 32-voice bank record */
#define FM_BANK_VOICES 128            /* 4 banks x 32 */

/* ---- patch byte offsets (msfa layout, operator 0 = OP6) ----------------- */
enum {
    FM_R1 = 0, FM_L1 = 4, FM_BP = 8, FM_LD, FM_RD, FM_LC, FM_RC, FM_RS,
    FM_AMS, FM_KVS, FM_OL, FM_MODE, FM_FC, FM_FF, FM_DET, FM_OP = 21,
    FM_PR1 = 126, FM_PL1 = 130, FM_ALG = 134, FM_FB, FM_OKS, FM_LFS,
    FM_LFD, FM_LPMD, FM_LAMD, FM_LKS, FM_LFW, FM_LPMS, FM_TRNSP, FM_NAME,
    FM_SIZE = 155,
    FM_OPS_ON = 155               /* byte 155 of the 156-byte patch: operator on/off */
};

/* ---- envelope ---------------------------------------------------------- */
typedef struct {
    int32_t rates[4];
    int32_t levels[4];
    int32_t outlevel;
    int32_t rate_scaling;
    int32_t level;          /* Q24 per doubling, 16 bits more than the DX7 */
    int32_t targetlevel;
    int32_t inc;
    int8_t rising;
    int8_t down;
    int8_t ix;
} fm_env_t;

/* ---- pitch envelope ---------------------------------------------------- */
typedef struct {
    int32_t rates[4];
    int32_t levels[4];
    int32_t level;
    int32_t targetlevel;
    int32_t inc;
    int8_t rising;
    int8_t down;
    int8_t ix;
} fm_penv_t;

/* ---- LFO --------------------------------------------------------------- */
typedef struct {
    uint32_t phase;
    uint32_t delta;
    uint32_t delaystate;
    uint32_t delayinc;
    uint32_t delayinc2;
    uint8_t waveform;
    uint8_t randstate;
    uint8_t sync;
} fm_lfo_t;

/* ---- operator parameters and buses ------------------------------------- */
typedef struct {
    int32_t gain[2];
    int32_t freq;
    int32_t phase;
} fm_op_params_t;

typedef struct {
    fm_env_t env[FM_OPS];
    fm_op_params_t op[FM_OPS];
    int32_t basepitch[FM_OPS];
    int32_t fb_buf[2];
    int32_t fb_shift;
    int32_t pitchmoddepth;
    int32_t pitchmodsens;
    fm_penv_t penv;
    int8_t algorithm;
    int8_t live;
    int8_t down;
    uint8_t note;           /* MIDI note this voice was started with (0xFF = free) */
    uint8_t held;           /* key still down (sustain handled by caller) */
    uint16_t age;           /* allocation order, for oldest-voice stealing */
} fm_voice_t;

/* ---- the two modulation buses are per-renderer, as in msfa's FmCore ----- */
typedef struct {
    int32_t bus[2][FM_N];
} fm_core_t;

typedef struct {
    fm_voice_t voice[FM_MAX_VOICES];
    fm_core_t core;
    fm_lfo_t lfo;
    uint8_t patch[FM_PATCH_BYTES];
    uint8_t algorithm;
    uint8_t feedback;
    int32_t pitchbend;      /* 0x2000 = centre (msfa layout) */
    uint16_t alloc_age;
    uint8_t voices;
} fm_synth_t;

/* ---- API --------------------------------------------------------------- */

void fm_engine_init(void);

/* 128-byte DX7 bank record -> 156-byte patch (msfa UnpackPatch). */
void fm_unpack_patch(uint8_t patch[FM_PATCH_BYTES], const uint8_t bulk[FM_PACKED_BYTES]);

/* Envelope helper the editor and the tests both need (msfa Env::scaleoutlevel). */
int32_t fm_scale_outlevel(int32_t outlevel);

/* Synth lifecycle --------------------------------------------------------*/
void fm_synth_init(fm_synth_t *s, uint8_t voices);
void fm_synth_set_patch(fm_synth_t *s, const uint8_t patch[FM_PATCH_BYTES]);
void fm_synth_note_on(fm_synth_t *s, uint8_t note, uint8_t velocity);
void fm_synth_note_off(fm_synth_t *s, uint8_t note);
void fm_synth_all_notes_off(fm_synth_t *s);
void fm_synth_pitch_bend(fm_synth_t *s, uint16_t value);   /* 0..16383 */
void fm_synth_set_algorithm(fm_synth_t *s, uint8_t algorithm);
void fm_synth_set_feedback(fm_synth_t *s, uint8_t feedback);

/* Render FM_N samples into out (overwrites, msfa block contract). */
void fm_synth_render(fm_synth_t *s, int32_t *out);

/* Nonzero while any voice is still allocated. */
int fm_synth_active(const fm_synth_t *s);
/* Number of voices currently allocated. */
int fm_synth_voice_count(const fm_synth_t *s);

/* Introspection for tests and the UI's algorithm diagram. */
int fm_algorithm_carriers(uint8_t algorithm);
uint8_t fm_algorithm_op_flags(uint8_t algorithm, int op);

#endif /* FM_ENGINE_H */
