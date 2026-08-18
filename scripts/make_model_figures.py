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
    os.environ.get("DARTMATIC_FIGASSETS") or ROOT / "build" / "figassets")

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

# Any Chromium will do; $DARTMATIC_BROWSER wins, then whatever is on PATH, then
# the usual install locations.
CHROME = [p for p in (
    os.environ.get("DARTMATIC_BROWSER"),
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


# ============================================================ figure 1: seg
def fig_segmentation() -> Fig:
    f = Fig(1180, 638)
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

    # ---- left: why the board goes ------------------------------------
    f.text(40, 292, "The board is thrown away on purpose.", size=11,
           anchor="start", weight="600", family=SANS)
    for i, line in enumerate([
        "Wires, numbers and lighting differ between rigs and change",
        "nothing about where a dart is. Masking them out leaves a",
        "problem that looks the same in any room.",
        "",
        "The warp does the other half. It folds in the board's own",
        "rotation, so 20 sits at 12 o'clock in all three views and every",
        "camera agrees where a canonical point is — which is what lets",
        "one shared tip channel mean anything across three cameras.",
    ]):
        f.text(40, 312 + i * 16, line, size=10.5, anchor="start", fill=MUTED,
               family=SANS)

    # ---- right: the tip, at the scale the argument happens ------------
    #
    # Three panels of one tip you can see, so the flare is measurable against
    # something; then a fourth, different dart where the tip is not visible at
    # all — which is the case the flare exists for.
    dx, dw, dgap = 528, 138, 12
    f.text(dx, 268, "The mask is wider than the steel, on purpose.", size=11,
           anchor="start", weight="600", family=SANS)
    f.img("tipdetail_raw", dx, 282, dw, dw, "the tip, 5× zoom", "visible, just")
    f.img("tipdetail_mask", dx + dw + dgap, 282, dw, dw, "what the model marks",
          "outline + apex")
    f.img("tipdetail_solid", dx + 2 * (dw + dgap), 282, dw, dw, "the mask alone",
          "a round cap, not a point")

    hx = dx + 3 * (dw + dgap) + 22          # set apart: different dart, different frame
    f.img("tipdetail_hard", hx, 282, dw, dw, "another dart, into black",
          "no visible tip at all")
    f.add('<line x1="%d" y1="282" x2="%d" y2="%d" stroke="%s" stroke-width="1" '
          'stroke-dasharray="3 3"/>' % (hx - 11, hx - 11, 282 + dw, RULE))

    for i, line in enumerate([
        "A dart's point is polished steel and about a tenth of its length — 2 to 4 pixels here, and often",
        "indistinguishable from what is behind it. In the first crop it fades into a cream segment several",
        "pixels before it actually ends. In the last, thrown into a black bed and further from the camera,",
        "there is nothing to see at all, and the mask is the only thing that knows where the dart stopped.",
    ]):
        f.text(dx, 472 + i * 16, line, size=10.5, anchor="start", fill=MUTED,
               family=SANS)

    # ---- bottom: why a flare and not a silhouette ---------------------
    f.add('<line x1="40" y1="558" x2="%d" y2="558" stroke="%s" stroke-width="1"/>'
          % (f.w - 40, RULE))
    f.text(40, 580,
           "So the training masks flare: a round-capped, tapered cap unioned over the point, rather than the exact silhouette. The size was fitted to "
           "hand-labelled",
           size=10.5, anchor="start", fill=MUTED, family=SANS)
    f.text(40, 596,
           "data rather than chosen by eye — measured 2 px from the apex, real labels run 9.0 px half-width and a pristine silhouette only 2.1, so without "
           "the flare",
           size=10.5, anchor="start", fill=MUTED, family=SANS)
    f.text(40, 612,
           "synthetic and real training data disagreed by about 4× at exactly the place the tip decision depends on.",
           size=10.5, anchor="start", fill=MUTED, family=SANS)
    return f


# ======================================================= figure 2: detector
def fig_detector() -> Fig:
    f = Fig(1420, 800)
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

    # One brace over all four groups, because all four go into every stem --
    # which is the thing the figure previously left ambiguous.
    bx = x_inst + 3 * (t + gapx) + 4
    f.brace(bx, y0 - 4, yt + t + 4)
    f.text(bx + 34, (y0 + yt + t) / 2 - 14, "into each stem:", size=10,
           anchor="start", weight="600")
    f.text(bx + 34, (y0 + yt + t) / 2, "3 RGB + 3 masks", size=9.5,
           anchor="start", fill=MUTED)
    f.text(bx + 34, (y0 + yt + t) / 2 + 13, "+ 3 tips = 9 ch", size=9.5,
           anchor="start", fill=MUTED)

    # --- per-camera stems: stage0 then stage1, both frozen -------------
    xs0, xs1 = 520, 584
    for c in range(3):
        y = stream_y[c]
        f.arrow(bx + 118, y, xs0 - 8, y)
        last = c == 2
        f.slab(xs0, y, 16, 360, STEM, "stage0" if last else None,
               "16 @ 360²" if last else None)
        f.arrow(xs0 + 40, y, xs1 - 8, y)
        f.slab(xs1, y, 24, 180, STEM, "stage1" if last else None,
               "24 @ 180²" if last else None)
    f.text((xs0 + xs1) / 2 + 16, stream_y[2] + 90, "shared weights, frozen",
           size=10, fill="#8250df")

    # --- funnel: two convs, and it does not narrow --------------------
    xf0, xf1 = 668, 736
    for c in range(3):
        f.arrow(xs1 + 42, stream_y[c], xf0 - 8, stream_y[1] - 16 + c * 16)
    f.slab(xf0, stream_y[1], 72, 180, FUNNEL, "conv 3×3", "72 @ 180²")
    f.arrow(xf0 + 50, stream_y[1], xf1 - 8, stream_y[1])
    fg = f.slab(xf1, stream_y[1], 72, 180, FUNNEL, "conv 1×1", "72 @ 180²")
    f.text((xf0 + xf1) / 2 + 18, stream_y[1] - 52, "the funnel", size=10.5,
           weight="600")

    # --- why, in the space to the right -------------------------------
    tx = 810
    f.text(tx, 128, "Three views, kept apart on purpose", size=11.5,
           anchor="start", weight="600", family=SANS)
    for i, line in enumerate([
        "Each camera runs through the same frozen, ImageNet-pretrained stem,",
        "and only then are the three merged. Merging any earlier throws away",
        "the parallax that lets three views agree on one point; merging later",
        "costs depth everything downstream needs.",
        "",
        "The funnel is where they meet. Each stem ends at 24 channels, so the",
        "three concatenate to 72 at 180 × 180, and a 3×3 conv then a 1×1 mix",
        "them across views — keeping all 72, so what reaches the encoder",
        "carries three views' worth of evidence rather than one.",
    ]):
        f.text(tx, 152 + i * 16, line, size=10.5, anchor="start", fill=MUTED,
               family=SANS)

    f.text(tx, 322, "Each camera's masks stay with its own RGB", size=11.5,
           anchor="start", weight="600", family=SANS)
    for i, line in enumerate([
        "A mask has to line up with the image the stem is looking at, so the",
        "instance masks are per view. The tip maps are shared — a tip lies on the",
        "board plane, so it warps to the same canonical point in every camera —",
        "and all three are copied onto each camera's own six, giving the 9",
        "channels stage0 actually receives.",
        "",
        "Nothing hands these over at runtime. The segmenter emits one blob of",
        "every dart present, so a dart's own channel is only ever the difference",
        "between successive masks — which is what the loop, below, is about.",
    ]):
        f.text(tx, 346 + i * 16, line, size=10.5, anchor="start", fill=MUTED,
               family=SANS)

    # ---------------- second row: the U-Net ---------------------------
    # Centres trace the U; slab heights and widths carry the shapes.
    enc = [("stage2", 40, 90, 580), ("stage3", 112, 45, 606),
           ("stage4", 960, 23, 622), ("bottleneck", 64, 23, 628)]
    dec = [("up4", 128, 45, 606), ("up3", 64, 90, 580),
           ("up2", 48, 180, 552), ("up1", 32, 360, 538)]
    ex0, step = 178, 106

    f.elbow([(fg[4], stream_y[1] + 34), (fg[4], 496),
             (ex0 + 20, 496), (ex0 + 20, 580 - 30)])

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
    # Anchored to the apex of the shallower skip rather than floating above
    # the row, where it read as a heading for everything below it.
    sk_x = (enc_pts[1][4] + dec_pts[0][4]) / 2
    sk_y = enc_pts[1][2] - 30
    f.add('<path d="M %.1f %.1f L %.1f %.1f" stroke="#8250df" stroke-width="1" '
          'fill="none"/>' % (sk_x, sk_y + 6, sk_x, sk_y + 22))
    f.text(sk_x, sk_y, "skip connections", size=10, fill="#8250df")

    # --- conditioning enters a second time -----------------------------
    condx, condy, condw = 40, 686, 176
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
    f.text(condx + condw / 2, condy + 66, "concatenated onto up2 and up1, so the",
           size=9.5, fill="#8250df", family=SANS)
    f.text(condx + condw / 2, condy + 79, "decoder can see which pixels belong",
           size=9.5, fill="#8250df", family=SANS)
    f.text(condx + condw / 2, condy + 92, "to darts it has already found",
           size=9.5, fill="#8250df", family=SANS)

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


# ======================================= figure 2b: why three cameras
def fig_triangulation() -> Fig:
    doc = json.loads((ASSETS / "triangulation.json").read_text())
    t, o = doc["tri"], doc["occ"]
    f = Fig(1180, 742)
    f.header("Why three cameras",
             "a dart's tip is the only part of it touching the board, so it is "
             "the only point all three views agree on",
             "geometry only", "no model involved")

    y, sz = 112, 150
    for c in range(3):
        f.img("tri_cam%d" % c, 44 + c * 170, y, sz, sz, "cam%d" % c,
              "tip invisible here" if c == t["blind_cam"] else "tip in plain view")
    for x in (200, 370):
        f.text(x + 12, y + sz / 2 + 5, "+", size=20, fill=MUTED)
    f.text(556, y + sz / 2 + 5, "=", size=20, fill=MUTED)

    f.img("tri_overlay", 578, y, sz, sz, "all three, added",
          "white = all three agree")
    f.img("tri_zoom", 748, y, sz, sz, "that agreement, 6× zoom",
          "one patch, on the tip")

    f.text(922, y + 24, "Warped, each view puts", size=10.5, anchor="start",
           weight="600", family=SANS)
    for i, line in enumerate([
        "the dart somewhere else —",
        "except at the tip, which is",
        "on the plane the warp was",
        "solved for.",
        "",
        "Blue + green + red make",
        "white only where all three",
        "cover the same pixel. Those",
        "patches are the tips.",
    ]):
        f.text(922, y + 44 + i * 15, line, size=10, anchor="start", fill=MUTED,
               family=SANS)

    # ---- the blind view -----------------------------------------------
    by = 318
    f.img("tri_blind", 44, by, 128, 128, "cam%d, 5× zoom" % t["blind_cam"],
          "nothing to see")
    f.text(204, by + 12, "No view has to show the steel.",
           size=11.5, anchor="start", weight="600", family=SANS)
    for i, line in enumerate([
        "The tip is never read off an image. Each warped silhouette is a ray that starts at the tip, and because the body stands off the",
        "board, every camera throws that ray in a different direction. Three rays, one shared origin — so where they cross is the tip,",
        "whether or not any camera could resolve the point itself. The steel on this dart is invisible in cam%d, and it holds even in" % t["blind_cam"],
        "frames where none of the three can see it.",
        "",
        "It is the flare from step 1 that makes this safe: a mask stopping short of the tip would start its ray late, and drag the crossing",
        "with it.",
    ]):
        f.text(204, by + 34 + i * 16, line, size=10.5, anchor="start", fill=MUTED,
               family=SANS)

    # ---- occlusion ----------------------------------------------------
    oy, osz = 500, 150
    f.add('<line x1="28" y1="474" x2="%d" y2="474" stroke="%s" stroke-width="1"/>'
          % (f.w - 28, RULE))
    f.img("occl_cut", 44, oy, osz, osz, "cam%d" % o["cut_cam"],
          "cut into %d pieces" % o["cut_parts"])
    f.img("occl_whole", 214, oy, osz, osz, "cam%d" % o["whole_cam"],
          "one piece")

    f.text(400, oy + 12, "And a later dart gets hidden behind an earlier one.",
           size=11.5, anchor="start", weight="600", family=SANS)
    for i, line in enumerate([
        "Yellow is the third dart thrown, magenta the ones already in the board. In cam%d an earlier dart lies straight across it" % o["cut_cam"],
        "and the silhouette comes apart; in cam%d nothing is in the way and it stays whole. Same instant, same dart, two answers." % o["whole_cam"],
        "",
        "This is why the instance masks are per view rather than shared. A dart's conditioning channel is what the segmenter",
        "gained minus what earlier darts already claimed, so an occluded dart's channel genuinely has a hole in it — and the",
        "training labels reproduce that hole rather than repairing it. The view with least occlusion carries the cleaner channel,",
        "and the funnel gets to weigh all three instead of being handed one blurred average.",
    ]):
        f.text(400, oy + 34 + i * 16, line, size=10.5, anchor="start", fill=MUTED,
               family=SANS)
    f.text(400, oy + 152, "tints come from the stored labels, only to show which pixels belong to which dart",
           size=9, anchor="start", fill=MUTED, family=SANS)

    f.text(f.w / 2, 722,
           "the flare from step 1 is visible above too — each silhouette ends in a rounded cap, and it is the caps that overlap",
           size=9.5, fill=MUTED, family=SANS)
    return f


# ================================================== figure 3: the AR loop
def fig_autoregressive() -> Fig:
    doc = json.loads((ASSETS / "passes.json").read_text())
    passes, nosep = doc["passes"], doc["nosep"]
    f = Fig(1080, 1218)
    f.header("The loop - one pass per throw",
             "darts arrive one at a time, so each frame is asked for one dart, "
             "against the ones already counted",
             "one real turn, three throws", "no per-dart label anywhere in the loop")

    t, x0, step = 170, 44, 274
    ROWA, ROWB, ROWC = 128, 354, 580
    heads = ["throw 1", "throw 2", "throw 3", "next cycle"]
    subs = ["1 dart on the board", "2 darts on the board", "3 darts on the board",
            "same frame, nothing new"]
    condsub = ["nothing counted yet", "dart 1", "darts 1 and 2", "darts 1, 2 and 3"]

    for i, p in enumerate(passes):
        x = x0 + i * step
        stop = p["stop"]
        f.text(x + t / 2, 116, heads[i], size=12, weight="700")

        # the frame the cameras actually delivered for this throw
        f.img("seq_frame%d" % i, x, ROWA, t, t, "what the cameras see", subs[i])
        f.arrow(x + t / 2, ROWA + t + 32, x + t / 2, ROWB - 6)

        # what it was told was already counted
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

    f.add('<line x1="28" y1="880" x2="%d" y2="880" stroke="%s" stroke-width="1"/>'
          % (f.w - 28, RULE))
    f.text(x0, 906,
           "The conditioning is the only thing stopping a dart being counted twice.",
           size=11, anchor="start", weight="600", family=SANS)
    for i, line in enumerate([
        "There is no tracker and no suppression between frames. The fourth column is the same three-dart frame as the third, and the only",
        "difference is that all three darts are now in the conditioning — which is enough to take the existence logit from +18.40 to -10.95.",
        "That is how the detector knows a turn is over rather than being told to stop after three.",
        "",
        "Every mask in row two came from differencing the segmenter between the frames above it: whatever the mask gained since the last",
        "dart was counted is the new dart. That is exactly the derivation the model was trained on, and it needs no per-dart labels.",
    ]):
        f.text(x0, 926 + i * 15, line, size=10, anchor="start", fill=MUTED,
               family=SANS)

    # ---- the one case this cannot be run in --------------------------
    sy, sw = 1046, 118
    f.text(x0, sy, "Which is why a frozen frame is a different problem.",
           size=11, anchor="start", weight="600", family=SANS)
    py = sy + 14
    f.img("nosep_cond", x0, py, sw, sw, "one claim", "all three darts")
    f.arrow(x0 + sw + 8, py + sw / 2, x0 + sw + 34, py + sw / 2)
    f.img("nosep_heat", x0 + sw + 42, py, sw, sw, "heatmap out", "nothing left")
    f.text(x0 + 2 * sw + 62, py + sw / 2 - 4, "exist %+.2f" % nosep["exist"],
           size=10.5, anchor="start", weight="700", fill="#cf222e")
    f.text(x0 + 2 * sw + 62, py + sw / 2 + 11, "stops after one dart",
           size=9.5, anchor="start", fill="#cf222e", family=SANS)

    tx = x0 + 2 * sw + 190
    for i, line in enumerate([
        "Replaying a saved capture has no “since” — every dart is already in the very first mask, so the first",
        "one claims all three and the model correctly reports nothing left. --replay therefore takes the connected",
        "blob under each tip instead, which recovers all three darts in 78% of a 40-frame sample against 95% for",
        "the stored labels; the rest are darts whose silhouettes touch. Replay is a diagnostic, not the path play takes.",
    ]):
        f.text(tx, py + 12 + i * 15, line, size=10, anchor="start", fill=MUTED,
               family=SANS)
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
                      ("why-three-cameras", fig_triangulation()),
                      ("architecture-detector", fig_detector()),
                      ("autoregressive-loop", fig_autoregressive())]:
        svg = OUT / f"{name}.svg"
        svg.write_text(fig.render(), encoding="utf-8")
        jpg = OUT / f"{name}.jpg"
        if rasterise(svg, jpg, fig.w, fig.h):
            svg.unlink()      # the raster is what ships; the SVG was scaffolding
            print(f"{name}.jpg  {jpg.stat().st_size / 1024:,.0f} KB")
        else:
            print(f"{name}.svg  {svg.stat().st_size / 1024:,.0f} KB")
