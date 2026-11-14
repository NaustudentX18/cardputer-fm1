/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 cardputer-fm1 contributors.
 *
 * The Cardputer ADV's display, as far as this firmware needs it: bring up the ST7789V2 on
 * SPI, and push a finished 240x135 RGB565 framebuffer.
 *
 * The renderer (fm_ui.c) owns the pixels and knows nothing about SPI; this file owns the
 * bus and knows nothing about what is drawn. That split is why the screenshots in docs/ are
 * the same code path the device uses.
 *
 * Pins: MOSI G35, SCLK G36, CS G37, DC G34, RST G33, backlight G38 (which also gates the
 * Stamp-S3A's RGB LED rail, see docs/board.md).
 *
 * Never run on hardware: no Cardputer ADV was attached while this was written.
 */
#ifndef FM_DISPLAY_H
#define FM_DISPLAY_H

#include <stdint.h>
#include "esp_err.h"
#include "fm_ui.h"

esp_err_t fm_display_init(void);

/* Push the whole framebuffer. 240*135*2 = 64,800 bytes, which is ~20 ms at 26 MHz SPI. */
esp_err_t fm_display_blit(const uint16_t *fb);

/* Band transfer, for the audio task's sake: the UI task can send 16-row strips between
 * audio deadlines instead of blocking on 20 ms of one call. */
esp_err_t fm_display_blit_band(const uint16_t *fb, int y0, int rows);

/* Backlight, 0..255. The panel's rail is shared with the RGB LED, so dimming it dims the
 * status LED too; that is hardware, not a bug here. */
void fm_display_backlight(uint8_t level);

#endif /* FM_DISPLAY_H */
