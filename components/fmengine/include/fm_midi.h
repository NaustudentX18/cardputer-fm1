/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 cardputer-fm1 contributors.
 *
 * MIDI: the FM-1's own message contract, as a portable parser plus a thin transport
 * interface. Transports (USB MIDI, BLE MIDI, the CAP header UART at 31.25 kbaud) only
 * hand bytes in; everything that decides what a message *means* lives here, so it can be
 * tested on the host without hardware.
 *
 * Contract, from M-VAVE's own "FM-1 MIDI" document:
 *   note channel (midich)      note on/off, program change, pitch bend, channel
 *                              aftertouch, CC1 mod wheel, CC64 sustain
 *   FX channel (effectch)      CC 0..23, the six-slot effect chain
 *   system realtime (no chan)  F8 clock, FA start, FB continue, FC stop
 *   SysEx                      F0 43 10 pp qq vv F7 single-parameter write,
 *                              plus DX7 VCED/VMEM voice dumps
 *   warning in their own doc: CC1 on the FX channel is Filter Type, not mod wheel
 */
#ifndef FM_MIDI_H
#define FM_MIDI_H

#include <stdint.h>
#include "fm_engine.h"
#include "fm_bank.h"

/* The FM-1 SDK caps application SysEx at 1024 bytes, which cannot hold a DX7 bank
 * (4104 bytes). DX7 bank import is a headline feature of the instrument, so this parser
 * buffers 4160: the deviation is deliberate and documented in docs/board.md. A frame that
 * overflows is still dropped whole, never truncated. */
#define FM_MIDI_SYSEX_MAX 4160

/* FX chain: six slots, in the FM-1's default order. */
enum {
    FM_FX_FILTER, FM_FX_REVERB, FM_FX_DELAY, FM_FX_DIST,
    FM_FX_CHORUS, FM_FX_PHASER, FM_FX_SLOTS
};

typedef struct {
    /* channels */
    uint8_t note_channel;        /* 0x00..0x0f, or 0xff = omni (the FM-1's default) */
    uint8_t fx_channel;          /* default 2 -> index 1 */

    /* effect slot state: on/off plus four parameter values, exactly the CC layout */
    uint8_t fx_on[FM_FX_SLOTS];
    uint8_t fx_param[FM_FX_SLOTS][4];

    /* controllers */
    uint16_t pitchbend;          /* 0x2000 centre */
    uint8_t modwheel;
    uint8_t sustain;

    /* transport */
    uint8_t running;             /* FA/FB seen, FC clears */
    uint32_t clock_count;        /* F8 count, for tempo estimation */

    /* channel-message assembly (per instance: several transports feed one parser) */
    uint8_t running_status;
    uint8_t data[2];
    uint8_t have;

    /* The last voice/bank a SysEx produced, kept so the caller can apply it after the
     * frame has already been consumed. Held in the DX7 128-byte packed form, because a
     * bank dump has 32 of them and a single voice is converted to it on arrival. */
    uint8_t patch_valid;
    uint8_t patch_packed[FM_PACKED_BYTES];
    uint8_t bank_valid;
    uint8_t bank_packed[32 * FM_PACKED_BYTES];

    /* SysEx assembly */
    uint16_t sysex_len;
    uint8_t sysex[FM_MIDI_SYSEX_MAX];
    uint8_t sysex_overflow;

    /* what the transport layer must react to */
    uint8_t dirty_patch;         /* a SysEx voice/bank arrived */
    uint8_t dirty_params;        /* an FM-1 parameter write arrived */
    uint8_t last_param;
    uint8_t last_value;
} fm_midi_t;

typedef struct {
    uint8_t note, velocity, channel;
} fm_midi_note_t;

void fm_midi_init(fm_midi_t *m);

/* Feed one byte. Returns a bitmask of events, so the caller can stay branch-free:
 *   FM_MIDI_EV_NOTE_ON, _NOTE_OFF, _PATCH, _PARAM, _TRANSPORT
 * Note/program data is written to the out-parameters when the relevant bit is set. */
enum {
    FM_MIDI_EV_NONE = 0,
    FM_MIDI_EV_NOTE_ON = 1,
    FM_MIDI_EV_NOTE_OFF = 2,
    FM_MIDI_EV_PATCH = 4,
    FM_MIDI_EV_PARAM = 8,
    FM_MIDI_EV_TRANSPORT = 16,
    FM_MIDI_EV_CONTROL = 32
};

typedef struct {
    uint32_t events;
    fm_midi_note_t note;         /* valid for NOTE_ON / NOTE_OFF */
    uint8_t program;             /* valid for PATCH: 0..127 */
    uint16_t bend;               /* valid for CONTROL when the controller is bend */
} fm_midi_out_t;

/* `out` may be NULL when the caller only wants side effects. */
uint32_t fm_midi_feed(fm_midi_t *m, uint8_t byte, fm_midi_out_t *out);

/* Take the voice a SysEx produced as a 156-byte patch the engine can play.
 * Returns 0, or -1 when no voice has arrived since the last call. For a bank dump this is
 * voice 1 of the 32; use fm_midi_take_bank for the whole bank. */
int fm_midi_take_patch(fm_midi_t *m, uint8_t patch[FM_PATCH_BYTES]);

/* Take the whole 32-voice bank (32 x 128 packed bytes) from the last bank dump, plus how
 * many voices it holds. Returns 0, or -1 when the last SysEx was not a bank. */
int fm_midi_take_bank(fm_midi_t *m, uint8_t packed[32 * FM_PACKED_BYTES]);

#endif /* FM_MIDI_H */
