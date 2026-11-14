/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2012 Google Inc.  (table definitions, from music-synthesizer-for-android)
 * Copyright 2026 cardputer-fm1 contributors.
 *
 * msfa lookup tables, generated at build time by tools/gen_tables.c (see that file).
 * Never edit by hand: tools/gen_tables.c prints exactly these arrays from msfa's own
 * Sin::init / Exp2::init / Freqlut::init, and test/host/test_tables.c proves equality. */
#ifndef FM_TABLES_H
#define FM_TABLES_H

#include <stdint.h>

#define FM_N 64            /* samples per control block (msfa LG_N = 6) */
#define FM_LG_N 6

#define FM_SIN_LG_N_SAMPLES 10
#define FM_SIN_N_SAMPLES (1 << FM_SIN_LG_N_SAMPLES)
#define FM_EXP2_LG_N_SAMPLES 10
#define FM_EXP2_N_SAMPLES (1 << FM_EXP2_LG_N_SAMPLES)
#define FM_FREQ_LG_N_SAMPLES 10
#define FM_FREQ_N_SAMPLES (1 << FM_FREQ_LG_N_SAMPLES)
#define FM_FREQ_SAMPLE_SHIFT (24 - FM_FREQ_LG_N_SAMPLES)
#define FM_FREQ_MAX_LOGFREQ_INT 20

#define FM_SAMPLE_RATE 44100

/* Sin: delta table, 2 * 1024 entries (msfa SIN_DELTA) */
extern const int32_t fm_sin_tab[FM_SIN_N_SAMPLES << 1];
/* Exp2: delta table, 2 * 1024 entries */
extern const int32_t fm_exp2_tab[FM_EXP2_N_SAMPLES << 1];
/* Freqlut at FM_SAMPLE_RATE: 1024 + 1 entries */
extern const int32_t fm_freq_tab[FM_FREQ_N_SAMPLES + 1];

/* Scalars msfa computes at start-up; generated from the same expressions. */
extern const uint32_t fm_lfo_unit;
extern const int32_t fm_penv_unit;
/* osc_freq's FINE term: floor(24204406.323123 * ln(1 + 0.01 f) + 0.5), f = 0..99 */
extern const int32_t fm_fine[100];

/* sha256 over the three tables as raw little-endian int32, for the build-time gate */
extern const char fm_tables_sha256[65];

#endif /* FM_TABLES_H */
