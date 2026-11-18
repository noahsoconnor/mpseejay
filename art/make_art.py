#!/usr/bin/env python3
"""SVG artwork for the Decks skin (drawn by the browser renderer, "art": "html").
Per-deck pictures follow the CONTROL parameter, so the screen takes the colour of the deck it is showing."""
import os

DECK = ["2f9bff", "ff4d6a", "36d97f", "b07cff"]   # deck A blue, B red, C green, D purple (also in make_strips.py)
FONT = "font-family:'Titillium Web',sans-serif;font-weight:700"
here = os.path.dirname(os.path.abspath(__file__))


def svg(name, w, h, body):
    open(os.path.join(here, name), "w").write(
        '<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" viewBox="0 0 %d %d">%s</svg>\n' % (w, h, w, h, body))


def text(x, y, s, size, fill, anchor="middle", spacing=0):
    return ('<text x="%g" y="%g" text-anchor="%s" dominant-baseline="central" style="%s;font-size:%gpx;'
            'letter-spacing:%gpx" fill="%s">%s</text>' % (x, y, anchor, FONT, size, spacing, fill, s))


for i, c in enumerate(DECK[:2]):
    L = "ABCD"[i]
    # deck badge: the letter on the deck colour
    svg("badge_%s.svg" % L.lower(), 120, 76,
        '<rect x="1" y="1" width="118" height="74" rx="10" fill="#%s"/>' % c
        + text(32, 38, "DECK", 15, "#0b0d10", spacing=1.5) + text(88, 40, L, 54, "#0b0d10"))
    # a thin rule in the deck colour (header underline and waveform edges)
    svg("rule_%s.svg" % L.lower(), 1252, 4, '<rect width="1252" height="4" rx="2" fill="#%s"/>' % c)

# the A / B deck switch: the deck on screen filled in its colour, the other dark with a coloured edge
for i, c in enumerate(DECK[:2]):
    L = "AB"[i]
    svg("deck_%s_on.svg" % L.lower(), 118, 80, '<rect x="2" y="2" width="114" height="76" rx="12" fill="#%s"/>' % c
        + text(30, 41, "DECK", 13, "#0b0d10", spacing=1.5) + text(82, 42, L, 50, "#0b0d10"))
    svg("deck_%s_off.svg" % L.lower(), 118, 80, '<rect x="3" y="3" width="112" height="74" rx="12" fill="#14171c" '
        'stroke="#%s" stroke-opacity="0.55" stroke-width="3"/>' % c
        + text(30, 41, "DECK", 13, "#566070", spacing=1.5) + text(82, 42, L, 50, "#%s" % c))

# jog centre cap (a knob image, turned with the value; JOG always reads back the middle so it points up)
svg("jogcap.svg", 116, 116,
    '<circle cx="58" cy="58" r="56" fill="#161a20" stroke="#3a424d" stroke-width="2"/>'
    '<circle cx="58" cy="58" r="44" fill="#1d2229"/>'
    '<rect x="55" y="6" width="6" height="26" rx="3" fill="#ffffff"/>'
    + text(58, 66, "JOG", 18, "#8a94a3", spacing=2))


def round_button(name, ring, fill, glyph, glyph_fill):
    svg(name, 150, 150,
        '<circle cx="75" cy="75" r="71" fill="%s" stroke="%s" stroke-width="6"/>' % (fill, ring) + glyph % glyph_fill)


# CUE: orange ring; lit while held.
CUE = text(75, 77, "CUE", 40, "%s", spacing=3)
round_button("cue.svg", "#ff9a1f", "#14171c", CUE, "#ff9a1f")
round_button("cue_on.svg", "#ffb04d", "#ff9a1f", CUE, "#0b0d10")
# PLAY / PAUSE: green ring with the play-pause glyph; filled green while playing.
PP = ('<path d="M44 52 L44 98 L82 75 Z" fill="%s"/><rect x="90" y="52" width="9" height="46" rx="2" fill="%s"/>'
      '<rect x="105" y="52" width="9" height="46" rx="2" fill="%s"/>')
svg("play_off.svg", 150, 150, '<circle cx="75" cy="75" r="71" fill="#14171c" stroke="#36d97f" stroke-width="6"/>'
    + PP % (("#36d97f",) * 3))
svg("play_on.svg", 150, 150, '<circle cx="75" cy="75" r="71" fill="#36d97f" stroke="#7af0b0" stroke-width="6"/>'
    + PP % (("#0b0d10",) * 3))


def pill(name, w, h, label, on, color="#ffffff"):
    fill, stroke, ink = (color, color, "#0b0d10") if on else ("#14171c", "#3a424d", "#c9d1db")
    svg(name, w, h, '<rect x="2" y="2" width="%d" height="%d" rx="%d" fill="%s" stroke="%s" stroke-width="3"/>'
        % (w - 4, h - 4, (h - 4) // 2, fill, stroke) + text(w / 2, h / 2 + 1, label, 22, ink, spacing=2))


pill("sync_off.svg", 180, 60, "SYNC", False)
pill("sync_on.svg", 180, 60, "SYNC", True, "#ffffff")
pill("leader_off.svg", 180, 60, "LEADER", False)
pill("leader_on.svg", 180, 60, "LEADER", True, "#ffb04d")


def chevrons(name, d, on):
    fill, ink = ("#e8edf2", "#0b0d10") if on else ("#14171c", "#e8edf2")
    xs = (36, 58) if d < 0 else (64, 86)
    arrows = "".join(('<path d="M%d 38 L%d 58 L%d 78 Z" fill="%s"/>' % (x + 18, x, x + 18, ink)) if d < 0 else
                     ('<path d="M%d 38 L%d 58 L%d 78 Z" fill="%s"/>' % (x - 18, x, x - 18, ink)) for x in xs)
    svg(name, 120, 150, '<rect x="2" y="2" width="116" height="146" rx="16" fill="%s" stroke="#3a424d" stroke-width="3"/>'
        % fill + arrows + text(60, 116, "SLOW" if d < 0 else "FAST", 17, ink, spacing=2))


chevrons("nudge_dn.svg", -1, False)
chevrons("nudge_dn_on.svg", -1, True)
chevrons("nudge_up.svg", 1, False)
chevrons("nudge_up_on.svg", 1, True)

# an invisible touch plate over each pad tile (the tile itself is a meter, which takes no touch)
svg("clear_row.svg", 920, 64, '<rect width="920" height="64" fill="#000" fill-opacity="0.001"/>')
svg("clear.svg", 138, 88, '<rect width="138" height="88" fill="#000" fill-opacity="0.001"/>')
print("wrote the SVG artwork")
