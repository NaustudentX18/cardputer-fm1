/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 cardputer-fm1 contributors.
 *
 * Firmware entry point: audio out through the ES8311, keys in, engine in between.
 *
 * Task layout, and why:
 *   core 1  audio   the I2S DMA ISR pulls blocks; the render runs in the ISR's caller
 *                   task at high priority so a 64-sample block (1.45 ms) is never late
 *   core 0  ui      keypad scan, display, MIDI: everything that may block
 * Parameters travel from UI to audio as 16-bit words through a lock-free single-producer
 * single-consumer ring, so the audio path never takes a mutex and never allocates.
 *
 * This file has NEVER been run on hardware: no Cardputer ADV is attached to the machine
 * it was written on. The ES8311 sequence and the TCA8418 init are transcribed from the
 * vendor sources cited in docs/board.md; the engine itself is gated bit-exact on the host.
 */
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "driver/i2s_std.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_cpu.h"

#include "board.h"
#include "fm_engine.h"
#include "fm_bank.h"
#include "fm_keys.h"
#include "fm_midi.h"
#include "fm_ui.h"
#include "fm_display.h"

static const char *TAG = "fm1";

/* ----------------------------------------------------------------- audio --- */
/* One block is FM_N samples; the DMA gets 4 blocks so an overrun costs 5.8 ms, not 1.45.
 * The engine's own block size is fixed by msfa's control rate, so this is the only knob. */
#define DMA_BLOCKS 4
#define I2S_BUF_FRAMES (FM_N * DMA_BLOCKS)

static i2s_chan_handle_t tx_chan;
static fm_synth_t synth;                 /* 9.2 KB, static: never on a stack */
static int32_t mix_buf[FM_N];
static fm_midi_t midi;                   /* ~8 KB: SysEx buffer for DX7 banks */
static uint16_t ui_fb[FM_UI_W * FM_UI_H]; /* 64,800 B of RGB565: the one big buffer */
static fm_ui_state_t ui;

/* ---- the codec ------------------------------------------------------------ */
/* ES8311 register writes, from M5Unified's speaker enable sequence
 * (M5Unified.cpp, _speaker_enabled_cb_cardputer_adv). Register 0x01 = 0xB5 tells the
 * codec to take its master clock from BCLK, which is required here because the board
 * has no MCLK line. See docs/board.md §3 for the vendor-driver defect that makes the
 * 0xB5/0xBA difference matter when the microphone is also used. */
static const uint8_t es8311_init[][2] = {
    {0x00, 0x80},   /* reset, then power on */
    {0x01, 0xB5},   /* MCLK = BCLK */
    {0x02, 0x18},   /* MULT_PRE = 3 */
    {0x0D, 0x01},   /* power up analog */
    {0x12, 0x00},   /* power up DAC */
    {0x13, 0x10},   /* enable headphone driver */
    {0x32, 0xBF},   /* DAC volume 0 dB */
    {0x37, 0x08},   /* bypass DAC equaliser */
};

static i2c_master_bus_handle_t i2c_bus;

static esp_err_t codec_write(uint8_t reg, uint8_t value)
{
    i2c_master_dev_handle_t dev;
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BOARD_CODEC_ADDR,
        .scl_speed_hz = 100000,
    };
    esp_err_t err = i2c_master_bus_add_device(i2c_bus, &cfg, &dev);
    if (err != ESP_OK)
        return err;
    uint8_t buf[2] = {reg, value};
    err = i2c_master_transmit(dev, buf, 2, 100);
    i2c_master_bus_rm_device(dev);
    return err;
}

static esp_err_t codec_init(void)
{
    for (unsigned i = 0; i < sizeof(es8311_init) / sizeof(es8311_init[0]); i++) {
        esp_err_t err = codec_write(es8311_init[i][0], es8311_init[i][1]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "codec write reg %02x failed: %s", es8311_init[i][0], esp_err_to_name(err));
            return err;
        }
    }
    ESP_LOGI(TAG, "ES8311 up: 44.1 kHz, MCLK from BCLK, mono DAC");
    return ESP_OK;
}

static esp_err_t audio_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(BOARD_I2S_PORT, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = DMA_BLOCKS;
    chan_cfg.dma_frame_num = FM_N;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &tx_chan, NULL), TAG, "i2s_new_channel");

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(BOARD_SAMPLE_RATE),
        /* The codec is mono, but the I2S frame is stereo: 32-bit slots, only the left
         * slot carries the mix. Sending 16-bit data in 32-bit slots avoids the codec
         * having to be told a second format. */
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = BOARD_I2S_BCLK,
            .ws = BOARD_I2S_LRCK,
            .dout = BOARD_I2S_DOUT,
            .din = BOARD_I2S_DIN,
            .invert_flags = {false, false, false},
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(tx_chan, &std_cfg), TAG, "i2s init");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(tx_chan), TAG, "i2s enable");
    return ESP_OK;
}

/* The audio task: render, convert to 16 bits, block until the DMA takes it. */
static void audio_task(void *arg)
{
    static int16_t pcm[FM_N * 2];
    (void)arg;
    uint32_t blocks = 0;
    uint32_t worst_us = 0;
    for (;;) {
        int64_t t0 = esp_timer_get_time();
        fm_synth_render(&synth, mix_buf);
        /* msfa's output is Q23-ish and can exceed 16 bits with several carriers, so the
         * master stage shifts and saturates rather than truncating. */
        for (int i = 0; i < FM_N; i++) {
            int32_t s = mix_buf[i] >> 8;
            if (s > 32767) s = 32767;
            if (s < -32768) s = -32768;
            pcm[i * 2] = (int16_t)s;
            pcm[i * 2 + 1] = (int16_t)s;      /* mono codec: same sample in both slots */
        }
        size_t written = 0;
        i2s_channel_write(tx_chan, pcm, sizeof(pcm), &written, portMAX_DELAY);
        uint32_t us = (uint32_t)(esp_timer_get_time() - t0);
        if (us > worst_us)
            worst_us = us;
        if (++blocks == 4000) {               /* ~5.8 s: report the real cost */
            ESP_LOGI(TAG, "audio: worst block %u us of 1451 us budget", worst_us);
            blocks = 0;
            worst_us = 0;
        }
    }
}

/* --------------------------------------------------------------- the panel --- */
/* TCA8418 keypad scanner, configured exactly as M5Cardputer does: matrix(7,8),
 * interrupts on G11. The event byte is state<<7 | (row*10 + col + 1). */
#define TCA_REG_CFG      0x01
#define TCA_REG_INT_STAT 0x02
#define TCA_REG_KEY_LCK  0x03
#define TCA_REG_KEY_EVENT_A 0x04
#define TCA_REG_KEY_CNT  0x03

static i2c_master_dev_handle_t keypad_dev;

static esp_err_t keypad_write(uint8_t reg, uint8_t value)
{
    uint8_t buf[2] = {reg, value};
    return i2c_master_transmit(keypad_dev, buf, 2, 50);
}

static esp_err_t keypad_read(uint8_t reg, uint8_t *out, size_t n)
{
    return i2c_master_transmit_receive(keypad_dev, &reg, 1, out, n, 50);
}

static esp_err_t keypad_init(void)
{
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BOARD_KEYPAD_ADDR,
        .scl_speed_hz = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(i2c_bus, &cfg, &keypad_dev), TAG, "keypad dev");
    /* 7 rows x 8 columns; the vendor's TCA8418 matrix(7, 8) */
    ESP_RETURN_ON_ERROR(keypad_write(TCA_REG_CFG, (7 - 1) | ((8 - 1) << 4)), TAG, "keypad cfg");
    ESP_RETURN_ON_ERROR(keypad_write(0x1D, 0x01), TAG, "keypad int cfg");   /* KE_IEN */
    return ESP_OK;
}

/* One raw event -> the logical (row, col) the legend table uses. The TCA8418's ROW lines
 * are the keyboard's column pairs and its COL lines the keyboard's rows, which is why
 * this arithmetic looks inverted. */
static int keypad_translate(uint8_t raw, uint16_t *key, int *pressed)
{
    *pressed = (raw & 0x80) != 0;
    unsigned v = (raw & 0x7f);
    if (v == 0)
        return -1;
    v--;
    unsigned raw_row = v / 10, raw_col = v % 10;
    if (raw_row >= 7 || raw_col >= 8)
        return -1;
    unsigned col = raw_row * 2 + (raw_col > 3 ? 1 : 0);
    unsigned row = raw_col % 4;
    if (row >= BOARD_KEY_ROWS || col >= BOARD_KEY_COLS)
        return -1;
    *key = board_keymap[row][col];
    return 0;
}

static int fn_down, octave;

/* Fn+F12 runs the on-device benchmark; defined below with the UI task. */
static void bench_task(void *arg);
/* Renders the current page into the framebuffer and pushes it to the panel. */
static void draw_ui(void);

/* Note tracking so a key release is matched to the note it started, even if the octave
 * changed in between. */
static int8_t key_note[BOARD_KEY_COUNT];

static void handle_event(uint16_t key, int pressed)
{
    if (key == FM_KEY_FN) {
        fn_down = pressed;
        return;
    }
    if (key == '[' && fn_down && pressed) {           /* OCT- */
        if (octave > FM_KEYS_OCTAVE_MIN) octave--;
        ESP_LOGI(TAG, "octave %d", octave);
        return;
    }
    if (key == ']' && fn_down && pressed) {           /* OCT+ */
        if (octave < FM_KEYS_OCTAVE_MAX) octave++;
        ESP_LOGI(TAG, "octave %d", octave);
        return;
    }
    if (fn_down && pressed && key >= '1' && key <= '9') {
        ui.page = (fm_page_t)(key - '1');
        if (ui.page >= FM_PAGE_COUNT)
            ui.page = FM_PAGE_HOME;
        draw_ui();
        return;
    }
    if (key == FM_KEY_F12 && fn_down && pressed) {
        /* Fn+F12: measure this device. The result is the polyphony decision. */
        xTaskCreatePinnedToCore(bench_task, "fm_bench", 8192, NULL, 3, NULL, 0);
        return;
    }
    int off = fm_keys_offset(key, fn_down);
    if (off < 0)
        return;
    if (pressed) {
        int note = fm_keys_note(off, octave);
        key_note[off] = (int8_t)note;
        /* No velocity on this keyboard: use a fixed mid velocity. The FM-1's pills are
         * velocity-free too; the difference is that its engine receives 100 always. */
        fm_synth_note_on(&synth, (uint8_t)note, 100);
    } else {
        int note = key_note[off];
        if (note >= 0)
            fm_synth_note_off(&synth, (uint8_t)note);
        key_note[off] = -1;
    }
}

/* MIDI in. A transport (USB MIDI, BLE, or the CAP header UART) calls this with bytes as
 * they arrive; everything that decides meaning is in fm_midi, which is gated on the host.
 * Running on the UI task keeps the audio deadline untouched. */
static void midi_feed(const uint8_t *bytes, size_t n)
{
    fm_midi_out_t out;
    for (size_t i = 0; i < n; i++) {
        memset(&out, 0, sizeof(out));
        uint32_t ev = fm_midi_feed(&midi, bytes[i], &out);
        if (ev & FM_MIDI_EV_NOTE_ON)
            fm_synth_note_on(&synth, out.note.note, out.note.velocity);
        if (ev & FM_MIDI_EV_NOTE_OFF)
            fm_synth_note_off(&synth, out.note.note);
        if (ev & FM_MIDI_EV_CONTROL)
            fm_synth_pitch_bend(&synth, midi.pitchbend);
        if (ev & FM_MIDI_EV_PATCH) {
            uint8_t patch[FM_PATCH_BYTES];
            if (fm_midi_take_patch(&midi, patch) == 0)
                fm_synth_set_patch(&synth, patch);
        }
        if (ev & FM_MIDI_EV_PARAM) {
            /* FM-1 single-parameter write: patch bytes 0..155, applied to the live patch.
             * Sounding notes keep their envelopes, as the FM-1 does; the change lands on
             * the next note-on because fm_synth_set_patch rebuilds voice state only then. */
            uint8_t patch[FM_PATCH_BYTES];
            for (int k = 0; k < FM_PATCH_BYTES; k++)
                patch[k] = synth.patch[k];
            patch[midi.last_param] = midi.last_value;
            fm_synth_set_patch(&synth, patch);
        }
    }
}

/* The one measurement that decides polyphony. Run on its own task so it cannot delay the
 * audio deadline while it runs; it prints through the ESP log at INFO. */
extern int fm_bench_main(void);

static void bench_task(void *arg)
{
    (void)arg;
    ESP_LOGW(TAG, "running the engine benchmark (about 6 s)...");
    fm_bench_main();
    ESP_LOGW(TAG, "benchmark done");
    vTaskDelete(NULL);
}

/* Page navigation: Fn + 1..9 selects a page, so every screen is reachable on the device
 * exactly as the screenshots show it. */
static void draw_ui(void)
{
    ui.program = 1;
    ui.battery_pct = 80;                 /* TODO: BOARD_BATT_ADC once on hardware */
    ui.voices_used = (uint8_t)fm_synth_voice_count(&synth);
    ui.voices_max = FM_VOICES;
    ui.patch = synth.patch;
    ui.algorithm = synth.patch[FM_ALG];
    memcpy(ui.name, "PIANO 1", 8);
    fm_ui_render(&ui, ui_fb);
    fm_display_blit(ui_fb);
}

static void ui_task(void *arg)
{
    uint8_t stat = 0;
    (void)arg;
    memset(&ui, 0, sizeof(ui));
    ui.page = FM_PAGE_HOME;
    ui.sub_pages = 1;
    draw_ui();
    for (;;) {
        if (keypad_read(TCA_REG_INT_STAT, &stat, 1) == ESP_OK && (stat & 0x01)) {
            uint8_t count = 0;
            if (keypad_read(TCA_REG_KEY_CNT, &count, 1) == ESP_OK) {
                count &= 0x0f;
                for (unsigned i = 0; i < count && i < 10; i++) {
                    uint8_t ev = 0;
                    if (keypad_read(TCA_REG_KEY_EVENT_A + i, &ev, 1) != ESP_OK)
                        break;
                    uint16_t key;
                    int pressed;
                    if (keypad_translate(ev, &key, &pressed) == 0)
                        handle_event(key, pressed);
                }
            }
            keypad_write(TCA_REG_INT_STAT, 0x01);      /* clear */
            keypad_write(TCA_REG_KEY_CNT, 0x00);
        }
        vTaskDelay(pdMS_TO_TICKS(5));                  /* one scan period, as the vendor does */
    }
}

/* ------------------------------------------------------------------ boot --- */
void app_main(void)
{
    ESP_LOGI(TAG, "cardputer-fm1: FM-1 class 6-op FM on ESP32-S3");
    fm_engine_init();

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = BOARD_I2C_PORT,
        .sda_io_num = BOARD_I2C_SDA,
        .scl_io_num = BOARD_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &i2c_bus));

    ESP_ERROR_CHECK(fm_display_init());
    ESP_ERROR_CHECK(audio_init());
    ESP_ERROR_CHECK(codec_init());
    ESP_ERROR_CHECK(keypad_init());

    /* Init first, then load the patch: fm_synth_init clears the struct, so a patch set
     * before it would be wiped. Voice 1 of the factory bank is PIANO 1, what the FM-1
     * itself boots with, so the instrument sounds like itself at power-on. */
    fm_synth_init(&synth, FM_VOICES);
    fm_midi_init(&midi);
    uint8_t patch[FM_PATCH_BYTES];
    if (fm_program_patch(patch, 0) == 0)
        fm_synth_set_patch(&synth, patch);
    else
        ESP_LOGW(TAG, "factory bank unavailable: engine starts on an init patch");

    for (int i = 0; i < BOARD_KEY_COUNT; i++)
        key_note[i] = -1;

    xTaskCreatePinnedToCore(audio_task, "fm_audio", 8192, NULL, configMAX_PRIORITIES - 2, NULL, 1);
    xTaskCreatePinnedToCore(ui_task, "fm_ui", 4096, NULL, 5, NULL, 0);
}
