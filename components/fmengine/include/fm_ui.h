/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 cardputer-fm1 contributors.
 *
 * The instrument's UI: a 240x135 renderer that owns a real RGB565 framebuffer.
 *
 * Why it is written this way:
 *   - It renders into memory, not into SPI. That means the exact same code produces the
 *     device's screen and the images in docs/ and on the landing page, so a screenshot can
 *     never be a mock-up of a UI that does not exist.
 *   - No allocation, no floating point, no text metrics from a font library: a 5x7 bitmap
 *     font is generated from readable ASCII art (tools/gen_font.py).
 *   - The layout is the FM-1's own: top bar = preset number + name, middle = the mode,
 *     bottom = mode name and page. The Cardputer's screen is 240x135 where the FM-1 has
 *     240x240, so each FM-1 page is relaid out, not scaled.
 *
 * Pages: HOME (oscilloscope + voices), EDIT (operator), ENV, LFO, ALG (32 routes), FX (six
 * slots), SEQ (16 steps), PRESET (browser), BENCH.
 */
#ifndef FM_UI_RENDER_H
#define FM_UI_RENDER_H

#include <stdint.h>
#include "fm_engine.h"
#include "fm_fx.h"
#include "fm_bank.h"

#define FM_UI_W 240
#define FM_UI_H 135
#define FM_UI_FONT_W 5
#define FM_UI_FONT_H 7
#define FM_UI_FONT_GLYPHS 95      /* printable ASCII 32..126 */
#define FM_UI_FONT_SCALE 1

/* RGB565 */
#define FM_RGB(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | (((b) & 0xF8) >> 3)))

/* The palette. Dark instrument panel, one accent, one dim, one warm. Chosen for a 135 px
 * tall LCD read at arm's length: high value contrast, no thin hairlines. */
#define FM_C_BG      FM_RGB(0x0F, 0x17, 0x2A)
#define FM_C_PANEL   FM_RGB(0x1B, 0x23, 0x36)
#define FM_C_EDGE    FM_RGB(0x2A, 0x35, 0x4B)
#define FM_C_TEXT    FM_RGB(0xF8, 0xFA, 0xFC)
#define FM_C_DIM     FM_RGB(0x94, 0xA3, 0xB8)
#define FM_C_ACCENT  FM_RGB(0x22, 0xC5, 0x5E)
#define FM_C_WARM    FM_RGB(0xFD, 0xBA, 0x74)
#define FM_C_ALERT   FM_RGB(0xEF, 0x44, 0x44)

typedef enum {
    FM_PAGE_HOME,
    FM_PAGE_EDIT,
    FM_PAGE_ENV,
    FM_PAGE_LFO,
    FM_PAGE_ALG,
    FM_PAGE_FX,
    FM_PAGE_SEQ,
    FM_PAGE_PRESET,
    FM_PAGE_BENCH,
    FM_PAGE_COUNT
} fm_page_t;

/* Everything the UI needs to draw one frame. Filled by the firmware (or a host harness);
 * the renderer never reaches into the engine itself, so it can be fed synthetic state for
 * tests and documentation. */
typedef struct {
    fm_page_t page;
    uint8_t sub_page;              /* 1/2 style page indicator */
    uint8_t sub_pages;             /* pages in this mode */

    /* top bar */
    uint16_t program;              /* 1..128 */
    char name[11];
    uint8_t battery_pct;           /* 0..100 */
    uint8_t voices_used;
    uint8_t voices_max;

    /* the live patch, for EDIT / ENV / LFO / ALG */
    const uint8_t *patch;          /* 156 bytes, or NULL */

    /* EDIT: which operator is selected, 0..5 (0 = OP6 in the msfa layout; shown as OP1) */
    uint8_t op;
    /* LFO / ENV display helpers */
    uint8_t lfo_wave;
    /* ALG: 0..31 */
    uint8_t algorithm;

    /* FX */
    const fm_fx_t *fx;
    uint8_t fx_cursor;

    /* SEQ */
    const uint8_t *steps;          /* 16 cell notes, 0xFF = empty */
    uint8_t seq_length;
    uint8_t seq_cursor;
    uint8_t seq_playing;

    /* PRESET browser */
    uint8_t preset_cursor;
    /* BENCH */
    const char *const *bench_line;   /* up to 6 short lines */
    uint8_t bench_lines;
} fm_ui_state_t;

/* The renderer. `fb` is FM_UI_W * FM_UI_H pixels, row-major, top-left first. */
void fm_ui_render(const fm_ui_state_t *st, uint16_t *fb);

/* True when a pixel was written: used by the tests to prove the renderer is not blank. */
int fm_ui_count_ink(const uint16_t *fb, uint16_t colour);

/* Draw one string; exposed for tests and for the header/footer helpers. */
void fm_ui_text(uint16_t *fb, int x, int y, const char *s, uint16_t colour, int scale);
int fm_ui_text_width(const char *s, int scale);

extern const uint8_t fm_ui_font[FM_UI_FONT_GLYPHS][FM_UI_FONT_W];

#endif /* FM_UI_RENDER_H */
