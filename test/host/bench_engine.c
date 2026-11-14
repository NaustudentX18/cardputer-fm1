/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 cardputer-fm1 contributors.
 *
 * Cycle benchmark for the ESP32-S3, and a host approximation of it.
 *
 * On the device this is what decides polyphony and sample rate: it measures the real
 * cost of one 64-sample block with N voices, so no estimate in the plan survives
 * contact with silicon. On the host it prints wall-clock time only, which is useful
 * for catching a regression and useless for predicting the S3.
 *
 *   host:   tools/run_host_tests.sh bench
 *   device: idf.py build flash monitor   (prints cycles/block and % of a core)
 */
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "fm_engine.h"
#include "fm_bank.h"

#ifdef ESP_PLATFORM
#include "esp_cpu.h"
#include "esp_log.h"
#include "esp_timer.h"
/* CCOUNT is 32-bit and wraps every ~17.9 s at 240 MHz. Every measured run here is well
 * under that, and the deltas are computed with unsigned arithmetic, so a wrap inside a
 * single measurement is still exact. */
static int64_t cycles_now(void) { return (int64_t)esp_cpu_get_cycle_count(); }
#else
#include <time.h>
static int64_t cycles_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;   /* nanoseconds on the host */
}
#endif

/* On the device, printf goes through the ESP log so the output is visible in a monitor and
 * does not fight the IDF's stdout handling. One line at a time, at INFO level. */
#ifdef ESP_PLATFORM
#define BENCH_PRINTF(...) do { esp_log_write(ESP_LOG_INFO, "fmbench", __VA_ARGS__); \
                               esp_log_write(ESP_LOG_INFO, "fmbench", "\n"); } while (0)
#else
#define BENCH_PRINTF(...) printf(__VA_ARGS__)
#endif

#define BLOCKS 2000          /* ~2.9 s of audio at 44.1 kHz */
#define MAX_V 12

static fm_synth_t s;

/* Worst case for this engine: an algorithm whose operators all sound (six carriers),
 * every envelope fast, feedback on. If this fits, real patches fit. */
static void worst_patch(uint8_t p[FM_PATCH_BYTES])
{
    memset(p, 0, FM_PATCH_BYTES);
    for (int op = 0; op < 6; op++) {
        uint8_t *o = p + op * FM_OP;
        o[FM_R1 + 0] = 99; o[FM_R1 + 1] = 60; o[FM_R1 + 2] = 50; o[FM_R1 + 3] = 45;
        o[FM_L1 + 0] = 99; o[FM_L1 + 1] = 99; o[FM_L1 + 2] = 99; o[FM_L1 + 3] = 99;
        o[FM_BP] = 39; o[FM_RS] = 7; o[FM_KVS] = 7; o[FM_OL] = 90;
        o[FM_MODE] = 0; o[FM_FC] = 1; o[FM_FF] = 0; o[FM_DET] = 7;
    }
    p[FM_PR1 + 0] = 99; p[FM_PR1 + 1] = 99; p[FM_PR1 + 2] = 99; p[FM_PR1 + 3] = 99;
    p[FM_ALG] = 31;             /* six carriers */
    p[FM_FB] = 7;
    p[FM_LFS] = 35; p[FM_LPMD] = 99; p[FM_LAMD] = 99; p[FM_LPMS] = 7; p[FM_AMS] = 3;
    p[FM_LKS] = 1; p[FM_LFW] = 0;
    p[FM_OPS_ON] = 0x3f;
}

int fm_bench_main(void)
{
    fm_engine_init();
    uint8_t patch[FM_PATCH_BYTES];
    worst_patch(patch);
    int32_t out[FM_N];   /* int32_t, not int: on xtensa int32_t is long int */

    BENCH_PRINTF("fm1 block cost: %d blocks x %d samples, worst-case patch\n", BLOCKS, FM_N);
    BENCH_PRINTF("voices  total_ms   per_block   per_sample   %% of 240MHz core\n");

    for (int v = 1; v <= MAX_V; v++) {
        fm_synth_init(&s, (uint8_t)v);
        fm_synth_set_patch(&s, patch);
        /* distinct notes so no retrigger collapses the load */
        for (int n = 0; n < v; n++)
            fm_synth_note_on(&s, (uint8_t)(48 + n * 3), 100);
        /* hold every key: the envelope stays at its loudest for the whole run */
        for (int n = 0; n < v; n++)
            s.voice[n].held = 1;

        int64_t t0 = cycles_now();
        for (int b = 0; b < BLOCKS; b++) {
            fm_synth_render(&s, out);
            /* keep the carriers alive: re-pin the sustain level so this measures the
             * steady state instead of a decay to silence */
            for (int n = 0; n < v; n++)
                for (int op = 0; op < 6; op++)
                    if (s.voice[n].env[op].ix >= 3)
                        s.voice[n].env[op].ix = 2;
        }
        int64_t t1 = cycles_now();
#ifdef ESP_PLATFORM
        double total_cyc = (double)(t1 - t0);
        double per_block = total_cyc / BLOCKS;
        double per_sample = per_block / FM_N;
        double core = per_sample * 100.0 / 240000000.0 * FM_SAMPLE_RATE;
        BENCH_PRINTF("%6d  %8.1f  %10.0f  %11.1f  %15.1f\n", v, total_cyc / 240000.0,
               per_block, per_sample, core);
#else
        double total_ns = (double)(t1 - t0);
        BENCH_PRINTF("%6d  %8.1f  %10.0f  %11.1f  %15s\n", v, total_ns / 1e6,
               total_ns / BLOCKS, total_ns / (BLOCKS * FM_N), "(host ns)");
#endif
    }

    /* the real patches, so the worst case can be compared with what people play */
    BENCH_PRINTF("\nper-voice cost for the 128 factory voices (1 voice, cycles/sample):\n");
    double worst = 0;
    unsigned worst_p = 0;
    for (unsigned p = 0; p < FM_BANK_VOICES; p++) {
        uint8_t patch2[FM_PATCH_BYTES];
        if (fm_program_patch(patch2, p) != 0)
            continue;
        fm_synth_init(&s, 1);
        fm_synth_set_patch(&s, patch2);
        fm_synth_note_on(&s, 60, 100);
        int64_t t0 = cycles_now();
        for (int b = 0; b < 200; b++)
            fm_synth_render(&s, out);
        int64_t t1 = cycles_now();
        double per_sample = (double)(t1 - t0) / (200.0 * FM_N);
        if (per_sample > worst) { worst = per_sample; worst_p = p; }
    }
#ifdef ESP_PLATFORM
    BENCH_PRINTF("  worst: program %u at %.1f cycles/sample -> %.1f %% of a core at 44.1 kHz\n",
           worst_p + 1, worst, worst * FM_SAMPLE_RATE * 100.0 / 240000000.0);
#else
    BENCH_PRINTF("  worst: program %u at %.1f host-ns/sample (not a device figure)\n", worst_p + 1, worst);
#endif
    return 0;
}

#ifndef ESP_PLATFORM
int main(void) { return fm_bench_main(); }
#endif
