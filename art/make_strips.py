#!/usr/bin/env python3
"""Filmstrips for the Decks meters, written as PNGs with the standard library only (frames stacked top to bottom).
One set of frames per deck letter (A..D), each in that deck's colour:
  wave.png   144 frames 14x84: a waveform bar, value = deck*36 + state*12 + level (state 0 to play, 1 played, 2 playhead)
  phase.png  136 frames 560x24: deck*34 + (0 = no reference, 1..33 = the deck's beat against the reference, 17 = in phase)
  beat.png    20 frames 232x24: deck*5 + (0 = none, 1..4 = beat of the bar)"""
import os, struct, zlib

def png(path, w, h, px):   # px: list of rows of (r,g,b,a)
    raw = b"".join(b"\0" + bytes(c for p in row for c in p) for row in px)
    def chunk(t, d): return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    open(path, "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
                           + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))

def hexc(s, a=255): return (int(s[0:2], 16), int(s[2:4], 16), int(s[4:6], 16), a)
CLEAR = (0, 0, 0, 0)
DECK = ["2f9bff", "ff4d6a", "36d97f", "b07cff"]   # deck A blue, B red, C green, D purple (also in make_art.py)
HEAD = hexc("ffffff")
def mix(c, bg, t): return tuple(round(c[i] * t + bg[i] * (1 - t)) for i in range(3)) + (255,)
GREEN, ORANGE, DIM, TICK = hexc("3ddc84"), hexc("ff8a1e"), hexc("1e232a"), hexc("8a94a3")

def rect(img, x0, y0, x1, y1, c):
    for y in range(max(0, y0), min(len(img), y1)):
        for x in range(max(0, x0), min(len(img[0]), x1)):
            img[y][x] = c

def strip(frames, w, h, draw):
    rows = []
    for f in range(frames):
        img = [[CLEAR] * w for _ in range(h)]
        draw(img, f)
        rows += img
    return rows

def wave(img, f):
    deck, f = divmod(f, 36)
    st, lvl = divmod(f, 12)
    h, w = len(img), len(img[0])
    col = hexc(DECK[deck])
    bar = max(2, round((h - 4) * lvl / 11))
    top, bot = (h - bar) // 2, (h + bar) // 2
    if st == 1: col = mix(col, hexc("0b0d10"), 0.32)          # played: the deck colour, dimmed
    rect(img, 2, h // 2, w - 2, h // 2 + 1, mix(hexc(DECK[deck]), hexc("0b0d10"), 0.25))   # centre line
    rect(img, 2, top, w - 2, h // 2, col)
    rect(img, 2, h // 2, w - 2, bot, mix(col, hexc("0b0d10"), 0.7))   # lower half a shade darker
    if st == 2:   # playhead: a full-height white line
        rect(img, w // 2 - 2, 0, w // 2 + 2, h, HEAD)

# the phase meter and beat counter are drawn by make_hw.py

here = os.path.dirname(os.path.abspath(__file__))
png(os.path.join(here, "wave.png"), 14, 84 * 144, strip(144, 14, 84, wave))
png(os.path.join(here, "wave_s.png"), 14, 40 * 144, strip(144, 14, 40, wave))   # half height: A and B rows

# the jog platter is drawn by make_hw.py

# browser rows (920x64), one picture per look (the skin shows the one for the row's brs_N, MPC's mode images):
# 0 normal, 1 selected, 2/3 on deck A/B, 4/5 the same selected, 6 folder, 7 folder selected (8: none, no picture).
# A track row has a darker cell at the right for its tempo.
BPM_X = 776
def row(img, f):
    h, w = len(img), len(img[0])
    base = hexc("12151a")
    if f in (6, 7): base = hexc("1c1f25")
    if f in (2, 4): base = mix(hexc(DECK[0]), hexc("0b0d10"), 0.28)
    if f in (3, 5): base = mix(hexc(DECK[1]), hexc("0b0d10"), 0.28)
    rect(img, 0, 0, w, h, base)
    if f in (1, 7): rect(img, 0, 0, w, h, hexc("2c333d"))
    if f in (4, 5): rect(img, 14, 0, w, h, mix(hexc(DECK[0] if f == 4 else DECK[1]), hexc("0b0d10"), 0.45))
    if f < 6:   # the tempo cell
        rect(img, BPM_X, 6, w - 8, h - 6, mix(hexc("000000"), img[h // 2][w - 4], 0.45))
    if f in (2, 3, 4, 5):   # a solid deck-coloured bar at the left edge
        rect(img, 0, 0, 14, h, hexc(DECK[0] if f in (2, 4) else DECK[1]))
    if f in (1, 4, 5, 7):   # selected: lit up, with a thick white frame (seen at a glance in a dark room)
        edge = hexc("ffffff"); t = 5
        rect(img, 0, 0, w, t, edge); rect(img, 0, h - t, w, h, edge); rect(img, 0, 0, t, h, edge); rect(img, w - t, 0, w, h, edge)

for f in range(8):
    png(os.path.join(here, "row_%d.png" % f), 920, 64, strip(1, 920, 64, lambda img, _f, f=f: row(img, f)))
for old in ("rows.png", "veil.png"):
    if os.path.exists(os.path.join(here, old)): os.remove(os.path.join(here, old))
print("wrote wave.png, wave_s.png, row_0..7.png")
