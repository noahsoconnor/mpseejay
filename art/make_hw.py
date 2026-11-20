#!/usr/bin/env python3
"""The hardware look: every bitmap the skin draws that should look like a physical control or screen.
Needs Pillow and Titillium Web; run in python:3.11-slim with Pillow (see docs/DEVELOPMENT.md).

  pads_a.png / pads_b.png / pads_s.png  backlit rubber pads, one frame per pad function and light
                                        (value = function * 3 + light: 0 dim, 1 on, 2 pressed; order of PF_* in src/deck.c)
  key_*.png                             keycaps (off / pressed), some with an LED
  deck_[ab]_on/off.png, sync_*.png      the deck switch and SYNC keys
  platter.png, jogcap.png               the jog: LED ring (deck A / B, then amber for the end warning), metal, vinyl
  beat.png, phase.png                   the beat counter and phase meter, as LEDs
  lcd_*.png, plate_*.png                the screens (header, waveforms, browser) and the brushed panels
"""
import math, os, random, sys
from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

here = os.path.dirname(os.path.abspath(__file__))
fonts = sys.argv[1] if len(sys.argv) > 1 else os.path.join(here, "..", "..", "vendor", "mpc-vst-plugins", "tools", "html_art", "fonts")
BOLD = os.path.join(fonts, "TitilliumWeb-Bold.ttf")
SS = 3                      # draw at 3x and shrink, for smooth edges
random.seed(7)              # the brushed-metal grain is the same on every build

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
    + [("LOOP %d" % b, "ff6bd5", i + 1) for i, b in enumerate([1, 2, 4, 8])]
    # 1.3: the browser's SORT pad (PF_B_SORT)
    + [("SORT", "ffc14d", 15)]
    # the browser's STARS - / + pads (PF_B_STAR_DN, PF_B_STAR_UP)
    + [("STARS -", "ffc14d", 3), ("STARS +", "ffc14d", 4)]
    # the browser's CHILL / HYPE pads (PF_B_CHILL, PF_B_HYPE)
    + [("CHILL", "5ac8fa", 7), ("HYPE", "ff5a3c", 8)])
# Pads that take the deck's colour (pad_deck_coloured in src/deck.c): CUE, pads 5-8, the hot cues and their SHIFT functions
DECK_COLOURED = {"CUE", "SYNC", "JUMP -", "JUMP +", "SHIFT", "HALF", "DOUBLE", "LEADER"} | {"CUE %d" % i for i in range(1, 9)}
DECK = {"a": "2f9bff", "b": "ff4d6a"}
AMBER = "ff9a1f"


# --- helpers -------------------------------------------------------------------------------------------------------
def rgb(h): return tuple(int(h[i:i + 2], 16) for i in (0, 2, 4))
def mix(a, b, t): return tuple(round(a[i] * t + b[i] * (1 - t)) for i in range(3))
def font(px): return ImageFont.truetype(BOLD, max(1, round(px * SS)))


def rr_mask(W, H, box, r):
    m = Image.new("L", (W, H), 0)
    ImageDraw.Draw(m).rounded_rectangle([round(v) for v in box], radius=round(r), fill=255)
    return m


def vgrad(W, H, top, bot):   # top colour to bottom colour
    g = Image.linear_gradient("L").resize((W, H))
    return Image.composite(Image.new("RGB", (W, H), bot), Image.new("RGB", (W, H), top), g).convert("RGBA")


def soft(W, H, box, r, col, alpha, blur):   # a blurred rounded shape: shadows, halos, LED glow
    m = rr_mask(W, H, box, r).filter(ImageFilter.GaussianBlur(blur)).point(lambda v: round(v * alpha))
    layer = Image.new("RGBA", (W, H), col + (0,))
    layer.putalpha(m)
    return layer


def shade(W, H, box, r, alpha, top=True):   # a light sheen over the top (or a dark band at the bottom) of a shape
    x0, y0, x1, y1 = box
    m = Image.new("L", (W, H), 0)
    g = Image.linear_gradient("L").resize((round(x1 - x0), round((y1 - y0) * 0.55)))
    if top: g = ImageChops.invert(g)
    g = g.point(lambda v: round(v * alpha / 255))   # alpha: the strongest opacity, 0-255
    m.paste(g, (round(x0), round(y0) if top else round(y1 - (y1 - y0) * 0.55)))
    m = ImageChops.multiply(m, rr_mask(W, H, box, r))
    layer = Image.new("RGBA", (W, H), (255, 255, 255, 0) if top else (0, 0, 0, 0))
    layer.putalpha(m)
    return layer


def spaced(d, cx, cy, s, f, fill, sp=0.0):   # text centred on (cx, cy) with letter spacing, as on a silkscreen
    sp *= SS
    widths = [f.getlength(ch) for ch in s]
    x = cx - (sum(widths) + sp * (len(s) - 1)) / 2
    for ch, w in zip(s, widths):
        d.text((x + w / 2, cy), ch, font=f, fill=fill, anchor="mm")
        x += w + sp


def shrink(img, w, h): return img.resize((w, h), Image.LANCZOS)


# --- the pads: backlit rubber ----------------------------------------------------------------------------------------
RUBBER_TOP, RUBBER_BOT = (50, 53, 58), (29, 31, 35)


def pad(TW, TH, label, col, num, light, big, small):
    W, H = TW * SS, TH * SS
    c = rgb(col)
    img = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    m, r = 4 * SS, (10 if TW > 100 else 6) * SS
    dy = 2 * SS if light == 2 else 0          # pressed: the pad sinks
    box = [m, m - SS + dy, W - m, H - m - SS + dy]
    img.alpha_composite(soft(W, H, [m, m + 2 * SS, W - m, H - m + SS], r, (0, 0, 0), 0.9 if light < 2 else 0.5, 2.5 * SS))
    if light:   # the LED light leaking round the pad
        img.alpha_composite(soft(W, H, box, r, c, 0.45 if light == 1 else 0.9, (3 if light == 1 else 5) * SS))
    tint = {0: 0.13, 1: 0.5, 2: 0.92}[light]
    body = vgrad(W, H, mix(c, RUBBER_TOP, tint), mix(c, RUBBER_BOT, tint * 0.8))
    if light:   # brighter in the middle, where the LED sits under the rubber
        rg = ImageChops.invert(Image.radial_gradient("L").resize((W, H)))
        rg = rg.point(lambda v: round(max(0, v - 70) * (0.7 if light == 1 else 1.2)))
        hot = Image.new("RGBA", (W, H), mix(c, (255, 255, 255), 0.55 if light == 2 else 0.8) + (0,))
        hot.putalpha(rg)
        body.alpha_composite(hot)
    img.paste(body, (0, 0), rr_mask(W, H, box, r))
    img.alpha_composite(shade(W, H, box, r, 26 if light < 2 else 40))
    img.alpha_composite(shade(W, H, box, r, 70, top=False))
    d = ImageDraw.Draw(img)
    d.rounded_rectangle(box, radius=r, outline=(6, 7, 8, 255), width=round(1.5 * SS))
    d.line([box[0] + r, box[1] + 2 * SS, box[2] - r, box[1] + 2 * SS], fill=(255, 255, 255, 46 if light < 2 else 120), width=SS)
    ink = {0: mix(c, (120, 126, 134), 0.55), 1: (255, 255, 255), 2: (16, 17, 19)}[light]
    if label:
        spaced(d, W / 2, (box[1] + box[3]) / 2 + SS, label, font(big), ink + (255,), 0.6 if TW > 100 else 0.2)
    if num:
        nf = font(small)
        nink = mix(ink, mix(c, RUBBER_TOP, tint), 0.55) if light else mix(c, (90, 95, 102), 0.35)
        d.text((box[0] + 7 * SS, box[1] + 5 * SS), str(num), font=nf, fill=nink + (255,), anchor="la")
    return shrink(img, TW, TH)


def pads():
    for fname, TW, TH, BIG, SMALL, deck in [("pads_a.png", 138, 86, 22, 12, "a"), ("pads_b.png", 138, 86, 22, 12, "b"),
                                            ("pads_s.png", 76, 62, 13, 9, None)]:
        nframes = len(FUNCS) * 3 + (1 if deck else 0)   # deck strips end with an empty frame (the browser is open)
        strip = Image.new("RGBA", (TW, TH * nframes), (0, 0, 0, 0))
        for f, (label, col, num) in enumerate(FUNCS):
            if deck and label in DECK_COLOURED: col = DECK[deck]
            for light in range(3):
                strip.paste(pad(TW, TH, label, col, num, light, BIG, SMALL), (0, (f * 3 + light) * TH))
        strip.save(os.path.join(here, fname), optimize=True)
        print("wrote %s: %d frames %dx%d" % (fname, nframes, TW, TH))


# --- keycaps ---------------------------------------------------------------------------------------------------------
CAP_TOP, CAP_BOT = (66, 70, 77), (37, 40, 45)


def keycap(name, w, h, label, led=None, led_on=False, pressed=False, tint=None, letter=None, ink=None):
    W, H = w * SS, h * SS
    img = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    m, lip, r = 3 * SS, 5 * SS, 8 * SS
    dy = 3 * SS if pressed else 0
    img.alpha_composite(soft(W, H, [m, m + 3 * SS, W - m, H - m + SS], r, (0, 0, 0), 0.95, 3 * SS))
    d = ImageDraw.Draw(img)
    d.rounded_rectangle([m, m + dy, W - m, H - m], radius=r, fill=(13, 14, 16, 255))   # the key's skirt
    box = [m + SS, m + dy, W - m - SS, H - m - lip + dy]
    if tint and not pressed:   # a backlit key: its colour round the edge
        img.alpha_composite(soft(W, H, box, r, tint, 0.55, 4 * SS))
    top, bot = CAP_TOP, CAP_BOT
    if tint: top, bot = mix(tint, CAP_TOP, 0.62), mix(tint, CAP_BOT, 0.5)
    if pressed: top, bot = mix(top, (0, 0, 0), 0.85), mix(bot, (0, 0, 0), 0.85)
    img.paste(vgrad(W, H, top, bot), (0, 0), rr_mask(W, H, box, r))
    img.alpha_composite(shade(W, H, box, r, 30 if not tint else 48))
    d = ImageDraw.Draw(img)
    d.rounded_rectangle(box, radius=r, outline=(8, 9, 10, 255), width=round(1.5 * SS))
    d.line([box[0] + r, box[1] + 2 * SS, box[2] - r, box[1] + 2 * SS], fill=(255, 255, 255, 60), width=SS)
    cy = (box[1] + box[3]) / 2 + SS
    if led:   # a small LED bar at the top of the cap
        lc = rgb(led)
        lw, lh = min(w * 0.3, 46) * SS, 4 * SS
        lb = [W / 2 - lw / 2, box[1] + 6 * SS, W / 2 + lw / 2, box[1] + 6 * SS + lh]
        if led_on:
            img.alpha_composite(soft(W, H, lb, 2 * SS, lc, 1.0, 4 * SS))
            d.rounded_rectangle(lb, radius=2 * SS, fill=mix(lc, (255, 255, 255), 0.7) + (255,))
        else:
            d.rounded_rectangle(lb, radius=2 * SS, fill=mix(lc, (20, 21, 24), 0.22) + (255,))
        cy += 5 * SS
    ink = ink or ((255, 255, 255) if tint else (226, 230, 235))
    if pressed: ink = mix(ink, top, 0.8)
    if letter:   # the deck keys: a big letter with DECK beside it
        spaced(d, box[0] + (box[2] - box[0]) * 0.3, cy, "DECK", font(12), mix(ink, top, 0.75) + (255,), 1.2)
        spaced(d, box[0] + (box[2] - box[0]) * 0.7, cy, letter, font(46), ink + (255,))
    else:
        spaced(d, W / 2, cy, label, font(min(h * 0.34, 21)), ink + (255,), 1.6)
    shrink(img, w, h).save(os.path.join(here, name), optimize=True)


def stars():
    """stars.png: 7 frames 110x20 for a browser row: 0 nothing, then 0 to 5 stars lit (gold) of 5."""
    import math
    W, H, F = 110, 20, 7
    img = Image.new("RGBA", (W * SS, H * F * SS), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    for f in range(1, F):
        for k in range(5):
            cx, cy, R = (11 + 22 * k) * SS, (f * H + 10) * SS, 9 * SS
            pts = [(cx + (R if j % 2 == 0 else R * 0.45) * math.sin(math.pi * j / 5),
                    cy - (R if j % 2 == 0 else R * 0.45) * math.cos(math.pi * j / 5)) for j in range(10)]
            on = k < f - 1
            d.polygon(pts, fill=(255, 193, 77, 255) if on else (52, 58, 68, 255))
    img.resize((W, H * F), Image.LANCZOS).save(os.path.join(here, "stars.png"), optimize=True)


def tags():
    """tags.png: 3 frames 84x28 for a browser row: 0 nothing, 1 CHILL, 2 HYPE (a lit pill)."""
    W, H = 84, 28
    img = Image.new("RGBA", (W * SS, H * 3 * SS), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    for f, (label, col) in enumerate([("CHILL", "5ac8fa"), ("HYPE", "ff5a3c")], start=1):
        c = tuple(int(col[i:i + 2], 16) for i in (0, 2, 4))
        y0 = f * H * SS
        d.rounded_rectangle([2 * SS, y0 + 3 * SS, (W - 2) * SS, y0 + (H - 3) * SS], radius=11 * SS, fill=c + (255,))
        spaced(d, W * SS / 2, y0 + H * SS / 2, label, font(13), (8, 9, 11, 255), 1.4)
    img.resize((W, H * 3), Image.LANCZOS).save(os.path.join(here, "tags.png"), optimize=True)


def keys():
    plain = [("bpm_up", "BPM +", 170, 60), ("bpm_dn", "BPM -", 170, 60), ("reset", "RESET", 170, 50),
             ("browse", "BROWSE", 170, 56), ("back", "BACK", 150, 50), ("pgup", "PAGE UP", 150, 50),
             ("pgdn", "PAGE DOWN", 170, 50), ("close", "CLOSE", 130, 50), ("jback", "JUMP BACK", 170, 70),
             ("jfwd", "JUMP FWD", 170, 70), ("refresh", "REFRESH", 150, 50),
             # 1.3: the START page
             ("st_bar_dn", "< BAR", 190, 66), ("st_beat_dn", "< BEAT", 190, 66), ("st_fine_dn", "< FINE", 190, 66),
             ("st_fine_up", "FINE >", 190, 66), ("st_beat_up", "BEAT >", 190, 66), ("st_bar_up", "BAR >", 190, 66),
             ("st_set", "START AT PLAYHEAD", 330, 66), ("st_auto", "AUTO", 190, 66), ("st_top", "TRACK TOP", 220, 66),
             ("st_play", "PLAY FROM START", 330, 66), ("st_done", "DONE", 130, 50)]
    for n, label, w, h in plain:
        keycap("key_%s.png" % n, w, h, label)
        keycap("key_%s_on.png" % n, w, h, label, pressed=True)
    for n, label, w, h, led in [("load_a", "LOAD A", 170, 50, DECK["a"]), ("load_b", "LOAD B", 170, 50, DECK["b"]),
                                ("eject", "EJECT", 130, 50, "ffb04d"), ("learn", "LEARN PADS", 230, 66, "ffb04d"), ("pl_a", "LEARN DECK A FADER", 270, 50, DECK["a"]), ("pl_b", "LEARN DECK B FADER", 270, 50, DECK["b"]), ("chill", "CHILL", 145, 50, "5ac8fa"), ("hype", "HYPE", 145, 50, "ff5a3c")]:
        keycap("key_%s.png" % n, w, h, label, led=led, led_on=True)
        keycap("key_%s_on.png" % n, w, h, label, led=led, led_on=True, pressed=True)
    keycap("sync_off.png", 170, 58, "SYNC", led="ffffff")
    keycap("sync_on.png", 170, 58, "SYNC", led="ffffff", led_on=True, tint=(150, 160, 172))
    for L in "ab":
        c = rgb(DECK[L])
        keycap("deck_%s_on.png" % L, 118, 80, "", tint=c, letter=L.upper())
        keycap("deck_%s_off.png" % L, 118, 80, "", letter=L.upper(), ink=mix(c, (226, 230, 235), 0.85))
    print("wrote the keycaps")


# --- the jog ---------------------------------------------------------------------------------------------------------
def platter():
    S, R = 2, 170                              # 2x, half the 340 px frame
    N = 2 * R * S
    c = N / 2
    base = Image.new("RGBA", (N, N), (0, 0, 0, 0))
    px = base.load()
    for y in range(N):
        for x in range(N):
            dx, dy = x + 0.5 - c, y + 0.5 - c
            r = math.hypot(dx, dy) / S
            if r > 167: continue
            a = math.atan2(dy, dx)
            if r > 158: v = (16, 17, 19)                          # the LED ring's channel (lit per frame)
            elif r > 152: v = (5, 6, 7)
            elif r > 119:                                         # brushed metal ring, the light catching it twice
                k = 62 + 30 * math.cos(2 * (a + 0.7)) + 8 * math.cos(6 * a)
                if r > 149: k += 30                               # bevelled outer edge
                elif r < 122: k -= 22
                v = (round(k), round(k + 2), round(k + 6))
            elif r > 115: v = (4, 5, 6)
            else:                                                 # the platter's top: fine grooves and a soft sheen
                k = 15 + (4 if int(r * 1.6) % 2 else 0) + 9 * max(0.0, math.cos(2 * (a - 0.9))) * (r / 115)
                v = (round(k), round(k + 1), round(k + 3))
            px[x, y] = v + (255,)
    d = ImageDraw.Draw(base)
    for i in range(24):                                           # engraved ticks on the metal ring
        a = math.radians(i * 15)
        r0, r1 = (125, 146) if i % 6 == 0 else (132, 144)
        d.line([c + math.sin(a) * r0 * S, c - math.cos(a) * r0 * S, c + math.sin(a) * r1 * S, c - math.cos(a) * r1 * S],
               fill=(14, 15, 17, 255), width=3 * S if i % 6 == 0 else 2 * S)
    frames = Image.new("RGBA", (340, 340 * 4), (0, 0, 0, 0))
    for f, col in enumerate([DECK["a"], DECK["b"], AMBER, AMBER]):
        img = base.copy()
        lc = rgb(col)
        ring = Image.new("L", (N, N), 0)
        ImageDraw.Draw(ring).ellipse([c - 165 * S, c - 165 * S, c + 165 * S, c + 165 * S], outline=255, width=5 * S)
        glow = Image.new("RGBA", (N, N), lc + (0,))
        glow.putalpha(ring.filter(ImageFilter.GaussianBlur(5 * S)).point(lambda v: min(255, v * 2)))
        img.alpha_composite(glow)
        led = Image.new("RGBA", (N, N), mix(lc, (255, 255, 255), 0.75) + (0,))
        led.putalpha(ring)
        img.alpha_composite(led)
        clip = Image.new("L", (N, N), 0)
        ImageDraw.Draw(clip).ellipse([c - 169 * S, c - 169 * S, c + 169 * S, c + 169 * S], fill=255)
        img.putalpha(ImageChops.multiply(img.getchannel("A"), clip))
        frames.paste(img.resize((340, 340), Image.LANCZOS), (0, f * 340))
    frames.save(os.path.join(here, "platter.png"), optimize=True)

    # the centre cap: spun aluminium with a white marker (a knob image, turned with the value)
    N = 116 * SS
    c = N / 2
    cap = Image.new("RGBA", (N, N), (0, 0, 0, 0))
    px = cap.load()
    for y in range(N):
        for x in range(N):
            dx, dy = x + 0.5 - c, y + 0.5 - c
            r = math.hypot(dx, dy) / SS
            if r > 56: continue
            a = math.atan2(dy, dx)
            k = 120 + 50 * math.cos(2 * (a + 0.7)) + (3 if int(r * 2.2) % 2 else 0)
            if r > 53: k = 40
            elif r > 50: k += 35 - (r - 50) * 10
            elif r < 20: k -= 25
            v = max(0, min(255, round(k)))
            px[x, y] = (v, v + 1 if v < 255 else v, min(255, v + 4), 255)
    d = ImageDraw.Draw(cap)
    d.rounded_rectangle([c - 3 * SS, 7 * SS, c + 3 * SS, 30 * SS], radius=3 * SS, fill=(250, 250, 250, 255),
                        outline=(30, 32, 36, 255), width=SS)
    shrink(cap, 116, 116).save(os.path.join(here, "jogcap.png"), optimize=True)
    print("wrote platter.png, jogcap.png")


# --- screens and panels ----------------------------------------------------------------------------------------------
def lcd(name, w, h):
    W, H = w * SS, h * SS
    img = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    r = 10 * SS
    d.rounded_rectangle([0, 0, W - 1, H - 1], radius=r, fill=(44, 47, 52, 255))                    # bezel edge catching light
    d.rounded_rectangle([0, 0, W - 1, H - 1 - 2 * SS], radius=r, fill=(4, 5, 6, 255))              # bezel
    box = [4 * SS, 4 * SS, W - 4 * SS, H - 6 * SS]
    img.paste(vgrad(W, H, (13, 16, 20), (7, 9, 11)), (0, 0), rr_mask(W, H, box, r - 3 * SS))
    img.alpha_composite(shade(W, H, box, r - 3 * SS, 9))                                           # glass
    inner = Image.new("L", (W, H), 0)                                                               # inner shadow
    ImageDraw.Draw(inner).rounded_rectangle(box, radius=r - 3 * SS, outline=255, width=5 * SS)
    sh = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    sh.putalpha(ImageChops.multiply(inner.filter(ImageFilter.GaussianBlur(4 * SS)), rr_mask(W, H, box, r - 3 * SS)))
    img.alpha_composite(sh)
    shrink(img, w, h).save(os.path.join(here, name), optimize=True)


def plate(name, w, h, wells=()):
    """A brushed, anodised panel; wells are recessed areas (x, y, w, h) where pads sit."""
    W, H = w * SS, h * SS
    grain = Image.new("L", (1, H // SS))
    grain.putdata([max(0, min(255, round(random.gauss(128, 9)))) for _ in range(H // SS)])
    grain = grain.resize((W // 4, H // SS)).resize((W, H))
    noise = Image.effect_noise((W, H), 14).point(lambda v: round(128 + (v - 128) * 0.5))
    tex = Image.blend(grain, noise, 0.35)
    base = Image.new("RGB", (W, H), (25, 27, 30))
    light = Image.new("RGB", (W, H), (33, 35, 39))
    metal = Image.composite(light, base, tex).convert("RGBA")
    img = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    r = 12 * SS
    img.paste(metal, (0, 0), rr_mask(W, H, [0, 0, W - 1, H - 1], r))
    img.alpha_composite(shade(W, H, [0, 0, W - 1, H - 1], r, 10))
    d = ImageDraw.Draw(img)
    d.rounded_rectangle([0, 0, W - 1, H - 1], radius=r, outline=(5, 6, 7, 255), width=2 * SS)
    d.line([r, 2 * SS, W - r, 2 * SS], fill=(255, 255, 255, 34), width=SS)
    for x, y, ww, hh in wells:
        b = [x * SS, y * SS, (x + ww) * SS, (y + hh) * SS]
        img.paste(vgrad(W, H, (9, 10, 12), (14, 15, 17)), (0, 0), rr_mask(W, H, b, 10 * SS))
        d.line([b[0] + 10 * SS, b[3], b[2] - 10 * SS, b[3]], fill=(255, 255, 255, 30), width=SS)   # lit lower lip
    for x, y in [(11, 11), (w - 11, 11), (11, h - 11), (w - 11, h - 11)]:                            # screws
        x, y = x * SS, y * SS
        d.ellipse([x - 5 * SS, y - 5 * SS, x + 5 * SS, y + 5 * SS], fill=(52, 55, 60, 255), outline=(8, 9, 10, 255), width=SS)
        d.line([x - 3 * SS, y - 2 * SS, x + 3 * SS, y + 2 * SS], fill=(14, 15, 17, 255), width=round(1.5 * SS))
    shrink(img, w, h).save(os.path.join(here, name), optimize=True)


def panels():
    plate("plate_head.png", 1252, 90)
    lcd("lcd_track.png", 458, 80)
    lcd("lcd_bpm.png", 528, 80)
    lcd("lcd_wave.png", 1252, 98)
    lcd("lcd_browser.png", 1252, 528)
    # the deck panel (x 14, y 326): a well under the 16 pads
    plate("plate_deck.png", 1252, 388, wells=[(4, 4, 588, 380)])
    print("wrote the screens and panels")


# --- the beat counter and phase meter: LEDs behind a dark lens -------------------------------------------------------
DECK4 = [DECK["a"], DECK["b"], "36d97f", "b07cff"]   # the strips keep 4 decks' frames (deck_id was A-D before 1.0)
GREEN = "36d97f"


def lens(img, box, col=None, r=4):   # one LED segment: dark when col is None, lit with a glow otherwise
    W, H = img.size
    d = ImageDraw.Draw(img)
    if col:
        c = rgb(col)
        img.alpha_composite(soft(W, H, box, r, c, 0.9, 3 * SS))
        img.paste(vgrad(W, H, mix(c, (255, 255, 255), 0.6), c), (0, 0), rr_mask(W, H, box, r))
    else:
        img.paste(vgrad(W, H, (26, 28, 32), (14, 15, 17)), (0, 0), rr_mask(W, H, box, r))
        d.line([box[0] + r, box[1] + SS, box[2] - r, box[1] + SS], fill=(255, 255, 255, 22), width=SS)


def meters():
    w, h = 232, 24
    strip = Image.new("RGBA", (w, h * 20), (0, 0, 0, 0))
    for fr in range(20):
        deck, f = divmod(fr, 5)
        img = Image.new("RGBA", (w * SS, h * SS), (0, 0, 0, 0))
        bw = (w - 3 * 8) / 4
        for i in range(4):
            box = [(i * (bw + 8) + 2) * SS, 4 * SS, (i * (bw + 8) + bw - 2) * SS, (h - 4) * SS]
            lens(img, box, ("ffffff" if i == 0 else DECK4[deck]) if f == i + 1 else None, 3 * SS)
        strip.paste(shrink(img, w, h), (0, fr * h))
    strip.save(os.path.join(here, "beat.png"), optimize=True)

    w, h = 560, 24
    strip = Image.new("RGBA", (w, h * 136), (0, 0, 0, 0))
    for fr in range(136):
        deck, f = divmod(fr, 34)
        img = Image.new("RGBA", (w * SS, h * SS), (0, 0, 0, 0))
        d = ImageDraw.Draw(img)
        slot = [2 * SS, 7 * SS, (w - 2) * SS, (h - 7) * SS]
        img.paste(vgrad(w * SS, h * SS, (5, 6, 7), (12, 13, 15)), (0, 0), rr_mask(w * SS, h * SS, slot, 5 * SS))
        d.line([slot[0] + 5 * SS, slot[3], slot[2] - 5 * SS, slot[3]], fill=(255, 255, 255, 26), width=SS)
        d.rectangle([(w / 2 - 1) * SS, 2 * SS, (w / 2 + 1) * SS, (h - 2) * SS], fill=(150, 156, 165, 255))
        if f:
            off = f - 17
            x = w / 2 + off * (w / 2 - 14) / 16
            lens(img, [(x - 11) * SS, 5 * SS, (x + 11) * SS, (h - 5) * SS], GREEN if abs(off) <= 1 else DECK4[deck], 3 * SS)
        strip.paste(shrink(img, w, h), (0, fr * h))
    strip.save(os.path.join(here, "phase.png"), optimize=True)
    print("wrote beat.png, phase.png")


if __name__ == "__main__":
    pads()
    stars()
    tags()
    keys()
    platter()
    panels()
    meters()
