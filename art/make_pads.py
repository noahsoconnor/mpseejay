#!/usr/bin/env python3
"""pads.png / pads_s.png: the pad tiles' filmstrips, big (PLAY) and small (browser) (needs Pillow and Titillium Web; run in python:3.11-slim with Pillow).
One frame per pad function and light: value = function * 3 + light (0 dim, 1 on, 2 pressed), the order of PF_* in
src/deck.c. Each tile is TW x TH; the pad number it sits on is printed in its corner."""
import os, sys
from PIL import Image, ImageDraw, ImageFont

here = os.path.dirname(os.path.abspath(__file__))
fonts = sys.argv[1] if len(sys.argv) > 1 else os.path.join(here, "..", "..", "vendor", "mpc-vst-plugins", "tools", "html_art", "fonts")
BOLD = os.path.join(fonts, "TitilliumWeb-Bold.ttf")

CUECOL = ["ff4d4d", "ff8c1a", "ffd23f", "3ddc84", "22d3ee", "3b82f6", "a855f7", "ec4899"]
FUNCS = (  # (label, colour, pad number), in the order of PF_* in src/deck.c
    [("CUE", "ff9a1f", 1), ("PLAY", "36d97f", 2), ("NUDGE -", "c9d1db", 3), ("NUDGE +", "c9d1db", 4),
     ("SYNC", "22d3ee", 5), ("JUMP -", "ffd23f", 6), ("JUMP +", "ffd23f", 7), ("LOOP", "ff6bd5", 15)]
    + [("CUE %d" % (i + 1), CUECOL[i], 0) for i in range(8)]   # on bank A and bank B, so no pad number
    + [("ROLL " + b, "b07cff", 0) for b in ["1/8", "1/4", "1/2", "1"]]
    # while the browser is open (PF_B_*)
    + [("PAGE UP", "c9d1db", 13), ("UP", "e8edf2", 14), ("CLOSE", "8a94a3", 16), ("BACK", "ffb04d", 9),
       ("DOWN", "e8edf2", 10), ("OPEN", "36d97f", 11), ("PAGE DN", "c9d1db", 5), ("LOAD A", "2f9bff", 1),
       ("LOAD B", "ff4d6a", 2), ("", "2a3039", 0)]
    # 0.8: deck switch, tempo, SHIFT, and the SHIFT functions
    + [("DECK A", "2f9bff", 13), ("DECK B", "ff4d6a", 14), ("BPM +", "e8edf2", 16), ("BPM -", "e8edf2", 12),
       ("SHIFT", "ffb04d", 8), ("2 BARS", "ff6bd5", 9), ("4 BARS", "ff6bd5", 10), ("8 BARS", "ff6bd5", 11),
       ("HALF", "ffd23f", 6), ("DOUBLE", "ffd23f", 7), ("LEADER", "ffb04d", 5), ("BPM +1", "e8edf2", 16),
       ("BPM -1", "e8edf2", 12)]
    # bank B: auto loops
    + [("LOOP %d" % b, "ff6bd5", i + 1) for i, b in enumerate([1, 2, 4, 8])])


def rgb(h): return tuple(int(h[i:i + 2], 16) for i in (0, 2, 4))
def mix(a, b, t): return tuple(round(a[i] * t + b[i] * (1 - t)) for i in range(3))


BG = rgb("101317")
SS = 3   # draw at 3x and shrink, for smooth edges
# Pads that take the deck's colour (pad_deck_coloured in src/deck.c): CUE, pads 5-8, the hot cues and their SHIFT functions
DECK_COLOURED = {"CUE", "SYNC", "JUMP -", "JUMP +", "SHIFT", "HALF", "DOUBLE", "LEADER"} | \
    {"CUE %d" % i for i in range(1, 9)}
DECKCOL = {"a": "2f9bff", "b": "ff4d6a"}
# pads_a.png / pads_b.png: the big PLAY tiles for deck A / B, plus one empty frame at the end (the browser is open);
# pads_s.png: the small browser map (no deck colours).
SIZES = [("pads_a.png", 138, 86, 26, 13, "a"), ("pads_b.png", 138, 86, 26, 13, "b"), ("pads_s.png", 76, 62, 14, 9, None)]
for fname, TW, TH, BIG, SMALL, deck in SIZES:
    big = ImageFont.truetype(BOLD, BIG)
    small = ImageFont.truetype(BOLD, SMALL)
    rad = 10 if TW > 100 else 6
    nframes = len(FUNCS) * 3 + (1 if deck else 0)
    strip = Image.new("RGBA", (TW, TH * nframes), (0, 0, 0, 0))
    for f, (label, col, num) in enumerate(FUNCS):
        if deck and label in DECK_COLOURED: col = DECKCOL[deck]
        c = rgb(col)
        for light in range(3):
            img = Image.new("RGBA", (TW * SS, TH * SS), (0, 0, 0, 0))
            d = ImageDraw.Draw(img)
            fill = {0: mix(c, BG, 0.10), 1: mix(c, BG, 0.38), 2: c}[light]
            edge = {0: mix(c, BG, 0.45), 1: c, 2: mix(c, (255, 255, 255), 0.6)}[light]
            d.rounded_rectangle([3 * SS, 3 * SS, (TW - 3) * SS, (TH - 3) * SS], radius=rad * SS, fill=fill + (255,),
                                outline=edge + (255,), width=3 * SS)
            img = img.resize((TW, TH), Image.LANCZOS)
            d = ImageDraw.Draw(img)
            ink = {0: mix(c, BG, 0.75), 1: (255, 255, 255), 2: (11, 13, 16)}[light]
            if label: d.text((TW / 2, TH / 2 + 2), label, font=big, fill=ink + (255,), anchor="mm")
            if num:
                d.text((TW * 0.08, TH * 0.09), str(num), font=small,
                       fill=(mix(ink, fill, 0.6) if light else mix(c, BG, 0.5)) + (255,), anchor="la")
            strip.paste(img, (0, (f * 3 + light) * TH))
    strip.save(os.path.join(here, fname), optimize=True)
    print("wrote %s: %d frames %dx%d (%d px)" % (fname, nframes, TW, TH, nframes * TH))
