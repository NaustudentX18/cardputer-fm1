/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 cardputer-fm1 contributors.
 *
 * Fn-layer piano: how a Cardputer ADV QWERTY keyboard plays the FM-1's 27-note range.
 *
 * The FM-1 has 27 silicone keys covering F3..G5 (two octaves and one note), plus a
 * velocity-free pill. The Cardputer has no piano keys, no velocity and no encoders, so
 * the instrument is played through the Fn layer in the tracker layout every chip musician
 * already knows:
 *
 *   upper octave   q 2 w 3 e r 5 t 6 y 7 u   -> C D E F G A B of octave+1
 *   lower octave   z s x d c v g b h n j m   -> C D E F G A B of octave
 *   extras         , l . ; /                 -> two more notes past B, and the octave keys
 *
 * That is 27 semitones with all five accidentals, exactly the FM-1's range. The octave
 * pair (OCT- / OCT+) is Fn+[/] because the FM-1 puts those on two dedicated buttons and
 * transpose on the ALGORITHM encoder.
 *
 * Pure logic, no hardware: test/host/test_keys.c proves the mapping, the octave clamp and
 * the "one note per physical key" rule. */
#ifndef FM_KEYS_H
#define FM_KEYS_H

#include <stdint.h>

/* Key identifiers are plain values: printable ASCII for the letter/digit/punctuation
 * keys, and 0x100+ codes for the specials the board driver reports. This header must not
 * depend on the board, because the mapping is tested on the host. */
#define FM_KEY_NONE 0
enum {
    FM_KEY_FN = 0x101, FM_KEY_SHIFT, FM_KEY_CTRL, FM_KEY_OPT, FM_KEY_ALT,
    FM_KEY_ENTER, FM_KEY_BKSP, FM_KEY_TAB, FM_KEY_SPACE,
    FM_KEY_ESC, FM_KEY_DEL,
    FM_KEY_UP, FM_KEY_DOWN, FM_KEY_LEFT, FM_KEY_RIGHT,
    FM_KEY_F1, FM_KEY_F2, FM_KEY_F3, FM_KEY_F4, FM_KEY_F5, FM_KEY_F6,
    FM_KEY_F7, FM_KEY_F8, FM_KEY_F9, FM_KEY_F10, FM_KEY_F11, FM_KEY_F12
};

/* The FM-1's default keybed: F3 = 53 to G5 = 79. */
#define FM_KEYS_FIRST_NOTE 53
#define FM_KEYS_COUNT 27
#define FM_KEYS_LAST_NOTE (FM_KEYS_FIRST_NOTE + FM_KEYS_COUNT - 1)

/* Octave offsets the FM-1 allows: -3..+3 around its own default. */
#define FM_KEYS_OCTAVE_MIN (-3)
#define FM_KEYS_OCTAVE_MAX 3

/* A physical key -> a note offset within the 27-key range, or -1 when the key is not a
 * note. `fn` selects the piano layer; without Fn the keyboard is the text/编辑 layer and
 * no key plays a note (so typing a patch name never sounds a note). */
int fm_keys_offset(uint16_t key, int fn);

/* Offset + octave -> MIDI note, clamped to 0..127. Returns -1 for offset < 0. */
int fm_keys_note(int offset, int octave);

/* The note a physical key plays at the current octave, or -1. */
int fm_keys_note_for(uint16_t key, int fn, int octave);

#endif /* FM_KEYS_H */
