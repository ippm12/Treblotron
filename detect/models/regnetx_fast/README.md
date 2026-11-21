# Dart models: production bundle

Two models, run in this order every frame set:

| File | Role | Input | Output |
|---|---|---|---|
| `dart_seg_unet.onnx` | segmenter (RegNetX-400MF encoder) | `input` (3, 3, 360, 640) float32 | `logits` (3, 1, 360, 640) |
| `multicam_unet_ar_heatmap_only.onnx` | tip locator | `input` (1, 21, 720, 720) float32 | `heatmap` (1, 360, 360) |

`manifest.json` records the imported model contracts, checksums, source
checkpoints, and original timing/accuracy measurements. Integration checks
are recorded separately below.

## Treblotron integration

This is the permanent bundle location. The top-level `regnetx-fast` import
directory is no longer needed. Windows DirectML and the CPU backend load these
float ONNX files; the Jetson backend loads offline-built TensorRT 11.x plans.
There are no machine names, credentials, or absolute deployment paths in the
build configuration.

TensorRT 11
[removed implicit calibration and precision flags](https://docs.nvidia.com/deeplearning/tensorrt/latest/getting-started/release-notes-11/11.0.0.html).
This deployment now targets 11.x only: INT8 uses explicit QuantizeLinear /
DequantizeLinear nodes, and FP16 uses explicitly typed operators. Both retain
the same linear FP32 public I/O. TensorRT 10 plans must be rebuilt.

From the repository root on a Jetson with TensorRT 11.x and its C++/CUDA
development packages installed, create an export environment that can see
JetPack's system TensorRT bindings. Install only the export dependencies with
pip; TensorRT itself comes from the JetPack packages:

```sh
python3 -m venv --system-site-packages build/model-export-venv
. build/model-export-venv/bin/activate
python -m pip install -r detect/models/regnetx_fast/requirements-export.txt
python -c "import tensorrt; print(tensorrt.__version__)"
python detect/models/regnetx_fast/engines.py build --precision int8 --rebuild &&
python detect/models/regnetx_fast/engines.py verify --precision int8
cmake --preset app-local-jetson -DTREBLOTRON_USE_SYSTEM_OPENCV=ON
cmake --build --preset app-local-jetson -j2
```

If a model fails verification, build and verify its FP16 fallback before
launching (for example, `--models ar` for the locator):

```sh
python detect/models/regnetx_fast/engines.py build --precision fp16 --models ar &&
python detect/models/regnetx_fast/engines.py verify --precision fp16 --models ar
```

`build` prepares the ONNX automatically and checks source/calibration hashes.
It rebuilds plans when the export, TensorRT version or workspace limit changes.
Use `--rebuild` after changing GPUs; use `--recalibrate` to regenerate exports.
Repeated builds revoke the verification receipt until `verify` succeeds again.
The old `.calib.cache` files are no longer used.

For a headless deployment, use the `server-tensorrt` preset instead. Run
`build-server-tensorrt/Treblotron_Server --selftest` after building.
An installed OpenCV 4.x can be used with
`cmake --preset server-tensorrt -DTREBLOTRON_USE_SYSTEM_OPENCV=ON` (also available
for the app preset). Otherwise CMake builds the bundled OpenCV. TensorRT runs
the models in either case; OpenCV CUDA modules are not needed by this pipeline.
The models and any generated plans/receipts are staged beside the executable
and included by CMake installation; rebuild after producing new engines.
Build plans on the target GPU/software stack. Do not copy them from Windows or
assume they are compatible with another TensorRT version.

The runtime tries `engines/ar.int8.plan` then `ar.fp16.plan`, independently of
`seg.int8.plan` then `seg.fp16.plan`. Each requires a `.plan.verified` receipt
written only after its TensorRT reference check passes. The receipt records
CRCs of both the ONNX and plan, so replacing either invalidates that approval.
CRCs detect stale artifacts; they do not authenticate untrusted engines.
An interrupted/failed verification removes the receipt. Missing or incompatible
plans produce an actionable startup error; these two models never compile at
startup. The existing optional palm/landmark models retain their FP32 cache/build
behavior. Internal INT8/FP16 plans must expose linear float32 I/O to the C++ API.

The fallback segmenter is retained for evaluation, but the application selects
RegNetX. Merely building `seg-fallback` does not switch the application's model.

Checks that also run on a PC with NumPy and ONNX Runtime:

```sh
python detect/models/regnetx_fast/engines.py verify --backend onnxruntime --models seg ar seg-fallback
python detect/models/regnetx_fast/engines.py prepare --precision int8
python detect/models/regnetx_fast/engines.py verify --backend prepared --precision int8
python -m unittest discover -s detect/tests/models -p 'test_*.py'
python tests/run.py detect --build-dir build-app-local-windows
```

A native TensorRT backend smoke test can be built from the repository root:

```sh
g++ -std=c++20 -O2 -Iinc_common -Idetect/inc_public -Idebug/inc_public \
  -Iexternal_libs/spdlog/include -I/usr/local/cuda/include \
  detect/trt_backend.cpp detect/tests/models/backend_smoke.cpp \
  -L/usr/local/cuda/lib64 -lnvinfer -lnvonnxparser -lcudart -pthread \
  -o build/backend-smoke
build/backend-smoke detect/models/regnetx_fast
```

`prepare` also works on a PC without TensorRT. Its CPU reference check never
creates an engine verification receipt. The TensorRT check on the deployment
GPU remains required.

### TensorRT 11 migration checks

Native validation on 2026-11-20 used an 8 GB Jetson Orin Nano, TensorRT
11.4.1.31, CUDA 13.2, L4T 39.2.1 and system OpenCV 4.6:

| GPU engine | Result against supplied float reference |
|---|---|
| RegNetX INT8 | 12/12 batches pass; mean mask IoU 0.989049; minimum 0.983312 |
| Locator INT8 | 12/12 decisions; no peak-cell movement; maximum peak-logit gap 3.174248 |
| Locator FP16 fallback | 12/12 decisions; no peak-cell movement; maximum peak-logit gap 0.070313 |

Both primary engines use all 24 calibration inputs. Engine compilation took
60 seconds for the segmenter and 108 seconds for the locator, excluding CPU
calibration. These are build times, not inference timings.

The Release headless server compiled against TensorRT 11, loaded both verified
INT8 plans, built its FP32 palm/landmark caches, and passed `--selftest` with the
hand filter enabled. Its three synthetic cycles took 171, 88 and 86 ms.
This short run does not establish steady-state throughput or the 50 ms target;
live cameras and the Jetson UI remain untested.

The CPU export tests use NumPy 1.26.4, ONNX 1.23.2 and ONNX Runtime 1.32.0.
All 12 cases per export pass the existing acceptance criteria:

| Export | Result against supplied float reference |
|---|---|
| RegNetX INT8 Q/DQ + FP32 | Mean mask IoU 0.9891; minimum 0.9842 |
| Locator INT8 Q/DQ + FP32 | 12/12 decisions; no peak-cell movement; maximum peak-logit gap 3.4718 |
| RegNetX FP16 | Mean mask IoU 0.9996; minimum 0.9988 |
| Locator FP16 | 12/12 decisions; no peak-cell movement; maximum peak-logit gap 0.0273 |
| Optional MobileNet INT8 Q/DQ + FP32 | Mean mask IoU 0.9799; minimum 0.9693 |

INT8 exports preserve FP32 scales and surrounding operations. Converting the
locator's Q/DQ graph to FP16 passed an unfused CPU check but failed the native
TensorRT 11.4.1 decision check; retaining FP32 fixed it. Separate FP16 exports
remain available as a fallback.

The export tests also check FP32 public I/O, ONNX graph validity, calibration
cache invalidation, and source-file preservation. The Windows DirectML server
still builds and passes its full synthetic self-test after the API cleanup.
CPU checks do not substitute for the on-device TensorRT verification step.

### Historical TensorRT 10 validation (2026-11-15)

On an 8 GB Jetson Orin Nano in 25 W mode, with TensorRT 10.3.0, CUDA 12.6,
Ubuntu 22.04 and system OpenCV 4.8:

- Both primary INT8 engines built with all 24 calibration inputs per model.
  Compilation took about 9 minutes for the segmenter and 18 minutes for the
  locator. This is an offline build cost, not a startup cost.
- Segmenter: all 12 reference batches passed; mean mask IoU 0.992367, minimum
  0.989558 against the supplied float outputs.
- Locator: all 12 dart/no-dart decisions matched; eight cases contained a dart
  in both outputs, with maximum peak movement of one heatmap cell (two input
  pixels). Maximum peak-logit difference was 1.249112.
- The production C++ backend loaded both verified plans and ran three passes
  per model. The Release headless server built through CMake and passed
  `--selftest --no-hand-filter`; installation included both plans and receipts.
- C++ contract tests passed on Windows and ARM64. All 36 ONNX reference cases
  (including the fallback segmenter), the Python verifier tests, the Windows
  detection suites, and the Windows DirectML server self-test passed.

Calibrated engine timing with transfers, measured separately using `trtexec`
with a 1-second warm-up and 5-second measurement window:

| Model | Mean latency | Mean GPU compute |
|---|---:|---:|
| RegNetX segmenter | 10.59 ms | 9.82 ms |
| Heatmap-only locator | 18.50 ms | 16.00 ms |
| Sum of separate measurements | 29.09 ms | 25.82 ms |

Reproduce from this bundle directory with:

```sh
/usr/src/tensorrt/bin/trtexec --loadEngine=engines/seg.int8.plan --warmUp=1000 --duration=5
/usr/src/tensorrt/bin/trtexec --loadEngine=engines/ar.int8.plan --warmUp=1000 --duration=5
```

These timings exclude application preprocessing and postprocessing. The three
synthetic server self-test cycles took 280, 164 and 136 ms, with the hand filter
disabled. Three cycles do not establish steady-state throughput, and the
50 ms end-to-end budget is not yet validated. Live cameras, the Jetson UI,
and free-running scoring accuracy still need testing. The original accuracy
evaluation below remains separate from these reference checks.

## What changes in the application

1. **Segmenter: drop-in.** Same input, output name, shape and meaning as the
   current `dart_seg_unet.onnx`. Different weights and encoder; internally it
   classifies before upsampling, which gives identical logits for less work.
2. **Locator: same weights as current production, one output removed.** The
   `exist_logit` output is gone. It was only ever
   `exist_scale * heatmap.max() + exist_bias`, and computing a max over the
   map on the GPU cost about 0.6 ms. Replace both existing gates with a single
   comparison on the raw heatmap:

       peak = heatmap.max()                      # raw logit, no sigmoid
       dart_present = peak >= PEAK_LOGIT_THRESHOLD
       row, col = argmax(heatmap)                # only if dart_present

   `PEAK_LOGIT_THRESHOLD` = 0.200671 (`models[].decision.peak_logit_threshold`
   in the manifest). It equals `max(logit(0.55), -exist_bias / exist_scale)`,
   so it gives exactly the decisions of "exist_logit >= 0 and sigmoid(peak) >= 0.55".
   exist_scale = 1.900297, exist_bias = 1.693196.
   If you would rather not touch the application yet, the locator ONNX already
   in production is the same network and stays valid next to the new segmenter.
3. **Segmenter mask: `logits > 0`.** Same as sigmoid then 0.5, without the sigmoid.

Everything else is unchanged: preprocessing, the 21-channel input layout
(`multicam_unet_ar_heatmap_only.cond.json`: instance_v1, tip sigma 5.0 px),
masked RGB, slot rules, and the cell-centre conversion
`x = (col + 0.5) * 2`, `y = (row + 0.5) * 2` in 720x720 board pixels.

### Input layouts (unchanged, repeated for reference)

Segmenter: three cameras as one batch; each 1280x720 frame resized to 640x360
(bilinear), RGB, divided by 255.

Locator, NCHW, all values in [0, 1]:

| Channels | Contents |
|---|---|
| 0-8 | cam0, cam1, cam2 RGB, warped to 720x720, **multiplied by the warped segmenter mask** |
| 9-17 | per-dart masks, camera-major: cam0 slots 0/1/2, cam1 slots 0/1/2, cam2 slots 0/1/2 |
| 18-20 | per-dart tip position, Gaussian sigma 5.0 px, one channel per slot |

Raw (unmasked) RGB makes the heatmap go flat without any error.

## Building engines on the Orin

    python3 engines.py build --precision fp16   # safe starting point
    python3 engines.py verify --precision fp16
    python3 engines.py build                    # 8-bit, calibrated on calibration/*.npz
    python3 engines.py verify

Prepared ONNX files, their metadata and engines go to `engines/`. `verify` runs 12
real inputs per model through the engine and compares with the float outputs
stored in `reference/`: mask overlap for the segmenter; dart/no-dart decision
and peak cell for the locator. It exits non-zero and says so if an engine
disagrees. **Ship an 8-bit engine only if its `verify` passes; otherwise use
the fp16 engine for that model.** The two can be mixed.

The 8-bit export uses CPU ONNX Runtime for symmetric min/max calibration over
all bundled inputs. Convolution weights use per-channel signed INT8 and
activations use per-tensor signed INT8, with zero points of zero. Convolution
outputs are left unquantized until needed by a downstream quantized operation;
residual and activation operations use FP32. This avoids entropy clipping of
rare bright dart peaks. The public inputs and outputs remain FP32.

After preparation, `trtexec` can build the explicitly typed graph too:

    trtexec --onnx=engines/seg.int8.onnx --saveEngine=engines/seg.int8.plan
    python3 engines.py verify --models seg

Do not supply `--int8`, `--fp16`, or `--calib` with TensorRT 11.

The original import's timing engines used placeholder ranges. The application
does not accept those timing-only engines: build and verify calibrated plans
with this script, which also checks manifest hashes, tensor formats, and finite
outputs. Validation results for this integration are recorded below.

## Imported timing estimates

The imported bundle reports these timing-only results and estimates for an
Orin in 25 W mode, TensorRT 10.3.0, with transfers:

| | 8-bit + FP16 | FP16 |
|---|---:|---:|
| Segmenter | 10.1 ms | about 13 ms (15.9 ms before the layout change; new layout not measured in FP16) |
| Locator | about 20.6 ms (21.2 ms with the existence output) | about 27.4 ms (28.0 ms) |
| Both | about 31 ms | about 40 ms |

The 8-bit timings came from engines with placeholder ranges, so they measure
speed only. Rebuilding an engine moves a model by 0.5-0.8 ms, because
TensorRT picks each layer's precision by timing it during the build.

## Getting the most out of it

Measured or certain:

- **Build once, load the saved `.plan`.** Never build at startup. Because
  builds vary, build two or three times and keep the fastest.
- **Keep both engines loaded with one execution context each**, and reuse the
  same input/output buffers every frame.
- **Do not sigmoid either output.** Thresholds above are on raw logits.
- **Use 25 W mode (`nvpmodel`) and `jetson_clocks`**; all timings assume it.

Likely gains, not measured here:

- **Upload less of the locator input.** It is 43.5 MB of float32 per frame and
  the copy to the GPU costs about 2.8 ms. Channels 9-20 (other darts' masks
  and tips) change only when a dart is counted, so keep the input buffer on
  the device and re-upload only channels 0-8 each frame.
- **Pinned (page-locked) host buffers** for whatever is uploaded. The integrated
  TensorRT backend already uses these and reuses its allocations.
- **Half-precision or 8-bit input tensors** (`--inputIOFormats=fp16:chw` in
  trtexec terms) halve or quarter the upload. All inputs are 8-bit images to
  begin with. This needs corresponding changes to the C++ buffer API and the
  verifier; the integrated runtime currently requires float32 I/O.
- **Skip the locator when nothing changed.** It only has work to do when the
  segmenter's mask differs from the last counted state.

Earlier TensorRT 10 experiments found explicit Q/DQ 8-bit models slower by
0.9 ms on the segmenter and 3.3 ms on the locator, and found no gain from
batching the three cameras through the locator stem. TensorRT 11 requires
explicit quantization; measure its new plans independently of those results.

## Accuracy

211 real test throws, 358 darts, other darts' true positions supplied:

| Pipeline | Darts found |
|---|---:|
| Current production, float | 354 |
| This bundle, float | 352 |
| This bundle, both models simulated 8-bit | 350 |
| Fallback segmenter + same locator, simulated 8-bit | 351 |

Known gaps: the 8-bit numbers come from a simulator, not from a TensorRT
engine (`verify` is the on-device check); the locator was trained on the
current production segmenter's masks; nothing has been evaluated free-running
on video.

## Fallback segmenter

`fallback/dart_seg_unet_mobilenet_qat.onnx` is the current production
architecture retrained to tolerate 8-bit, in the same fast layout and with
the same interface. It is 1.2 ms slower (11.3 ms) and is here in case the
RegNetX masks cause trouble with the locator in real use:

    python3 engines.py build --models seg-fallback
    python3 engines.py verify --models seg-fallback

## Files

- `dart_seg_unet.onnx`, `multicam_unet_ar_heatmap_only.onnx` (+ `.cond.json`)
- `fallback/dart_seg_unet_mobilenet_qat.onnx`
- `manifest.json`
- `calibration/seg.npz`, `calibration/ar.npz`: real training inputs, uint8;
  divide by 255. The locator set uses this segmenter's masks.
- `reference/<model>.npz`: real inputs (uint8) and float outputs (`expected`).
- `engines.py`: build and verify on the Jetson.
- `prepare_onnx.py`, `requirements-export.txt`: portable INT8/FP16 exports for TensorRT 11.
