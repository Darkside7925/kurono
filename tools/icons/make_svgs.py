#!/usr/bin/env python3
"""Author Kurono's modern app icon sets as SVG sources.

Original artwork for Kurono, in two sets that share one superellipse tile:
  - colourful (src/assets/icons/*.svg): soft vertical gradients, a faint top
    sheen and solid white glyphs.
  - minimal (src/assets/icons/minimal/*.svg): dark glass tiles with a hairline
    edge and thin monochrome line glyphs.
Run this after editing the definitions below, then run icons_to_blob.py to
rebuild the embedded bitmap blob.
"""
import math, os, sys

OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), '../../src/assets/icons')
S = 128

def squircle(n=5.0, steps=160, inset=0.0):
    r = S / 2 - inset
    c = S / 2
    pts = []
    for i in range(steps):
        t = 2 * math.pi * i / steps
        ct, st = math.cos(t), math.sin(t)
        x = c + r * (abs(ct) ** (2 / n)) * (1 if ct >= 0 else -1)
        y = c + r * (abs(st) ** (2 / n)) * (1 if st >= 0 else -1)
        pts.append(f'{x:.2f},{y:.2f}')
    return 'M' + ' L'.join(pts) + ' Z'

TILE = squircle()

def dial_ticks(cx, cy, r_in, r_out, count, start_deg=135, sweep=270):
    """Tick marks along an arc, as one path of short radial strokes."""
    d = []
    for i in range(count):
        a = math.radians(start_deg + sweep * i / (count - 1))
        x0, y0 = cx + r_in * math.cos(a), cy + r_in * math.sin(a)
        x1, y1 = cx + r_out * math.cos(a), cy + r_out * math.sin(a)
        d.append(f'M{x0:.2f},{y0:.2f} L{x1:.2f},{y1:.2f}')
    return ' '.join(d)

def arc(cx, cy, r, a0, a1):
    x0, y0 = cx + r * math.cos(math.radians(a0)), cy + r * math.sin(math.radians(a0))
    x1, y1 = cx + r * math.cos(math.radians(a1)), cy + r * math.sin(math.radians(a1))
    large = 1 if (a1 - a0) % 360 > 180 else 0
    return f'M{x0:.2f},{y0:.2f} A{r},{r} 0 {large} 1 {x1:.2f},{y1:.2f}'

GLYPH_SHADOW = '<filter id="gs" x="-20%" y="-20%" width="140%" height="140%"><feDropShadow dx="0" dy="1.6" stdDeviation="1.6" flood-color="#000" flood-opacity="0.18"/></filter>'

def tile(name, top, bottom, glyph, sheen=0.16, dark_glyph=False):
    return f'''<svg xmlns="http://www.w3.org/2000/svg" width="{S}" height="{S}" viewBox="0 0 {S} {S}">
  <defs>
    <linearGradient id="bg" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="{top}"/><stop offset="1" stop-color="{bottom}"/></linearGradient>
    <linearGradient id="sh" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#fff" stop-opacity="{sheen}"/><stop offset="0.55" stop-color="#fff" stop-opacity="0"/></linearGradient>
    <clipPath id="clip"><path d="{TILE}"/></clipPath>
    {GLYPH_SHADOW}
  </defs>
  <path d="{TILE}" fill="url(#bg)"/>
  <path d="{TILE}" fill="url(#sh)"/>
  <g clip-path="url(#clip)" filter="url(#gs)">{glyph}</g>
</svg>
'''

W = '#FFFFFF'
ICONS = {
    # >_ prompt on graphite
    'terminal': tile('terminal', '#3A3E4A', '#15171D',
        f'<path d="M38 46 L58 64 L38 82" fill="none" stroke="{W}" stroke-width="9" stroke-linecap="round" stroke-linejoin="round"/>'
        f'<rect x="66" y="76" width="26" height="9" rx="4.5" fill="#7CF2B6"/>'),
    # folder
    'files': tile('files', '#62C2FF', '#2667F0',
        f'<path d="M30 46 q0-8 8-8 h17 q4 0 6.5 3 l4 4.5 q2 2.5 6 2.5 h19 q8 0 8 8 v31 q0 8-8 8 h-53 q-8 0-8-8 z" fill="{W}" opacity="0.62"/>'
        f'<path d="M30 58 q0-7 8-7 h52 q8 0 8 7 v24 q0 8-8 8 h-52 q-8 0-8-8 z" fill="{W}"/>'),
    # 2x2 operator keys
    'calculator': tile('calculator', '#FFB547', '#FF6E2C',
        f'<rect x="34" y="34" width="26" height="26" rx="8" fill="{W}"/>'
        f'<rect x="68" y="34" width="26" height="26" rx="8" fill="{W}" opacity="0.62"/>'
        f'<rect x="34" y="68" width="26" height="26" rx="8" fill="{W}" opacity="0.62"/>'
        f'<rect x="68" y="68" width="26" height="26" rx="8" fill="{W}"/>'
        f'<path d="M47 41 v12 M41 47 h12" stroke="#FF8A34" stroke-width="4" stroke-linecap="round"/>'
        f'<path d="M75 81 h12 M75 87 h12" stroke="#FF8A34" stroke-width="3.6" stroke-linecap="round" transform="translate(0 -3)"/>'),
    # note page with a warm header
    'editor': tile('editor', '#FFFFFF', '#E9ECF2',
        f'<path d="M24 0 h80 v36 h-80 z" fill="#FFCF4A"/><path d="M24 36 h80" stroke="#F2B624" stroke-width="2"/>'
        f'<path d="M36 56 h56 M36 70 h56 M36 84 h38" stroke="#C3C8D3" stroke-width="5" stroke-linecap="round"/>', sheen=0.0),
    # tuning dial: a knob with its indicator, and the tick arc it sweeps
    'settings': tile('settings', '#9A8CFF', '#5B3FE4',
        f'<path d="{dial_ticks(64, 66, 34, 41, 13)}" stroke="{W}" stroke-width="4" stroke-linecap="round" opacity="0.55"/>'
        f'<path d="{dial_ticks(64, 66, 34, 41, 13, 135, 157.5)}" stroke="{W}" stroke-width="4" stroke-linecap="round"/>'
        f'<circle cx="64" cy="66" r="24" fill="{W}"/>'
        f'<path d="M64 66 L52.5 54.5" stroke="#6A4FF0" stroke-width="6" stroke-linecap="round"/>'
        f'<circle cx="64" cy="66" r="4.5" fill="#6A4FF0"/>'),
    # globe
    'browser': tile('browser', '#5BD3FF', '#2A72E8',
        f'<circle cx="64" cy="64" r="30" fill="none" stroke="{W}" stroke-width="6"/>'
        f'<ellipse cx="64" cy="64" rx="12.5" ry="30" fill="none" stroke="{W}" stroke-width="5"/>'
        f'<path d="M36 54 h56 M36 74 h56" stroke="{W}" stroke-width="5" stroke-linecap="round"/>'),
    # activity pulse on dark glass
    'tasks': tile('tasks', '#2A2F3D', '#10131A',
        f'<path d="M26 70 h20 l8-22 l12 40 l10-28 l6 10 h20" fill="none" stroke="#3EE58C" stroke-width="7" stroke-linecap="round" stroke-linejoin="round"/>'),
    # play
    'media': tile('media', '#FF7096', '#E02A62',
        f'<path d="M52 40 q0-7 6-3.5 l34 22 q5.5 3.5 0 7 l-34 22 q-6 3.5-6-3.5 z" fill="{W}"/>'),
    # house
    'home': tile('home', '#86B4FF', '#4B5CF2',
        f'<path d="M64 34 l32 26 q3 2.5-1 2.5 h-5 v28 q0 6-6 6 h-12 v-20 h-16 v20 h-12 q-6 0-6-6 v-28 h-5 q-4 0-1-2.5 z" fill="{W}"/>'),
    # web: orbit around a planet (generic, not any browser's mark)
    'web': tile('web', '#FFA25A', '#8B4FE6',
        f'<circle cx="64" cy="64" r="18" fill="{W}"/>'
        f'<ellipse cx="64" cy="64" rx="36" ry="13" fill="none" stroke="{W}" stroke-width="5" transform="rotate(-22 64 64)" opacity="0.9"/>'),
    # four dots
    'generic': tile('generic', '#9AA4BA', '#566078',
        f'<circle cx="50" cy="50" r="10" fill="{W}"/><circle cx="78" cy="50" r="10" fill="{W}" opacity="0.62"/>'
        f'<circle cx="50" cy="78" r="10" fill="{W}" opacity="0.62"/><circle cx="78" cy="78" r="10" fill="{W}"/>'),
}

def minimal(glyph):
    """Dark glass tile, hairline edge, thin monochrome line glyph."""
    inner = squircle(inset=1.2)
    return f'''<svg xmlns="http://www.w3.org/2000/svg" width="{S}" height="{S}" viewBox="0 0 {S} {S}">
  <defs>
    <linearGradient id="bg" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#2A2C34"/><stop offset="1" stop-color="#16171C"/></linearGradient>
  </defs>
  <path d="{TILE}" fill="url(#bg)"/>
  <path d="{inner}" fill="none" stroke="#FFFFFF" stroke-opacity="0.16" stroke-width="2.4"/>
  <g fill="none" stroke="#F2F3F6" stroke-width="5.5" stroke-linecap="round" stroke-linejoin="round">{glyph}</g>
</svg>
'''

MINIMAL = {
    'terminal':   minimal('<path d="M40 48 L56 64 L40 80"/><path d="M66 82 H88"/>'),
    'files':      minimal('<path d="M34 50 q0-8 8-8 h14 q3.5 0 5.5 2.5 l4 4.5 q2 2 5 2 h17 q8 0 8 8 v26 q0 8-8 8 h-45.5 q-8 0-8-8 z"/>'),
    'calculator': minimal('<path d="M47 37 v18 M38 46 h18"/><path d="M72 46 h18"/><path d="M40 74 l14 14 M54 74 l-14 14"/><path d="M72 77 h18 M72 85 h18"/>'),
    'editor':     minimal('<path d="M42 32 h34 l14 14 v42 q0 8-8 8 h-40 q-8 0-8-8 v-48 q0-8 8-8 z"/><path d="M76 32 v14 h14"/><path d="M48 64 h32 M48 78 h22"/>'),
    'settings':   minimal(f'<path d="{arc(64, 66, 38, 135, 405)}" stroke-opacity="0.45"/><path d="{arc(64, 66, 38, 135, 292)}"/>'
                          '<circle cx="64" cy="66" r="20"/><path d="M64 66 L55 57"/>'),
    'browser':    minimal('<circle cx="64" cy="64" r="28"/><ellipse cx="64" cy="64" rx="11" ry="28"/><path d="M38 54 h52 M38 74 h52"/>'),
    'tasks':      minimal('<path d="M28 70 h18 l8-22 l12 40 l10-28 l6 10 h18"/>'),
    'media':      minimal('<path d="M53 42 q0-6 5.5-3 l31 20 q5 3 0 6.5 l-31 20 q-5.5 3-5.5-3 z"/>'),
    'home':       minimal('<path d="M36 62 L64 38 L92 62"/><path d="M44 56 v28 q0 6 6 6 h28 q6 0 6-6 v-28"/><path d="M58 90 v-16 h12 v16"/>'),
    'web':        minimal('<circle cx="64" cy="64" r="16"/><ellipse cx="64" cy="64" rx="36" ry="12" transform="rotate(-22 64 64)" stroke-opacity="0.75"/>'),
    'generic':    minimal('<circle cx="50" cy="50" r="9"/><circle cx="78" cy="50" r="9"/><circle cx="50" cy="78" r="9"/><circle cx="78" cy="78" r="9"/>'),
}

ORDER = ['terminal', 'files', 'calculator', 'editor', 'settings', 'browser',
         'tasks', 'media', 'home', 'web', 'generic']

if __name__ == '__main__':
    os.makedirs(os.path.join(OUT, 'minimal'), exist_ok=True)
    for name in ORDER:
        with open(os.path.join(OUT, name + '.svg'), 'w') as f:
            f.write(ICONS[name])
        with open(os.path.join(OUT, 'minimal', name + '.svg'), 'w') as f:
            f.write(MINIMAL[name])
    print('wrote', 2 * len(ORDER), 'icons to', os.path.abspath(OUT))
