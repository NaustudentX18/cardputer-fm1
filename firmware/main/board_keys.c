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
    {'`', '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', BOARD_K_BKSP},
    {BOARD_K_TAB, 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\\'},
    {BOARD_K_FN, BOARD_K_SHIFT, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', BOARD_K_ENTER},
    {BOARD_K_CTRL, BOARD_K_OPT, BOARD_K_ALT, 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', BOARD_K_SPACE},
};

/* The Fn layer, as M5Cardputer ships it (added in eccba25): ESC and F1..F12 on the
 * number row, DELETE on backspace, arrows on ; , . / . */
const uint16_t board_keymap_fn[BOARD_KEY_ROWS][BOARD_KEY_COLS] = {
    {BOARD_K_ESC, BOARD_K_F1, BOARD_K_F2, BOARD_K_F3, BOARD_K_F4, BOARD_K_F5, BOARD_K_F6,
     BOARD_K_F7, BOARD_K_F8, BOARD_K_F9, BOARD_K_F10, BOARD_K_F11, BOARD_K_F12, BOARD_K_DEL},
    {BOARD_K_TAB, 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\\'},
    {BOARD_K_FN, BOARD_K_SHIFT, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', '\0', '\'', BOARD_K_ENTER},
    {BOARD_K_CTRL, BOARD_K_OPT, BOARD_K_ALT, 'z', 'x', 'c', 'v', 'b', 'n', 'm',
     BOARD_K_LEFT, BOARD_K_DOWN, BOARD_K_RIGHT, BOARD_K_UP},
};
