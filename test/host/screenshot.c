/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 cardputer-fm1 contributors.
 *
 * Screenshot tool: renders the real UI through the real renderer and writes PNGs.
 *
 * This is not a mock-up generator. It links components/fmengine/src/fm_ui.c, fills an
 * fm_ui_state_t, and calls fm_ui_render() into a 240x135 RGB565 buffer. The image written
 * here is the pixel data the device would push to its ST7789V2, so if the UI is wrong the
 * screenshots are wrong in the same way.
 *
 *   tools/render_screens.sh        -> docs/screens/ (PNG, 4x)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "fm_ui.h"
#include "fm_engine.h"
#include "fm_bank.h"

#define ESCALE 1   /* the device's own pixels; the build upscales for the page */

static uint16_t fb[FM_UI_W * FM_UI_H];

/* --- minimal PNG writer (zlib "stored" blocks: no dependency, no compressor) --- */
static uint32_t crc_table[256];
static void crc_init(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[i] = c;
    }
}
static uint32_t crc32_buf(const uint8_t *p, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++)
        c = crc_table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}
static void be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}
static void chunk(FILE *f, const char *type, const uint8_t *data, uint32_t len)
{
    uint8_t hdr[8];
    be32(hdr, len);
    memcpy(hdr + 4, type, 4);
    fwrite(hdr, 1, 8, f);
    if (len)
        fwrite(data, 1, len, f);
    uint8_t *c = malloc(len + 4);
    memcpy(c, type, 4);
    if (len)
        memcpy(c + 4, data, len);
    uint8_t cbuf[4];
    be32(cbuf, crc32_buf(c, len + 4));
    fwrite(cbuf, 1, 4, f);
    free(c);
}

/* scale x4 nearest-neighbour so the 240x135 panel reads at page size */
static int write_png(const char *path, const uint16_t *rgb565, int w, int h, int scale)
{
    int W = w * scale, H = h * scale;
    size_t raw_len = (size_t)H * (1 + (size_t)W * 3);
    uint8_t *raw = calloc(raw_len, 1);
    if (!raw)
        return -1;
    for (int y = 0; y < H; y++) {
        uint8_t *row = raw + (size_t)y * (1 + (size_t)W * 3);
        row[0] = 0;                              /* filter: none */
        for (int x = 0; x < W; x++) {
            uint16_t p = rgb565[(y / scale) * w + (x / scale)];
            uint8_t r = (uint8_t)((p >> 11) & 0x1F), g = (uint8_t)((p >> 5) & 0x3F), b = (uint8_t)(p & 0x1F);
            row[1 + x * 3 + 0] = (uint8_t)((r << 3) | (r >> 2));
            row[1 + x * 3 + 1] = (uint8_t)((g << 2) | (g >> 4));
            row[1 + x * 3 + 2] = (uint8_t)((b << 3) | (b >> 2));
        }
    }
    /* zlib stream of stored deflate blocks */
    size_t nblk = (raw_len + 65534) / 65535;
    size_t zlen = 2 + nblk * 5 + raw_len + 4;
    uint8_t *z = malloc(zlen);
    if (!z) { free(raw); return -1; }
    size_t o = 0;
    z[o++] = 0x78; z[o++] = 0x01;
    size_t off = 0;
    while (off < raw_len) {
        size_t blk = raw_len - off;
        if (blk > 65535) blk = 65535;
        z[o++] = (uint8_t)((off + blk >= raw_len) ? 1 : 0);
        z[o++] = (uint8_t)(blk & 0xFF);
        z[o++] = (uint8_t)(blk >> 8);
        z[o++] = (uint8_t)(~blk & 0xFF);
        z[o++] = (uint8_t)((~blk >> 8) & 0xFF);
        memcpy(z + o, raw + off, blk);
        o += blk;
        off += blk;
    }
    /* adler32 */
    uint32_t a = 1, bsum = 0;
    for (size_t i = 0; i < raw_len; i++) {
        a = (a + raw[i]) % 65521;
        bsum = (bsum + a) % 65521;
    }
    be32(z + o, (bsum << 16) | a);
    o += 4;

    FILE *f = fopen(path, "wb");
    if (!f) { free(raw); free(z); return -1; }
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    fwrite(sig, 1, 8, f);
    uint8_t ihdr[13];
    be32(ihdr, (uint32_t)W);
    be32(ihdr + 4, (uint32_t)H);
    ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
    chunk(f, "IHDR", ihdr, 13);
    chunk(f, "IDAT", z, (uint32_t)o);
    chunk(f, "IEND", NULL, 0);
    fclose(f);
    free(raw);
    free(z);
    return 0;
}

/* ------------------------------------------------------------------ state --- */
static fm_fx_t fx;
static uint8_t patch[FM_PATCH_BYTES];
static uint8_t steps[16];

static void base_state(fm_ui_state_t *st, fm_page_t page, const char *name, unsigned program)
{
    memset(st, 0, sizeof(*st));
    st->page = page;
    st->program = program;
    strncpy(st->name, name, 10);
    st->battery_pct = 78;
    st->voices_used = 3;
    st->voices_max = FM_VOICES;
    st->patch = patch;
    st->sub_pages = 2;
    st->sub_page = 1;
    st->fx = &fx;
}

int main(int argc, char **argv)
{
    const char *outdir = argc > 1 ? argv[1] : "docs/screens";
    char path[512];

    crc_init();
    fm_fx_init(&fx);
    fm_program_patch(patch, 0);                       /* PIANO 1 */
    for (int i = 0; i < 16; i++)
        steps[i] = (i % 3 == 0) ? (uint8_t)(53 + i) : 0xFF;

    fm_ui_state_t st;

    /* HOME: the FM-1's default screen */
    base_state(&st, FM_PAGE_HOME, "PIANO 1", 1);
    fm_ui_render(&st, fb);
    snprintf(path, sizeof(path), "%s/home.png", outdir);
    if (write_png(path, fb, FM_UI_W, FM_UI_H, ESCALE)) return 1;
    printf("%s\n", path);

    /* EDIT: OP1 (slot 0 in the msfa layout is OP6; the UI shows 6-op order) */
    base_state(&st, FM_PAGE_EDIT, "TINE EP", 32);
    st.op = 0;
    fm_program_patch(patch, 31);
    fm_ui_render(&st, fb);
    snprintf(path, sizeof(path), "%s/edit.png", outdir);
    if (write_png(path, fb, FM_UI_W, FM_UI_H, ESCALE)) return 1;
    printf("%s\n", path);

    /* ENV */
    fm_program_patch(patch, 0);
    base_state(&st, FM_PAGE_ENV, "PIANO 1", 1);
    fm_ui_render(&st, fb);
    snprintf(path, sizeof(path), "%s/env.png", outdir);
    if (write_png(path, fb, FM_UI_W, FM_UI_H, ESCALE)) return 1;
    printf("%s\n", path);

    /* LFO */
    patch[FM_LFW] = 4;                                /* sine selected */
    base_state(&st, FM_PAGE_LFO, "SYN PAD 1", 4);
    fm_ui_render(&st, fb);
    snprintf(path, sizeof(path), "%s/lfo.png", outdir);
    if (write_png(path, fb, FM_UI_W, FM_UI_H, ESCALE)) return 1;
    printf("%s\n", path);

    /* ALG: a real routing diagram from the real table */
    fm_program_patch(patch, 0);
    base_state(&st, FM_PAGE_ALG, "PIANO 1", 1);
    st.algorithm = patch[FM_ALG];
    fm_ui_render(&st, fb);
    snprintf(path, sizeof(path), "%s/alg.png", outdir);
    if (write_png(path, fb, FM_UI_W, FM_UI_H, ESCALE)) return 1;
    printf("%s\n", path);

    /* FX: chain with two slots enabled */
    base_state(&st, FM_PAGE_FX, "WARM PAD", 21);
    st.fx_cursor = 1;
    fx.p.on[FM_FX_SLOT_REVERB] = 1;
    fx.p.on[FM_FX_SLOT_DELAY] = 1;
    fm_ui_render(&st, fb);
    snprintf(path, sizeof(path), "%s/fx.png", outdir);
    if (write_png(path, fb, FM_UI_W, FM_UI_H, ESCALE)) return 1;
    printf("%s\n", path);

    /* SEQ */
    base_state(&st, FM_PAGE_SEQ, "PATTERN 1", 1);
    st.steps = steps;
    st.seq_length = 16;
    st.seq_cursor = 3;
    st.seq_playing = 1;
    fm_ui_render(&st, fb);
    snprintf(path, sizeof(path), "%s/seq.png", outdir);
    if (write_png(path, fb, FM_UI_W, FM_UI_H, ESCALE)) return 1;
    printf("%s\n", path);

    /* PRESET browser */
    base_state(&st, FM_PAGE_PRESET, "BROWSE", 1);
    st.preset_cursor = 12;
    fm_ui_render(&st, fb);
    snprintf(path, sizeof(path), "%s/preset.png", outdir);
    if (write_png(path, fb, FM_UI_W, FM_UI_H, ESCALE)) return 1;
    printf("%s\n", path);

    /* BENCH */
    static const char *lines[6] = {
        "VOICES  CYCLES/SAMPLE  % CORE",
        "  12        197.4        3.6",
        "   8        131.2        2.4",
        "TINE EP     174.0        3.2",
        "WORST  #103 201.6        3.7",
        "BUDGET      5442          100",
    };
    base_state(&st, FM_PAGE_BENCH, "BENCH", 1);
    st.bench_line = lines;
    st.bench_lines = 6;
    fm_ui_render(&st, fb);
    snprintf(path, sizeof(path), "%s/bench.png", outdir);
    if (write_png(path, fb, FM_UI_W, FM_UI_H, ESCALE)) return 1;
    printf("%s\n", path);

    return 0;
}
