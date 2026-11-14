/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 cardputer-fm1 contributors.
 *
 * Keymap gate: the Cardputer's QWERTY played as the FM-1's 27-key bed.
 *  1. every mapped key is unique (one physical key, one note)
 *  2. the 27 offsets are exactly 0..26 with no gap or duplicate
 *  3. the range at octave 0 is F3..G5, the FM-1's own
 *  4. the base layer never plays a note (typing a patch name must be silent)
 *  5. the octave clamp is -3..+3 and the MIDI result stays in 0..127 */
#include <stdio.h>
#include <string.h>
#include "fm_keys.h"

static int failures;

/* candidate keys that could plausibly be piano keys, so a duplicate in the table
 * (same key twice) is caught without enumerating 0..0xFFFF meaninglessly */
static const uint16_t all_keys[] = {
    'z','s','x','d','c','v','g','b','h','n','j','m',
    'q','2','w','3','e','r','5','t','6','y','7','u',
    ',','l','.','p','o','i','9','8','0','k','/',';','\'','[',']','-','=','\\',
    'a','f','1','4', FM_KEY_FN, FM_KEY_SPACE, FM_KEY_ENTER, FM_KEY_TAB,
    FM_KEY_SHIFT, FM_KEY_CTRL, FM_KEY_ALT, FM_KEY_OPT, FM_KEY_BKSP,
    FM_KEY_UP, FM_KEY_DOWN, FM_KEY_LEFT, FM_KEY_RIGHT,
    FM_KEY_ESC, FM_KEY_DEL, FM_KEY_F1, FM_KEY_F12, 0
};

int main(void)
{
    /* 1 + 2: offsets are 0..26, each once */
    int seen[FM_KEYS_COUNT];
    memset(seen, 0, sizeof(seen));
    int mapped = 0;
    for (unsigned i = 0; all_keys[i]; i++) {
        int off = fm_keys_offset(all_keys[i], 1);
        if (off < 0)
            continue;
        mapped++;
        if (off >= FM_KEYS_COUNT) {
            printf("  FAIL key 0x%02x offset %d out of range\n", all_keys[i], off);
            failures++;
            continue;
        }
        seen[off]++;
    }
    for (int i = 0; i < FM_KEYS_COUNT; i++) {
        if (seen[i] != 1) {
            printf("  FAIL offset %d mapped %d times (want 1)\n", i, seen[i]);
            failures++;
        }
    }
    if (mapped != FM_KEYS_COUNT) {
        printf("  FAIL %d keys mapped, want %d\n", mapped, FM_KEYS_COUNT);
        failures++;
    }
    printf("  1-2. %d keys, offsets 0..%d each exactly once: %s\n",
           mapped, FM_KEYS_COUNT - 1, failures ? "FAIL" : "ok");
    int f0 = failures;

    /* 3: the range is the FM-1's */
    int lo = fm_keys_note(0, 0), hi = fm_keys_note(FM_KEYS_COUNT - 1, 0);
    if (lo != 53 || hi != 79) {
        printf("  FAIL range at octave 0 is %d..%d, want 53..79 (F3..G5)\n", lo, hi);
        failures++;
    }
    printf("  3. octave 0 plays MIDI %d..%d (F3..G5, the FM-1's bed): %s\n",
           lo, hi, failures > f0 ? "FAIL" : "ok");
    f0 = failures;

    /* 4: base layer is silent */
    int typed = 0;
    for (unsigned i = 0; all_keys[i]; i++)
        if (fm_keys_offset(all_keys[i], 0) != -1)
            typed++;
    if (typed) {
        printf("  FAIL %d keys play a note without Fn\n", typed);
        failures++;
    }
    printf("  4. base layer never plays a note: %s\n", failures > f0 ? "FAIL" : "ok");
    f0 = failures;

    /* 5: octave clamp, and MIDI stays in range at the extremes */
    if (fm_keys_note(0, 99) != fm_keys_note(0, FM_KEYS_OCTAVE_MAX) ||
        fm_keys_note(0, -99) != fm_keys_note(0, FM_KEYS_OCTAVE_MIN)) {
        printf("  FAIL octave clamp not applied\n");
        failures++;
    }
    for (int o = -6; o <= 6; o++)
        for (int off = 0; off < FM_KEYS_COUNT; off++) {
            int n = fm_keys_note(off, o);
            if (n < 0 || n > 127) {
                printf("  FAIL note %d at octave %d offset %d\n", n, o, off);
                failures++;
            }
        }
    if (fm_keys_offset(0, 1) != -1) {
        printf("  FAIL key 0 mapped\n");
        failures++;
    }
    printf("  5. octave clamp -3..+3, MIDI stays 0..127 at every extreme: %s\n",
           failures > f0 ? "FAIL" : "ok");

    /* show the layout, so a reviewer can see what plays what */
    printf("\n  Fn layer, octave 0:\n");
    for (int oct = 0; oct < 2; oct++) {
        printf("   %s: ", oct ? "upper" : "lower");
        for (int i = 0; i < 12; i++) {
            int off = oct * 12 + i;
            static const char *names[12] = {"C ","C#","D ","D#","E ","F ","F#","G ","G#","A ","A#","B "};
            printf("%s=%d ", names[i], fm_keys_note(off, 0));
        }
        printf("\n");
    }
    printf("   top three: ,=77 l=78 .=79\n");

    printf("\n%s\n", failures ? "FAILED" : "PASS");
    return failures ? 1 : 0;
}
