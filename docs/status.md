# Status

Last updated: 2026-11-14. Everything below is either a test that runs or a thing that has
never been run. There is no third category.

## Verified by a test that runs on this machine

```
tools/run_host_tests.sh
```

| Area | Test | Result |
|---|---|---|
| Engine vs upstream msfa | `test/host/test_engine.c` + `msfa_ref.cc` | 606,720 samples, **0 mismatches** |
| Tables | generated from msfa's own init, hash-gated | `1ede81dd…37f2` |
| Factory bank | `test/host/test_bank.c` | sha256 `ecad0b86…6aad25` = M-VAVE's published hash |
| Voice/SysEx formats | same | VCED, VMEM, param write, overflow, checksum rules |
| MIDI contract | `test/host/test_midi.c` | 8 groups incl. FX CC map and realtime-in-SysEx |
| Piano keymap | `test/host/test_keys.c` | 27 notes, F3–G5, base layer silent, octave clamp |
| FX chain properties | `test/host/test_fx.c` | 10 properties: bypass, per-slot switch, LPF/HPF, monotonic cutoff, reverb tail, delay timing, distortion bounds, chorus/phaser stability, 5 s noise soak, reorder |
| Timing harness | `test/host/bench_engine.c` | compiles for both: host prints ns/sample, device prints cycles/sample and % of core via `esp_cpu_get_cycle_count()`. Reachable on hardware at **Fn+F12** |

## Firmware build (compiles, links, sizes)

```
source /data/toolchains/esp-idf/export.sh
idf.py set-target esp32s3 && idf.py build
```

cardputer_fm1.bin 0x41880 = 268 KB. Flash data 80 KB, DIRAM 75.7 KB of 341 KB.
The app partition has 74% free, so the UI, sequencer and persistence all fit.

## Written but never executed on hardware

| Area | File | What is unproven |
|---|---|---|
| Codec init | `firmware/main/main.c` | all eight ES8311 writes ACK, and audio actually comes out |
| I²S | same | 44.1 kHz stereo frame accepted by a mono codec |
| Keypad | same | TCA8418 matrix(7,8) config, event decode, FIFO drain |
| Task layout | same | the audio task meets 1.45 ms per block |
| Firmware build | `CMakeLists.txt`, `sdkconfig.defaults` | **builds**: `cardputer_fm1.bin` 268 KB, DIRAM 22% (266 KB free), IRAM 100% used (the engine is in flash, as intended) |

## Not started

- The 240×135 UI (every FM-1 page).
- MIDI transports bound to the parser: USB MIDI, CAP-header UART, BLE.
- Sequencer, arpeggiator.
- Persistence (double-buffered CRC sectors), battery gauge, deep sleep.
- Mono/legato/unison voice modes, and the 10-minute soak.

## The one number that can invalidate the plan

Two numbers, both printed by firmware that is already built:

1. **Fn+F12** runs `fm_bench_main()` on its own task (~6 s) and logs cycles/sample and % of
   a 240 MHz core for 1..12 voices on a worst-case patch, then the worst of the 128 factory
   voices. This is the measurement the polyphony decision must be made from.
2. The audio task logs `worst block N us of 1451 us budget` every ~5.8 s: the steady-state
   deadline margin during real playing.

Everything in the plan about 12 voices at 44.1 kHz is an estimate until (1) is read off a
real Cardputer ADV.
