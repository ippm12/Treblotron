"""Build the three model figures in docs/images/ from real data.

    python scripts/make_model_figures.py

Every thumbnail here is produced by running the shipped ONNX models over a real
labelled frame — nothing is mocked up. The point is that a reader can see what
is actually in each tensor, which is the part a box-and-arrow diagram cannot
convey: what "9 per-view instance masks" means is obvious the moment you see
three dart silhouettes in three colours.

Assets come from make_figure_assets.py. Figures are laid out as SVG for crisp
text, then rasterised through headless Chrome — a raster renders identically
everywhere and needs no opinion about how GitHub treats data: URIs inside an
SVG. Output is JPEG rather than PNG: these canvases are mostly photograph, so
PNG roughly doubles the file for no visible gain.
"""
from __future__ import annotations

import base64
import os
import math
import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

import cv2

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "docs" / "images"
ASSETS = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(
    os.environ.get("TREBLOTRON_FIGASSETS") or ROOT / "build" / "figassets")

BG, INK, MUTED, RULE = "#f6f8fa", "#1f2328", "#57606a", "#d0d7de"
SLOT = ["#5078ff", "#5cdc78", "#ff7850"]          # matches figassets SLOT_BGR
MONO = "ui-monospace, SFMono-Regular, Menlo, monospace"
SANS = "-apple-system, Segoe UI, Roboto, sans-serif"

# Solid palettes for slab(): (top face, front face, right face).
STEM   = ('#e0e7ff', '#c7d2fe', '#a5b4fc')
FUNNEL = ('#c7d2fe', '#a5b4fc', '#818cf8')
ENC    = ('#dcfce7', '#bbf7d0', '#86efac')
NECK   = ('#fee2e2', '#fecaca', '#fca5a5')
DEC    = ('#ede9fe', '#ddd6fe', '#c4b5fd')

# Rendered at RASTER_SCALE, then downsampled to OUTPUT_SCALE. Supersampling
# rather than rendering straight to the final size: it costs nothing at build
# time and keeps 9 px labels legible after the shrink.
#
# OUTPUT_SCALE 1.4 puts the widest figure near 2000 px, which is still better
# than 2× the ~890 px GitHub renders a README image at.
RASTER_SCALE = 2
OUTPUT_SCALE = 1.4
JPEG_QUALITY = 86

# Assets that are flat colour rather than photographs. PNG keeps their edges
# crisp and compresses them smaller than JPEG would.
FLAT_ASSETS = ("inst_", "tip_slot", "tipdetail_solid")

# Any Chromium will do; $TREBLOTRON_BROWSER wins, then whatever is on PATH, then
# the usual install locations.
CHROME = [p for p in (
    os.environ.get("TREBLOTRON_BROWSER"),
    shutil.which("chrome"), shutil.which("chromium"),
    shutil.which("google-chrome"), shutil.which("msedge"),
    r"C:\Program Files\Google\Chrome\Application\chrome.exe",
    r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe",
    "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
    "/usr/bin/chromium-browser",
) if p]


class Fig:
    def __init__(self, w: int, h: int):
        self.w, self.h, self.p = w, h, []

    def add(self, s): self.p.append(s)

    def text(self, x, y, s, size=12, fill=INK, anchor="middle", weight="normal",
             family=MONO):
        self.add(f'<text x="{x:.1f}" y="{y:.1f}" font-size="{size}" fill="{fill}" '
                 f'text-anchor="{anchor}" font-weight="{weight}" '
                 f'font-family="{family}">{esc(s)}</text>')

    def img(self, name, x, y, w, h, label=None, sublabel=None, border=INK):
        """Embed an asset, resampled and encoded for the size it is drawn at.

        A 720×720 source behind a 118 px panel is ~35× more pixels than any
        reader will see, and the three figures came to 1.1 MB on a page that
        loads every visit. Everything is resized to RASTER_SCALE× the drawn
        size, which is the resolution the PNG is rasterised at, so nothing
        visible is lost.

        Photographs go to JPEG; flat-colour masks stay PNG, where a palette
        beats JPEG on both size and edge quality.
        """
        path = ASSETS / f"{name}.png"
        if not path.exists():
            raise SystemExit(f"missing asset {path} — run make_figure_assets.py first")

        src = cv2.imread(str(path), cv2.IMREAD_COLOR)
        if src is None:
            raise SystemExit(f"could not read {path}")

        tw, th = int(round(w * RASTER_SCALE)), int(round(h * RASTER_SCALE))
        if src.shape[1] > tw or src.shape[0] > th:
            # INTER_AREA is the right filter for shrinking; anything else
            # aliases the wire grid in the board photos into moiré.
            src = cv2.resize(src, (tw, th), interpolation=cv2.INTER_AREA)

        flat = any(k in name for k in FLAT_ASSETS)
        if flat:
            ok, buf = cv2.imencode(".png", src)
            mime = "png"
        else:
            ok, buf = cv2.imencode(".jpg", src,
                                   [cv2.IMWRITE_JPEG_QUALITY, JPEG_QUALITY])
            mime = "jpeg"
        if not ok:
            raise SystemExit(f"could not encode {path}")

        b64 = base64.b64encode(buf.tobytes()).decode()
        self.add(f'<image x="{x:.1f}" y="{y:.1f}" width="{w:.1f}" height="{h:.1f}" '
                 f'preserveAspectRatio="none" href="data:image/{mime};base64,{b64}"/>')
        self.add(f'<rect x="{x:.1f}" y="{y:.1f}" width="{w:.1f}" height="{h:.1f}" '
                 f'fill="none" stroke="{border}" stroke-width="1.2"/>')
        if label:
            self.text(x + w / 2, y + h + 14, label, size=10)
        if sublabel:
            self.text(x + w / 2, y + h + 26, sublabel, size=9, fill=MUTED)
        return x + w, y + h

    def block(self, x, y, w, h, title, sub=None, fill="#ddd6fe"):
        self.add(f'<rect x="{x:.1f}" y="{y:.1f}" width="{w:.1f}" height="{h:.1f}" '
                 f'rx="6" fill="{fill}" stroke="{INK}" stroke-width="1.2"/>')
        self.text(x + w / 2, y + h / 2 + (0 if sub is None else -4), title,
                  size=11, weight="600")
        if sub:
            self.text(x + w / 2, y + h / 2 + 12, sub, size=9, fill=MUTED)
        return x + w, y + h

    def arrow(self, x1, y1, x2, y2, colour=MUTED, width=1.8, dashed=False):
        d = ' stroke-dasharray="4 3"' if dashed else ""
        self.add(f'<path d="M {x1:.1f} {y1:.1f} L {x2:.1f} {y2:.1f}" stroke="{colour}" '
                 f'stroke-width="{width}" fill="none"{d} marker-end="url(#ar)"/>')

    def curve(self, x1, y1, x2, y2, lift, colour="#8250df", dashed=True):
        mid = (x1 + x2) / 2
        d = ' stroke-dasharray="5 4"' if dashed else ""
        self.add(f'<path d="M {x1:.1f} {y1:.1f} C {mid:.1f} {y1-lift:.1f} '
                 f'{mid:.1f} {y2-lift:.1f} {x2:.1f} {y2:.1f}" stroke="{colour}" '
                 f'stroke-width="1.6" fill="none"{d} marker-end="url(#as)"/>')

    def slab(self, x, cy, channels, spatial, fill, label=None, sub=None):
        """A tensor drawn as a solid, sized to what it holds.

        Height is the spatial extent, width is the channel count. Both are
        compressed — square-root and log respectively — because 720 against 23,
        and 960 against 16, are ratios no page can show literally without the
        small end vanishing. Relative order is preserved, magnitude is not.

        Returns (left, right, top, bottom, cx).
        """
        h = 20 + 90 * math.sqrt(spatial / 720)
        w = 8 + 46 * math.log10(1 + channels) / math.log10(961)
        y, d = cy - h / 2, 12
        light, mid, dark = fill
        self.add(f'<polygon points="{x:.1f},{y:.1f} {x+d:.1f},{y-d:.1f} '
                 f'{x+w+d:.1f},{y-d:.1f} {x+w:.1f},{y:.1f}" fill="{light}" '
                 f'stroke="{INK}" stroke-width="1"/>')
        self.add(f'<polygon points="{x+w:.1f},{y:.1f} {x+w+d:.1f},{y-d:.1f} '
                 f'{x+w+d:.1f},{y+h-d:.1f} {x+w:.1f},{y+h:.1f}" fill="{dark}" '
                 f'stroke="{INK}" stroke-width="1"/>')
        self.add(f'<rect x="{x:.1f}" y="{y:.1f}" width="{w:.1f}" height="{h:.1f}" '
                 f'fill="{mid}" stroke="{INK}" stroke-width="1"/>')
        cx = x + (w + d) / 2
        if label:
            self.text(cx, y + h + 15, label, size=10, weight="600")
        if sub:
            self.text(cx, y + h + 27, sub, size=9, fill=MUTED)
        return x, x + w + d, y - d, y + h, cx

    def elbow(self, pts, colour=MUTED, width=1.8, dashed=False):
        """Orthogonal connector through a list of points.

        Long diagonals across a busy figure are unreadable; right angles let a
        line cross the page without anyone having to trace where it started.
        """
        d = ' stroke-dasharray="5 4"' if dashed else ""
        path = " L ".join(f"{x:.1f} {y:.1f}" for x, y in pts)
        self.add(f'<path d="M {path}" stroke="{colour}" stroke-width="{width}" '
                 f'fill="none"{d} marker-end="url(#ar)"/>')

    def brace(self, x, y0, y1, colour=MUTED):
        self.add(f'<path d="M {x+6:.1f} {y0:.1f} q -6 0 -6 6 L {x:.1f} {(y0+y1)/2-6:.1f} '
                 f'q 0 6 -6 6 q 6 0 6 6 L {x:.1f} {y1-6:.1f} q 0 6 6 6" '
                 f'stroke="{colour}" stroke-width="1.2" fill="none"/>')

    def render(self):
        return (
            f'<svg xmlns="http://www.w3.org/2000/svg" '
            f'xmlns:xlink="http://www.w3.org/1999/xlink" '
            f'viewBox="0 0 {self.w} {self.h}" width="{self.w}" height="{self.h}">\n'
            f'<defs>'
            f'<marker id="ar" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="6" '
            f'markerHeight="6" orient="auto"><path d="M0,0 L10,5 L0,10 z" fill="{MUTED}"/></marker>'
            f'<marker id="as" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="6" '
            f'markerHeight="6" orient="auto"><path d="M0,0 L10,5 L0,10 z" fill="#8250df"/></marker>'
            f'</defs>\n<rect width="{self.w}" height="{self.h}" rx="10" fill="{BG}"/>\n'
            + "\n".join(self.p) + "\n</svg>\n")

    def header(self, title, subtitle, right=None, right2=None):
        self.text(28, 32, title, size=16, anchor="start", weight="700")
        self.text(28, 52, subtitle, size=11.5, anchor="start", fill=MUTED, family=SANS)
        if right:
            self.text(self.w - 28, 32, right, size=11, anchor="end", fill=MUTED)
        if right2:
            self.text(self.w - 28, 50, right2, size=10, anchor="end", fill=MUTED)
        self.add(f'<line x1="28" y1="66" x2="{self.w-28}" y2="66" '
                 f'stroke="{RULE}" stroke-width="1"/>')


def esc(s):
    return (str(s).replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;"))


# ============================================================================
# The figures
# ============================================================================
#
# Rule for what goes in an image: a label anchored to a thing stays, a sentence
# does not. Panel captions, tensor shapes and readouts only mean something next
# to what they point at; explanation reflows and belongs in the README.
#
# That is not only tidiness. A figure 1400 px wide with 10 px prose renders at
# about 350 px on a phone -- a quarter scale, which turns the text into 2.5 px
# of grey. Markdown reflows; baked text cannot. Dropping the prose also lets
# several of these lose a third of their width, so what remains renders larger.
#
# Some figures are split for the same reason: two arguments in one image force
# the README to say both before showing either.


# ------------------------------------------------------- step 1: the segmenter
def fig_segmentation() -> Fig:
    f = Fig(1180, 268)
    f.header("Step 1 of 2 - the segmenter",
             "dart_seg_unet finds dart pixels, so the detector never sees the board itself",
             "MobileNetV3 encoder + U-Net decoder", "3 × 360 × 640  →  3 × 1 × 360 × 640")

    y, iw, ih = 104, 208, 117           # 16:9 camera frames
    f.img("raw_cam0", 40, y, iw, ih, "camera frame", "1280 × 720")
    f.block(276, y + 28, 104, 60, "seg U-Net", "batch of 3", "#bbf7d0")
    f.arrow(252, y + 58, 272, y + 58)
    f.arrow(384, y + 58, 404, y + 58)
    f.img("overlay_cam0", 408, y, iw, ih, "mask, over the frame", "sigmoid > 0.5")
    f.arrow(620, y + 58, 640, y + 58)
    f.img("maskedrgb_cam0", 644, y, iw, ih, "RGB × mask", "board removed")
    f.arrow(856, y + 58, 876, y + 58)
    f.img("warped_masked_cam0", 880, y, ih, ih, "warped to the board",
          "720 × 720 canonical")
    return f


# ------------------------------------------------- step 1 detail: the tip flare
def fig_tipflare() -> Fig:
    f = Fig(700, 268)
    f.header("The mask is wider than the steel",
             "one tip at 5× zoom, and a second dart where there is no tip to see",
             "training labels", "round cap, not a point")

    y, sz, gap = 100, 138, 12
    f.img("tipdetail_raw", 40, y, sz, sz, "the tip, 5× zoom", "visible, just")
    f.img("tipdetail_mask", 40 + sz + gap, y, sz, sz, "what the model marks",
          "outline + apex")
    f.img("tipdetail_solid", 40 + 2 * (sz + gap), y, sz, sz, "the mask alone",
          "a round cap")

    hx = 40 + 3 * (sz + gap) + 20       # set apart: different dart, different frame
    f.img("tipdetail_hard", hx, y, sz, sz, "another dart", "no visible tip at all")
    f.add('<line x1="%d" y1="%d" x2="%d" y2="%d" stroke="%s" stroke-width="1" '
          'stroke-dasharray="3 3"/>' % (hx - 10, y, hx - 10, y + sz, RULE))
    return f


# ------------------------------------------------------ why three cameras
def fig_triangulation() -> Fig:
    t = json.loads((ASSETS / "triangulation.json").read_text())["tri"]
    f = Fig(1060, 330)
    f.header("Why three cameras",
             "the tip is the only part of a dart touching the board, so it is the "
             "only point all three views agree on",
             "geometry only", "no model involved")

    # Panels are spaced to leave room for the operators between them; at a
    # tighter pitch the + and = land on top of the images and vanish into the
    # dark board behind them.
    y, sz, pitch = 104, 140, 168
    for c in range(3):
        f.img("tri_cam%d" % c, 40 + c * pitch, y, sz, sz, "cam%d" % c,
              "0.0 contrast at the tip" if c == t["blind_cam"] else "tip visible")
    for x in (194, 362):
        f.text(x, y + sz / 2 + 6, "+", size=19, fill=MUTED)
    f.text(530, y + sz / 2 + 6, "=", size=19, fill=MUTED)

    f.img("tri_overlay", 546, y, sz, sz, "all three, added", "white = all agree")
    f.img("tri_zoom", 702, y, sz, sz, "6× zoom", "one patch, on the tip")

    f.add('<line x1="856" y1="%d" x2="856" y2="%d" stroke="%s" stroke-width="1" '
          'stroke-dasharray="3 3"/>' % (y, y + sz, RULE))
    f.img("tri_blind", 872, y, sz, sz, "cam%d, 5× zoom" % t["blind_cam"],
          "nothing to see")
    return f


# ------------------------------------------------- and darts hide each other
def fig_occlusion() -> Fig:
    o = json.loads((ASSETS / "triangulation.json").read_text())["occ"]
    # No right-hand badges: the title alone is already wide for this canvas, and
    # the colour key reads better under the panels than opposite them.
    f = Fig(620, 356)
    f.header("A later dart hides behind an earlier one",
             "the same dart, the same instant, two cameras")

    y, sz = 104, 180
    f.img("occl_cut", 60, y, sz, sz, "cam%d" % o["cut_cam"],
          "cut into %d pieces" % o["cut_parts"])
    f.img("occl_whole", 60 + sz + 40, y, sz, sz, "cam%d" % o["whole_cam"],
          "one piece")
    f.text(f.w / 2, 336,
           "yellow: thrown third   ·   magenta: already in the board   ·   tints from the stored labels",
           size=9, fill=MUTED, family=SANS)
    return f


# ---------------------------------------------------------- step 2: detector
def fig_detector() -> Fig:
    f = Fig(1120, 800)
    f.header("Step 2 of 2 - the detector",
             "multicam_unet_ar · three views stay separate until the funnel, and the "
             "conditioning enters twice",
             "3.9M parameters", "21 × 720 × 720  →  360 × 360 + 1")

    t, gapx = 64, 6
    x_rgb, x_inst = 44, 132
    y0, dy = 108, 96

    f.text(x_rgb, 96, "per camera", size=10, anchor="start", fill=MUTED, family=SANS)
    f.text(x_inst, 96, "its own 3 instance slots - here 2 are in use", size=10,
           anchor="start", fill=MUTED, family=SANS)

    stream_y = []
    for c in range(3):
        y = y0 + c * dy
        stream_y.append(y + t / 2)
        f.img("stage2_rgb_cam%d" % c, x_rgb, y, t, t)
        f.text(x_rgb + t / 2, y + t + 11, "cam%d" % c, size=9)
        for k in range(3):
            f.img("inst_cam%d_slot%d" % (c, k), x_inst + k * (t + gapx), y, t, t,
                  border=SLOT[k])

    yt = y0 + 3 * dy + 14
    for k in range(3):
        f.img("tip_slot%d" % k, x_inst + k * (t + gapx), yt, t, t, border=SLOT[k])
    f.text(x_rgb, yt + t / 2 - 4, "the same 3,", size=9.5, anchor="start", fill=MUTED)
    f.text(x_rgb, yt + t / 2 + 9, "copied into", size=9.5, anchor="start", fill=MUTED)
    f.text(x_rgb, yt + t / 2 + 22, "every camera", size=9.5, anchor="start", fill=MUTED)
    f.text(x_inst + 1.5 * (t + gapx) - 3, yt + t + 13,
           "tip maps, σ=5 - enlarged to be visible", size=9, fill=MUTED)

    bx = x_inst + 3 * (t + gapx) + 4
    f.brace(bx, y0 - 4, yt + t + 4)
    f.text(bx + 30, (y0 + yt + t) / 2 - 14, "into each stem:", size=10,
           anchor="start", weight="600")
    f.text(bx + 30, (y0 + yt + t) / 2, "3 RGB + 3 masks", size=9.5,
           anchor="start", fill=MUTED)
    f.text(bx + 30, (y0 + yt + t) / 2 + 13, "+ 3 tips = 9 ch", size=9.5,
           anchor="start", fill=MUTED)

    xs0, xs1 = 512, 576
    for c in range(3):
        y = stream_y[c]
        f.arrow(bx + 112, y, xs0 - 8, y)
        last = c == 2
        f.slab(xs0, y, 16, 360, STEM, "stage0" if last else None,
               "16 @ 360²" if last else None)
        f.arrow(xs0 + 40, y, xs1 - 8, y)
        f.slab(xs1, y, 24, 180, STEM, "stage1" if last else None,
               "24 @ 180²" if last else None)
    f.text((xs0 + xs1) / 2 + 16, stream_y[2] + 96, "shared weights, frozen",
           size=10, fill="#8250df")

    xf0, xf1 = 660, 728
    for c in range(3):
        f.arrow(xs1 + 42, stream_y[c], xf0 - 8, stream_y[1] - 16 + c * 16)
    f.slab(xf0, stream_y[1], 72, 180, FUNNEL, "conv 3×3", "72 @ 180²")
    f.arrow(xf0 + 50, stream_y[1], xf1 - 8, stream_y[1])
    fg = f.slab(xf1, stream_y[1], 72, 180, FUNNEL, "conv 1×1", "72 @ 180²")
    f.text((xf0 + xf1) / 2 + 18, stream_y[1] - 52, "the funnel — 72 wide throughout",
           size=10.5, weight="600")

    # ---------------- second row: the U-Net ---------------------------
    enc = [("stage2", 40, 90, 580), ("stage3", 112, 45, 606),
           ("stage4", 960, 23, 622), ("bottleneck", 64, 23, 628)]
    dec = [("up4", 128, 45, 606), ("up3", 64, 90, 580),
           ("up2", 48, 180, 552), ("up1", 32, 360, 538)]
    ex0, step = 210, 95

    # The riser stays on the funnel's centre line -- that column is clear of up2
    # at 780 -- but starts below the slab's label and shape rather than at its
    # bottom edge, because both are centred on the same x and a riser from the
    # edge draws straight through them. The gap it leaves is where the label
    # sits, so the line still reads as coming out of the cube.
    #
    # The horizontal run at 512 clears the tip-row caption above it (487) and
    # every slab below it -- up3's top face is 554.
    f.elbow([(fg[4], 306), (fg[4], 512),
             (ex0 + 20, 512), (ex0 + 20, 580 - 30)])

    enc_pts, prev = [], None
    for i, (name, ch, sp, cy) in enumerate(enc):
        x = ex0 + i * step
        g = f.slab(x, cy, ch, sp, NECK if i == 3 else ENC, name,
                   "%d @ %d²" % (ch, sp))
        enc_pts.append(g)
        if prev:
            f.arrow(prev[1] + 2, prev[3] - 8, x - 6, cy)
        prev = g

    dec_pts = []
    for i, (name, ch, sp, cy) in enumerate(dec):
        x = ex0 + (i + 4) * step
        g = f.slab(x, cy, ch, sp, DEC, name, "%d @ %d²" % (ch, sp))
        dec_pts.append(g)
        f.arrow(prev[1] + 2, prev[3] - 8, x - 6, cy)
        prev = g

    for ei, di, lift in ((1, 0, 30), (0, 1, 52)):
        f.curve(enc_pts[ei][4], enc_pts[ei][2] - 4,
                dec_pts[di][4], dec_pts[di][2] - 4, lift)
    # At the curves' apex. Higher and it reads as a label for the funnel
    # elbow that runs across at 512.
    f.text((enc_pts[0][4] + dec_pts[1][4]) / 2, 534, "skip connections",
           size=10, fill="#8250df")

    condx, condy, condw = 96, 686, 176
    f.add('<rect x="%d" y="%d" width="%d" height="46" rx="6" fill="#fde68a" '
          'stroke="%s" stroke-width="1.2"/>' % (condx, condy, condw, INK))
    f.text(condx + condw / 2, condy + 19, "the same 12 channels", size=10,
           weight="600")
    f.text(condx + condw / 2, condy + 33, "resampled to each skip", size=9,
           fill=MUTED)

    f.elbow([(condx + condw / 2, yt + t + 26), (condx + condw / 2, condy - 6)],
            colour="#8250df", dashed=True)
    for i in (2, 3):
        f.elbow([(condx + condw, condy + 23), (dec_pts[i][4], condy + 23),
                 (dec_pts[i][4], dec_pts[i][3] + 34)],
                colour="#8250df", dashed=True)

    hx = ex0 + 8 * step + 4
    f.img("seq_heat2", hx, 492, 96, 96, "heatmap", "360 × 360")
    f.arrow(prev[1] + 2, prev[3] - 8, hx - 6, 540)
    f.text(hx + 48, 626, "exist = max(heatmap)", size=9.5, fill=MUTED)
    f.text(hx + 48, 638, "× scale + bias", size=9.5, fill=MUTED)

    f.text(f.w / 2, 782,
           "box height is the spatial size, box width the channel count — both compressed, "
           "so the order is right and the magnitude is not",
           size=9.5, fill=MUTED, family=SANS)
    return f


# -------------------------------------------------------- the loop, per throw
def fig_autoregressive() -> Fig:
    doc = json.loads((ASSETS / "passes.json").read_text())
    passes = doc["passes"]
    f = Fig(1080, 856)
    f.header("One pass per throw",
             "darts arrive one at a time, so each frame is asked for one dart, "
             "against the ones already counted",
             "one real turn", "no per-dart label anywhere")

    t, x0, step = 170, 44, 274
    ROWA, ROWB, ROWC = 116, 342, 568
    heads = ["throw 1", "throw 2", "throw 3", "next cycle"]
    subs = ["1 dart on the board", "2 darts on the board", "3 darts on the board",
            "same frame, nothing new"]
    condsub = ["nothing counted yet", "dart 1", "darts 1 and 2", "darts 1, 2 and 3"]

    for i, p in enumerate(passes):
        x = x0 + i * step
        stop = p["stop"]
        f.text(x + t / 2, 104, heads[i], size=12, weight="700")

        f.img("seq_frame%d" % i, x, ROWA, t, t, "what the cameras see", subs[i])
        f.arrow(x + t / 2, ROWA + t + 32, x + t / 2, ROWB - 6)

        if i == 0:
            f.add('<rect x="%d" y="%d" width="%d" height="%d" rx="4" '
                  'fill="#eef1f4" stroke="%s" stroke-width="1.2" '
                  'stroke-dasharray="5 4"/>' % (x, ROWB, t, t, RULE))
            f.text(x + t / 2, ROWB + t / 2, "empty", size=11, fill=MUTED)
        else:
            f.img("seq_cond%d" % i, x, ROWB, t, t)
        f.text(x + t / 2, ROWB + t + 14, "conditioning in", size=10)
        f.text(x + t / 2, ROWB + t + 26, condsub[i], size=9, fill=MUTED)
        f.arrow(x + t / 2, ROWB + t + 34, x + t / 2, ROWC - 6)

        f.img("seq_heat%d" % i, x, ROWC, t, t, "heatmap out",
              "nothing above threshold" if stop else "one peak, one dart")

        vy = ROWC + t + 34
        colour = "#cf222e" if stop else "#1a7f37"
        f.text(x + t / 2, vy + 14, "exist %+.2f" % p["exist"], size=11,
               weight="700", fill=colour)
        f.text(x + t / 2, vy + 30,
               "peak %.3f" % p["peak"] if stop
               else "peak %.3f  -  %.1f px" % (p["peak"], p["err"]),
               size=10, fill=MUTED)
        f.text(x + t / 2, vy + 48,
               "nothing new, turn over" if stop else "accept, and feed it back",
               size=10, fill=colour, family=SANS)
    return f


# ------------------------------------------- and why replay is a different job
def fig_frozenframe() -> Fig:
    nosep = json.loads((ASSETS / "passes.json").read_text())["nosep"]
    f = Fig(620, 300)
    f.header("A frozen frame has no \u201csince\u201d",
             "hand the first dart the whole mask and it claims all three",
             "replay only", "not the live path")

    y, sz = 104, 150
    f.img("nosep_cond", 40, y, sz, sz, "one claim", "all three darts")
    f.arrow(40 + sz + 10, y + sz / 2, 40 + sz + 40, y + sz / 2)
    f.img("nosep_heat", 40 + sz + 50, y, sz, sz, "heatmap out", "nothing left")

    vx = 40 + 2 * sz + 68
    f.text(vx, y + sz / 2 - 6, "exist %+.2f" % nosep["exist"], size=11,
           anchor="start", weight="700", fill="#cf222e")
    f.text(vx, y + sz / 2 + 10, "peak %.3f" % nosep["peak"], size=10,
           anchor="start", fill=MUTED)
    f.text(vx, y + sz / 2 + 28, "stops after", size=10,
           anchor="start", fill="#cf222e", family=SANS)
    f.text(vx, y + sz / 2 + 41, "one dart", size=10,
           anchor="start", fill="#cf222e", family=SANS)
    return f


def rasterise(svg_path: Path, out_path: Path, width: int, height: int):
    """Render the SVG through a browser, then shrink and encode it.

    Chrome only screenshots to PNG, and a lossless PNG of a photo-bearing
    canvas is the wrong format — the three figures came to 1.1 MB that way, on
    a page that loads on every visit. Shrinking the supersampled render and
    re-encoding as JPEG roughly halves it with no visible cost.
    """
    browser = next((b for b in CHROME if Path(b).exists()), None)
    if not browser:
        print(f"  no Chrome/Edge found — leaving {svg_path.name} as SVG")
        return False

    with tempfile.TemporaryDirectory() as td:
        html = Path(td) / "f.html"
        raw = Path(td) / "raw.png"
        html.write_text(
            f'<html><body style="margin:0">'
            f'<img src="{svg_path.as_uri()}" width="{width}"></body></html>',
            encoding="utf-8")
        subprocess.run(
            [browser, "--headless=new", "--disable-gpu", "--no-sandbox",
             f"--force-device-scale-factor={RASTER_SCALE}",
             f"--screenshot={raw}",
             f"--window-size={width},{height}", html.as_uri()],
            capture_output=True, timeout=180)
        if not raw.exists():
            return False

        im = cv2.imread(str(raw), cv2.IMREAD_COLOR)

    shrink = OUTPUT_SCALE / RASTER_SCALE
    if shrink < 1.0:
        im = cv2.resize(im, (int(round(im.shape[1] * shrink)),
                             int(round(im.shape[0] * shrink))),
                        interpolation=cv2.INTER_AREA)
    return cv2.imwrite(str(out_path), im,
                       [cv2.IMWRITE_JPEG_QUALITY, JPEG_QUALITY])


if __name__ == "__main__":
    OUT.mkdir(parents=True, exist_ok=True)
    for name, fig in [("pipeline-segmentation", fig_segmentation()),
                      ("tip-flare", fig_tipflare()),
                      ("why-three-cameras", fig_triangulation()),
                      ("occlusion", fig_occlusion()),
                      ("architecture-detector", fig_detector()),
                      ("autoregressive-loop", fig_autoregressive()),
                      ("frozen-frame", fig_frozenframe())]:
        svg = OUT / f"{name}.svg"
        svg.write_text(fig.render(), encoding="utf-8")
        jpg = OUT / f"{name}.jpg"
        if rasterise(svg, jpg, fig.w, fig.h):
            svg.unlink()      # the raster is what ships; the SVG was scaffolding
            print(f"{name}.jpg  {jpg.stat().st_size / 1024:,.0f} KB")
        else:
            print(f"{name}.svg  {svg.stat().st_size / 1024:,.0f} KB")
