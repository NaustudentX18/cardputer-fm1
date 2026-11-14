# Cardputer ADV board notes

Every number here has a source. Nothing has been measured on hardware yet: no Cardputer
ADV is attached to the machine this was written on, so the "on hardware" column is empty
by definition. Treat unverified rows as the first things to check on a real unit.

## 1. Pins

| Function | GPIO | Source | On hardware |
|---|---|---|---|
| LCD MOSI / SCLK / CS / DC / RST / BL | 35 / 36 / 37 / 34 / 33 / 38 | M5Stack docs pinmap + schematic sheet 1 | — |
| I²S BCLK / LRCK / DOUT / DIN | 41 / 43 / 42 / 46 | schematic sheet 3, confirmed by M5Unified `spk_cfg`/`mic_cfg` | — |
| Codec I²C SDA / SCL, address | 8 / 9, `0x18` | schematic sheet 3 | — |
| Keypad TCA8418 SDA/SCL/INT, address | 8 / 9 / 11, `0x34` | schematic sheet 2, M5Cardputer `TCA8418.cpp` | — |
| IMU BMI270 address | `0x69` (SDO pulled high) | schematic sheet 3 | — |
| microSD CS / MOSI / CLK / MISO | 12 / 14 / 40 / 39 | M5Stack pinmap | — |
| Battery ADC | 10, divider ÷2 | M5Stack pinmap | — |
| CAP header RESET / INT / BUSY / CS / TX / RX | 3 / 4 / 6 / 5 / 13 / 15 | schematic sheet 4 | — |

**There is no MCLK line.** G45 is unwired. The ES8311 must be told to derive its master
clock from BCLK, which is register `0x01 = 0xB5`.

## 2. The keyboard is not the original Cardputer's

The ADV replaces the 74HC138 GPIO matrix with a **TCA8418 I²C keypad scanner**. The matrix
itself is electrically identical (same 8×7 nets `Y0..Y7`, same 22 Ω series resistors), so
one legend table serves both boards.

Two facts that trip up a naive port:

1. **The scanner's ROW lines are the keyboard's column pairs, and its COL lines are the
   keyboard's rows.** The vendor's remap is `col = raw_row * 2 + (raw_col > 3)`,
   `row = raw_col % 4`.
2. The event byte is `state << 7 | (row * 10 + col + 1)`. Divide by **10**, not 8.

The FIFO is 10 events deep and the part has no ghosting diodes, so N-key rollover is
bounded by the scanner, not the matrix. Our UI task polls the FIFO every 5 ms, which is
what M5Cardputer itself does.

## 3. Audio: mono codec, and a vendor-driver defect worth knowing

The ES8311 is a **mono** codec. The FM-1's engine and FX chain are stereo. This firmware
renders stereo internally and sums for the codec; the FM-1's genuine stereo image does not
survive the port, and that is a documented deviation, not an oversight.

`M5Unified`'s speaker-enable and microphone-enable sequences **disagree about register
`0x01`**: speaker writes `0xB5`, microphone writes `0xBA`, and whichever runs last wins.
Worse, the microphone's *disable* path writes `0x00 = 0x00`, which powers down the whole
codec — so `Mic.end()` silences the speaker until something re-runs the speaker enable.
This firmware writes `0xB5` once at boot and never calls either vendor path, so the defect
cannot bite. If microphone input is ever added (for the sampler), it must re-assert
register `0x01` afterwards.

## 4. Why 44.1 kHz

`fm_tables.c` is generated for 44,100 Hz because two msfa constants depend on the rate:
`Lfo::unit_` (36558) and `PitchEnv::unit_` (1143), plus the frequency LUT. Changing the
sample rate means regenerating the tables with `tools/gen_tables.c`, not editing a `#define`.

## 5. RAM budget (static, 512 KB with no PSRAM)

| Item | Bytes | Verified |
|---|---|---|
| `fm_synth_t` (12 voices + 2×64-sample buses) | 9,216 | measured on host |
| factory bank | 16,384 | flash-const, copied only on program change |
| tables (sin, exp2, freq, fine) | 20,884 | flash-const |
| I²S DMA (4 blocks × 64 frames × 2 ch × 2 B) | 1,024 | configured |

The engine itself is cheap because state is per-voice and everything large is `const`.
What is *not* yet accounted for: IDF, NimBLE, TinyUSB, the LCD band buffers and the FX
chain. Those need a real `idf.py size` on hardware.

## 6. What to check first on a real unit

1. `idf.py monitor` shows the ES8311 writes all ACK — if they do not, the codec is not at
   `0x18` and nothing else will work.
2. A held key produces sound, and the note number matches `tools/run_host_tests.sh`'s
   printed layout (Fn+z = MIDI 53).
3. The audio task's own log line: `worst block N us of 1451 us budget`. That single number
   decides polyphony. If it approaches 1451, drop to 8 voices or 32 kHz.
4. I²C bus scan lists exactly `0x18`, `0x34`, `0x69`.
