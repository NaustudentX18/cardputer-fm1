/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 cardputer-fm1 contributors.
 *
 * The factory bank and the DX7 voice formats: 128-voice bank, program change,
 * single-voice (VCED) and bank (VMEM) SysEx, and the FM-1's own single-parameter
 * SysEx write. */
#ifndef FM_BANK_H
#define FM_BANK_H

#include <stdint.h>
#include "fm_engine.h"

#define FM_BANK_BYTES (FM_BANK_VOICES * FM_PACKED_BYTES)   /* 16384 */

/* The FM-1's 128 factory voices, four banks of 32, in M-VAVE's own order. */
extern const uint8_t fm_factory_bank[FM_BANK_BYTES];
extern const char fm_factory_bank_sha256[65];

/* Voice names are 10 space-padded ASCII bytes at offset 118 of each 128-byte record. */
void fm_voice_name(char out[11], const uint8_t packed[FM_PACKED_BYTES]);

/* Program 0..127 -> 156-byte patch. Returns 0, or -1 when program is out of range. */
int fm_program_patch(uint8_t patch[FM_PATCH_BYTES], unsigned program);

/* DX7 SysEx --------------------------------------------------------------*/
#define FM_SYSEX_VOICE_BYTES 163      /* F0 43 0n 00 01 1B <155> chk F7 */
#define FM_SYSEX_BANK_BYTES 4104      /* F0 43 0n 09 20 00 <4096> chk F7 */

enum {
    FM_SYSEX_OK = 0,
    FM_SYSEX_NOT_FOR_US = -1,   /* another manufacturer / format: ignore quietly */
    FM_SYSEX_BAD_LENGTH = -2,
    FM_SYSEX_BAD_CHECKSUM = -3, /* accepted anyway, reported (editors ship broken ones) */
    FM_SYSEX_NO_SPACE = -4
};

/* Classify and decode a complete SysEx frame. On FM_SYSEX_OK (and
 * FM_SYSEX_BAD_CHECKSUM) the caller's buffers hold the decoded data:
 *  - a single voice -> patch[156]
 *  - a bank        -> packed[32 * 128], plus the device number in *device (0..15)
 * `bytes` must be the whole frame including F0 and F7. */
int fm_sysex_decode(const uint8_t *msg, unsigned bytes,
                    uint8_t patch[FM_PATCH_BYTES],
                    uint8_t packed[32 * FM_PACKED_BYTES],
                    uint8_t *device);

/* The FM-1's own parameter write: F0 43 10 pp qq vv F7 (param = pp*128+qq, 0..155).
 * Returns 0 and writes *param / *value, or -1 when the frame is not that form. */
int fm_sysex_param_write(const uint8_t *msg, unsigned bytes, uint8_t *param, uint8_t *value);

#endif /* FM_BANK_H */
