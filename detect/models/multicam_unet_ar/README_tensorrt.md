# TensorRT FP16 deployment bundle (Jetson Orin et al.)

Files:
- `multicam_unet_ar.onnx` — exported autoregressive model, fixed input
  `(1, 10, 720, 720)`.  Channels: 3 cameras x RGB (9) + 1 shared
  conditioning mask.
- `README_tensorrt.md` — this file.

## Preprocessing at inference time
Match the training pipeline exactly:
1. Each camera image: load as BGR, convert to RGB, resize to 720x720,
   divide by 255 (no ImageNet mean/std).
2. Concatenate cam0, cam1, cam2 RGB along the channel axis.
3. Append a 720x720 mask channel containing the conditioning state:
   gaussian blobs (sigma=5.0, in input pixels) at every dart tip the
   model has already placed, combined with element-wise MAX (not sum --
   values stay in [0, 1]); all-zero when no darts are pre-placed.
   Pass as the 10th channel, normalised the same way as the RGB.
4. Layout is `NCHW`.

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
