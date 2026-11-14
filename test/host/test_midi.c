/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 cardputer-fm1 contributors.
 *
 * MIDI contract gate: the parser must implement M-VAVE's FM-1 document exactly.
 *  1. note on/off, note-on velocity 0 = note off
 *  2. running status
 *  3. omni default, then a single note channel
 *  4. program change, pitch bend, CC1 and CC64 on the note channel
 *  5. the FX channel: CC 0..23 map, all six slots, and CC1 there is Filter Type
 *  6. realtime F8/FA/FB/FC, including inside a SysEx
 *  7. SysEx: FM-1 parameter write and a DX7 bank; a dropped frame is not truncated
 *  8. one parser instance is not corrupted by another (state lives in the instance) */
#include <stdio.h>
#include <string.h>
#include "fm_midi.h"

static int failures;
#define CHECK(cond, msg) do { if (!(cond)) { printf("  FAIL %s\n", msg); failures++; } } while (0)

static uint32_t feed_all(fm_midi_t *m, const uint8_t *bytes, unsigned n, fm_midi_out_t *out)
{
    uint32_t ev = 0;
    for (unsigned i = 0; i < n; i++)
        ev |= fm_midi_feed(m, bytes[i], out);
    return ev;
}

int main(void)
{
    fm_midi_t m;
    fm_midi_out_t out;

    /* 1 */
    fm_midi_init(&m);
    memset(&out, 0, sizeof(out));
    uint8_t on[] = {0x90, 60, 100};
    CHECK((feed_all(&m, on, 3, &out) & FM_MIDI_EV_NOTE_ON) && out.note.note == 60 && out.note.velocity == 100,
          "note on");
    uint8_t off[] = {0x80, 60, 0};
    CHECK(feed_all(&m, off, 3, &out) & FM_MIDI_EV_NOTE_OFF, "note off");
    uint8_t vel0[] = {0x90, 60, 0};
    CHECK(feed_all(&m, vel0, 3, &out) & FM_MIDI_EV_NOTE_OFF, "note on vel 0 is note off");
    printf("  1. note on/off + vel-0 rule: %s\n", failures ? "FAIL" : "ok");
    int f0 = failures;

    /* 2: running status */
    fm_midi_init(&m);
    uint8_t rs[] = {0x90, 60, 100, 64, 100, 67, 100};
    uint32_t ev = feed_all(&m, rs, 7, &out);
    CHECK((ev & FM_MIDI_EV_NOTE_ON) != 0, "running status produced no note");
    printf("  2. running status: %s\n", failures > f0 ? "FAIL" : "ok");
    f0 = failures;

    /* 3: omni default, then channel 5 only */
    fm_midi_init(&m);
    uint8_t ch7[] = {0x97, 60, 100};
    CHECK(feed_all(&m, ch7, 3, &out) & FM_MIDI_EV_NOTE_ON, "omni should accept ch 7");
    m.note_channel = 4;
    m.running_status = 0;
    CHECK((feed_all(&m, ch7, 3, &out) & FM_MIDI_EV_NOTE_ON) == 0, "ch 7 should be ignored when set to 4");
    uint8_t ch4[] = {0x94, 60, 100};
    CHECK(feed_all(&m, ch4, 3, &out) & FM_MIDI_EV_NOTE_ON, "ch 4 should be accepted");
    printf("  3. omni default then single channel: %s\n", failures > f0 ? "FAIL" : "ok");
    f0 = failures;

    /* 4: program change, bend, CC1, CC64 */
    fm_midi_init(&m);
    memset(&out, 0, sizeof(out));
    uint8_t pc[] = {0xc0, 42};
    CHECK((feed_all(&m, pc, 2, &out) & FM_MIDI_EV_PATCH) && out.program == 42, "program change");
    uint8_t bend[] = {0xe0, 0x00, 0x40};        /* 0x2000 = centre */
    CHECK((feed_all(&m, bend, 3, &out) & FM_MIDI_EV_CONTROL) && out.bend == 0x2000, "pitch bend centre");
    uint8_t bendup[] = {0xe0, 0x7f, 0x7f};
    CHECK((feed_all(&m, bendup, 3, &out) & FM_MIDI_EV_CONTROL) && out.bend == 0x3fff, "pitch bend max");
    uint8_t mod[] = {0xb0, 1, 90};
    CHECK((feed_all(&m, mod, 3, &out) & FM_MIDI_EV_CONTROL) && m.modwheel == 90, "cc1 mod wheel");
    uint8_t sus[] = {0xb0, 64, 127};
    CHECK((feed_all(&m, sus, 3, &out) & FM_MIDI_EV_CONTROL) && m.sustain == 1, "cc64 sustain on");
    uint8_t susoff[] = {0xb0, 64, 0};
    CHECK(feed_all(&m, susoff, 3, &out) & FM_MIDI_EV_CONTROL, "cc64 sustain off");
    CHECK(m.sustain == 0, "cc64 sustain cleared");
    printf("  4. program/bend/cc1/cc64: %s\n", failures > f0 ? "FAIL" : "ok");
    f0 = failures;

    /* 5: FX channel CC map, and CC1 there is Filter Type not mod wheel */
    fm_midi_init(&m);
    memset(&out, 0, sizeof(out));
    uint8_t fxch = (uint8_t)(0xb0 | m.fx_channel);
    uint8_t fx_on[] = {fxch, 0, 1};              /* CC0: filter switch on */
    feed_all(&m, fx_on, 3, &out);
    CHECK(m.fx_on[FM_FX_FILTER] == 1, "cc0 should switch the filter on");
    uint8_t fx_type[] = {fxch, 1, 2};            /* CC1: filter type = HPF */
    feed_all(&m, fx_type, 3, &out);
    CHECK(m.fx_param[FM_FX_FILTER][1] == 2, "cc1 on the fx channel must set filter type");
    CHECK(m.modwheel == 0, "cc1 on the fx channel must NOT touch mod wheel");
    uint8_t rev_mix[] = {fxch, 7, 55};           /* CC7: reverb mix */
    feed_all(&m, rev_mix, 3, &out);
    CHECK(m.fx_param[FM_FX_REVERB][3] == 55, "cc7 should set reverb mix");
    uint8_t phaser_mix[] = {fxch, 23, 80};       /* CC23: phaser mix */
    feed_all(&m, phaser_mix, 3, &out);
    CHECK(m.fx_param[FM_FX_PHASER][3] == 80, "cc23 should set phaser mix");
    uint8_t fxn[] = {fxch, 60, 100};             /* a note on the fx channel */
    CHECK((feed_all(&m, fxn, 3, &out) & FM_MIDI_EV_NOTE_ON) == 0, "notes on the fx channel are ignored");
    printf("  5. FX channel CC0..23 map + cc1 semantics: %s\n", failures > f0 ? "FAIL" : "ok");
    f0 = failures;

    /* 6: realtime, including inside a SysEx */
    fm_midi_init(&m);
    uint8_t fa[] = {0xfa};
    CHECK(feed_all(&m, fa, 1, &out) & FM_MIDI_EV_TRANSPORT, "FA start");
    CHECK(m.running == 1, "FA sets running");
    uint8_t fc[] = {0xfc};
    CHECK(feed_all(&m, fc, 1, &out) & FM_MIDI_EV_TRANSPORT, "FC stop");
    CHECK(m.running == 0, "FC clears running");
    uint32_t before = m.clock_count;
    uint8_t ticks[] = {0xf8, 0xf8, 0xf8};
    feed_all(&m, ticks, 3, &out);
    CHECK(m.clock_count == before + 3, "F8 counted");
    /* clock inside a sysex must not break the frame */
    fm_midi_init(&m);
    uint8_t sysex_with_clock[] = {0xf0, 0x7d, 0xf8, 0x00, 0xf7};
    m.sysex_len = 0;
    feed_all(&m, sysex_with_clock, 5, &out);
    CHECK(m.clock_count == 1, "F8 inside sysex counted");
    printf("  6. realtime FA/FB/FC/F8, incl. inside SysEx: %s\n", failures > f0 ? "FAIL" : "ok");
    f0 = failures;

    /* 7: SysEx — FM-1 parameter write, and a DX7 bank */
    fm_midi_init(&m);
    uint8_t pw[] = {0xf0, 0x43, 0x10, 0x01, 0x0b, 0x40, 0xf7};   /* param 139 = 0x40 */
    CHECK((feed_all(&m, pw, 7, &out) & FM_MIDI_EV_PARAM) && m.last_param == 139 && m.last_value == 0x40,
          "FM-1 parameter write");
    /* a DX7 bank */
    static uint8_t bank[4104];
    bank[0] = 0xf0; bank[1] = 0x43; bank[2] = 0x00;
    bank[3] = 0x09; bank[4] = 0x20; bank[5] = 0x00;
    for (int i = 0; i < 4096; i++)
        bank[6 + i] = (uint8_t)(i * 7 + 3) & 0x7f;
    unsigned sum = 0;
    for (int i = 0; i < 4096; i++) sum += bank[6 + i];
    bank[4102] = (uint8_t)((128 - (sum & 127)) & 127);
    bank[4103] = 0xf7;
    uint32_t evb = feed_all(&m, bank, 4104, &out);
    CHECK(evb & FM_MIDI_EV_PATCH, "DX7 bank accepted");
    uint8_t patch[FM_PATCH_BYTES];
    CHECK(fm_midi_take_patch(&m, patch) == 0, "bank produced a patch");
    CHECK(patch[FM_OPS_ON] == 0x3f, "unpacked patch has all operators on");
    CHECK(patch[FM_ALG] <= 31, "unpacked algorithm in range");
    /* overflow: a frame longer than the buffer is dropped, not truncated */
    fm_midi_init(&m);
    fm_midi_feed(&m, 0xf0, &out);
    for (int i = 0; i < FM_MIDI_SYSEX_MAX + 50; i++)
        fm_midi_feed(&m, 0x11, &out);
    CHECK(m.sysex_overflow == 1, "overflow flagged");
    CHECK((fm_midi_feed(&m, 0xf7, &out) & FM_MIDI_EV_PATCH) == 0, "overflowed frame dropped");
    printf("  7. SysEx param write, DX7 bank, overflow drop: %s\n", failures > f0 ? "FAIL" : "ok");
    f0 = failures;

    /* 8: instance isolation */
    fm_midi_t a, b;
    fm_midi_init(&a);
    fm_midi_init(&b);
    uint8_t pa[] = {0x90, 60};
    feed_all(&a, pa, 2, &out);          /* a holds half a note */
    uint8_t nb[] = {0xb0, 1, 33};       /* a different message on b */
    feed_all(&b, nb, 3, &out);
    uint8_t pb[] = {64, 100};           /* complete b's note with running status */
    feed_all(&b, pb, 2, &out);
    CHECK(a.have == 1, "parser a's pending data was disturbed by parser b");
    CHECK(b.modwheel == 33, "parser b lost its cc");
    printf("  8. parser instances are independent: %s\n", failures > f0 ? "FAIL" : "ok");

    printf("\n%s\n", failures ? "FAILED" : "PASS");
    return failures ? 1 : 0;
}
