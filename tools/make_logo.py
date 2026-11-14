#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 cardputer-fm1 contributors.
"""Draw the project mark: six operators stacked into an 'F', plus the modulation bus.

    tools/make_logo.py docs/logo.svg docs/logo-mark.svg

Why this mark: the instrument's identity is six sine operators wired by a routing table.
The mark stacks six bars in the F's proportions and routes two of them into a bus that
leaves the glyph, so the silhouette reads 'F' at 16 px and 'six operators' at 256 px.
Two files are written: the full lockup (mark + wordmark) and the mark alone for favicons.
"""
import sys

BG = "#0F172A"
INK = "#F8FAFC"
ACCENT = "#22C55E"
WARM = "#FDBA74"
DIM = "#94A3B8"


def mark(x, y, s):
    """Six operator bars on a shared stem, reading as an F, with a modulation bus.

    The stem is what makes it a letter rather than a stack of pills: six bars alone read as
    a list. Left-aligned bars + a stem = F at any size; the six bars are still six
    operators, which is the whole identity.
    """
    o = []
    bw = 26 * s          # bar unit
    bh = 8.5 * s         # bar height
    gap = 5.0 * s
    x0 = x
    y0 = y
    stem_w = 5.0 * s
    bars = [
        (0, 3.0, INK),      # top arm
        (1, 1.0, INK),
        (2, 1.0, ACCENT),   # a carrier in the stack
        (3, 2.0, INK),      # crossbar
        (4, 1.0, ACCENT),
        (5, 1.0, INK),
    ]
    # stem behind the bars
    stem_h = 5 * (bh + gap) + bh
    o.append(
        f'<rect x="{x0:.2f}" y="{y0:.2f}" width="{stem_w:.2f}" height="{stem_h:.2f}" rx="{stem_w / 2:.2f}" fill="{INK}"/>'
    )
    for dy, wu, col in bars:
        o.append(
            f'<rect x="{x0 + stem_w - 1:.2f}" y="{y0 + dy * (bh + gap):.2f}" '
            f'width="{wu * bw + (wu - 1) * gap + stem_w / 2:.2f}" height="{bh:.2f}" rx="{bh / 2:.2f}" fill="{col}"/>'
        )
    # the modulation bus: two operators feed it, it leaves the glyph
    bus_x = x0 + stem_w + 2 * bw + gap + 14 * s
    top_y = y0 + 3 * (bh + gap) + bh / 2
    bot_y = y0 + 2 * (bh + gap) + bh / 2
    o.append(
        f'<path d="M{x0 + stem_w + bw + gap:.2f} {top_y:.2f} H{bus_x:.2f} V{bot_y:.2f}" '
        f'fill="none" stroke="{WARM}" stroke-width="{max(1.6, 2.2 * s):.2f}" '
        f'stroke-linecap="round" stroke-linejoin="round"/>'
    )
    # feedback: the top operator loops back on itself
    fy = y0 + bh / 2
    o.append(
        f'<path d="M{x0 + stem_w + bw:.2f} {fy:.2f} h{6 * s:.2f} v{-(bh / 2 + 3 * s):.2f} '
        f'h{-(6 * s + bw / 2):.2f}" fill="none" stroke="{ACCENT}" '
        f'stroke-width="{max(1.6, 2.2 * s):.2f}" stroke-linecap="round" stroke-linejoin="round"/>'
    )
    return "\n".join(o), x0 + bus_x - x0 + 10 * s


def lockup():
    s = 1.0
    body, mark_right = mark(52, 30, s)
    w, h = 420, 200
    return f'''<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {w} {h}" width="{w}" height="{h}" role="img" aria-label="Cardputer FM-1">
  <rect width="{w}" height="{h}" fill="{BG}"/>
  {body}
  <text x="{mark_right:.0f}" y="92" font-family="ui-monospace, 'JetBrains Mono', monospace" font-size="34" font-weight="500" fill="{INK}" letter-spacing="-0.5">cardputer</text>
  <text x="{mark_right:.0f}" y="128" font-family="ui-monospace, 'JetBrains Mono', monospace" font-size="34" font-weight="500" fill="{ACCENT}" letter-spacing="-0.5">fm-1</text>
  <text x="{mark_right:.0f}" y="152" font-family="ui-monospace, 'JetBrains Mono', monospace" font-size="10" fill="{DIM}" letter-spacing="1.6">SIX-OPERATOR FM · ESP32-S3</text>
</svg>
'''


def mark_only():
    s = 1.0
    body, _ = mark(26, 12, s)
    return f'''<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 132 132" width="132" height="132" role="img" aria-label="Cardputer FM-1 mark">
  <rect width="132" height="132" rx="28" fill="{BG}"/>
  {body}
</svg>
'''


if __name__ == "__main__":
    out_full = sys.argv[1] if len(sys.argv) > 1 else "docs/logo.svg"
    out_mark = sys.argv[2] if len(sys.argv) > 2 else "docs/logo-mark.svg"
    open(out_full, "w").write(lockup())
    open(out_mark, "w").write(mark_only())
    print("wrote", out_full, "and", out_mark)
