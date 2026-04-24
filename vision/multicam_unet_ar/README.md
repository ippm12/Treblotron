# multicam_unet_ar

Autoregressive U-Net for dart-tip detection on three stacked camera views,
exported from the sibling project `C:\projects\DartModelTraining`
(`export.py tensorrt`).

Consumed by `vision/tensorrt_vision_source.cpp` on the Jetson build
(`-DDARTLENS_USE_TENSORRT=ON`).

## Inputs

- `input`: `float32 [1, 10, 720, 720]` (NCHW)
  - Channels 0-2: camera 0 RGB, `/255`, BGR→RGB swap done by caller.
  - Channels 3-5: camera 1 RGB, same preprocessing.
  - Channels 6-8: camera 2 RGB, same preprocessing.
  - Channel 9: conditioning mask — `max` of 720×720 Gaussians
    (σ = 5.0, radius ≈ 4σ) at each already-confirmed dart tip. All zeros
    when no darts are confirmed.

## Outputs (all pre-sigmoid logits, opset 17)

- `heatmap`: `float32 [1, 360, 360]`
- `offset`:  `float32 [1, 2, 360, 360]` — `offset[0]=dx`, `offset[1]=dy`,
  each clamped to `[-0.5, +0.5]` cell units.
- `exist_logit`: `float32 [1]`

## Decoding (one dart per forward pass)

```
if exist_logit >= 0 and sigmoid(heatmap).max() >= 0.55:
    flat = argmax(heatmap)                  # sigmoid-argmax == logit-argmax
    r, c = divmod(flat, 360)
    peak_r = r + 0.5 + offset[0, r, c]
    peak_c = c + 0.5 + offset[1, r, c]
    # Convert back to 720-space:
    tx, ty = peak_c * 2, peak_r * 2
```

When no dart is detected, `exist_logit < 0`.

## Files

- `multicam_unet_ar.onnx` — shipped to the build output via a CMake
  `POST_BUILD` copy.
- `multicam_unet_ar.trt` — generated next to the `.onnx` on first run
  (FP16 engine, 30-90 s build), then reloaded from disk on subsequent
  launches. Rebuilt automatically when the `.onnx` file mtime is newer.
