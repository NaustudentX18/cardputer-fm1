/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 cardputer-fm1 contributors.
 *
 * The FM-1 MIDI contract, as a byte-at-a-time parser. See fm_midi.h for the contract and
 * its source. No transmission, no allocation, no hardware: a transport pushes bytes in and
 * reads an event mask back, which is what makes the whole surface testable on the host.
 *
 * Deliberate choices:
 *   - Running status is supported (a real MIDI cable uses it constantly).
 *   - System realtime bytes (F8..FF) may interleave *inside* any other message, including
 *     a SysEx. That is legal MIDI and the FM-1 follows clock while a voice dump streams.
 *   - A SysEx that overflows the buffer is dropped whole, not truncated: a half voice is
 *     worse than none.
 *   - Note-on with velocity 0 is note-off, per the FM-1's own spec and the MIDI rule. */
#include <string.h>
#include "fm_midi.h"

void fm_midi_init(fm_midi_t *m)
{
    memset(m, 0, sizeof(*m));
    m->note_channel = 0xff;          /* omni, as the FM-1 ships */
    m->fx_channel = 1;               /* MIDI channel 2 */
    m->pitchbend = 0x2000;
    /* The FM-1's factory effect state: all six slots present, all off, values at their
     * documented defaults (filter off, reverb room, delay off, distortion off, chorus off,
     * phaser off). */
    for (int s = 0; s < FM_FX_SLOTS; s++) {
        m->fx_on[s] = 0;
        m->fx_param[s][0] = 0;
        m->fx_param[s][1] = 0;
        m->fx_param[s][2] = 0;
        m->fx_param[s][3] = 0;
    }
}

/* The CC 0..23 map, verbatim from the FM-1 document: 4 CCs per slot, in slot order. */
static void apply_fx_cc(fm_midi_t *m, uint8_t cc, uint8_t value)
{
    if (cc > 23)
        return;
    unsigned slot = cc / 4;
    unsigned which = cc % 4;
    if (which == 0) {
        m->fx_on[slot] = value > 0;
    } else {
        m->fx_param[slot][which] = value;
    }
}

static uint32_t handle_channel(fm_midi_t *m, uint8_t status, uint8_t d1, uint8_t d2,
                              fm_midi_out_t *out)
{
    uint8_t type = status & 0xf0;
    uint8_t chan = status & 0x0f;
    uint32_t ev = 0;

    /* FX channel first: on that channel CC1 is Filter Type, per M-VAVE's own warning. */
    if (chan == m->fx_channel) {
        if (type == 0xb0) {
            apply_fx_cc(m, d1, d2);
            return FM_MIDI_EV_CONTROL;
        }
        /* Notes, bend and aftertouch on the FX channel are ignored: the FM-1 routes
         * performance to the note channel and parameters to the FX channel. */
        return FM_MIDI_EV_NONE;
    }

    int is_note_channel = (m->note_channel == 0xff) || (chan == m->note_channel);
    if (!is_note_channel)
        return FM_MIDI_EV_NONE;

    switch (type) {
    case 0x90:                                    /* note on */
        if (d2 == 0) {
            if (out) { out->note.note = d1; out->note.velocity = 0; out->note.channel = chan; }
            ev |= FM_MIDI_EV_NOTE_OFF;
        } else {
            if (out) { out->note.note = d1; out->note.velocity = d2; out->note.channel = chan; }
            ev |= FM_MIDI_EV_NOTE_ON;
        }
        break;
    case 0x80:                                    /* note off */
        if (out) { out->note.note = d1; out->note.velocity = d2; out->note.channel = chan; }
        ev |= FM_MIDI_EV_NOTE_OFF;
        break;
    case 0xc0:                                    /* program change: one data byte */
        if (out) out->program = d1;
        ev |= FM_MIDI_EV_PATCH;
        break;
    case 0xe0: {                                  /* pitch bend, 14 bit */
        uint16_t v = (uint16_t)((uint16_t)d1 | ((uint16_t)d2 << 7));
        m->pitchbend = v;
        if (out) out->bend = v;
        ev |= FM_MIDI_EV_CONTROL;
        break;
    }
    case 0xd0:                                    /* channel aftertouch */
        ev |= FM_MIDI_EV_CONTROL;                 /* accepted, not yet modulated */
        break;
    case 0xb0: {                                  /* control change */
        if (d1 == 1) {
            m->modwheel = d2;
            ev |= FM_MIDI_EV_CONTROL;
        } else if (d1 == 64) {
            m->sustain = d2 >= 64;
            ev |= FM_MIDI_EV_CONTROL;
        } else {
            /* any other CC on the note channel: ignored, as the FM-1 document says the
             * parameters are all on the FX channel */
        }
        break;
    }
    default:
        break;
    }
    return ev;
}

uint32_t fm_midi_feed(fm_midi_t *m, uint8_t b, fm_midi_out_t *out)
{
    uint32_t ev = 0;

    /* System realtime may appear anywhere, even inside a SysEx. */
    if (b >= 0xf8) {
        switch (b) {
        case 0xf8:
            m->clock_count++;
            break;
        case 0xfa:
        case 0xfb:                                /* the FM-1 treats Continue as Start */
            m->running = 1;
            ev |= FM_MIDI_EV_TRANSPORT;
            break;
        case 0xfc:
            m->running = 0;
            ev |= FM_MIDI_EV_TRANSPORT;
            break;
        default:
            break;                                /* F9/FD/FE: ignored */
        }
        return ev;
    }

    if (b >= 0xf0) {
        if (b == 0xf0) {
            m->sysex_len = 1;
            m->sysex[0] = 0xf0;
            m->sysex_overflow = 0;
            return FM_MIDI_EV_NONE;
        }
        if (m->sysex_len) {
            if (b == 0xf7 && !m->sysex_overflow && m->sysex_len < FM_MIDI_SYSEX_MAX) {
                m->sysex[m->sysex_len++] = 0xf7;
                uint8_t patch[FM_PATCH_BYTES];
                uint8_t bank[32 * FM_PACKED_BYTES];
                uint8_t dev = 0;
                uint8_t param, value;
                int r = fm_sysex_decode(m->sysex, m->sysex_len, patch, bank, &dev);
                if (r == FM_SYSEX_OK || r == FM_SYSEX_BAD_CHECKSUM) {
                    /* remember it in the packed form: a bank is 32 of those, and a single
                     * voice fits the same slot as its first entry */
                    if (r == FM_SYSEX_BAD_LENGTH || m->sysex[3] == 0x00) {
                        for (int k = 0; k < FM_PACKED_BYTES; k++)
                            m->patch_packed[k] = patch[k];
                    } else {
                        memcpy(m->patch_packed, bank, FM_PACKED_BYTES);
                        memcpy(m->bank_packed, bank, sizeof(m->bank_packed));
                        m->bank_valid = 1;
                    }
                    m->patch_valid = 1;
                    m->dirty_patch = 1;
                    ev |= FM_MIDI_EV_PATCH;
                } else if (fm_sysex_param_write(m->sysex, m->sysex_len, &param, &value) == 0) {
                    m->last_param = param;
                    m->last_value = value;
                    m->dirty_params = 1;
                    ev |= FM_MIDI_EV_PARAM;
                }
            }
            m->sysex_len = 0;                     /* whatever happened, the frame is done */
            return ev;
        }
        /* any other system-common byte cancels nothing and means nothing here */
        return FM_MIDI_EV_NONE;
    }

    /* inside a SysEx: accumulate data, or note the overflow and keep discarding */
    if (m->sysex_len) {
        if (!m->sysex_overflow) {
            if (m->sysex_len < FM_MIDI_SYSEX_MAX)
                m->sysex[m->sysex_len++] = b;
            else
                m->sysex_overflow = 1;
        }
        return FM_MIDI_EV_NONE;
    }

    /* channel voice, with running status (state lives in the instance, so several
     * transports can share one parser without corrupting each other) */
    if (b & 0x80) {
        m->running_status = b;
        m->have = 0;
        return FM_MIDI_EV_NONE;
    }

    if (!m->running_status)
        return FM_MIDI_EV_NONE;

    m->data[m->have++] = b;
    uint8_t type = m->running_status & 0xf0;
    unsigned need = (type == 0xc0 || type == 0xd0) ? 1u : 2u;
    if (m->have < need)
        return FM_MIDI_EV_NONE;
    m->have = 0;
    return handle_channel(m, m->running_status, m->data[0], need == 2 ? m->data[1] : 0, out);
}

int fm_midi_take_patch(fm_midi_t *m, uint8_t patch[FM_PATCH_BYTES])
{
    if (!m->patch_valid)
        return -1;
    fm_unpack_patch(patch, m->patch_packed);
    m->patch_valid = 0;
    m->dirty_patch = 0;
    return 0;
}

int fm_midi_take_bank(fm_midi_t *m, uint8_t packed[32 * FM_PACKED_BYTES])
{
    /* only a bank dump fills all 32; a single voice leaves the rest untouched, and the
     * caller decides whether that is acceptable */
    if (!m->patch_valid || !m->bank_valid)
        return -1;
    memcpy(packed, m->bank_packed, 32 * FM_PACKED_BYTES);
    m->bank_valid = 0;
    m->patch_valid = 0;
    m->dirty_patch = 0;
    return 0;
}
