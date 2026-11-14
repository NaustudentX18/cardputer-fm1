/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 cardputer-fm1 contributors.
 *
 * Factory bank access and the DX7 / FM-1 SysEx formats. No allocation, no libc
 * beyond memcpy, so this file links into the firmware unchanged. */
#include "fm_bank.h"

static void fm_copy(uint8_t *dst, const uint8_t *src, unsigned n)
{
    for (unsigned i = 0; i < n; i++)
        dst[i] = src[i];
}

void fm_voice_name(char out[11], const uint8_t packed[FM_PACKED_BYTES])
{
    for (int i = 0; i < 10; i++) {
        uint8_t c = packed[118 + i];
        out[i] = (c >= 32 && c < 127) ? (char)c : ' ';
    }
    out[10] = 0;
    /* trailing spaces off, so the UI does not print a padded box */
    for (int i = 9; i >= 0 && out[i] == ' '; i--)
        out[i] = 0;
}

int fm_program_patch(uint8_t patch[FM_PATCH_BYTES], unsigned program)
{
    if (program >= FM_BANK_VOICES)
        return -1;
    fm_unpack_patch(patch, fm_factory_bank + program * FM_PACKED_BYTES);
    return 0;
}

/* SysEx checksum, DX7 form: the low 7 bits of the two's complement of the sum. */
static uint8_t fm_sysex_checksum(const uint8_t *data, unsigned n)
{
    unsigned sum = 0;
    for (unsigned i = 0; i < n; i++)
        sum += data[i];
    return (uint8_t)((128 - (sum & 127)) & 127);
}

int fm_sysex_decode(const uint8_t *msg, unsigned bytes,
                    uint8_t patch[FM_PATCH_BYTES],
                    uint8_t packed[32 * FM_PACKED_BYTES],
                    uint8_t *device)
{
    if (bytes < 6 || msg[0] != 0xf0 || msg[bytes - 1] != 0xf7)
        return FM_SYSEX_NOT_FOR_US;
    if (msg[1] != 0x43)                    /* Yamaha */
        return FM_SYSEX_NOT_FOR_US;

    unsigned dev = msg[2] & 0x0f;
    if (device)
        *device = (uint8_t)dev;

    if (msg[3] == 0x00 && msg[4] == 0x01 && msg[5] == 0x1b) {
        /* single voice, VCED */
        if (bytes != FM_SYSEX_VOICE_BYTES)
            return FM_SYSEX_BAD_LENGTH;
        const uint8_t *data = msg + 6;
        uint8_t checksum = msg[bytes - 2];
        int bad = fm_sysex_checksum(data, 155) != checksum;
        /* VCED is the 155-byte packed-ish form; msfa unpacks from a bank record,
         * so wrap these 155 bytes into a 128-byte record is not possible. Instead
         * copy the 155 voice bytes and set the two bytes the 156-byte layout adds. */
        for (int i = 0; i < 155; i++)
            patch[i] = data[i];
        patch[FM_OPS_ON] = 0x3f;
        patch[FM_ALG] &= 31;
        return bad ? FM_SYSEX_BAD_CHECKSUM : FM_SYSEX_OK;
    }

    if (msg[3] == 0x09 && msg[4] == 0x20 && msg[5] == 0x00) {
        /* 32-voice bank, VMEM */
        if (bytes != FM_SYSEX_BANK_BYTES)
            return FM_SYSEX_BAD_LENGTH;
        const uint8_t *data = msg + 6;
        uint8_t checksum = msg[bytes - 2];
        int bad = fm_sysex_checksum(data, 4096) != checksum;
        fm_copy(packed, data, 32 * FM_PACKED_BYTES);
        return bad ? FM_SYSEX_BAD_CHECKSUM : FM_SYSEX_OK;
    }

    return FM_SYSEX_NOT_FOR_US;
}

int fm_sysex_param_write(const uint8_t *msg, unsigned bytes, uint8_t *param, uint8_t *value)
{
    /* F0 43 10 pp qq vv F7 */
    if (bytes != 7 || msg[0] != 0xf0 || msg[1] != 0x43 || msg[2] != 0x10 || msg[6] != 0xf7)
        return -1;
    if (msg[3] >= 128 || msg[4] >= 128 || msg[5] >= 128)
        return -1;
    unsigned p = (unsigned)msg[3] * 128u + msg[4];
    if (p > 155)
        return -1;
    *param = (uint8_t)p;
    *value = msg[5];
    return 0;
}
