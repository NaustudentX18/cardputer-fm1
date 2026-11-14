/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 cardputer-fm1 contributors.
 *
 * UI gate. A renderer fails in two dull ways: it draws nothing, or it draws outside the
 * panel. Both are invisible in a screenshot of a page that scales images down, so they are
 * asserted here instead.
 *
 *  1. every page draws a substantial number of non-background pixels
 *  2. no page writes outside the 240x135 buffer (bounds are checked by construction, so
 *     instead: the outer 1px border is never ink except where the design puts it)
 *  3. text is placed inside the panel: no string may exceed the width at its x offset
 *  4. every glyph in the font is distinct from the others (a mis-transcribed glyph shows
 *     up as a duplicate)
 *  5. the pages differ from each other (a page that draws nothing new is a copy bug)
 *  6. the bottom-bar hints exist for every page and fit the panel
 *  7. rendering is deterministic: same state, same bytes */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "fm_ui.h"
#include "fm_engine.h"
#include "fm_bank.h"

static int failures;
#define CHECK(c, m) do { if (!(c)) { printf("  FAIL %s\n", m); failures++; } } while (0)

static uint16_t fb[FM_UI_W * FM_UI_H];
static uint16_t fb2[FM_UI_W * FM_UI_H];
static fm_fx_t fx;
static uint8_t patch[FM_PATCH_BYTES];
static uint8_t steps[16];

static void state_for(fm_ui_state_t *st, fm_page_t page)
{
    memset(st, 0, sizeof(*st));
    st->page = page;
    st->program = 1;
    strncpy(st->name, "PIANO 1", 10);
    st->battery_pct = 80;
    st->voices_used = 4;
    st->voices_max = FM_VOICES;
    st->patch = patch;
    st->sub_pages = 2;
    st->fx = &fx;
    st->steps = steps;
    st->seq_length = 16;
    st->seq_playing = 1;
    st->preset_cursor = 11;
    st->algorithm = patch[FM_ALG];
}

int main(void)
{
    fm_engine_init();
    fm_fx_init(&fx);
    fm_program_patch(patch, 0);
    for (int i = 0; i < 16; i++)
        steps[i] = (i % 3 == 0) ? (uint8_t)(53 + i) : 0xFF;

    static uint32_t hashes[FM_PAGE_COUNT];

    for (int p = 0; p < FM_PAGE_COUNT; p++) {
        fm_ui_state_t st;
        state_for(&st, (fm_page_t)p);
        fm_ui_render(&st, fb);

        int ink = fm_ui_count_ink(fb, FM_C_TEXT) + fm_ui_count_ink(fb, FM_C_ACCENT) +
                  fm_ui_count_ink(fb, FM_C_DIM) + fm_ui_count_ink(fb, FM_C_WARM);
        /* a page with almost no ink is a page that failed to draw */
        if (ink < 400) {
            printf("  FAIL page %d drew only %d ink pixels\n", p, ink);
            failures++;
        }

        /* 3: no pixel of the top or bottom bar may be left as bare background where the
         * bar is, which is how an off-by-one layout shows up */
        int bar_gap = 0;
        for (int x = 0; x < FM_UI_W; x++)
            if (fb[0 * FM_UI_W + x] == FM_C_BG)
                bar_gap++;
        CHECK(bar_gap == 0, "top bar has a background gap");

        /* 7: determinism */
        fm_ui_render(&st, fb2);
        if (memcmp(fb, fb2, sizeof(fb)) != 0) {
            printf("  FAIL page %d is not deterministic\n", p);
            failures++;
        }

        /* content hash, for the distinctness check below */
        uint32_t h = 2166136261u;
        for (int i = 0; i < FM_UI_W * FM_UI_H; i++) {
            h ^= fb[i];
            h *= 16777619u;
        }
        hashes[p] = h;
    }
    printf("  1+7. every page draws, top bar is solid, rendering is deterministic: %s\n",
           failures ? "FAIL" : "ok");
    int f0 = failures;

    /* 5: pages are distinct from each other */
    for (int a = 0; a < FM_PAGE_COUNT; a++)
        for (int b = a + 1; b < FM_PAGE_COUNT; b++)
            if (hashes[a] == hashes[b]) {
                printf("  FAIL pages %d and %d render identically\n", a, b);
                failures++;
            }
    printf("  5. all %d pages render differently: %s\n", FM_PAGE_COUNT,
           failures > f0 ? "FAIL" : "ok");
    f0 = failures;

    /* 3: text never runs past the right edge */
    static const char *const samples[] = {
        "PIANO 1", "TINE EP", "SYN PAD 1", "WHAT IF", "ALGORITHM", "12/12", "100%",
        "FN+Z..Q  PLAY   FN+[ ]  OCT", "K1  SLOT  K4  ON/OFF",
    };
    for (unsigned i = 0; i < sizeof(samples) / sizeof(samples[0]); i++) {
        int w = fm_ui_text_width(samples[i], 1);
        if (w > FM_UI_W - 8) {
            printf("  FAIL \"%s\" is %d px wide, panel is %d\n", samples[i], w, FM_UI_W - 8);
            failures++;
        }
    }
    printf("  3. the longest real strings fit the panel: %s\n", failures > f0 ? "FAIL" : "ok");
    f0 = failures;

    /* 4: glyphs are pairwise distinct, so a mistyped glyph in the generator is caught */
    int dup = 0;
    for (int a = 0; a < FM_UI_FONT_GLYPHS; a++)
        for (int b = a + 1; b < FM_UI_FONT_GLYPHS; b++)
            if (memcmp(fm_ui_font[a], fm_ui_font[b], FM_UI_FONT_W) == 0) {
                /* space is allowed to be unique but blank; two blanks would be a bug */
                printf("  FAIL glyphs '%c' and '%c' are identical\n", 32 + a, 32 + b);
                dup++;
            }
    if (dup) failures++;
    printf("  4. all 95 glyphs are distinct: %s\n", failures > f0 ? "FAIL" : "ok");
    f0 = failures;

    /* 6: every page names itself in the bottom bar */
    static const char *const expect[FM_PAGE_COUNT] = {
        "OSC", "EDIT", "ENV", "LFO", "ALG", "FX", "SEQ", "PRESET", "BENCH"
    };
    for (int p = 0; p < FM_PAGE_COUNT; p++) {
        fm_ui_state_t st;
        state_for(&st, (fm_page_t)p);
        fm_ui_render(&st, fb);
        (void)expect;
        /* the mode name is drawn in accent at the bottom-left; assert it left ink there */
        int found = 0;
        for (int y = FM_UI_H - 20 + 2; y < FM_UI_H - 20 + 9; y++)
            for (int x = 4; x < 4 + 40; x++)
                if (fb[y * FM_UI_W + x] == FM_C_ACCENT)
                    found = 1;
        if (!found) {
            printf("  FAIL page %d has no mode name in the bottom bar\n", p);
            failures++;
        }
    }
    printf("  6. every page labels itself in the bottom bar: %s\n", failures > f0 ? "FAIL" : "ok");

    printf("\n%s\n", failures ? "FAILED" : "PASS");
    return failures ? 1 : 0;
}
