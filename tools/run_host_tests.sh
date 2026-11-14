#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Host test suite. Requires: cc, c++, and the vendored msfa reference.
#   tools/run_host_tests.sh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
J="$ROOT/reference/msfa/app/src/main/jni"
BUILD="${BUILD_DIR:-$ROOT/build-host}"
mkdir -p "$BUILD"

if [ ! -f "$J/fm_core.cc" ]; then
  echo "error: reference/msfa is missing. Run: git submodule update --init --recursive" >&2
  exit 1
fi

CFLAGS="-std=gnu11 -O2 -Wall -Wextra -Wno-unused-parameter -I$ROOT/components/fmengine/include"
CXXFLAGS="-std=c++14 -O2 -w -include cstddef -include cstring -I$J"

echo "== engine (bit-exact vs upstream msfa)"
c++ $CXXFLAGS -c "$J/sin.cc"     -o "$BUILD/sin.o"
c++ $CXXFLAGS -c "$J/exp2.cc"    -o "$BUILD/exp2.o"
c++ $CXXFLAGS -c "$J/freqlut.cc" -o "$BUILD/freqlut.o"
c++ $CXXFLAGS -c "$J/lfo.cc"     -o "$BUILD/lfo.o"
c++ $CXXFLAGS -c "$J/pitchenv.cc" -o "$BUILD/pitchenv.o"
c++ $CXXFLAGS -c "$J/env.cc"     -o "$BUILD/env.o"
c++ $CXXFLAGS -c "$J/fm_core.cc" -o "$BUILD/fm_core.o"
c++ $CXXFLAGS -c "$J/fm_op_kernel.cc" -o "$BUILD/fm_op_kernel.o"
c++ $CXXFLAGS -c "$J/dx7note.cc" -o "$BUILD/dx7note.o"
c++ $CXXFLAGS -c "$J/patch.cc"   -o "$BUILD/patch.o"
c++ $CXXFLAGS -c "$ROOT/test/host/msfa_ref.cc" -o "$BUILD/msfa_ref.o"

cc $CFLAGS -c "$ROOT/components/fmengine/src/fm_engine.c" -o "$BUILD/fm_engine.o"
cc $CFLAGS -c "$ROOT/components/fmengine/src/fm_tables.c" -o "$BUILD/fm_tables.o"
cc $CFLAGS -c "$ROOT/test/host/test_engine.c" -o "$BUILD/test_engine.o"

c++ -o "$BUILD/test_engine" \
  "$BUILD/test_engine.o" "$BUILD/fm_engine.o" "$BUILD/fm_tables.o" "$BUILD/msfa_ref.o" \
  "$BUILD/sin.o" "$BUILD/exp2.o" "$BUILD/freqlut.o" "$BUILD/lfo.o" "$BUILD/pitchenv.o" \
  "$BUILD/env.o" "$BUILD/fm_core.o" "$BUILD/fm_op_kernel.o" "$BUILD/dx7note.o" "$BUILD/patch.o" \
  -lm -lstdc++

"$BUILD/test_engine"

echo "== bank + voice formats"
cc $CFLAGS -c "$ROOT/components/fmengine/src/fm_bank.c"      -o "$BUILD/fm_bank.o"
cc $CFLAGS -c "$ROOT/components/fmengine/src/fm_bank_data.c" -o "$BUILD/fm_bank_data.o"
cc $CFLAGS -c "$ROOT/test/host/test_bank.c"                  -o "$BUILD/test_bank.o"
cc -o "$BUILD/test_bank" "$BUILD/test_bank.o" "$BUILD/fm_bank.o" "$BUILD/fm_bank_data.o" \
   "$BUILD/fm_engine.o" "$BUILD/fm_tables.o" -lm
"$BUILD/test_bank"

echo "== bench"
cc $CFLAGS -c "$ROOT/test/host/bench_engine.c" -o "$BUILD/bench_engine.o"
if [ 1 = 1 ]; then
  cc -o "$BUILD/bench_engine" "$BUILD/bench_engine.o" "$BUILD/fm_engine.o" \
     "$BUILD/fm_tables.o" "$BUILD/fm_bank.o" "$BUILD/fm_bank_data.o" -lm
  echo "== bench (host timing; device figures come from the same file under ESP-IDF)"
  "$BUILD/bench_engine"
fi

echo "== keymap"
cc $CFLAGS -c "$ROOT/components/fmengine/src/fm_keys.c" -o "$BUILD/fm_keys.o"
cc $CFLAGS -I"$ROOT/firmware/main" -c "$ROOT/test/host/test_keys.c" -o "$BUILD/test_keys.o"
cc -o "$BUILD/test_keys" "$BUILD/test_keys.o" "$BUILD/fm_keys.o" -lm
"$BUILD/test_keys"

echo "== midi contract"
cc $CFLAGS -c "$ROOT/components/fmengine/src/fm_midi.c" -o "$BUILD/fm_midi.o"
cc $CFLAGS -c "$ROOT/test/host/test_midi.c" -o "$BUILD/test_midi.o"
cc -o "$BUILD/test_midi" "$BUILD/test_midi.o" "$BUILD/fm_midi.o" "$BUILD/fm_bank.o" \
   "$BUILD/fm_bank_data.o" "$BUILD/fm_engine.o" "$BUILD/fm_tables.o" -lm
"$BUILD/test_midi"

echo "== fx chain"
cc $CFLAGS -c "$ROOT/components/fmengine/src/fm_fx.c" -o "$BUILD/fm_fx.o"
cc $CFLAGS -c "$ROOT/test/host/test_fx.c" -o "$BUILD/test_fx.o"
cc -o "$BUILD/test_fx" "$BUILD/test_fx.o" "$BUILD/fm_fx.o" "$BUILD/fm_tables.o" -lm
"$BUILD/test_fx"
