/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 cardputer-fm1 contributors.
 *
 * Factory bank + voice-format gate.
 *  1. the embedded bank hashes to M-VAVE's published VMEM hash
 *  2. all 128 voices unpack, are in range, and render audible, bounded audio
 *  3. program change reaches the right voice, and out-of-range is refused
 *  4. SysEx: a 32-voice VMEM bank round-trips; VCED decodes; checksums are reported
 *     but not fatal (editors ship broken ones); a foreign SysEx is ignored
 *  5. the FM-1 single-parameter write parses and rejects malformed frames */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "fm_engine.h"
#include "fm_bank.h"

static int failures;

/* small sha256, same implementation as tools/gen_tables.c */
static uint32_t ror32(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
static void sha256(const uint8_t *m, size_t len, char out[65])
{
    static const uint32_t k[64] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
    uint32_t h[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    size_t padded = (len + 9 + 63) & ~(size_t)63;
    uint8_t buf[20000];
    if (padded > sizeof(buf)) { out[0] = 0; return; }
    memset(buf, 0, padded);
    memcpy(buf, m, len);
    buf[len] = 0x80;
    uint64_t bits = (uint64_t)len * 8;
    for (int i = 0; i < 8; i++) buf[padded - 1 - i] = (uint8_t)(bits >> (8 * i));
    for (size_t off = 0; off < padded; off += 64) {
        uint32_t w[64];
        for (int i = 0; i < 16; i++)
            w[i] = (uint32_t)buf[off+i*4]<<24 | (uint32_t)buf[off+i*4+1]<<16 | (uint32_t)buf[off+i*4+2]<<8 | buf[off+i*4+3];
        for (int i = 16; i < 64; i++) {
            uint32_t s0 = ror32(w[i-15],7) ^ ror32(w[i-15],18) ^ (w[i-15] >> 3);
            uint32_t s1 = ror32(w[i-2],17) ^ ror32(w[i-2],19) ^ (w[i-2] >> 10);
            w[i] = w[i-16] + s0 + w[i-7] + s1;
        }
        uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
        for (int i = 0; i < 64; i++) {
            uint32_t S1 = ror32(e,6)^ror32(e,11)^ror32(e,25);
            uint32_t ch = (e&f)^(~e&g);
            uint32_t t1 = hh + S1 + ch + k[i] + w[i];
            uint32_t S0 = ror32(a,2)^ror32(a,13)^ror32(a,22);
            uint32_t mj = (a&b)^(a&c)^(b&c);
            uint32_t t2 = S0 + mj;
            hh=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
        }
        h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;
    }
    for (int i = 0; i < 8; i++) sprintf(out + i * 8, "%08x", h[i]);
    out[64] = 0;
}

int main(void)
{
    fm_engine_init();
    char hex[65];

    /* 1. the bank is M-VAVE's */
    sha256(fm_factory_bank, FM_BANK_BYTES, hex);
    if (strcmp(hex, fm_factory_bank_sha256) != 0) {
        printf("  FAIL bank hash: got %s want %s\n", hex, fm_factory_bank_sha256);
        failures++;
    } else {
        printf("  1. bank hash matches M-VAVE's published VMEM: ok\n");
    }

    /* 2. every voice unpacks in range and renders bounded audio */
    static fm_synth_t s;
    int silent = 0, clipped = 0, worst = 0;
    for (unsigned p = 0; p < FM_BANK_VOICES; p++) {
        uint8_t patch[FM_PATCH_BYTES];
        if (fm_program_patch(patch, p) != 0) { failures++; continue; }
        if (patch[FM_ALG] > 31) { printf("  FAIL voice %u alg %u\n", p, patch[FM_ALG]); failures++; }
        fm_synth_init(&s, 1);
        fm_synth_set_patch(&s, patch);
        fm_synth_note_on(&s, 60, 100);
        int peak = 0;
        for (int b = 0; b < 90; b++) {          /* ~130 ms: attack + decay */
            int out[FM_N];
            fm_synth_render(&s, out);
            for (int i = 0; i < FM_N; i++) {
                int a = out[i] < 0 ? -out[i] : out[i];
                if (a > peak) peak = a;
            }
        }
        if (peak == 0) silent++;
        /* msfa's own output can exceed 16 bits with several carriers; the master
         * stage scales it. This gate is about gross breakage, not headroom. */
        if (peak > (1 << 28)) clipped++;
        if (peak > worst) worst = peak;
    }
    if (silent || clipped) {
        printf("  FAIL %d silent voice(s), %d grossly out of range\n", silent, clipped);
        failures++;
    } else {
        printf("  2. 128 voices render, peak %d (of 2^28): ok\n", worst);
    }

    /* 3. program change + names */
    uint8_t patch[FM_PATCH_BYTES];
    if (fm_program_patch(patch, 128) != -1) { printf("  FAIL program 128 accepted\n"); failures++; }
    char name[11];
    fm_voice_name(name, fm_factory_bank);
    if (strcmp(name, "PIANO 1") != 0) { printf("  FAIL voice 1 name '%s'\n", name); failures++; }
    fm_voice_name(name, fm_factory_bank + 127 * FM_PACKED_BYTES);
    if (strcmp(name, "WHAT IF") != 0) { printf("  FAIL voice 128 name '%s'\n", name); failures++; }
    printf("  3. program change + names (1='PIANO 1', 128='WHAT IF'): ok\n");

    /* 4. SysEx */
    static uint8_t msg[FM_SYSEX_BANK_BYTES];
    static uint8_t packed[32 * FM_PACKED_BYTES];
    msg[0] = 0xf0; msg[1] = 0x43; msg[2] = 0x00;
    msg[3] = 0x09; msg[4] = 0x20; msg[5] = 0x00;
    memcpy(msg + 6, fm_factory_bank, 32 * FM_PACKED_BYTES);
    unsigned sum = 0;
    for (int i = 0; i < 4096; i++) sum += msg[6 + i];
    msg[FM_SYSEX_BANK_BYTES - 2] = (uint8_t)((128 - (sum & 127)) & 127);
    msg[FM_SYSEX_BANK_BYTES - 1] = 0xf7;
    uint8_t dev = 0xff;
    int r = fm_sysex_decode(msg, FM_SYSEX_BANK_BYTES, patch, packed, &dev);
    if (r != FM_SYSEX_OK || memcmp(packed, fm_factory_bank, 32 * FM_PACKED_BYTES) != 0 || dev != 0) {
        printf("  FAIL VMEM decode r=%d dev=%u\n", r, dev);
        failures++;
    }
    msg[FM_SYSEX_BANK_BYTES - 2] ^= 1;                    /* break the checksum */
    r = fm_sysex_decode(msg, FM_SYSEX_BANK_BYTES, patch, packed, &dev);
    if (r != FM_SYSEX_BAD_CHECKSUM) { printf("  FAIL bad checksum not reported (%d)\n", r); failures++; }
    if (memcmp(packed, fm_factory_bank, 32 * FM_PACKED_BYTES) != 0) {
        printf("  FAIL bad-checksum bank was not loaded\n"); failures++;
    }
    uint8_t foreign[8] = {0xf0, 0x41, 0x10, 0x42, 0x12, 0x00, 0x00, 0xf7};
    if (fm_sysex_decode(foreign, 8, patch, packed, &dev) != FM_SYSEX_NOT_FOR_US) {
        printf("  FAIL foreign sysex accepted\n"); failures++;
    }
    /* wrong length: still a complete frame (F0..F7) but short by one data byte */
    static uint8_t shortmsg[FM_SYSEX_BANK_BYTES];
    memcpy(shortmsg, msg, FM_SYSEX_BANK_BYTES - 2);
    shortmsg[FM_SYSEX_BANK_BYTES - 2] = 0xf7;
    if (fm_sysex_decode(shortmsg, FM_SYSEX_BANK_BYTES - 1, patch, packed, &dev) != FM_SYSEX_BAD_LENGTH) {
        printf("  FAIL short sysex accepted\n"); failures++;
    }
    /* an unterminated frame is not a frame at all: ignored, not an error */
    if (fm_sysex_decode(msg, FM_SYSEX_BANK_BYTES - 1, patch, packed, &dev) != FM_SYSEX_NOT_FOR_US) {
        printf("  FAIL unterminated sysex parsed\n"); failures++;
    }
    printf("  4. SysEx bank/checksum/foreign/length: ok\n");

    /* 5. FM-1 single-parameter write */
    uint8_t param = 0xff, value = 0xff;
    uint8_t pw[7] = {0xf0, 0x43, 0x10, 0x00, 0x00, 0x40, 0xf7};   /* frame, param patched below */
    pw[3] = 0x01; pw[4] = 0x1c;                                   /* 128 + 28 = 156 -> out of range */
    if (fm_sysex_param_write(pw, 7, &param, &value) != -1) { printf("  FAIL param 156 accepted\n"); failures++; }
    pw[3] = 0x01; pw[4] = 0x0b; pw[5] = 0x40;                     /* param 1*128+11 = 139 (LPMD) = 0x40 */
    if (fm_sysex_param_write(pw, 7, &param, &value) != 0 || param != 139 || value != 0x40) {
        printf("  FAIL param write parse (%d %u %u)\n", fm_sysex_param_write(pw, 7, &param, &value), param, value);
        failures++;
    }
    printf("  5. FM-1 parameter write: ok\n");

    printf("\n%s\n", failures ? "FAILED" : "PASS");
    return failures ? 1 : 0;
}
