#!/usr/bin/env python3
"""
Generates the toolbar icon set as stroke-style SVGs, one light-ink and one
dark-ink variant per glyph, for the icon themes to pick between.

    python3 gen_toolbar_icons.py

Writes <name>-onlight.svg (dark ink, for the light theme) and
<name>-ondark.svg (light ink, for the dark theme) beside itself. Accent
icons (start, stop) carry their fill in both variants.

Design language: 24x24 grid, 1.8 stroke, round caps and joins, no fills
except the accents. The old bitmaps this replaces were a mixed bag of
colors and weights; these draw from one grid with one pen.
"""

INK_LIGHT_THEME = "#3c3c43"   # dark ink on a light toolbar
INK_DARK_THEME = "#d8d8dc"    # light ink on a dark toolbar
ACCENT_GREEN = "#34c759"
ACCENT_RED = "#ff453a"

# Every glyph: the body of an SVG document, {ink} is the stroke color.
GLYPHS = {
    # Monitor with a waveform on it - the demo device.
    "demo-screen": """
        <rect x="3" y="4.5" width="18" height="12" rx="1.6"/>
        <polyline points="6,12.5 8.5,12.5 10,8.5 12,15 13.8,10 15.5,12.5 18,12.5"/>
        <line x1="9" y1="20" x2="15" y2="20"/>
        <line x1="12" y1="16.5" x2="12" y2="20"/>
    """,
    # A document sheet, folded corner, two text lines.
    "file-doc": """
        <path d="M6 3.5 h8.2 L19 8.3 V20.5 H6 Z"/>
        <path d="M14 3.5 V8.5 H19"/>
        <line x1="9" y1="12" x2="16" y2="12"/>
        <line x1="9" y1="15.5" x2="16" y2="15.5"/>
    """,
    # USB trident; the small numeral tells the generation.
    "usb2": """
        <line x1="12" y1="4" x2="12" y2="17"/>
        <polyline points="12,7.2 8.6,9.4 8.6,13.4"/>
        <polyline points="12,7.2 15.4,9.4 15.4,11.6"/>
        <circle cx="15.4" cy="13.2" r="0.55"/>
        <rect x="10.4" y="17" width="3.2" height="3.4" rx="0.7"/>
        <text x="16.4" y="8.4" font-size="7" font-weight="600">2</text>
    """,
    "usb3": """
        <line x1="12" y1="4" x2="12" y2="17"/>
        <polyline points="12,7.2 8.6,9.4 8.6,13.4"/>
        <polyline points="12,7.2 15.4,9.4 15.4,11.6"/>
        <circle cx="15.4" cy="13.2" r="0.55"/>
        <rect x="10.4" y="17" width="3.2" height="3.4" rx="0.7"/>
        <text x="16.4" y="8.4" font-size="7" font-weight="600">3</text>
    """,
    # Open folder.
    "folder-open": """
        <path d="M3.5 6.5 h5.4 l2 2.2 h9.6 v9.8 a1.4 1.4 0 0 1 -1.4 1.4 H3.5 Z"/>
        <path d="M3.5 19.9 L6.2 12 h15.9"/>
    """,
    # Save: a sheet dropping into a tray.
    "save-tray": """
        <polyline points="7.5,3.5 16.5,3.5 16.5,10"/>
        <polyline points="7.5,10 7.5,3.5"/>
        <rect x="4" y="11.5" width="16" height="9" rx="1.6"/>
        <line x1="12" y1="4.5" x2="12" y2="14"/>
        <polyline points="8.8,10.5 12,14 15.2,10.5"/>
        <line x1="8" y1="17.5" x2="16" y2="17.5"/>
    """,
    # Gear: ring, teeth, hub.
    "gear": """
        <circle cx="12" cy="12" r="3"/>
        <circle cx="12" cy="12" r="6.2"/>
        <g>{teeth}</g>
    """,
    # Single run: play with a 1.
    "single-run": """
        <path d="M7 5.5 L16.5 12 L7 18.5 Z"/>
        <text x="16" y="19.5" font-size="8" font-weight="600">1</text>
    """,
    # Repeat: loop of two arrows.
    "repeat-run": """
        <path d="M6.5 8.5 a5.5 5.5 0 0 1 10.4 -1.2"/>
        <polyline points="16.2,4.2 17,7.6 13.6,8.4"/>
        <path d="M17.5 15.5 a5.5 5.5 0 0 1 -10.4 1.2"/>
        <polyline points="7.8,19.8 7,16.4 10.4,15.6"/>
    """,
    # Zoom to fit: magnifier with arrows pulling in and out.
    "zoom-fit": """
        <circle cx="10.8" cy="10.8" r="6.3"/>
        <line x1="15.4" y1="15.4" x2="20" y2="20"/>
        <polyline points="8.2,13.4 8.2,8.2 13.4,8.2"/>
        <polyline points="13.4,13.4 8.2,13.4 8.2,8.2"/>
        <line x1="8.2" y1="13.4" x2="13.4" y2="8.2"/>
    """,
    # Zoom in.
    "zoom-in": """
        <circle cx="10.8" cy="10.8" r="6.3"/>
        <line x1="15.4" y1="15.4" x2="20" y2="20"/>
        <line x1="8.3" y1="10.8" x2="13.3" y2="10.8"/>
        <line x1="10.8" y1="8.3" x2="10.8" y2="13.3"/>
    """,
    # Zoom out.
    "zoom-out": """
        <circle cx="10.8" cy="10.8" r="6.3"/>
        <line x1="15.4" y1="15.4" x2="20" y2="20"/>
        <line x1="8.3" y1="10.8" x2="13.3" y2="10.8"/>
    """,
    # Cursors: two measure flags facing each other.
    "cursors": """
        <line x1="7" y1="4" x2="7" y2="20"/>
        <line x1="17" y1="4" x2="17" y2="20"/>
        <path d="M7 7 L3.4 10.5 L7 14 Z"/>
        <path d="M17 10 L20.6 13.5 L17 17 Z"/>
    """,
    # Decoder: waveform with a plus.
    "decoder": """
        <polyline points="3,14.5 6,14.5 8,8 11,18 13.5,11.5 15,14.5 16.5,14.5"/>
        <line x1="17.5" y1="6.5" x2="17.5" y2="11.5"/>
        <line x1="15" y1="9" x2="20" y2="9"/>
    """,
    # Decoding result: document with a check.
    "decode-result": """
        <path d="M6 3.5 h8.2 L19 8.3 V20.5 H6 Z"/>
        <path d="M14 3.5 V8.5 H19"/>
        <polyline points="8.6,14.5 11,17 15.6,11.8"/>
    """,
    # Help: question in a circle.
    "help-book": """
        <circle cx="12" cy="12" r="8.5"/>
        <path d="M9.4 9.6 a2.6 2.6 0 1 1 3.4 2.5 c-0.8 0.3 -0.8 0.9 -0.8 1.6"/>
        <circle cx="12" cy="16.6" r="0.7"/>
    """,
    # Sliders: the device options.
    "sliders": """
        <line x1="4" y1="7" x2="20" y2="7"/>
        <circle cx="9" cy="7" r="1.9"/>
        <line x1="4" y1="12" x2="20" y2="12"/>
        <circle cx="15" cy="12" r="1.9"/>
        <line x1="4" y1="17" x2="20" y2="17"/>
        <circle cx="7" cy="17" r="1.9"/>
    """,
    # Channel select: the board with a count.
    "channels": """
        <rect x="3.5" y="5.5" width="17" height="13" rx="1.6"/>
        <line x1="3.5" y1="9.8" x2="20.5" y2="9.8"/>
        <line x1="3.5" y1="14.1" x2="20.5" y2="14.1"/>
        <circle cx="7" cy="7.7" r="0.8"/>
        <circle cx="7" cy="12" r="0.8"/>
        <circle cx="7" cy="16.3" r="0.8"/>
    """,
}

# Accent icons are filled shapes; one shape per variant, same in both.
ACCENTS = {
    "start-play": f"""
        <path d="M8.2 5.2 L18 12 L8.2 18.8 Z" fill="{ACCENT_GREEN}"
            stroke="{ACCENT_GREEN}" stroke-width="1.8" stroke-linejoin="round"/>
    """,
    "stop-square": f"""
        <rect x="6.6" y="6.6" width="10.8" height="10.8" rx="1.8"
            fill="{ACCENT_RED}" stroke="{ACCENT_RED}" stroke-width="1.8"/>
    """,
}

# The gear teeth, rotated copies of one stub.
TEETH = "".join(
    f'<line x1="12" y1="3.2" x2="12" y2="5.6" '
    f'transform="rotate({angle} 12 12)"/>'
    for angle in range(0, 360, 45)
)

TEMPLATE = """<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24"
    fill="none" stroke="{ink}" stroke-width="1.8"
    stroke-linecap="round" stroke-linejoin="round">{body}
</svg>
"""


def svg_for(name, ink):
    if name in ACCENTS:
        return TEMPLATE.format(ink=ink, body=ACCENTS[name])
    body = GLYPHS[name]
    if "{teeth}" in body:
        body = body.replace("{teeth}", TEETH)
    # Text glyphs take the ink as fill, not as stroke.
    body = body.replace(
        'font-weight="600">',
        f'font-weight="600" fill="{ink}" stroke="none">')
    return TEMPLATE.format(ink=ink, body=body)


def main():
    import os
    target = os.path.dirname(os.path.abspath(__file__))
    for name in list(GLYPHS) + list(ACCENTS):
        for suffix, ink in (
            ("onlight", INK_LIGHT_THEME),   # dark ink, light theme
            ("ondark", INK_DARK_THEME),     # light ink, dark theme
        ):
            path = os.path.join(target, f"{name}-{suffix}.svg")
            with open(path, "w") as f:
                f.write(svg_for(name, ink))
        print(f"{name}: -onlight.svg -ondark.svg")


if __name__ == "__main__":
    main()
