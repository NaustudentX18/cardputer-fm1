/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 cardputer-fm1 contributors.
 *
 * The Cardputer ADV keyboard legend, transcribed from M5Cardputer's
 * _key_value_map[4][14] (Keyboard.h). It is data, not code: the Fn layer is what
 * this instrument uses for playing, so it must be exactly right.
 *
 * Logical layout (row, col):
 *   row 0: ` 1 2 3 4 5 6 7 8 9 0 - = BkSp
 *   row 1: Tab q w e r t y u i o p [ ] \
 *   row 2: FN SHIFT a s d f g h j k l ; ' Enter
 *   row 3: CTRL OPT ALT z x c v b n m , . / Space
 */
#include "board.h"

const uint16_t board_keymap[BOARD_KEY_ROWS][BOARD_KEY_COLS] = {
    {'`', '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', FM_KEY_BKSP},
    {FM_KEY_TAB, 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\\'},
    {FM_KEY_FN, FM_KEY_SHIFT, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', FM_KEY_ENTER},
    {FM_KEY_CTRL, FM_KEY_OPT, FM_KEY_ALT, 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', FM_KEY_SPACE},
};

/* The Fn layer, as M5Cardputer ships it (added in eccba25): ESC and F1..F12 on the
 * number row, DELETE on backspace, arrows on ; , . / . */
const uint16_t board_keymap_fn[BOARD_KEY_ROWS][BOARD_KEY_COLS] = {
    {FM_KEY_ESC, FM_KEY_F1, FM_KEY_F2, FM_KEY_F3, FM_KEY_F4, FM_KEY_F5, FM_KEY_F6,
     FM_KEY_F7, FM_KEY_F8, FM_KEY_F9, FM_KEY_F10, FM_KEY_F11, FM_KEY_F12, FM_KEY_DEL},
    {FM_KEY_TAB, 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\\'},
    {FM_KEY_FN, FM_KEY_SHIFT, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', '\0', '\'', FM_KEY_ENTER},
    {FM_KEY_CTRL, FM_KEY_OPT, FM_KEY_ALT, 'z', 'x', 'c', 'v', 'b', 'n', 'm',
     FM_KEY_LEFT, FM_KEY_DOWN, FM_KEY_RIGHT, FM_KEY_UP},
};
