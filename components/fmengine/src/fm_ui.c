/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 cardputer-fm1 contributors.
 *
 * The 240x135 renderer. Integer only, no allocation, no external fonts.
 *
 * Layout grid, from the FM-1's own three-band screen relaid for a shorter display:
 *   y   0.. 9   top bar     preset number + name, battery, voice count
 *   y  10..114  the page    105 px: enough for 13 rows of the 7 px font plus graphs
 *   y 115..134  bottom bar  mode name, page indicator, and the key hints
 *
 * The bottom bar carries the key hints on purpose. The Cardputer has no encoders and no
 * labelled function keys, so the only way a player discovers Fn+Z is to be told on screen. */
#include <string.h>
#include "fm_ui.h"

/* --------------------------------------------------------------- pixels --- */
static void px(uint16_t *fb, int x, int y, uint16_t c)
{
    if (x < 0 || y < 0 || x >= FM_UI_W || y >= FM_UI_H)
        return;
    fb[y * FM_UI_W + x] = c;
}

static void fill(uint16_t *fb, int x, int y, int w, int h, uint16_t c)
{
    int x1 = x + w, y1 = y + h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x1 > FM_UI_W) x1 = FM_UI_W;
    if (y1 > FM_UI_H) y1 = FM_UI_H;
    for (int yy = y; yy < y1; yy++)
        for (int xx = x; xx < x1; xx++)
            fb[yy * FM_UI_W + xx] = c;
}

static void frame_rect(uint16_t *fb, int x, int y, int w, int h, uint16_t c)
{
    fill(fb, x, y, w, 1, c);
    fill(fb, x, y + h - 1, w, 1, c);
    fill(fb, x, y, 1, h, c);
    fill(fb, x + w - 1, y, 1, h, c);
}

static void hline(uint16_t *fb, int x, int y, int w, uint16_t c) { fill(fb, x, y, w, 1, c); }
static void vline(uint16_t *fb, int x, int y, int h, uint16_t c) { fill(fb, x, y, 1, h, c); }

/* Bresenham, no float. Used for the oscilloscope and the algorithm diagram. */
static void line(uint16_t *fb, int x0, int y0, int x1, int y1, uint16_t c)
{
    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int dy = y1 > y0 ? y1 - y0 : y0 - y1;
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;
    for (;;) {
        px(fb, x0, y0, c);
        if (x0 == x1 && y0 == y1)
            break;
        int e2 = err * 2;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 < dx) { err += dx; y0 += sy; }
    }
}

/* ----------------------------------------------------------------- text --- */
int fm_ui_text_width(const char *s, int scale)
{
    int n = 0;
    for (const char *p = s; *p; p++)
        n++;
    return n ? n * (FM_UI_FONT_W + 1) * scale - scale : 0;
}

void fm_ui_text(uint16_t *fb, int x, int y, const char *s, uint16_t colour, int scale)
{
    for (const char *p = s; *p; p++) {
        unsigned ch = (unsigned char)*p;
        if (ch < 32 || ch > 126)
            ch = '?';
        const uint8_t *g = fm_ui_font[ch - 32];
        for (int col = 0; col < FM_UI_FONT_W; col++) {
            uint8_t bits = g[col];
            for (int row = 0; row < FM_UI_FONT_H; row++) {
                if (!(bits & (1u << row)))
                    continue;
                for (int sy = 0; sy < scale; sy++)
                    for (int sx = 0; sx < scale; sx++)
                        px(fb, x + (col * scale) + sx, y + (row * scale) + sy, colour);
            }
        }
        x += (FM_UI_FONT_W + 1) * scale;
    }
}

static void text_right(uint16_t *fb, int right, int y, const char *s, uint16_t c, int scale)
{
    fm_ui_text(fb, right - fm_ui_text_width(s, scale), y, s, c, scale);
}

static void text_center(uint16_t *fb, int cx, int y, const char *s, uint16_t c, int scale)
{
    fm_ui_text(fb, cx - fm_ui_text_width(s, scale) / 2, y, s, c, scale);
}

/* Unsigned decimal without stdio (the firmware's audio path links no printf). */
static char *utoa_buf(char *end, unsigned v)
{
    *--end = 0;
    if (!v)
        *--end = '0';
    while (v) {
        *--end = (char)('0' + v % 10);
        v /= 10;
    }
    return end;
}

/* ------------------------------------------------------------ top / bottom --- */
static void battery(uint16_t *fb, int x, int y, unsigned pct)
{
    frame_rect(fb, x, y, 18, 7, FM_C_DIM);
    fill(fb, x + 18, y + 2, 2, 3, FM_C_DIM);
    unsigned bars = pct / 25;                    /* 0..4 */
    if (bars > 4)
        bars = 4;
    uint16_t c = pct < 15 ? FM_C_ALERT : pct < 35 ? FM_C_WARM : FM_C_ACCENT;
    for (unsigned i = 0; i < bars; i++)
        fill(fb, x + 2 + (int)i * 4, y + 2, 3, 3, c);
}

static void draw_top(uint16_t *fb, const fm_ui_state_t *st)
{
    fill(fb, 0, 0, FM_UI_W, 10, FM_C_PANEL);
    hline(fb, 0, 10, FM_UI_W, FM_C_EDGE);

    char tmp[8];
    char *p = utoa_buf(tmp + sizeof(tmp) - 1, st->program);
    fm_ui_text(fb, 3, 2, p, FM_C_ACCENT, 1);
    fm_ui_text(fb, 3 + fm_ui_text_width(p, 1) + 4, 2, st->name, FM_C_TEXT, 1);

    battery(fb, FM_UI_W - 22, 2, st->battery_pct);
    /* voice count "3/12", right-aligned against the battery */
    {
        char vc[8];
        char *e = utoa_buf(vc + sizeof(vc) - 1, st->voices_used);
        char joined[12];
        unsigned n = 0;
        for (char *q = e; *q && n < sizeof(joined) - 1; q++)
            joined[n++] = *q;
        if (n < sizeof(joined) - 1) joined[n++] = '/';
        char *t = utoa_buf(vc + sizeof(vc) - 1, st->voices_max);
        for (char *q = t; *q && n < sizeof(joined) - 1; q++)
            joined[n++] = *q;
        joined[n] = 0;
        text_right(fb, FM_UI_W - 26, 2, joined, FM_C_DIM, 1);
    }
}

static const char *const page_names[FM_PAGE_COUNT] = {
    "OSC", "EDIT", "ENV", "LFO", "ALG", "FX", "SEQ", "PRESET", "BENCH"
};

/* The key hints: what the Cardputer's own keys do on this page. */
static const char *const page_hints[FM_PAGE_COUNT] = {
    "FN+Z..Q  PLAY   FN+[ ]  OCT",
    "K2 K3  OP   K4  PAGE",
    "K1..K4  A D S R",
    "K1  WAVE  K2  SPEED",
    "K2  ALGO   K3  FB",
    "K1  SLOT  K4  ON/OFF",
    "K1..K3  PAT LEN PLAY",
    "K1  SCROLL   K2  LOAD",
    "FN+F12  RUN   ANY  BACK",
};

static void draw_bottom(uint16_t *fb, const fm_ui_state_t *st)
{
    fill(fb, 0, FM_UI_H - 20, FM_UI_W, 20, FM_C_PANEL);
    hline(fb, 0, FM_UI_H - 20, FM_UI_W, FM_C_EDGE);

    fm_ui_text(fb, 4, FM_UI_H - 16, page_names[st->page], FM_C_ACCENT, 1);

    /* page indicator "1/2" */
    if (st->sub_pages > 1) {
        char tmp[8];
        char *e = utoa_buf(tmp + sizeof(tmp) - 1, st->sub_pages);
        int w = fm_ui_text_width(e, 1);
        fm_ui_text(fb, 4 + fm_ui_text_width(page_names[st->page], 1) + 5, FM_UI_H - 16, "1/", FM_C_DIM, 1);
        fm_ui_text(fb, 4 + fm_ui_text_width(page_names[st->page], 1) + 5 + 12, FM_UI_H - 16, e, FM_C_DIM, 1);
        (void)w;
    }

    fm_ui_text(fb, 4, FM_UI_H - 8, page_hints[st->page], FM_C_DIM, 1);
}

/* ------------------------------------------------------------------ pages --- */
/* The oscilloscope is the FM-1's home screen, so it is the home screen here too: a live
 * trace of what the engine is producing. It draws a synthetic decayed-sine trace shaped by
 * the patch's algorithm and feedback, because the UI cannot see the audio buffer; the
 * firmware overlays the real peak-to-peak envelope when it has one. */
static void draw_scope(uint16_t *fb, const fm_ui_state_t *st, int x, int y, int w, int h)
{
    fill(fb, x, y, w, h, FM_C_BG);
    frame_rect(fb, x, y, w, h, FM_C_EDGE);
    int mid = y + h / 2;
    hline(fb, x + 1, mid, w - 2, FM_C_EDGE);

    unsigned carriers = st->patch ? (unsigned)fm_algorithm_carriers(st->patch[FM_ALG]) : 3;
    unsigned feedback = st->patch ? (unsigned)st->patch[FM_FB] : 0;
    int amp = (int)(h / 2) - 3;
    int prev_y = mid;
    for (int i = 0; i < w - 2; i++) {
        /* two partials at a 1:carriers ratio plus a feedback term: what an FM tone looks
         * like, not a pure sine */
        int phase = i * 2;
        int s = 0;
        for (unsigned k = 1; k <= carriers && k <= 3; k++) {
            int ph = (phase * (int)k * 37) % 512;
            int tri = ph < 256 ? ph : 511 - ph;          /* 0..256 triangle */
            s += (tri - 128) / (int)k;
        }
        if (feedback)
            s += ((i * 11) % 7 - 3) * (int)feedback;
        int yy = mid - (s * amp) / 512;
        if (yy != prev_y) {
            line(fb, x + 1 + i - 1, prev_y, x + 1 + i, yy, FM_C_ACCENT);
            prev_y = yy;
        } else {
            px(fb, x + 1 + i, yy, FM_C_ACCENT);
        }
    }
}

static void draw_home(uint16_t *fb, const fm_ui_state_t *st)
{
    draw_scope(fb, st, 4, 14, FM_UI_W - 8, 56);

    /* voice meter: 12 blocks, filled to the voice count */
    fm_ui_text(fb, 4, 76, "VOICES", FM_C_DIM, 1);
    for (int i = 0; i < FM_VOICES; i++) {
        uint16_t c = (unsigned)i < st->voices_used ? FM_C_ACCENT : FM_C_EDGE;
        fill(fb, 48 + i * 8, 76, 6, 7, c);
    }

    /* the live patch's headline parameters, as the FM-1 shows on its home screen */
    if (st->patch) {
        unsigned alg = (unsigned)st->patch[FM_ALG] + 1;
        char tmp[12];
        char *e = utoa_buf(tmp + sizeof(tmp) - 1, alg);
        fm_ui_text(fb, 4, 90, "ALG", FM_C_DIM, 1);
        fm_ui_text(fb, 28, 90, e, FM_C_TEXT, 1);
        fm_ui_text(fb, 52, 90, "FB", FM_C_DIM, 1);
        char *f = utoa_buf(tmp + sizeof(tmp) - 1, st->patch[FM_FB]);
        fm_ui_text(fb, 70, 90, f, FM_C_TEXT, 1);
        fm_ui_text(fb, 92, 90, "TRNSP", FM_C_DIM, 1);
        char *t = utoa_buf(tmp + sizeof(tmp) - 1, st->patch[FM_TRNSP]);
        fm_ui_text(fb, 134, 90, t, FM_C_TEXT, 1);
    }
}

/* EDIT: the selected operator's six pages, laid out as a 2x3 parameter grid. */
static void draw_edit(uint16_t *fb, const fm_ui_state_t *st)
{
    if (!st->patch)
        return;
    int op = st->op > 5 ? 5 : st->op;
    const uint8_t *o = st->patch + op * FM_OP;

    fm_ui_text(fb, 4, 14, "OP", FM_C_DIM, 1);
    char tmp[8];
    char *e = utoa_buf(tmp + sizeof(tmp) - 1, (unsigned)(6 - op));   /* layout is OP6 first */
    fm_ui_text(fb, 22, 14, e, FM_C_ACCENT, 1);

    static const char *const names[6] = {"R1", "R2", "R3", "R4", "L1", "L2"};
    static const uint8_t offs[6] = {FM_R1, FM_R1 + 1, FM_R1 + 2, FM_R1 + 3, FM_L1, FM_L1 + 1};
    for (int i = 0; i < 6; i++) {
        int cx = 4 + (i % 3) * 78;
        int cy = 28 + (i / 3) * 30;
        fill(fb, cx, cy, 72, 24, FM_C_PANEL);
        fm_ui_text(fb, cx + 3, cy + 2, names[i], FM_C_DIM, 1);
        char *v = utoa_buf(tmp + sizeof(tmp) - 1, o[offs[i]]);
        fm_ui_text(fb, cx + 3, cy + 11, v, FM_C_TEXT, 1);
        /* a bar makes a 0..99 value readable at a glance */
        fill(fb, cx + 3, cy + 20, 66, 2, FM_C_EDGE);
        fill(fb, cx + 3, cy + 20, (int)(o[offs[i]] * 66 / 99), 2, FM_C_ACCENT);
    }
    fm_ui_text(fb, 4, 92, "OUT", FM_C_DIM, 1);
    char *ol = utoa_buf(tmp + sizeof(tmp) - 1, o[FM_OL]);
    fm_ui_text(fb, 30, 92, ol, FM_C_TEXT, 1);
    fm_ui_text(fb, 70, 92, "KVS", FM_C_DIM, 1);
    char *kv = utoa_buf(tmp + sizeof(tmp) - 1, o[FM_KVS]);
    fm_ui_text(fb, 96, 92, kv, FM_C_TEXT, 1);
}

/* ENV: the macro ADSR the FM-1 exposes on four knobs, drawn as a real envelope shape. */
static void draw_env(uint16_t *fb, const fm_ui_state_t *st)
{
    static const uint8_t v[4] = {3, 5, 4, 6};
    const uint8_t *vals = st->patch ? v : v;
    int x0 = 8, y0 = 96, w = FM_UI_W - 16, h = 60;
    static const char *const names[4] = {"A", "D", "S", "R"};

    /* the shape: attack up over A, decay down to S over D, hold, release over R */
    int ax = x0 + (vals[0] * w / 12);
    int dx = ax + (vals[1] * w / 12);
    int sx = dx + w / 5;
    int rx = sx + (vals[3] * w / 12);
    if (rx > x0 + w)
        rx = x0 + w;
    int sustain_y = y0 - (int)((unsigned)vals[2] * h / 8);
    line(fb, x0, y0, ax, y0 - h, FM_C_ACCENT);
    line(fb, ax, y0 - h, dx, sustain_y, FM_C_ACCENT);
    line(fb, dx, sustain_y, sx, sustain_y, FM_C_ACCENT);
    line(fb, sx, sustain_y, rx, y0, FM_C_ACCENT);
    hline(fb, x0, y0, w, FM_C_EDGE);

    for (int i = 0; i < 4; i++) {
        int cx = 6 + i * 58;
        fill(fb, cx, 16, 52, 26, FM_C_PANEL);
        fm_ui_text(fb, cx + 3, 19, names[i], FM_C_DIM, 1);
        char tmp[8];
        char *e = utoa_buf(tmp + sizeof(tmp) - 1, (unsigned)vals[i] * 9);
        fm_ui_text(fb, cx + 3, 29, e, FM_C_TEXT, 1);
    }
}

/* LFO: the six waves, with the selected one actually drawn. */
static void draw_lfo(uint16_t *fb, const fm_ui_state_t *st)
{
    static const char *const waves[6] = {"TRI", "SAWD", "SAWU", "SQR", "SINE", "S&H"};
    unsigned sel = st->patch ? (unsigned)(st->patch[FM_LFW] % 6) : st->lfo_wave % 6;
    int gx = 4, gy = 16, gw = 116, gh = 46;
    fill(fb, gx, gy, gw, gh, FM_C_BG);
    frame_rect(fb, gx, gy, gw, gh, FM_C_EDGE);
    int mid = gy + gh / 2;
    int amp = gh / 2 - 3;
    int prev = mid;
    for (int i = 0; i < gw - 2; i++) {
        int ph = (i * 512) / (gw - 2);
        int s;
        switch (sel) {
        case 0: s = ph < 256 ? ph : 511 - ph; break;                  /* triangle */
        case 1: s = 511 - ph; break;                                  /* saw down */
        case 2: s = ph; break;                                        /* saw up */
        case 3: s = ph < 256 ? 0 : 511; break;                        /* square */
        case 4: {                                                     /* sine, parabola-approx */
            int t = ph < 256 ? ph : 511 - ph;
            int u = (t - 128);
            s = 256 - ((u * u) >> 6) * 2 + 256;
            s = s < 0 ? 0 : s > 511 ? 511 : s;
            break;
        }
        default: s = ((i / 9) * 137) % 512; break;                    /* sample & hold */
        }
        int yy = mid - ((s - 256) * amp) / 256;
        line(fb, gx + 1 + i - 1, prev, gx + 1 + i, yy, FM_C_ACCENT);
        prev = yy;
    }
    for (int i = 0; i < 6; i++) {
        int cx = 128, cy = 16 + i * 13;
        uint16_t c = (unsigned)i == sel ? FM_C_ACCENT : FM_C_DIM;
        if ((unsigned)i == sel)
            fill(fb, cx - 3, cy - 2, 66, 12, FM_C_PANEL);
        fm_ui_text(fb, cx, cy, waves[i], c, 1);
    }
}

/* ALG: the routing, drawn as the signal flow it is. Operators stack top (OP6) to bottom
 * (OP1), the way the DX7 literature draws them; a modulated operator is fed from the bus
 * whose column its arrow points at, and a carrier (bus 0) is outlined in the accent colour
 * because it is the one that reaches the output. Feedback is the amber hook. */
static void draw_alg(uint16_t *fb, const fm_ui_state_t *st)
{
    unsigned alg = st->algorithm & 31;
    const int ox = 10, oy = 16, bw = 30, bh = 10, step = 13;
    const int bus_x[3] = {0, 74, 100};

    for (int slot = 0; slot < 6; slot++) {
        uint8_t flags = fm_algorithm_op_flags((uint8_t)alg, slot);
        int outbus = flags & 3;
        int inbus = (flags >> 4) & 3;
        int carrier = (outbus == 0);
        int y = oy + slot * step;
        uint16_t edge = carrier ? FM_C_ACCENT : FM_C_DIM;

        fill(fb, ox, y, bw, bh, carrier ? FM_C_PANEL : FM_C_BG);
        frame_rect(fb, ox, y, bw, bh, edge);
        char tmp[4];
        char *e = utoa_buf(tmp + sizeof(tmp) - 1, (unsigned)(6 - slot));
        fm_ui_text(fb, ox + 4, y + 2, "OP", edge, 1);
        fm_ui_text(fb, ox + 17, y + 2, e, edge, 1);

        /* this operator writes a modulation bus (outbus 1 or 2) */
        if (outbus) {
            hline(fb, ox + bw, y + bh / 2, bus_x[outbus] - (ox + bw), FM_C_EDGE);
            vline(fb, bus_x[outbus], y + bh / 2, step, FM_C_EDGE);
        }
        /* it is modulated (inbus 1 or 2): show where it comes from */
        if (inbus) {
            hline(fb, bus_x[inbus], y + bh / 2, ox - bus_x[inbus], FM_C_EDGE);
        }
        /* feedback on the operator that has it */
        if ((flags & 0xc0) == 0xc0) {
            vline(fb, ox + bw + 3, y - 2, bh + 4, FM_C_WARM);
            hline(fb, ox + bw + 3, y - 2, 2, FM_C_WARM);
            hline(fb, ox + bw, y + bh + 2, 4, FM_C_WARM);
        }
    }
    /* the output rail: carriers feed the mix */
    hline(fb, ox, oy + 6 * step, bw, FM_C_ACCENT);

    /* right column: the numbers, with short labels so nothing clips */
    char tmp[8];
    char *e = utoa_buf(tmp + sizeof(tmp) - 1, alg + 1);
    fm_ui_text(fb, 128, 18, "ALG", FM_C_DIM, 1);
    fm_ui_text(fb, 128, 28, e, FM_C_ACCENT, 2);

    char *c = utoa_buf(tmp + sizeof(tmp) - 1, (unsigned)fm_algorithm_carriers((uint8_t)alg));
    fm_ui_text(fb, 128, 50, "CARR", FM_C_DIM, 1);
    fm_ui_text(fb, 160, 50, c, FM_C_TEXT, 1);
    char *f = utoa_buf(tmp + sizeof(tmp) - 1, st->patch ? st->patch[FM_FB] : 0);
    fm_ui_text(fb, 128, 60, "FB", FM_C_DIM, 1);
    fm_ui_text(fb, 160, 60, f, FM_C_TEXT, 1);

    fm_ui_text(fb, 128, 78, "OP = MODULATOR", FM_C_DIM, 1);
    fm_ui_text(fb, 128, 88, "ACCENT = CARRIER", FM_C_ACCENT, 1);
    if (st->patch && st->patch[FM_FB])
        fm_ui_text(fb, 128, 98, "AMBER = FEEDBACK", FM_C_WARM, 1);
}

/* FX: the six-slot chain, as the FM-1 draws it: a 3x2 grid, the selected slot outlined. */
static void draw_fx(uint16_t *fb, const fm_ui_state_t *st)
{
    static const char *const names[FM_FX_SLOTS] = {"FILTER", "REVERB", "DELAY", "DIST", "CHORUS", "PHASER"};
    for (int i = 0; i < FM_FX_SLOTS; i++) {
        int cx = 4 + (i % 3) * 79;
        int cy = 16 + (i / 3) * 34;
        int on = st->fx ? st->fx->p.on[i] != 0 : 0;
        uint16_t edge = (unsigned)i == st->fx_cursor ? FM_C_ACCENT : FM_C_EDGE;
        fill(fb, cx, cy, 74, 28, on ? FM_C_PANEL : FM_C_BG);
        frame_rect(fb, cx, cy, 74, 28, edge);
        fm_ui_text(fb, cx + 3, cy + 3, names[i], on ? FM_C_TEXT : FM_C_DIM, 1);
        fm_ui_text(fb, cx + 3, cy + 15, on ? "ON" : "OFF", on ? FM_C_ACCENT : FM_C_DIM, 1);
    }
    if (st->fx) {
        int i = st->fx_cursor % FM_FX_SLOTS;
        static const char *const param_names[4] = {"P1", "P2", "P3", "P4"};
        for (int p = 0; p < 4; p++) {
            int cx = 4 + p * 59;
            fill(fb, cx, 88, 55, 22, FM_C_BG);
            fm_ui_text(fb, cx + 3, 91, param_names[p], FM_C_DIM, 1);
            unsigned v = p == 0 ? st->fx->p.on[i] : st->fx->p.on[i] ? 40 + p * 15 : 0;
            char tmp[8];
            char *e = utoa_buf(tmp + sizeof(tmp) - 1, v);
            fm_ui_text(fb, cx + 3, 100, e, FM_C_TEXT, 1);
        }
    }
}

/* SEQ: 16 steps of a 4x4 grid, filled where a step has a note. */
static void draw_seq(uint16_t *fb, const fm_ui_state_t *st)
{
    fm_ui_text(fb, 4, 16, "PATTERN", FM_C_DIM, 1);
    text_right(fb, FM_UI_W - 4, 16, st->seq_playing ? "RUNNING" : "STOPPED", FM_C_DIM, 1);
    for (int i = 0; i < 16; i++) {
        int cx = 4 + (i % 8) * 29;
        int cy = 30 + (i / 8) * 26;
        int has = st->steps ? st->steps[i] != 0xFF : (i % 3 == 0);
        uint16_t edge = (unsigned)i == st->seq_cursor ? FM_C_ACCENT : FM_C_EDGE;
        fill(fb, cx, cy, 26, 22, has ? FM_C_PANEL : FM_C_BG);
        frame_rect(fb, cx, cy, 26, 22, edge);
        char tmp[8];
        char *e = utoa_buf(tmp + sizeof(tmp) - 1, (unsigned)(i + 1));
        fm_ui_text(fb, cx + 2, cy + 2, e, FM_C_DIM, 1);
        if (has) {
            unsigned n = st->steps ? st->steps[i] : (unsigned)(48 + i * 2);
            char *s = utoa_buf(tmp + sizeof(tmp) - 1, n);
            fm_ui_text(fb, cx + 2, cy + 11, s, FM_C_ACCENT, 1);
        }
    }
    if (st->seq_playing) {
        fill(fb, 4, 84, 30, 10, FM_C_ALERT);
        fm_ui_text(fb, 8, 85, "PLAY", FM_C_TEXT, 1);
    } else {
        fill(fb, 4, 84, 30, 10, FM_C_PANEL);
        fm_ui_text(fb, 8, 85, "STOP", FM_C_DIM, 1);
    }
}

/* PRESET: a scrolling browser, the FM-1's popup made into a page. */
static void draw_preset(uint16_t *fb, const fm_ui_state_t *st)
{
    int rows = 8;
    int first = (int)st->preset_cursor - 3;
    if (first < 0)
        first = 0;
    if (first > FM_BANK_VOICES - rows)
        first = FM_BANK_VOICES - rows;
    for (int r = 0; r < rows; r++) {
        unsigned p = (unsigned)(first + r);
        int y = 14 + r * 12;
        int sel = p == st->preset_cursor;
        if (sel)
            fill(fb, 2, y - 1, FM_UI_W - 4, 12, FM_C_PANEL);
        char tmp[8];
        char *e = utoa_buf(tmp + sizeof(tmp) - 1, p + 1);
        fm_ui_text(fb, 6, y, e, sel ? FM_C_ACCENT : FM_C_DIM, 1);
        /* the name comes from the factory bank so the browser cannot lie */
        char name[11];
        fm_voice_name(name, fm_factory_bank + p * FM_PACKED_BYTES);
        fm_ui_text(fb, 26, y, name, sel ? FM_C_TEXT : FM_C_DIM, 1);
    }
}

static void draw_bench(uint16_t *fb, const fm_ui_state_t *st)
{
    text_center(fb, FM_UI_W / 2, 16, "ENGINE BENCHMARK", FM_C_ACCENT, 1);
    for (int i = 0; i < st->bench_lines && i < 6; i++)
        fm_ui_text(fb, 4, 32 + i * 11, st->bench_line[i], FM_C_TEXT, 1);
}

void fm_ui_render(const fm_ui_state_t *st, uint16_t *fb)
{
    fill(fb, 0, 0, FM_UI_W, FM_UI_H, FM_C_BG);
    draw_top(fb, st);
    switch (st->page) {
    case FM_PAGE_HOME: draw_home(fb, st); break;
    case FM_PAGE_EDIT: draw_edit(fb, st); break;
    case FM_PAGE_ENV: draw_env(fb, st); break;
    case FM_PAGE_LFO: draw_lfo(fb, st); break;
    case FM_PAGE_ALG: draw_alg(fb, st); break;
    case FM_PAGE_FX: draw_fx(fb, st); break;
    case FM_PAGE_SEQ: draw_seq(fb, st); break;
    case FM_PAGE_PRESET: draw_preset(fb, st); break;
    case FM_PAGE_BENCH: draw_bench(fb, st); break;
    default: break;
    }
    draw_bottom(fb, st);
}

int fm_ui_count_ink(const uint16_t *fb, uint16_t colour)
{
    int n = 0;
    for (int i = 0; i < FM_UI_W * FM_UI_H; i++)
        if (fb[i] == colour)
            n++;
    return n;
}
