# Dart segmentation deployment bundle

Files:
- `dart_seg_unet.onnx` — exported segmentation U-Net.  Fixed input
  `(3, 3, 360, 640)` (NCHW).
- `README_seg.md` — this file.

## Pipeline integration
This model runs *before* the AR detector.  Per-frame flow on the Jetson:

1. Capture 3 raw camera images (1280x720 RGB).
2. Resize each to 640x360 (linear), divide by 255 → float32 NCHW.
3. Stack along batch axis → `(3, 3, 360, 640)` and run this engine.
4. Apply sigmoid + threshold (0.5) to the `logits` output → 3 binary masks
   at 640x360.
5. Upsample each mask back to the camera's native 1280x720 (nearest).
6. Multiply native RGB × mask → masked RGB still at 1280x720.
7. Warp the masked RGB once via the cached per-camera homography → 720x720
   canonical image.  This is one of the 3 inputs to the AR detector.

## Outputs
- `logits` — `(3, 1, 360, 640)` pre-sigmoid.

## Building the TensorRT engine on the Jetson

```bash
trtexec \
  --onnx=dart_seg_unet.onnx \
  --fp16 \
  --saveEngine=dart_seg_unet.fp16.plan \
  --workspace=2048

# Benchmark
trtexec --loadEngine=dart_seg_unet.fp16.plan --useCudaGraph --iterations=200
```

The Jetson Orin Nano Super should hit **3-6 ms / forward** on a 3-batch
at FP16 (the whole 3-camera pass).

## Notes
- Trained on synth-only data with 0.97 val IoU; produces useful (but
  noisy) masks on real images.  Don't expect pixel-perfect output.
- The AR detector at inference time was trained on the noised version of
  these masks, so real-data noise is in-distribution.
- Batch size is **fixed at 3**.  If you need a different batch
  shape, either re-export with a different value of `SEG_BATCH` in
  `export.py`, or rebuild ONNX with dynamic axes.
