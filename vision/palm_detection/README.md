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

Then drop `blazepalm.onnx` here. The build will pick it up on the next
launch and produce a cached `.trt` engine alongside.

## Expected IO contract

The C++ side hardcodes tensor names and shapes — when you swap in a
different export (e.g. the full BlazePalm vs Lite, or a different opset)
the constants in `tensorrt_vision_source.cpp` (`PALM_INPUT_NAME`,
`PALM_OUT_SCORES`, `PALM_OUT_BOXES`, and the shape constants) must match.

### Inputs

- `input`: `float32 [1, 3, 192, 192]` (NCHW, RGB, range `[0, 1]`)
  - Caller resizes the 720×720 warped frame to 192×192 with bilinear
    interpolation before BGR→RGB swap and `/255` normalization.

### Outputs

- `classificators`: `float32 [1, 2016, 1]` — per-anchor logits. Sigmoid +
  max gives the highest palm-presence confidence anywhere in the frame.
- `regressors`:     `float32 [1, 2016, 18]` — per-anchor box / keypoint
  regression. Bound to a device buffer because TRT requires it, but
  never copied back to host (we don't need the location).

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

## Files

- `blazepalm.onnx` — exported per the recipe above. Shipped to the build
  output via a CMake `POST_BUILD` copy.
- `blazepalm.trt` — generated next to the `.onnx` on first run (FP16,
  small engine, ~5-15 s build), then reloaded from disk on subsequent
  launches. Rebuilt automatically when the `.onnx` mtime is newer.
