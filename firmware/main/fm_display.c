/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 cardputer-fm1 contributors.
 *
 * ST7789V2 bring-up and blit. Unproven on hardware; the commands are the standard
 * ST7789 sequence used by M5GFX, with the Cardputer's own pin map.
 */
#include <string.h>
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "esp_check.h"

#include "board.h"
#include "fm_display.h"

static const char *TAG = "display";

#define LCD_HOST SPI2_HOST
#define LCD_CLK_HZ (26 * 1000 * 1000)

static spi_device_handle_t lcd;

/* ST7789 command bytes */
#define ST_NOP 0x00
#define ST_SWRESET 0x01
#define ST_SLPIN 0x10
#define ST_SLPOUT 0x11
#define ST_NORON 0x13
#define ST_INVOFF 0x20
#define ST_INVON 0x21
#define ST_DISPON 0x29
#define ST_CASET 0x2A
#define ST_RASET 0x2B
#define ST_RAMWR 0x2C
#define ST_MADCTL 0x36
#define ST_COLMOD 0x3A
#define ST_PORCTRL 0xB2
#define ST_GCTRL 0xB7
#define ST_VCOMS 0xBB
#define ST_LCMCTRL 0xC0
#define ST_VDVVRHEN 0xC2
#define ST_VRHS 0xC3
#define ST_VDVSET 0xC4
#define ST_FRCTR2 0xC6
#define ST_PWCTRL1 0xD0
#define ST_PVGAMCTRL 0xE0
#define ST_NVGAMCTRL 0xE1

static void lcd_cmd(uint8_t cmd)
{
    gpio_set_level(BOARD_LCD_DC, 0);
    spi_transaction_t t = {.length = 8, .tx_buffer = &cmd};
    spi_device_polling_transmit(lcd, &t);
}

static void lcd_data(const uint8_t *data, int len)
{
    if (!len)
        return;
    gpio_set_level(BOARD_LCD_DC, 1);
    spi_transaction_t t = {.length = (size_t)len * 8, .tx_buffer = data};
    spi_device_polling_transmit(lcd, &t);
}

static void lcd_cmd_data(uint8_t cmd, const uint8_t *data, int len)
{
    lcd_cmd(cmd);
    lcd_data(data, len);
}

static void set_window(int x0, int y0, int x1, int y1)
{
    uint8_t c[4];
    lcd_cmd(ST_CASET);
    c[0] = (uint8_t)(x0 >> 8); c[1] = (uint8_t)x0;
    c[2] = (uint8_t)(x1 >> 8); c[3] = (uint8_t)x1;
    lcd_data(c, 4);
    lcd_cmd(ST_RASET);
    c[0] = (uint8_t)(y0 >> 8); c[1] = (uint8_t)y0;
    c[2] = (uint8_t)(y1 >> 8); c[3] = (uint8_t)y1;
    lcd_data(c, 4);
    lcd_cmd(ST_RAMWR);
}

esp_err_t fm_display_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << BOARD_LCD_DC) | (1ULL << BOARD_LCD_CS) | (1ULL << BOARD_LCD_RST),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "gpio");

    gpio_set_level(BOARD_LCD_CS, 0);            /* the panel is the only device on the bus */

    spi_bus_config_t bus = {
        .mosi_io_num = BOARD_LCD_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = BOARD_LCD_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = FM_UI_W * 16 * 2,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO), TAG, "spi bus");

    spi_device_interface_config_t dev = {
        .clock_speed_hz = LCD_CLK_HZ,
        .mode = 0,
        .spics_io_num = -1,
        .queue_size = 4,
    };
    ESP_RETURN_ON_ERROR(spi_bus_add_device(LCD_HOST, &dev, &lcd), TAG, "spi dev");

    /* hardware reset */
    gpio_set_level(BOARD_LCD_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(BOARD_LCD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));

    lcd_cmd(ST_SWRESET);
    vTaskDelay(pdMS_TO_TICKS(120));
    lcd_cmd(ST_SLPOUT);
    vTaskDelay(pdMS_TO_TICKS(120));

    uint8_t colmod = 0x55;                      /* 16 bits per pixel */
    lcd_cmd_data(ST_COLMOD, &colmod, 1);

    /* The panel is 135 px tall in a 240x240 controller: offset the visible window. */
    uint8_t madctl = 0x60;                      /* MX | MV: landscape */
    lcd_cmd_data(ST_MADCTL, &madctl, 1);

    uint8_t porctrl[5] = {0x0C, 0x0C, 0x00, 0x33, 0x33};
    lcd_cmd_data(ST_PORCTRL, porctrl, 5);
    uint8_t gctrl = 0x35;
    lcd_cmd_data(ST_GCTRL, &gctrl, 1);
    uint8_t vcoms = 0x19;
    lcd_cmd_data(ST_VCOMS, &vcoms, 1);
    uint8_t lcm = 0x2C;
    lcd_cmd_data(ST_LCMCTRL, &lcm, 1);
    uint8_t vdvvrhen = 0x01;
    lcd_cmd_data(ST_VDVVRHEN, &vdvvrhen, 1);
    uint8_t vrhs = 0x12;
    lcd_cmd_data(ST_VRHS, &vrhs, 1);
    uint8_t vdvs = 0x20;
    lcd_cmd_data(ST_VDVSET, &vdvs, 1);
    uint8_t frctr2 = 0x0F;
    lcd_cmd_data(ST_FRCTR2, &frctr2, 1);
    uint8_t pwctrl[2] = {0xA4, 0xA1};
    lcd_cmd_data(ST_PWCTRL1, pwctrl, 2);

    uint8_t pgamma[14] = {0xD0, 0x08, 0x11, 0x08, 0x0C, 0x15, 0x39, 0x33, 0x50, 0x36, 0x13, 0x14, 0x29, 0x2D};
    lcd_cmd_data(ST_PVGAMCTRL, pgamma, 14);
    uint8_t ngamma[14] = {0xD0, 0x08, 0x10, 0x08, 0x06, 0x06, 0x39, 0x44, 0x51, 0x0B, 0x16, 0x14, 0x2F, 0x31};
    lcd_cmd_data(ST_NVGAMCTRL, ngamma, 14);

    lcd_cmd(ST_INVON);                          /* this panel needs inversion */
    lcd_cmd(ST_NORON);
    vTaskDelay(pdMS_TO_TICKS(10));
    lcd_cmd(ST_DISPON);
    vTaskDelay(pdMS_TO_TICKS(100));

    fm_display_backlight(255);
    ESP_LOGI(TAG, "ST7789V2 up: %dx%d on SPI%d at %d MHz", FM_UI_W, FM_UI_H, LCD_HOST, LCD_CLK_HZ / 1000000);
    return ESP_OK;
}

/* The framebuffer is RGB565 in host order; the panel wants big-endian bytes. Swapping in
 * the transfer buffer (not in the renderer) keeps fm_ui.c byte-order agnostic. */
static uint16_t band[FM_UI_W * 16];

esp_err_t fm_display_blit_band(const uint16_t *fb, int y0, int rows)
{
    if (rows > 16) rows = 16;
    int n = FM_UI_W * rows;
    for (int i = 0; i < n; i++) {
        uint16_t p = fb[y0 * FM_UI_W + i];
        band[i] = (uint16_t)((p >> 8) | (p << 8));
    }
    set_window(0, y0, FM_UI_W - 1, y0 + rows - 1);
    gpio_set_level(BOARD_LCD_DC, 1);
    spi_transaction_t t = {.length = (size_t)n * 16, .tx_buffer = band};
    return spi_device_polling_transmit(lcd, &t) == ESP_OK ? ESP_OK : ESP_FAIL;
}

esp_err_t fm_display_blit(const uint16_t *fb)
{
    for (int y = 0; y < FM_UI_H; y += 16) {
        int rows = FM_UI_H - y < 16 ? FM_UI_H - y : 16;
        esp_err_t e = fm_display_blit_band(fb, y, rows);
        if (e != ESP_OK)
            return e;
    }
    return ESP_OK;
}

void fm_display_backlight(uint8_t level)
{
    static int ready;
    if (!ready) {
        ledc_timer_config_t t = {
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .duty_resolution = LEDC_TIMER_8_BIT,
            .timer_num = LEDC_TIMER_1,
            .freq_hz = 5000,
            .clk_cfg = LEDC_AUTO_CLK,
        };
        ledc_timer_config(&t);
        ledc_channel_config_t c = {
            .gpio_num = BOARD_LCD_BL,
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel = LEDC_CHANNEL_1,
            .timer_sel = LEDC_TIMER_1,
            .duty = 0,
            .hpoint = 0,
        };
        ledc_channel_config(&c);
        ready = 1;
    }
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, level);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
}
