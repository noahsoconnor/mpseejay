#!/usr/bin/env python3
"""The few SVGs the skin still uses (drawn by the browser renderer, "art": "html"): the deck-coloured rule under the
header, and the invisible touch plates over the pad tiles and browser rows. The rest of the art is in make_hw.py."""
import os

DECK = ["2f9bff", "ff4d6a"]   # deck A blue, B red (also in make_strips.py and make_hw.py)
here = os.path.dirname(os.path.abspath(__file__))


def svg(name, w, h, body):
    open(os.path.join(here, name), "w").write(
        '<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" viewBox="0 0 %d %d">%s</svg>\n' % (w, h, w, h, body))


for i, c in enumerate(DECK):
    svg("rule_%s.svg" % "ab"[i], 1252, 4, '<rect width="1252" height="4" rx="2" fill="#%s"/>' % c)
svg("clear_row.svg", 920, 64, '<rect width="920" height="64" fill="#000" fill-opacity="0.001"/>')
svg("clear_wave.svg", 1252, 98, '<rect width="1252" height="98" fill="#000" fill-opacity="0.001"/>')   # tap the waveform: START page
svg("clear.svg", 138, 88, '<rect width="138" height="88" fill="#000" fill-opacity="0.001"/>')
print("wrote the SVG artwork")
