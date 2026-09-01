# TensorRT FP16 deployment bundle (Jetson Orin et al.)

Files:
- `multicam_unet_ar.onnx` — exported autoregressive model, fixed input
  `(1, 21, 720, 720)`.
- `multicam_unet_ar.cond.json` — which conditioning layout this ONNX
  expects.  Check it before wiring anything: `instance_v1` is the 21-channel
  layout below, `gaussian_dot` is the older 10-channel one.
- `README_tensorrt.md` — this file.

## Input layout (21 channels, NCHW)

| index | contents |
|---|---|
| 0-8   | 3 cameras x RGB, camera-major (cam0 RGB, cam1 RGB, cam2 RGB) |
| 9-17  | per-dart masks, camera-major: cam0 slots 0/1/2, cam1 slots 0/1/2, cam2 slots 0/1/2 |
| 18-20 | per-dart tip position, one channel per slot, shared across cameras |

Everything is float in `[0, 1]`.

## Preprocessing at inference time
Match the training pipeline exactly:

1. Each camera image: load as BGR, convert to RGB, warp to the canonical
   720x720 board frame with that camera's `full_transform`, divide by 255
   (no ImageNet mean/std).  If the model was trained with `--use-masked-rgb`,
   multiply each camera's RGB by its warped segmentation mask first.
2. Concatenate cam0, cam1, cam2 RGB -> channels 0-8.
3. **Per-dart masks (channels 9-17).**  The segmentation model emits ONE
   binary mask of every dart present, so a dart's own mask is only ever
   recoverable as a difference of successive cumulative masks.  Keep the
   segmenter output from each AR step; for the dart counted at step `j`:

       channel_j = M_j - M_(j-1)          (M_0 = empty)

   where `M_j` is the warped seg mask captured just after dart `j` landed.
   Do this per camera, and write dart `j` into the same slot index in all
   three cameras.  Unused slots are all-zero.

   Thin rims around a dart that has not moved, and holes where a later dart
   is occluded by an earlier one, are both expected -- the model was trained
   with them.  Do not try to clean them up.
4. **Tip positions (channels 18-20).**  A Gaussian, sigma=5.0 in input
   pixels, at the canonical tip of the dart in that slot.  Slot index must
   match the mask above: channel `18 + k` is the tip of the dart whose mask
   is in slots `9 + k`, `12 + k`, `15 + k`.  A whole-dart mask says where a
   dart is but not where its tip is, and the pairing is what tells the model
   which is which.
5. Slot assignment is arbitrary but must be *consistent within one forward*.
   Training randomised it, so any assignment works -- but do not renumber
   between AR steps within a throw.

> A dart that could not be segmented still occupies its slot with an
> all-zero mask.  Dropping it renumbers every later dart and silently
> changes what the model is being told.

## Outputs
- `heatmap`        — `(1, 360, 360)` pre-sigmoid logits.  Apply sigmoid.
- `exist_logit`    — `(1,)` pre-sigmoid.  Gate peak extraction on >= 0.
  Always produce at most one peak per forward: argmax on the heatmap,
  threshold 0.55 on the sigmoid, and take the cell centre.  There is no
  offset head; `exist_logit` is the heatmap peak itself passed through a
  learned scale/bias, so the two gates agree by construction.

## Building the TensorRT engine on the Jetson

```bash
trtexec \
  --onnx=multicam_unet_ar.onnx \
  --fp16 \
  --saveEngine=multicam_unet_ar.fp16.plan \
  --workspace=2048

# Benchmark the engine
trtexec --loadEngine=multicam_unet_ar.fp16.plan --useCudaGraph --iterations=200
```

The Jetson Orin Nano Super should hit **4-8 ms / forward** in FP16.

## Runtime (Python, minimal)

```python
import tensorrt as trt, pycuda.driver as cuda, pycuda.autoinit

logger = trt.Logger(trt.Logger.WARNING)
with open("multicam_unet_ar.fp16.plan", "rb") as f:
    engine = trt.Runtime(logger).deserialize_cuda_engine(f.read())
ctx = engine.create_execution_context()
# Allocate I/O buffers from engine tensor shapes, then ctx.execute_v2(...)
```

Or `polygraphy run multicam_unet_ar.onnx --trt --fp16` for a quick sanity
check without writing runtime code.

## Notes
- This bundle is for the autoregressive model trained with `--autoregressive`
  + `--aux-loss`.  The Hailo bundle in this same directory is for the legacy
  non-AR model; the two are independent.
- `python export.py tensorrt` also runs an FP16 precision simulation against
  the PyTorch FP32 baseline.  Ship only if the printed verdict is "negligible".
- For INT8, you'll need a calibration npy and `--int8` on trtexec.  Not
  currently emitted for this path.
