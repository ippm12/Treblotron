# palm_detection

Google MediaPipe BlazePalm — palm detector used as a presence-only signal
to drive the "removing darts" state in `vision/tensorrt_vision_source.cpp`
(see `vision_source.hpp` for the state machine).

We run BlazePalm on one of the three pre-warped 720×720 camera frames per
inference cycle (round-robin), resizing to the model's native 192×192. We
care only about *whether a hand is present*, never about the location, so
the regressor output is bound but ignored.

## Source

BlazePalm Lite weights ship with the official MediaPipe distribution as
TFLite. They need to be converted to ONNX once, on a host machine with a
Python ML environment. The Jetson never sees Python.

Recommended conversion path (host machine, not the Jetson):

```bash
pip install tf2onnx tensorflow

# Pull the official weights — the lite variant is faster on Orin and
# accurate enough for "is a hand present" detection.
wget https://storage.googleapis.com/mediapipe-assets/palm_detection_lite.tflite

python -m tf2onnx.convert \
    --tflite palm_detection_lite.tflite \
    --output blazepalm.onnx \
    --opset 17

# Verify the IO matches the contract below before copying to the Jetson:
python - <<'PY'
import onnx
m = onnx.load("blazepalm.onnx")
print("inputs :", [(i.name, [d.dim_value for d in i.type.tensor_type.shape.dim]) for i in m.graph.input])
print("outputs:", [(o.name, [d.dim_value for d in o.type.tensor_type.shape.dim]) for o in m.graph.output])
PY
```

Then drop `blazepalm.onnx` here. The runtime requires the I/O tensors
to have these exact names: `input` (input), `classificators` (output,
`[1, 2016, 1]`), `regressors` (output, `[1, 2016, 18]`). If your
converter produced generic names like `input_1` / `Identity` /
`Identity_1`, rename them on the host machine before checking the model
in — the engine load step asserts on the canonical names and refuses to
start with a clear error on the loading screen if any are missing.

The build will pick the ONNX up on the next launch and produce a cached
`.trt` engine alongside.

## Expected IO contract

The C++ side hardcodes tensor names and shapes — when you swap in a
different export (e.g. the full BlazePalm vs Lite, or a different opset)
the constants in `tensorrt_vision_source.cpp` (`PALM_INPUT_NAME`,
`PALM_OUT_SCORES`, `PALM_OUT_BOXES`, and the shape constants) must match.

### Inputs

- `input`: `float32 [1, 192, 192, 3]` (**NHWC**, RGB, range **`[0, 1]`**)
  - **NHWC, not NCHW.** The BlazePalm ONNX has the channels in the last
    position — pixels are interleaved (R, G, B, R, G, B, ...) row by row.
    The graph starts with a `Transpose` that flips NHWC→NCHW for its
    internal convs; feeding planar CHW data into the input mangles
    through that transpose into noise.
  - Caller resizes the *raw* (unwarped) camera frame to 192×192 with
    bilinear interpolation, then normalizes with `pixel / 255.0`.
  - **Empirically verified**: feeding `[-1, 1]` to the *full* BlazePalm
    export produces saturated logits (sigmoid≈1.0) on every frame
    regardless of image content. Only `[0, 1]` produces sane scores.
    See the long comment at `tensorrt_vision_source.cpp:1240-1246` for
    the test that nailed this down.
  - Channel order is RGB with **no swap** at this stage —
    `camera_api.cpp` already did `BGR→RGB` at capture time.
  - Input is the *raw* frame, not the perspective-warped dart-detection
    frame. The warp flattens the board plane and shears out-of-plane
    objects (hands), which BlazePalm was not trained for.

### Outputs

- `classificators`: `float32 [1, 2016, 1]` — per-anchor logits. Sigmoid +
  max gives the highest palm-presence confidence anywhere in the frame.
- `regressors`:     `float32 [1, 2016, 18]` — per-anchor box / keypoint
  regression. Now copied D2H each cycle so the second-stage hand-landmark
  detector can place its oriented ROI crop on the winning anchor (see
  the "Hand landmark stage" section below). The 18-float layout per
  anchor (verified in `verify_landmark_filter.py` against the actual
  ONNX outputs):
  - `[0..1]` — bbox center offset `(dx, dy)` from the anchor center, in
    192-pixel input space.
  - `[2..3]` — bbox `(w, h)` in 192-pixel input space.
  - `[4..17]` — 7 keypoints × `(x, y)`, anchor-center-relative, in
    192-pixel input space. KP indices: 0=wrist, 1=index MCP,
    2=middle-finger MCP (used for orientation), 3=ring MCP, 4=pinky
    MCP, 5=thumb CMC, 6=thumb tip area.

## Decoding (presence only)

```cpp
// Per-anchor logits → max sigmoid → threshold.
float bestLogit = classificators[0];
for(uint32_t i = 1; i < N_PALM_ANCHORS; i++) {
    bestLogit = std::max(bestLogit, classificators[i]);
}
const float bestProb = 1.0f / (1.0f + std::exp(-bestLogit));
const bool palmDetectedThisCycle = (bestProb >= PALM_PRESENCE_THRESHOLD);
```

## Hand landmark stage (FP filter for BlazePalm)

BlazePalm produces high-confidence false positives on dart shapes in
the bullseye region — close enough in score to real hands that no
single threshold separates them. The MediaPipe landmark stage was
designed exactly for this: it tries to fit 21 hand keypoints to the
palm-detected ROI and emits a `presence` sigmoid that collapses to
~0 when no plausible skeleton fits. We run it conditionally, only
when palm has already passed its own threshold, and AND the two
results before driving the `Removing` state transition.

### Source

```bash
# Pull the official Tasks bundle (zip with both stage tflites).
wget https://storage.googleapis.com/mediapipe-models/hand_landmarker/hand_landmarker/float16/latest/hand_landmarker.task

# Extract — the 5.5 MB tflite is the landmark model.
unzip hand_landmarker.task            # → hand_landmarks_detector.tflite

python -m tf2onnx.convert \
    --tflite hand_landmarks_detector.tflite \
    --output hand_landmark.onnx \
    --opset 17

# Rename generic IO names to the canonical set the C++ runtime expects.
python rename_io_landmark.py hand_landmark.onnx
```

### Expected IO contract

Tensor names hardcoded in `tensorrt_vision_source.cpp` —
`LANDMARK_INPUT_NAME`, `LANDMARK_OUT_LANDMARKS`, `LANDMARK_OUT_PRESENCE`,
`LANDMARK_OUT_HANDEDNESS`, `LANDMARK_OUT_WORLD`.

#### Input

- `input`: `float32 [1, 224, 224, 3]` (**NHWC**, RGB, range **`[0, 1]`**)
  - Same `pixel / 255.0` normalization as the dart model. Do **not**
    feed `[-1, 1]` — the model is sensitive enough that the wrong
    range collapses presence to 0 on real hands.
  - The crop is an *oriented* 224×224 patch from the same raw
    (unwarped) camera frame BlazePalm saw. Construction:
    1. Top-1 anchor's bbox center `(bcx, bcy)` and size `(w, h)` mapped
       up from 192-space to raw-frame pixels.
    2. Wrist (KP0) → middle-finger MCP (KP2) angle → rotate so fingers
       point up in the destination.
    3. Long-side-of-bbox × 2.6 = source square edge; warpAffine into
       the 224×224 destination.
    4. Center shifted by `-0.5 × longSide` along the wrist→middle axis
       (MediaPipe `detection_to_roi` convention — keeps the palm
       centered, not the wrist).

#### Outputs

- `landmarks`: `float32 [1, 63]` — 21 landmarks × `(x, y, z)` in
  *input-pixel* coords (0..224). Bound but unused at runtime.
- `presence`: `float32 [1, 1]` — sigmoid hand-presence score. **This
  is the FP filter**, AND'd with the BlazePalm presence to gate
  `Removing` transitions.
- `handedness`: `float32 [1, 1]` — left/right sigmoid. Bound but unused.
- `world_landmarks`: `float32 [1, 63]` — metric, hand-relative. Bound
  but unused.

### Decoding (presence only)

```cpp
// Stage 1: BlazePalm passes? → Stage 2: build oriented crop and run
// landmark engine. AND the two booleans.
const bool palmStagePass     = (palmProb >= PALM_PRESENCE_THRESHOLD);
bool       landmarkStagePass = false;
if(palmStagePass) {
    // ... decode top anchor, build affine, warpAffine, /255, run engine ...
    landmarkStagePass = (presence[0] >= LANDMARK_PRESENCE_THRESHOLD);
}
const bool palmDetected = palmStagePass && landmarkStagePass;
```

The Python verifier `verify_landmark_filter.py` runs this whole pipeline
on a saved camera frame and prints both stage scores — useful for
threshold sweeping without rebuilding the C++.

## If you used a PINTO_model_zoo palm export instead

Some PINTO exports bake the normalization *into* the ONNX graph (a
prepended mul/sub op). In that case the caller should feed raw `[0, 1]`
or even `[0, 255]` uint8, and doing the `[-1, 1]` conversion on top
double-normalizes. Inspect your ONNX with Netron — if you see a Mul by
`1/127.5` and Sub by `1.0` near the input, the preprocessing is inside
the graph and this file's tensor-pack loop needs to change to match.
The standalone `tf2onnx` conversion from `palm_detection_lite.tflite`
does *not* add that.

## Files

- `blazepalm.onnx` — exported per the recipe above. Shipped to the build
  output via a CMake `POST_BUILD` copy.
- `blazepalm_anchors.hpp` — auto-generated anchor centers (2016×2 floats)
  for the BlazePalm 192×192 input. Regenerate via
  `verify_landmark_filter.py:generate_anchors()` if the BlazePalm config
  ever changes.
- `hand_landmark.onnx` — landmark stage, IO-renamed via
  `rename_io_landmark.py`. Shipped via the same CMake copy rule.
- `hand_landmark_fp32.trt` — generated next to the `.onnx` on first run.
- `blazepalm.trt` — generated next to the `.onnx` on first run (FP16,
  small engine, ~5-15 s build), then reloaded from disk on subsequent
  launches. Rebuilt automatically when the `.onnx` mtime is newer.
