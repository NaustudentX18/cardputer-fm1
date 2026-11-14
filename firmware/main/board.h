/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 cardputer-fm1 contributors.
 *
 * Board support for the M5Stack Cardputer ADV: the pins and the parts of the machine
 * the engine does not care about. Every value here is from the published schematic and
 * vendor driver sources; the "verified" column in docs/board.md says which ones have
 * been confirmed on hardware (none yet: no device on this workstation).
 *
 * The Cardputer ADV is not the original Cardputer. Three differences matter:
 *   - the keyboard is an I2C TCA8418 keypad scanner at 0x34, not a GPIO matrix;
 *   - audio goes through an ES8311 codec at 0x18 (mono DAC), not a bare I2S amplifier;
 *   - the internal I2C bus (SDA G8 / SCL G9) is shared by keypad, codec and IMU.
 */
#ifndef BOARD_H
#define BOARD_H

#include <stdint.h>

/* ---- display: ST7789V2, 240x135, 4-wire SPI ---------------------------- */
#define BOARD_LCD_W 240
#define BOARD_LCD_H 135
#define BOARD_LCD_MOSI 35
#define BOARD_LCD_SCLK 36
#define BOARD_LCD_CS   37
#define BOARD_LCD_DC   34
#define BOARD_LCD_RST  33
#define BOARD_LCD_BL   38      /* also gates the Stamp-S3A's RGB LED rail */

/* ---- audio: ES8311 (I2C 0x18) + NS4150B, no MCLK ----------------------- */
#define BOARD_I2S_BCLK 41
#define BOARD_I2S_LRCK 43
#define BOARD_I2S_DOUT 42      /* host -> codec DSDIN */
#define BOARD_I2S_DIN  46      /* codec ASDOUT -> host (mic) */
#define BOARD_I2S_PORT 1       /* I2S_NUM_1; port 0 stays free */
#define BOARD_CODEC_ADDR 0x18
#define BOARD_SAMPLE_RATE 44100
/* The codec is mono and has no MCLK line: it must derive its clock from BCLK
 * (M5Unified writes 0xB5 to register 0x01 for speaker; see docs/board.md). */

/* ---- shared internal I2C ---------------------------------------------- */
#define BOARD_I2C_SDA 8
#define BOARD_I2C_SCL 9
#define BOARD_I2C_PORT 0
#define BOARD_KEYPAD_ADDR 0x34     /* TCA8418 */
#define BOARD_KEYPAD_INT  11
#define BOARD_IMU_ADDR    0x69     /* BMI270 */

/* ---- microSD, sharing SPI with the CAP header ------------------------- */
#define BOARD_SD_CS   12
#define BOARD_SD_MOSI 14
#define BOARD_SD_CLK  40
#define BOARD_SD_MISO 39

/* ---- battery ---------------------------------------------------------- */
#define BOARD_BATT_ADC 10          /* divider, /2 */

/* ---- CAP header (free pins, unused by this firmware yet) --------------- */
#define CAP_RESET 3
#define CAP_INT   4
#define CAP_BUSY  6
#define CAP_CS    5
#define CAP_TX    13
#define CAP_RX    15

/* ---- keypad geometry -------------------------------------------------- */
/* TCA8418 matrix(7, 8) as M5Cardputer configures it. The vendor remap is
 *   x = raw_row * 2 + (raw_col >= 4);  y = raw_col % 4
 * which yields the same logical (row 0..3, col 0..13) as the original
 * Cardputer's GPIO reader, so one legend table serves both boards. */
#define BOARD_KEY_ROWS 4
#define BOARD_KEY_COLS 14
#define BOARD_KEY_COUNT (BOARD_KEY_ROWS * BOARD_KEY_COLS)

/* Logical key ids come from fm_keys.h: one definition, used by the board driver and by
 * the piano mapping, so a legend entry cannot disagree with what a key plays. */
#include "fm_keys.h"

/* The legend, [row][col], base layout. Layer 1 is Fn, layer 2 is Fn+Shift. */
extern const uint16_t board_keymap[BOARD_KEY_ROWS][BOARD_KEY_COLS];
extern const uint16_t board_keymap_fn[BOARD_KEY_ROWS][BOARD_KEY_COLS];

#endif /* BOARD_H */
