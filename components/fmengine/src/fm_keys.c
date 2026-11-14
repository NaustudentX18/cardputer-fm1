/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 cardputer-fm1 contributors.
 *
 * The Fn-layer piano mapping. See fm_keys.h for the layout and why it is this one.
 * Semitone offsets are from the bottom key (offset 0 = F3 = 53 = C of the lower octave
 * in the tracker layout, i.e. offset 0 is the 'z' row's C). */
#include "fm_keys.h"

/* 27 semitones, lower octave then upper, in the order the hands find them. */
static const struct {
    uint16_t key;
    int8_t offset;
} fm_piano[] = {
    /* lower octave: C  C# D  D# E  F  F# G  G# A  A# B */
    {'z', 0},  {'s', 1},  {'x', 2},  {'d', 3},  {'c', 4},  {'v', 5},
    {'g', 6},  {'b', 7},  {'h', 8},  {'n', 9},  {'j', 10}, {'m', 11},
    /* upper octave, one octave up */
    {'q', 12}, {'2', 13}, {'w', 14}, {'3', 15}, {'e', 16}, {'r', 17},
    {'5', 18}, {'t', 19}, {'6', 20}, {'y', 21}, {'7', 22}, {'u', 23},
    /* the last three, so the range is 27 and ends on the FM-1's top note */
    {',', 24}, {'l', 25}, {'.', 26},
};

#define FM_PIANO_N (int)(sizeof(fm_piano) / sizeof(fm_piano[0]))

int fm_keys_offset(uint16_t key, int fn)
{
    if (!fn)
        return -1;              /* the base layer types; it does not play */
    if (key == 0)
        return -1;
    for (int i = 0; i < FM_PIANO_N; i++)
        if (fm_piano[i].key == key)
            return fm_piano[i].offset;
    return -1;
}

int fm_keys_note(int offset, int octave)
{
    if (offset < 0)
        return -1;
    if (octave < FM_KEYS_OCTAVE_MIN)
        octave = FM_KEYS_OCTAVE_MIN;
    if (octave > FM_KEYS_OCTAVE_MAX)
        octave = FM_KEYS_OCTAVE_MAX;
    int note = FM_KEYS_FIRST_NOTE + offset + octave * 12;
    if (note < 0)
        return 0;
    if (note > 127)
        return 127;
    return note;
}

int fm_keys_note_for(uint16_t key, int fn, int octave)
{
    return fm_keys_note(fm_keys_offset(key, fn), octave);
}
