#!/usr/bin/env bash
# Vendors the upstream Apache-2.0 reference the engine is gated against, and the
# M-VAVE factory bank. Both are third-party; neither is committed.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "$ROOT/reference"
if [ ! -d "$ROOT/reference/msfa" ]; then
  git clone --depth 1 https://github.com/google/music-synthesizer-for-android.git "$ROOT/reference/msfa"
fi
echo "msfa at $(git -C "$ROOT/reference/msfa" rev-parse --short HEAD)"
echo "factory bank: embedded already (components/fmengine/src/fm_bank_data.c)"
