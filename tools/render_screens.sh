#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Renders the real UI to docs/screens/*.png at the panel's own resolution, then upscales
# 4x with point filtering for the page. Requires: cc, and `magick` for the upscale.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:-$ROOT/docs/screens}"
mkdir -p "$OUT"
BIN="$ROOT/build-host/screenshot"
mkdir -p "$ROOT/build-host"

cc -std=gnu11 -O2 -Wall -Wextra -Wno-unused-parameter \
   -I "$ROOT/components/fmengine/include" \
   "$ROOT/test/host/screenshot.c" \
   "$ROOT/components/fmengine/src/fm_ui.c" \
   "$ROOT/components/fmengine/src/fm_ui_font.c" \
   "$ROOT/components/fmengine/src/fm_engine.c" \
   "$ROOT/components/fmengine/src/fm_tables.c" \
   "$ROOT/components/fmengine/src/fm_bank.c" \
   "$ROOT/components/fmengine/src/fm_bank_data.c" \
   "$ROOT/components/fmengine/src/fm_fx.c" \
   -o "$BIN" -lm

"$BIN" "$OUT"

# ImageMagick 7 ships `magick`; 6 ships `convert`. The CI runner has only the latter, and
# silently skipping the upscale produced a commit whose images did not match the script.
IM=""
if command -v magick >/dev/null 2>&1; then
  IM="magick"
elif command -v convert >/dev/null 2>&1; then
  IM="convert"
fi
if [ -n "$IM" ]; then
  for f in "$OUT"/*.png; do
    "$IM" "$f" -filter point -resize 400% -strip -define png:compression-level=9 "$f"
  done
  echo "upscaled 4x with point filtering ($IM)"
else
  echo "error: ImageMagick not found (need 'magick' or 'convert')" >&2
  exit 1
fi
