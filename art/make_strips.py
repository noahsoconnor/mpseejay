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

def phase(img, f):
    deck, f = divmod(f, 34)
    h, w = len(img), len(img[0])
    rect(img, 0, h // 2 - 1, w, h // 2 + 1, DIM)
    rect(img, w // 2 - 1, 0, w // 2 + 1, h, TICK)
    if f == 0: return
    off = f - 17
    x = w // 2 + round(off * (w / 2 - 12) / 16)
    rect(img, x - 10, 3, x + 10, h - 3, GREEN if abs(off) <= 1 else hexc(DECK[deck]))

def beat(img, f):
    deck, f = divmod(f, 5)
    h, w = len(img), len(img[0])
    bw = (w - 3 * 8) // 4
    for i in range(4):
        x = i * (bw + 8)
        on = f == i + 1
        rect(img, x, 0, x + bw, h, (HEAD if i == 0 else hexc(DECK[deck])) if on else DIM)

here = os.path.dirname(os.path.abspath(__file__))
png(os.path.join(here, "wave.png"), 14, 84 * 144, strip(144, 14, 84, wave))
png(os.path.join(here, "wave_s.png"), 14, 40 * 144, strip(144, 14, 40, wave))   # half height: A and B rows
png(os.path.join(here, "phase.png"), 560, 24 * 136, strip(136, 560, 24, phase))
png(os.path.join(here, "beat.png"), 232, 24 * 20, strip(20, 232, 24, beat))

def platter(img, f):   # 4 frames 340x340: the jog platter, ring in deck A / B colour, then amber for A / B (end warning)
    import math
    n = len(img); c = n / 2; ring = hexc(DECK[f]) if f < 2 else hexc("ff9a1f")   # 2, 3: the end-of-track flash
    for y in range(n):
        for x in range(n):
            acc = [0, 0, 0, 0]
            for sy in (0.25, 0.75):
                for sx in (0.25, 0.75):
                    r = math.hypot(x + sx - c, y + sy - c)
                    if r > 167: px = (0, 0, 0, 0)
                    elif r > 161: px = ring
                    elif r > 153: px = (10, 12, 15, 255)
                    elif r > 150: px = (42, 48, 57, 255)
                    else:
                        t = r / 150
                        g = round(34 - 18 * t)
                        px = (g, g + 3, g + 7, 255)
                        if 74 < r < 146 and int(r) % 7 == 0: px = (g + 9, g + 12, g + 16, 255)   # grooves
                        a = math.degrees(math.atan2(x + sx - c, c - y - sy)) % 15
                        if 128 < r < 144 and (a < 1.2 or a > 13.8):   # tick marks in the ring colour
                            px = mix(ring, (g, g, g, 255), 0.6)
                    for i in range(4): acc[i] += px[i] * (px[3] if i < 3 else 1)
            a = acc[3] / 4
            img[y][x] = (round(acc[0] / acc[3]) if acc[3] else 0, round(acc[1] / acc[3]) if acc[3] else 0,
                         round(acc[2] / acc[3]) if acc[3] else 0, round(a))

png(os.path.join(here, "platter.png"), 340, 340 * 4, strip(4, 340, 340, platter))

# browser rows (920x64): 0 normal, 1 selected, 2/3 on deck A/B, 4/5 the same selected, 6 folder, 7 folder selected, 8 empty
def row(img, f):
    h, w = len(img), len(img[0])
    if f == 8: return
    base = hexc("12151a")
    if f in (6, 7): base = hexc("1c1f25")
    if f in (2, 4): base = mix(hexc(DECK[0]), hexc("0b0d10"), 0.28)
    if f in (3, 5): base = mix(hexc(DECK[1]), hexc("0b0d10"), 0.28)
    rect(img, 0, 0, w, h, base)
    if f in (2, 3, 4, 5):   # a solid deck-coloured bar at the left edge
        rect(img, 0, 0, 14, h, hexc(DECK[0] if f in (2, 4) else DECK[1]))
    if f in (1, 4, 5, 7):   # selected: lit up, with a thick white frame (seen at a glance in a dark room)
        if f in (1, 7): rect(img, 0, 0, w, h, hexc("2c333d"))
        if f in (4, 5): rect(img, 14, 0, w, h, mix(hexc(DECK[0] if f == 4 else DECK[1]), hexc("0b0d10"), 0.45))
        edge = hexc("ffffff"); t = 5
        rect(img, 0, 0, w, t, edge); rect(img, 0, h - t, w, h, edge); rect(img, 0, 0, t, h, edge); rect(img, w - t, 0, w, h, edge)

def veil(img, f):   # 1: the grey veil over a played track's row
    if f: rect(img, 0, 0, len(img[0]), len(img), (10, 12, 15, 200))

png(os.path.join(here, "rows.png"), 920, 64 * 9, strip(9, 920, 64, row))
png(os.path.join(here, "veil.png"), 920, 64 * 2, strip(2, 920, 64, veil))
print("wrote wave.png, wave_s.png, phase.png, beat.png, platter.png, rows.png, veil.png")
