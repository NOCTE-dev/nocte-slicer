#!/usr/bin/env python3
# NØCTE Slicer — Copyright (c) 2026 NØCTE Engineering. AGPL-3.0-or-later.
"""Reproducible generator for the NØCTE app-identity assets under ``resources/images``.

The NØCTE brand is strictly monochrome: pure black (#000000) and pure white (#FFFFFF), with
grey only where anti-aliasing produces it. The mark is a ring enclosing an ``N``; the wordmark
is ``NØCTE`` in a condensed, uniform-stroke geometric sans.

Why the assets are *drawn* rather than resampled
------------------------------------------------
The brand originals are large rasters (``_src/`` below). Downsampling them to a 16 px icon turns
the 6.7 %-of-diameter ring into a sub-pixel grey smear. So this script measures the originals
once (see ``MARK`` / ``WORD`` — every constant below is a pixel measurement of
``NOCTE_TRANSPARENTE.png``) and re-draws the same geometry at each target size, thickening the
stroke where a target is too small to carry the nominal weight. The same geometry model emits
the SVG files, so vector and raster targets cannot drift apart.

The originals are the user's own brand files and stay in the gitignored ``_src/`` cache; only
derived assets are written into ``resources/``.

Usage
-----
    python tools/nocte/branding/make_assets.py            # fetch if needed, then generate
    python tools/nocte/branding/make_assets.py --fetch    # force re-download of the originals
    python tools/nocte/branding/make_assets.py --measure  # re-measure the originals and report
    python tools/nocte/branding/make_assets.py --out DIR  # write elsewhere (e.g. for review)

Requires Pillow and an authenticated ``gh`` CLI (only for ``--fetch`` / the first run).
"""

import argparse
import math
import os
import subprocess
import sys

from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
SRC_DIR = os.path.join(HERE, "_src")
OUT_DIR = os.path.join(REPO, "resources", "images")

# Brand originals in the user's private repos: (repo, path in repo, local name).
SOURCES = (
    ("alma", "branding/LOGO_NOCTE.png", "LOGO_NOCTE.png"),
    ("alma", "branding/NOCTE_TRANSPARENTE.png", "NOCTE_TRANSPARENTE.png"),
    ("nocte-sentinel", "src/assets/nocte-wordmark.png", "nocte-wordmark.png"),
    ("nocte-ide", "packages/ide/src/renderer/src/assets/logo-outline.png", "logo-outline.png"),
    ("nocte-ide", "packages/ide/src/renderer/src/assets/logo-filled.png", "logo-filled.png"),
    ("nocte-ide", "ICONO/ICONO_NOCTE-IDE.png", "ICONO_NOCTE-IDE.png"),
)
OWNER = "NOCTE-dev"

# The reference original. LOGO_NOCTE.png is the same lockup flattened onto black; the *_IDE and
# logo-{filled,outline} files carry the "IDE PRØ" sub-brand and are not used for the slicer.
REFERENCE = "NOCTE_TRANSPARENTE.png"

BLACK = (0, 0, 0, 255)
WHITE = (255, 255, 255, 255)

# --------------------------------------------------------------------------------------------
# Geometry, measured from NOCTE_TRANSPARENTE.png (1582x994, white on transparent).
# Run with --measure to reproduce these numbers.
# --------------------------------------------------------------------------------------------

# The mark occupies a 359x360 px box; ring outer diameter 357 px, ring and N stems 23-24 px.
# The N stems sit at x 97..119 and 240..262 and span y 75..285.
MARK = {
    "stroke": 24.0 / 360.0,       # 0.0667 of the box
    "outer_r": 178.5 / 360.0,     # 0.4958 of the box
    "stem_cx": (108.0 / 359.0, 251.0 / 359.0),
    "stem_top": 75.0 / 360.0,
    "stem_bot": 285.0 / 360.0,
}

# The wordmark is 753 px wide on a 206 px cap height; strokes are 28-29 px.
# Letter boxes (x within the wordmark): N 1..124, O/ 170..307, C 352..474, T 502..617, E 653..751.
WORD = {
    "stroke": 28.5 / 206.0,       # 0.1383 of the cap height
    # (glyph, advance width, gap to the next glyph), both in cap heights
    "glyphs": (
        ("N", 124.0 / 206.0, 46.0 / 206.0),
        ("O", 138.0 / 206.0, 45.0 / 206.0),
        ("C", 123.0 / 206.0, 28.0 / 206.0),
        ("T", 116.0 / 206.0, 36.0 / 206.0),
        ("E",  99.0 / 206.0, 0.0),
    ),
}

# Lockup: mark 360 tall, 72 px of air, then a 206 px cap-height wordmark, optically centred.
LOCKUP_GAP = 72.0 / 360.0         # 0.20 of the mark height
LOCKUP_CAP = 206.0 / 360.0        # 0.5722 of the mark height

# The C is an open ring; its aperture faces right (degrees, clockwise from 3 o'clock).
C_APERTURE = (35, 325)

SS = 8                            # raster supersampling factor
MIN_STROKE_PX = 1.55              # thinnest stroke that still reads on screen


def word_width_units():
    """Wordmark width in cap heights."""
    return sum(w + g for _, w, g in WORD["glyphs"])


def lockup_ratio():
    """Lockup width / mark height."""
    return 1.0 + LOCKUP_GAP + LOCKUP_CAP * word_width_units()


# --------------------------------------------------------------------------------------------
# Fetching the originals
# --------------------------------------------------------------------------------------------

def fetch(force=False):
    os.makedirs(SRC_DIR, exist_ok=True)
    for repo, path, name in SOURCES:
        dest = os.path.join(SRC_DIR, name)
        if os.path.exists(dest) and not force:
            continue
        api = "repos/%s/%s/contents/%s" % (OWNER, repo, path)
        print("  fetch %s:%s" % (repo, path))
        with open(dest, "wb") as handle:
            proc = subprocess.run(
                ["gh", "api", api, "-H", "Accept: application/vnd.github.raw"],
                stdout=handle, stderr=subprocess.PIPE, universal_newlines=False,
            )
        if proc.returncode != 0:
            os.remove(dest)
            raise SystemExit("gh api failed for %s: %s" % (api, proc.stderr.decode("utf-8", "replace")))


def measure():
    """Re-derive the constants above from the reference original."""
    path = os.path.join(SRC_DIR, REFERENCE)
    alpha = Image.open(path).convert("RGBA").split()[3]
    w, h = alpha.size
    px = alpha.load()

    def cols():
        return [any(px[x, y] > 16 for y in range(h)) for x in range(w)]

    on = cols()
    xs = [x for x, v in enumerate(on) if v]
    runs, start = [], xs[0]
    for a, b in zip(xs, xs[1:]):
        if b - a > 5:
            runs.append((start, a))
            start = b
    runs.append((start, xs[-1]))
    print("  %s: %dx%d, column clusters %s" % (REFERENCE, w, h, runs))
    print("  first cluster = the mark, the rest are the N O/ C T E letters")


# --------------------------------------------------------------------------------------------
# Raster drawing (all coordinates in supersampled pixels)
# --------------------------------------------------------------------------------------------

def _rect(draw, x0, y0, x1, y1, color):
    draw.rectangle([x0, y0, x1, y1], fill=color)


def draw_mark(draw, x, y, size, color, stroke):
    """Ring + N, inside the square box (x, y, size). `stroke` is in pixels."""
    # Pillow grows an outline inwards from the bounding box, so the box is the *outer* edge here,
    # whereas the SVG emitters below place a centred stroke and inset by stroke/2 instead.
    r = MARK["outer_r"] * size
    cx, cy = x + size / 2.0, y + size / 2.0
    draw.ellipse([cx - r, cy - r, cx + r, cy + r], outline=color, width=int(round(stroke)))

    top = y + MARK["stem_top"] * size
    bot = y + MARK["stem_bot"] * size
    left_cx = x + MARK["stem_cx"][0] * size
    right_cx = x + MARK["stem_cx"][1] * size
    _rect(draw, left_cx - stroke / 2.0, top, left_cx + stroke / 2.0, bot, color)
    _rect(draw, right_cx - stroke / 2.0, top, right_cx + stroke / 2.0, bot, color)

    # Diagonal: a slanted bar of the same perpendicular weight, so its horizontal cut is wider.
    run = right_cx - left_cx
    rise = bot - top
    sh = stroke * math.hypot(run, rise) / rise
    draw.polygon([(left_cx - stroke / 2.0, top), (left_cx - stroke / 2.0 + sh, top),
                  (right_cx + stroke / 2.0, bot), (right_cx + stroke / 2.0 - sh, bot)], fill=color)


def draw_word(draw, x, y, cap, color, stroke):
    """NØCTE, cap height `cap`, top-left at (x, y). `stroke` is in pixels."""
    s = stroke
    for name, adv, gap in WORD["glyphs"]:
        w = adv * cap
        if name == "N":
            _rect(draw, x, y, x + s, y + cap, color)
            _rect(draw, x + w - s, y, x + w, y + cap, color)
            sh = s * math.hypot(w - s, cap) / cap
            draw.polygon([(x, y), (x + sh, y), (x + w, y + cap), (x + w - sh, y + cap)], fill=color)
        elif name == "O":
            ow = 0.60 * cap                      # the O itself; the slash overhangs it
            ox = x + (w - ow) / 2.0
            draw.ellipse([ox, y, ox + ow, y + cap], outline=color, width=int(round(s)))
            draw.line([(x + s * 0.55, y + cap - s * 0.55), (x + w - s * 0.55, y + s * 0.55)],
                      fill=color, width=int(round(s)))
        elif name == "C":
            # Ring with the aperture on the right: 35°..325° clockwise through bottom/left/top.
            draw.arc([x, y, x + w, y + cap],
                     C_APERTURE[0], C_APERTURE[1], fill=color, width=int(round(s)))
        elif name == "T":
            _rect(draw, x, y, x + w, y + s, color)
            _rect(draw, x + (w - s) / 2.0, y, x + (w + s) / 2.0, y + cap, color)
        elif name == "E":
            _rect(draw, x, y, x + s, y + cap, color)
            _rect(draw, x, y, x + w, y + s, color)
            _rect(draw, x, y + (cap - s) / 2.0, x + w * 0.92, y + (cap + s) / 2.0, color)
            _rect(draw, x, y + cap - s, x + w, y + cap, color)
        x += w + gap * cap


# --------------------------------------------------------------------------------------------
# Raster targets
# --------------------------------------------------------------------------------------------

def render_icon(px, plate=True):
    """Square app icon: white mark on a black rounded square (or on transparency)."""
    n = px * SS
    img = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    draw = ImageDraw.Draw(img)
    if plate:
        draw.rounded_rectangle([0, 0, n - 1, n - 1], radius=0.22 * n, fill=BLACK)
    mark_frac = 0.80 if px <= 24 else (0.72 if px <= 48 else 0.64)
    size = mark_frac * n
    off = (n - size) / 2.0
    stroke = max(MARK["stroke"] * size, MIN_STROKE_PX * SS)
    draw_mark(draw, off, off, size, WHITE, stroke)
    return img.resize((px, px), Image.LANCZOS)


def render_lockup(w, h, fg, bg=None, pad=0.10, rule=False):
    """Horizontal mark + NØCTE lockup, fitted into (w, h)."""
    W, H = w * SS, h * SS
    img = Image.new("RGBA", (W, H), bg if bg else (0, 0, 0, 0))
    draw = ImageDraw.Draw(img)

    avail_w, avail_h = W * (1 - 2 * pad), H * (1 - 2 * pad)
    mark_h = min(avail_h, avail_w / lockup_ratio())
    total_w = mark_h * lockup_ratio()
    x = (W - total_w) / 2.0
    y = (H - mark_h) / 2.0

    stroke = max(MARK["stroke"] * mark_h, MIN_STROKE_PX * SS)
    draw_mark(draw, x, y, mark_h, fg, stroke)

    cap = LOCKUP_CAP * mark_h
    wstroke = max(WORD["stroke"] * cap, MIN_STROKE_PX * SS)
    draw_word(draw, x + mark_h + LOCKUP_GAP * mark_h, y + (mark_h - cap) / 2.0, cap, fg, wstroke)

    if rule:
        t = max(2 * SS, int(0.016 * H))
        draw.rectangle([int(0.036 * W), H - t, W - int(0.036 * W) - 1, H - 1], fill=fg)
    return img.resize((w, h), Image.LANCZOS)


# --------------------------------------------------------------------------------------------
# SVG targets — the same geometry, emitted as paths/rects/circles only.
# nanosvg (used by BitmapCache::load_svg) does not render <text>, so there is none.
# Colours are pure #000000 / #FFFFFF, which BitmapCache's dark-mode palette never rewrites.
# --------------------------------------------------------------------------------------------

def svg_mark(x, y, size, color, stroke):
    r = MARK["outer_r"] * size - stroke / 2.0
    cx, cy = x + size / 2.0, y + size / 2.0
    top, bot = y + MARK["stem_top"] * size, y + MARK["stem_bot"] * size
    lx, rx = x + MARK["stem_cx"][0] * size, x + MARK["stem_cx"][1] * size
    sh = stroke * math.hypot(rx - lx, bot - top) / (bot - top)
    f = "fill:%s;" % color
    return (
        '<circle cx="%.2f" cy="%.2f" r="%.2f" style="fill:none;stroke:%s;stroke-width:%.2f;"/>'
        '<rect x="%.2f" y="%.2f" width="%.2f" height="%.2f" style="%s"/>'
        '<rect x="%.2f" y="%.2f" width="%.2f" height="%.2f" style="%s"/>'
        '<polygon points="%.2f,%.2f %.2f,%.2f %.2f,%.2f %.2f,%.2f" style="%s"/>'
    ) % (cx, cy, r, color, stroke,
         lx - stroke / 2.0, top, stroke, bot - top, f,
         rx - stroke / 2.0, top, stroke, bot - top, f,
         lx - stroke / 2.0, top, lx - stroke / 2.0 + sh, top,
         rx + stroke / 2.0, bot, rx + stroke / 2.0 - sh, bot, f)


def svg_word(x, y, cap, color, s):
    f = "fill:%s;" % color
    out = []
    for name, adv, gap in WORD["glyphs"]:
        w = adv * cap
        if name == "N":
            sh = s * math.hypot(w - s, cap) / cap
            out.append('<rect x="%.2f" y="%.2f" width="%.2f" height="%.2f" style="%s"/>' % (x, y, s, cap, f))
            out.append('<rect x="%.2f" y="%.2f" width="%.2f" height="%.2f" style="%s"/>' % (x + w - s, y, s, cap, f))
            out.append('<polygon points="%.2f,%.2f %.2f,%.2f %.2f,%.2f %.2f,%.2f" style="%s"/>'
                       % (x, y, x + sh, y, x + w, y + cap, x + w - sh, y + cap, f))
        elif name == "O":
            ow = 0.60 * cap
            ox = x + (w - ow) / 2.0
            out.append('<ellipse cx="%.2f" cy="%.2f" rx="%.2f" ry="%.2f" '
                       'style="fill:none;stroke:%s;stroke-width:%.2f;"/>'
                       % (ox + ow / 2.0, y + cap / 2.0, (ow - s) / 2.0, (cap - s) / 2.0, color, s))
            out.append('<line x1="%.2f" y1="%.2f" x2="%.2f" y2="%.2f" '
                       'style="fill:none;stroke:%s;stroke-width:%.2f;"/>'
                       % (x + s * 0.55, y + cap - s * 0.55, x + w - s * 0.55, y + s * 0.55, color, s))
        elif name == "C":
            rx, ry = (w - s) / 2.0, (cap - s) / 2.0
            cx, cy = x + w / 2.0, y + cap / 2.0
            a0, a1 = math.radians(C_APERTURE[1]), math.radians(C_APERTURE[0])
            out.append('<path d="M %.2f,%.2f A %.2f,%.2f 0 1 0 %.2f,%.2f" '
                       'style="fill:none;stroke:%s;stroke-width:%.2f;"/>'
                       % (cx + rx * math.cos(a0), cy + ry * math.sin(a0), rx, ry,
                          cx + rx * math.cos(a1), cy + ry * math.sin(a1), color, s))
        elif name == "T":
            out.append('<rect x="%.2f" y="%.2f" width="%.2f" height="%.2f" style="%s"/>' % (x, y, w, s, f))
            out.append('<rect x="%.2f" y="%.2f" width="%.2f" height="%.2f" style="%s"/>'
                       % (x + (w - s) / 2.0, y, s, cap, f))
        elif name == "E":
            out.append('<rect x="%.2f" y="%.2f" width="%.2f" height="%.2f" style="%s"/>' % (x, y, s, cap, f))
            out.append('<rect x="%.2f" y="%.2f" width="%.2f" height="%.2f" style="%s"/>' % (x, y, w, s, f))
            out.append('<rect x="%.2f" y="%.2f" width="%.2f" height="%.2f" style="%s"/>'
                       % (x, y + (cap - s) / 2.0, w * 0.92, s, f))
            out.append('<rect x="%.2f" y="%.2f" width="%.2f" height="%.2f" style="%s"/>'
                       % (x, y + cap - s, w, s, f))
        x += w + gap * cap
    return "".join(out)


def svg_header(w, h, vb=None):
    vw, vh = vb if vb else (w, h)
    return ('<?xml version="1.0" encoding="UTF-8"?>'
            '<svg xmlns="http://www.w3.org/2000/svg" width="%g" height="%g" viewBox="0 0 %g %g">'
            % (w, h, vw, vh))


def svg_icon(w, h, vb=None, plate=True, circle_plate=False):
    vw, vh = vb if vb else (w, h)
    n = min(vw, vh)
    body = []
    if plate:
        if circle_plate:
            body.append('<circle cx="%.2f" cy="%.2f" r="%.2f" style="fill:#000000;"/>'
                        % (vw / 2.0, vh / 2.0, n / 2.0))
        else:
            body.append('<rect width="%g" height="%g" rx="%.2f" ry="%.2f" style="fill:#000000;"/>'
                        % (vw, vh, 0.22 * n, 0.22 * n))
    size = 0.64 * n
    body.append(svg_mark((vw - size) / 2.0, (vh - size) / 2.0, size, "#FFFFFF", MARK["stroke"] * size))
    return svg_header(w, h, vb) + "".join(body) + "</svg>"


def svg_lockup(w, h, color, pad=0.10, rule=False, align_left=False):
    avail_w, avail_h = w * (1 - 2 * pad), h * (1 - 2 * pad)
    mark_h = min(avail_h, avail_w / lockup_ratio())
    total_w = mark_h * lockup_ratio()
    x = w * pad if align_left else (w - total_w) / 2.0
    y = (h - mark_h) / 2.0
    cap = LOCKUP_CAP * mark_h
    body = svg_mark(x, y, mark_h, color, MARK["stroke"] * mark_h)
    body += svg_word(x + mark_h * (1 + LOCKUP_GAP), y + (mark_h - cap) / 2.0, cap,
                     color, WORD["stroke"] * cap)
    if rule:
        t = max(2.0, 0.016 * h)
        body += ('<rect x="%.2f" y="%.2f" width="%.2f" height="%.2f" style="fill:%s;"/>'
                 % (0.036 * w, h - t, w - 2 * 0.036 * w, t, color))
    return svg_header(w, h) + body + "</svg>"


# --------------------------------------------------------------------------------------------
# Targets. Every raster target is written at the exact size of the file it replaces.
# --------------------------------------------------------------------------------------------

ICO_SIZES = {
    "OrcaSlicer.ico": [16, 24, 32, 48, 64, 128, 256],
    "OrcaSlicerTitle.ico": [154],
    "OrcaSlicer-mac_256px.ico": [256],
}

PNG_ICONS = (
    "OrcaSlicer.png", "OrcaSlicerTitle.png", "OrcaSlicer_32px.png", "OrcaSlicer_64.png",
    "OrcaSlicer_128px.png", "OrcaSlicer-mac_128px.png", "OrcaSlicer_154.png",
    "OrcaSlicer_154_title.png", "OrcaSlicer_192px.png", "OrcaSlicer_192px_grayscale.png",
)
PNG_TRANSPARENT = ("OrcaSlicer_192px_transparent.png",)


def existing_size(name):
    """The pixel size of the file we are replacing, so the replacement matches it exactly."""
    with Image.open(os.path.join(OUT_DIR, name)) as im:
        return im.size


def generate(out_dir):
    os.makedirs(out_dir, exist_ok=True)
    written = []

    def write_png(name, img):
        img.save(os.path.join(out_dir, name), "PNG")
        written.append("%s  %dx%d" % (name, img.size[0], img.size[1]))

    for name in PNG_ICONS:
        w, h = existing_size(name)
        img = render_icon(max(w, h))
        if (w, h) != img.size:
            img = img.resize((w, h), Image.LANCZOS)
        write_png(name, img)

    for name in PNG_TRANSPARENT:
        w, h = existing_size(name)
        # No plate: a white mark for dark surfaces would vanish on the light ConfigWizard page,
        # so this variant is the black mark on transparency.
        n = max(w, h) * SS
        img = Image.new("RGBA", (n, n), (0, 0, 0, 0))
        draw = ImageDraw.Draw(img)
        size = 0.92 * n
        off = (n - size) / 2.0
        draw_mark(draw, off, off, size, BLACK, MARK["stroke"] * size)
        write_png(name, img.resize((w, h), Image.LANCZOS))

    for name, sizes in ICO_SIZES.items():
        base = render_icon(max(sizes))
        path = os.path.join(out_dir, name)
        base.save(path, "ICO", sizes=[(s, s) for s in sizes])
        written.append("%s  %s" % (name, ",".join("%dx%d" % (s, s) for s in sizes)))

    # macOS bundle icon. Pillow can only write .icns on some platforms; skip cleanly elsewhere.
    try:
        icns = render_icon(1024)
        icns.save(os.path.join(out_dir, "OrcaSlicer.icns"), "ICNS")
        written.append("OrcaSlicer.icns  1024x1024")
    except Exception as error:                                     # pragma: no cover
        print("  skipped OrcaSlicer.icns (%s: %s)" % (type(error).__name__, error))

    svgs = {
        # square app icon (flatpak / hicolor scalable)
        "OrcaSlicer.svg": svg_icon(64, 64),
        # circular badge used by resources/web/homepage and scripts/msix
        "OrcaSlicer_gradient_circle.svg": svg_icon(1024, 1024, vb=(1280, 1280), circle_plate=True),
        # About dialog, loaded by name at 125 px (AboutDialog.cpp)
        "OrcaSlicer_about.svg": svg_lockup(560, 125, "#000000", pad=0.12, rule=True, align_left=True),
        "OrcaSlicer_about_dark.svg": svg_lockup(560, 125, "#FFFFFF", pad=0.12, rule=True, align_left=True),
        # splash screen (GUI_App.cpp SplashScreen)
        "splash_logo.svg": svg_lockup(480, 480, "#000000", pad=0.08),
        "splash_logo_dark.svg": svg_lockup(480, 480, "#FFFFFF", pad=0.08),
        # TroubleshootDialog header
        "OrcaSlicer_horizontal_light.svg": svg_lockup(214, 80, "#000000", pad=0.06, align_left=True),
        "OrcaSlicer_horizontal_dark.svg": svg_lockup(214, 80, "#FFFFFF", pad=0.06, align_left=True),
    }
    for name, text in svgs.items():
        with open(os.path.join(out_dir, name), "w", encoding="utf-8", newline="\n") as handle:
            handle.write(text)
        written.append("%s  (svg)" % name)

    for line in written:
        print("  wrote %s" % line)
    print("  %d file(s)" % len(written))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--fetch", action="store_true", help="re-download the brand originals")
    parser.add_argument("--measure", action="store_true", help="re-measure the reference original")
    parser.add_argument("--out", default=OUT_DIR, help="output directory (default: resources/images)")
    args = parser.parse_args(argv)

    fetch(force=args.fetch)
    if args.measure:
        measure()
    generate(args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
