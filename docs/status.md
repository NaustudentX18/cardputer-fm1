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
| Host timing | `test/host/bench_engine.c` | shape only, not a device figure |

## Written but never executed on hardware

| Area | File | What is unproven |
|---|---|---|
| Codec init | `firmware/main/main.c` | all eight ES8311 writes ACK, and audio actually comes out |
| I²S | same | 44.1 kHz stereo frame accepted by a mono codec |
| Keypad | same | TCA8418 matrix(7,8) config, event decode, FIFO drain |
| Task layout | same | the audio task meets 1.45 ms per block |
| Firmware build | `CMakeLists.txt`, `sdkconfig.defaults` | it links and fits in 8 MB |

## Not started

- The 240×135 UI (every FM-1 page).
- The six-slot FX chain (the CC map that drives it is decoded and tested).
- MIDI transports bound to the parser: USB MIDI, CAP-header UART, BLE.
- Sequencer, arpeggiator.
- Persistence (double-buffered CRC sectors), battery gauge, deep sleep.
- Mono/legato/unison voice modes, and the 10-minute soak.

## The one number that can invalidate the plan

The audio task logs

```
worst block N us of 1451 us budget
```

every ~5.8 s. That single number decides polyphony. Everything in the plan about 12 voices
at 44.1 kHz is an estimate until it is read off a real Cardputer ADV.
