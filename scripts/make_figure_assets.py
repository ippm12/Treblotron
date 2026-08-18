"""Render every thumbnail the model figures need, from real labelled data.

    python scripts/make_figure_assets.py --training-repo ../DartModelTraining

Runs both shipped ONNX models over real frames, so the figures in the README
show genuine model output rather than mock-ups. Needs a DartModelTraining
checkout for the frames, the board transforms and the tip labels; it is a
documentation tool, not part of the build.

Nothing here uses a per-dart label as model input. The segmenter emits one blob
of every dart present, so a dart's own pixels are only ever recoverable as a
difference of successive masks -- and the loop sequence below is three frames
of one real turn, differenced exactly as the runtime does it.

Feed the output directory to scripts/make_model_figures.py.
"""
from __future__ import annotations

import argparse
import json
import math
import os
import sys
from pathlib import Path

import numpy as np
import cv2

ROOT = Path(__file__).resolve().parent.parent

# One real turn, recovered by nesting tips across captures: a 1-dart frame whose
# tip reappears in a 2-dart frame, and both again in a 3-dart frame. The board
# drifts 0.5 px across the three, so they are the same turn shot as it happened.
SEQUENCE = ("6e88dc4e-6dfe-41e7-93db-fe5dcf2bd301",
            "6f33a025-019e-4ee5-bf9c-50f7ae40f95d",
            "503e903d-3c92-45f0-8983-1f60e21762cf")

# The segmenter figure only needs one clear frame; it is an argument about masking.
SEG_FRAME = "020ad6f9-58a3-433c-b205-59e4720c719f"

# A dart thrown into a black bed, where the steel point is invisible.
HARD_TIP = ("f3dcf817-5e77-41a3-a72a-35787fdedf63", 0, 1)

# The same frame the segmenter figure uses for its hard tip: dart 1's steel is
# invisible in cam0, which is what makes the three-view intersection worth
# showing. (uuid, blind camera, blind dart)
TRIANGULATION = ("f3dcf817-5e77-41a3-a72a-35787fdedf63", 0, 1)

# A dart passing behind an earlier one: cut into pieces in cam2, whole in cam0.
# (uuid, dart, camera that loses it, camera that keeps it)
OCCLUSION = ("69d012ed-8d53-46ce-95a6-be1f9ade3d77", 2, 2, 0)

# Pure primaries, so that overlapping views mix predictably: two views give a
# secondary (cyan/magenta/yellow) and all three give white. The white is the
# answer, and it needs no legend.
CAM_BGR = [(255, 60, 60), (60, 255, 60), (60, 60, 255)]

IMG, SIGMA, MAX_DARTS = 720, 5.0, 3
SLOT_BGR = [(255, 120, 80), (90, 220, 120), (90, 140, 255)]


def default_training_repo() -> Path:
    """Env var, then a sibling checkout — never a path baked into the file."""
    env = os.environ.get("DARTMODELTRAINING")
    return Path(env) if env else ROOT.parent / "DartModelTraining"


ap = argparse.ArgumentParser(description=__doc__,
                             formatter_class=argparse.RawDescriptionHelpFormatter)
ap.add_argument("--training-repo", type=Path, default=default_training_repo(),
                help="DartModelTraining checkout (or set $DARTMODELTRAINING); "
                     "defaults to a sibling of this repo")
ap.add_argument("--models", type=Path,
                default=ROOT / "detect" / "models" / "multicam_unet_ar",
                help="directory holding the shipped .onnx files")
ap.add_argument("--out", type=Path, default=ROOT / "build" / "figassets",
                help="where to write the thumbnails")
args = ap.parse_args()

TRAIN = args.training_repo.expanduser().resolve()
DATA = TRAIN / "data"
MODELS = args.models.expanduser().resolve()
OUT = args.out.expanduser().resolve()

if not DATA.is_dir():
    sys.exit(f"no training data at {DATA}\n"
             f"pass --training-repo or set $DARTMODELTRAINING")
if not (MODELS / "dart_seg_unet.onnx").is_file():
    sys.exit(f"no models at {MODELS}")

sys.path.insert(0, str(TRAIN))
import dart_instances as di                                   # noqa: E402
import onnxruntime as ort                                     # noqa: E402

OUT.mkdir(parents=True, exist_ok=True)
print(f"training data : {DATA}\nmodels        : {MODELS}\nout           : {OUT}\n")

seg = ort.InferenceSession(str(MODELS / "dart_seg_unet.onnx"),
                           providers=["CPUExecutionProvider"])
ar = ort.InferenceSession(str(MODELS / "multicam_unet_ar.onnx"),
                          providers=["CPUExecutionProvider"])
sin, ain = seg.get_inputs()[0].name, ar.get_inputs()[0].name

tips_all = json.loads((DATA / "normalized_tips.json").read_text())
dets = json.loads((DATA / "board_detections.json").read_text())


def save(name, bgr):
    cv2.imwrite(str(OUT / f"{name}.png"), bgr)


def transform(uuid, cam):
    d = dets.get(f"{uuid}_cam{cam}") or {}
    t = d.get("full_transform") or d.get("homography")
    if t is None:
        sys.exit(f"no board transform for {uuid}_cam{cam}")
    return np.asarray(t, np.float64).reshape(3, 3)


class Frame:
    """One capture: raw views, the segmenter's masks, and both warped to board."""

    def __init__(self, uuid):
        self.uuid = uuid
        self.raw = [cv2.imread(str(DATA / "images" / f"{uuid}_cam{c}.png"))
                    for c in range(3)]
        if any(im is None for im in self.raw):
            sys.exit(f"missing images for {uuid}")

        logits = seg.run(None, {sin: np.stack([
            cv2.cvtColor(cv2.resize(im, (640, 360)), cv2.COLOR_BGR2RGB)
              .transpose(2, 0, 1).astype(np.float32) / 255.0
            for im in self.raw])})[0]

        self.H = [transform(uuid, c) for c in range(3)]
        self.mask, self.masked, self.warped, self.wmask, self.wplain = [], [], [], [], []
        for c, im in enumerate(self.raw):
            m = (logits[c].squeeze() > 0).astype(np.uint8) * 255
            m = cv2.resize(m, (im.shape[1], im.shape[0]),
                           interpolation=cv2.INTER_NEAREST)
            mr = cv2.bitwise_and(im, im, mask=m)
            self.mask.append(m)
            self.masked.append(mr)
            self.warped.append(cv2.warpPerspective(mr, self.H[c], (IMG, IMG)))
            self.wmask.append(cv2.warpPerspective(m, self.H[c], (IMG, IMG),
                                                  flags=cv2.INTER_NEAREST))
            self.wplain.append(cv2.warpPerspective(im, self.H[c], (IMG, IMG)))

        self.tips = np.asarray(tips_all[uuid]["points"], np.float64)

    @property
    def rgb9(self):
        return np.concatenate(
            [cv2.cvtColor(w, cv2.COLOR_BGR2RGB).transpose(2, 0, 1)
             for w in self.warped], axis=0).astype(np.float32) / 255.0

    def claim(self):
        """The whole segmentation mask, per view — all the runtime ever has."""
        return np.stack([w > 0 for w in self.wmask])


def gaussian(cx, cy, size=IMG, sigma=SIGMA):
    out = np.zeros((size, size), np.float32)
    r = int(math.ceil(4 * sigma))
    y0, y1 = max(0, int(cy) - r), min(size, int(cy) + r + 1)
    x0, x1 = max(0, int(cx) - r), min(size, int(cx) + r + 1)
    if y1 <= y0 or x1 <= x0:
        return out
    xx, yy = np.meshgrid(np.arange(x0, x1, dtype=np.float32),
                         np.arange(y0, y1, dtype=np.float32))
    out[y0:y1, x0:x1] = np.exp(-((xx - cx) ** 2 + (yy - cy) ** 2) / (2 * sigma ** 2))
    return out


def bloom(gray, k=27):
    """Grow a few-pixel feature so it survives being shrunk to a thumbnail.

    Sigma is 5 px in a 720 px frame; at figure scale that is well under one
    pixel. The figures say they are enlarged.
    """
    return cv2.GaussianBlur(cv2.dilate(gray, np.ones((k, k), np.uint8)), (0, 0), 3)


def infer(rgb9, claims, pts):
    """One forward pass. `claims` are cumulative masks, in counting order —
    conditioning_channels differences them, which is the runtime derivation."""
    stack = np.zeros((3, MAX_DARTS, IMG, IMG), bool)
    for j, cl in enumerate(claims[:MAX_DARTS]):
        stack[:, j] = cl
    extra = di.conditioning_channels(stack, pts, list(range(len(claims))),
                                     tip_sigma=SIGMA).astype(np.float32) / 255.0
    hm, ex = ar.run(None, {ain: np.concatenate(
        [rgb9, extra], axis=0)[None].astype(np.float32)})
    return 1.0 / (1.0 + np.exp(-hm[0])), float(np.reshape(ex, -1)[0]), extra


def peak_of(hm):
    r, c = divmod(int(hm.argmax()), hm.shape[1])
    return (float(hm[r, c]),
            ((c + 0.5) * IMG / hm.shape[1], (r + 0.5) * IMG / hm.shape[0]))


def dim(bgr, k):
    return (cv2.cvtColor(cv2.cvtColor(bgr, cv2.COLOR_BGR2GRAY),
                         cv2.COLOR_GRAY2BGR) * k).astype(np.uint8)


def heat_vis(hm, base_bgr):
    """Heatmap over the dimmed board — position is the point, and a bare
    360x360 peak on black says nothing about where it is."""
    big = cv2.resize((np.clip(hm, 0, 1) * 255).astype(np.uint8), (IMG, IMG),
                     interpolation=cv2.INTER_NEAREST)
    heat = cv2.applyColorMap(bloom(big), cv2.COLORMAP_INFERNO)
    base = dim(base_bgr, 0.45)
    s = (bloom(big).astype(np.float32) / 255.0)[..., None]
    return (base * (1 - s) + heat * s).astype(np.uint8)


def cond_vis(channels, base_bgr):
    """The 12 conditioning channels as the pass received them, in cam0."""
    out = dim(base_bgr, 0.28)
    for j in range(MAX_DARTS):
        out[channels[j] > 0] = SLOT_BGR[j]          # cam0 lives in channels 0..2
    for j in range(MAX_DARTS):
        g = (channels[3 * MAX_DARTS + j] * 255).astype(np.uint8)
        if g.any():
            out = np.maximum(out, cv2.applyColorMap(bloom(g, k=19),
                                                    cv2.COLORMAP_BONE))
    return out


# ========================================= step 1: what the segmenter does
sf = Frame(SEG_FRAME)
for c in range(3):
    save(f"raw_cam{c}", sf.raw[c])
    save(f"segmask_cam{c}", cv2.cvtColor(sf.mask[c], cv2.COLOR_GRAY2BGR))
    save(f"maskedrgb_cam{c}", sf.masked[c])
    over = sf.raw[c].copy()
    m = sf.mask[c] > 0
    over[m] = (0.35 * over[m] + 0.65 * np.array([80, 255, 120])).astype(np.uint8)
    save(f"overlay_cam{c}", over)
    save(f"warped_masked_cam{c}", sf.warped[c])
    save(f"warped_plain_cam{c}", sf.wplain[c])
print("step 1: segmentation over", SEG_FRAME[:8])


# ============================ step 2 and the loop: one real turn, as it landed
frames = [Frame(u) for u in SEQUENCE]
for j, fr in enumerate(frames):
    if len(fr.tips) != j + 1:
        sys.exit(f"{fr.uuid} has {len(fr.tips)} tips, expected {j + 1}")

known_pts, claims, passes = [], [], []
inst_shown = None
for step in range(4):
    fr = frames[min(step, 2)]                    # pass 4 re-reads the last frame
    hm, exl, channels = infer(fr.rgb9, claims, known_pts)
    peak, tip = peak_of(hm)
    err = float(np.linalg.norm(fr.tips - np.asarray(tip), axis=1).min())
    stop = exl < 0.0 or peak < 0.55

    # The plain warped view, not the masked one the model eats — this row is
    # "a dart arrived", and the segmenter figure already made the masking argument.
    save(f"seq_frame{step}", fr.wplain[0])
    save(f"seq_heat{step}", heat_vis(hm, fr.wplain[0]))
    save(f"seq_cond{step}", cond_vis(channels, fr.wplain[0])
         if step else dim(fr.wplain[0], 0.28))

    passes.append({"step": step, "darts": len(fr.tips),
                   "exist": round(exl, 2), "peak": round(peak, 3),
                   "err": round(err, 1), "stop": stop})
    print(f"throw {step + 1}: {len(fr.tips)} on board, exist={exl:+.2f} "
          f"peak={peak:.3f} err={err:.1f}px {'STOP' if stop else 'accept'}")

    if step == 2:
        # The detector figure illustrates this pass, so its panels are a real pass
        # rather than an assembled picture: two darts counted, the third slot
        # genuinely zeroed, and a heatmap with the one remaining dart in it.
        inst_shown = channels
    if step == 3:
        break
    known_pts.append(tip)
    claims.append(fr.claim())

# Every channel here came from differencing the segmenter between the throws
# above, never from a label.
for c in range(3):
    save(f"stage2_rgb_cam{c}", frames[2].warped[c])
    for k in range(MAX_DARTS):
        img = np.zeros((IMG, IMG, 3), np.uint8)
        img[inst_shown[c * MAX_DARTS + k] > 0] = SLOT_BGR[k]
        save(f"inst_cam{c}_slot{k}", img)
for k in range(MAX_DARTS):
    g = (inst_shown[3 * MAX_DARTS + k] * 255).astype(np.uint8)
    save(f"tip_slot{k}", cv2.applyColorMap(bloom(g), cv2.COLORMAP_INFERNO))
print("step 2: throw 3's conditioning — 2 slots filled, from differencing")


# ------------------------- and why a frozen frame cannot do the same ----------
#
# Replay has no "since": every dart is in the very first mask. Hand the first
# dart the whole thing and it claims all three, so the model correctly reports
# nothing left and a three-dart capture replays as one.
last = frames[2]
hm_ns, ex_ns, ch_ns = infer(last.rgb9, [last.claim()], [known_pts[0]])
save("nosep_cond", cond_vis(ch_ns, last.wplain[0]))
save("nosep_heat", heat_vis(hm_ns, last.wplain[0]))
nosep = {"exist": round(ex_ns, 2), "peak": round(peak_of(hm_ns)[0], 3)}
print(f"frozen frame, whole mask: exist={nosep['exist']:+.2f} "
      f"peak={nosep['peak']:.3f}")

(OUT / "passes.json").write_text(json.dumps(
    {"sequence": list(SEQUENCE), "passes": passes, "nosep": nosep}, indent=2))


# =========================================== tip detail, at the scale it happens
#
# The flare is only arguable a couple of pixels from the end of a dart. Crop
# tight around one tip, in the raw frame, so the figure can show the polished
# point beside the mask that covers it.
CROP, ZOOM = 96, 5


def tip_crop(frame, cam, dart, name_prefix, mark=True):
    inv = np.linalg.inv(frame.H[cam])
    pt = cv2.perspectiveTransform(
        frame.tips.reshape(-1, 1, 2), inv).reshape(-1, 2)[dart]
    im, msk = frame.raw[cam], frame.mask[cam]
    h, w = im.shape[:2]
    tx, ty = (int(round(v)) for v in pt)
    x0, y0 = max(0, min(tx - CROP // 2, w - CROP)), max(0, min(ty - CROP // 2, h - CROP))
    big = cv2.resize(im[y0:y0 + CROP, x0:x0 + CROP], None, fx=ZOOM, fy=ZOOM,
                     interpolation=cv2.INTER_NEAREST)
    mbig = cv2.resize(msk[y0:y0 + CROP, x0:x0 + CROP], None, fx=ZOOM, fy=ZOOM,
                      interpolation=cv2.INTER_NEAREST)
    save(f"{name_prefix}_raw", big)

    outline = big.copy()
    cnts, _ = cv2.findContours(mbig, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_SIMPLE)
    cv2.drawContours(outline, cnts, -1, (120, 255, 140), 2)
    if mark:
        cv2.drawMarker(outline, ((tx - x0) * ZOOM, (ty - y0) * ZOOM), (80, 80, 255),
                       cv2.MARKER_CROSS, 18, 2)
    save(f"{name_prefix}_mask", outline)

    solid = np.zeros_like(big)
    solid[mbig > 0] = (120, 255, 140)
    if mark:
        cv2.drawMarker(solid, ((tx - x0) * ZOOM, (ty - y0) * ZOOM), (80, 80, 255),
                       cv2.MARKER_CROSS, 18, 2)
    save(f"{name_prefix}_solid", solid)
    return tx, ty


# Whichever tip sits furthest inside the frame, so the crop is not clipped.
h0, w0 = sf.raw[0].shape[:2]
rt = cv2.perspectiveTransform(sf.tips.reshape(-1, 1, 2),
                              np.linalg.inv(sf.H[0])).reshape(-1, 2)
best = max(range(len(rt)), key=lambda i: min(rt[i][0], w0 - rt[i][0],
                                             rt[i][1], h0 - rt[i][1]))
tx, ty = tip_crop(sf, 0, best, "tipdetail")
print(f"tip detail: dart {best} at raw ({tx}, {ty}), {ZOOM}x zoom")

hu, hc, hd = HARD_TIP
hf = Frame(hu)
hx, hy = tip_crop(hf, hc, hd, "tipdetail_hard_tmp", mark=True)
os.replace(OUT / "tipdetail_hard_tmp_mask.png", OUT / "tipdetail_hard.png")
for leftover in ("tipdetail_hard_tmp_raw", "tipdetail_hard_tmp_solid"):
    (OUT / f"{leftover}.png").unlink(missing_ok=True)
print(f"hard tip: {hu[:8]} cam{hc} dart{hd} at raw ({hx}, {hy})")


# ================================= why three cameras: the views intersect
#
# A dart's tip is the only part of it touching the board plane, so it is the
# only point the three homographies agree on -- everything else stands off the
# plane and lands somewhere different in each view. Intersecting the three
# warped silhouettes therefore collapses to a blob at each tip, and no single
# view has to show the steel for that to work.
tu, blind_cam, blind_dart = TRIANGULATION
tf = Frame(tu)
tip = tf.tips[blind_dart]

board = dim(tf.wplain[0], 0.42)
layers = []
for c in range(3):
    m = tf.wmask[c] > 0
    lay = np.zeros_like(board)
    lay[m] = CAM_BGR[c]
    layers.append(lay)
    panel = board.copy()
    panel[m] = CAM_BGR[c]
    save(f"tri_cam{c}", panel)

# Additive, so two views make a secondary colour and three make white.
stack = np.clip(layers[0].astype(np.int32) + layers[1] + layers[2],
                0, 255).astype(np.uint8)
covered = (tf.wmask[0] > 0) | (tf.wmask[1] > 0) | (tf.wmask[2] > 0)
overlay = board.copy()
overlay[covered] = stack[covered]

inter = (tf.wmask[0] > 0) & (tf.wmask[1] > 0) & (tf.wmask[2] > 0)
n_lab, lab, stats, cents = cv2.connectedComponentsWithStats(
    inter.astype(np.uint8), 8)
best, err = 0, None
for i in range(1, n_lab):
    d = float(np.linalg.norm(cents[i] - tip))
    if err is None or d < err:
        best, err = i, d
blob_px = int(stats[best, cv2.CC_STAT_AREA])

marked = overlay.copy()
for t_ in tf.tips:
    cv2.circle(marked, (int(round(t_[0])), int(round(t_[1]))), 30,
               (255, 255, 255), 1)
cv2.drawMarker(marked, (int(round(tip[0])), int(round(tip[1]))), (255, 255, 255),
               cv2.MARKER_CROSS, 26, 2)
save("tri_overlay", marked)

# The same thing at 6x, where the white is unmistakably a point, not a smear.
Z, HALF = 6, 46
cx, cy = int(round(tip[0])), int(round(tip[1]))
x0 = max(0, min(cx - HALF, IMG - 2 * HALF))
y0 = max(0, min(cy - HALF, IMG - 2 * HALF))
zoom = cv2.resize(overlay[y0:y0 + 2 * HALF, x0:x0 + 2 * HALF], None, fx=Z, fy=Z,
                  interpolation=cv2.INTER_NEAREST)
cv2.drawMarker(zoom, ((cx - x0) * Z, (cy - y0) * Z), (255, 255, 255),
               cv2.MARKER_CROSS, 30, 2)
save("tri_zoom", zoom)

# What the blind camera actually shows at the same place.
inv = np.linalg.inv(tf.H[blind_cam])
bp = cv2.perspectiveTransform(tip.reshape(1, 1, 2), inv).reshape(2)
bim = tf.raw[blind_cam]
bh, bw = bim.shape[:2]
BC, BZ = 96, 5
bx0 = max(0, min(int(round(bp[0])) - BC // 2, bw - BC))
by0 = max(0, min(int(round(bp[1])) - BC // 2, bh - BC))
blind = cv2.resize(bim[by0:by0 + BC, bx0:bx0 + BC], None, fx=BZ, fy=BZ,
                   interpolation=cv2.INTER_NEAREST)
cv2.drawMarker(blind, (int(round(bp[0]) - bx0) * BZ, int(round(bp[1]) - by0) * BZ),
               (80, 80, 255), cv2.MARKER_CROSS, 18, 2)
save("tri_blind", blind)

tri = {"uuid": tu, "blind_cam": blind_cam, "blind_dart": blind_dart,
       "err": round(err, 1), "inter_px": blob_px, "union_px": int(covered.sum())}
tri["inter_pct"] = round(100.0 * int(inter.sum()) / tri["union_px"], 1)


# ------------------------------- and what an earlier dart hides ------------
#
# Straight rods and a pinhole camera: a dart passing behind an earlier one is
# cut in that view and whole in another. The stored labels are used here only
# to tint which pixels belong to which dart -- nothing is fed to a model.
ou, od, cut_cam, whole_cam = OCCLUSION
of = Frame(ou)
oinst = np.zeros((3, MAX_DARTS, IMG, IMG), bool)
for c in range(3):
    oinst[c] = di.decode(cv2.imread(
        str(DATA / "real_warped_instance_masks" / f"{ou}_cam{c}.png"),
        cv2.IMREAD_COLOR))

# The box has to hold the dart in both views, or the two panels are not
# comparable -- the same dart projects to a different place in each.
both = oinst[cut_cam, od] | oinst[whole_cam, od]
oys, oxs = np.nonzero(both)
ox0, ox1 = max(0, oxs.min() - 45), min(IMG, oxs.max() + 45)
oy0, oy1 = max(0, oys.min() - 45), min(IMG, oys.max() + 45)

# Yellow and magenta, deliberately not the camera primaries used above: in
# these two panels colour means *which dart*, not which view.
LATER_BGR, EARLIER_BGR = (60, 240, 255), (230, 80, 220)


def occl_panel(cam):
    crop = of.wplain[cam][oy0:oy1, ox0:ox1].copy()
    mine = oinst[cam, od][oy0:oy1, ox0:ox1]
    other = np.zeros_like(mine)
    for j in range(MAX_DARTS):
        if j != od:
            other |= oinst[cam, j][oy0:oy1, ox0:ox1]
    crop[other] = (0.4 * crop[other] + 0.6 * np.array(EARLIER_BGR)).astype(np.uint8)
    crop[mine] = (0.35 * crop[mine] + 0.65 * np.array(LATER_BGR)).astype(np.uint8)
    return crop


save("occl_cut", occl_panel(cut_cam))
save("occl_whole", occl_panel(whole_cam))


def pieces(mask):
    nn, _, st, _ = cv2.connectedComponentsWithStats(mask.astype(np.uint8), 8)
    return sum(1 for i in range(1, nn) if st[i, cv2.CC_STAT_AREA] >= 40)


occ = {"uuid": ou, "dart": od, "cut_cam": cut_cam, "whole_cam": whole_cam,
       "cut_parts": pieces(oinst[cut_cam, od]),
       "whole_parts": pieces(oinst[whole_cam, od]),
       "cut_px": int(oinst[cut_cam, od].sum()),
       "whole_px": int(oinst[whole_cam, od].sum())}

(OUT / "triangulation.json").write_text(
    json.dumps({"tri": tri, "occ": occ}, indent=2))
print(f"triangulation: {tu[:8]} blind cam{blind_cam} dart{blind_dart}, "
      f"blob {blob_px} px, centroid {err:.1f} px from the label")
print(f"occlusion:     {ou[:8]} dart{od} cut into {occ['cut_parts']} pieces in "
      f"cam{cut_cam}, whole in cam{whole_cam}")

print(f"\nwrote {len(list(OUT.glob('*.png')))} thumbnails to {OUT}")
