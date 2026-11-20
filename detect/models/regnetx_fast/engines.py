#!/usr/bin/env python3
"""Build and check the TensorRT engines of a dart-model production bundle.

Run from the extracted bundle on the Jetson. Needs only what JetPack ships:
Python 3, numpy and the `tensorrt` module. Nothing in the bundle is modified;
everything written goes to ./engines.

  python3 engines.py build                  # 8-bit engines, calibrated on the bundled real inputs
  python3 engines.py build --precision fp16 # float engines (no calibration)
  python3 engines.py verify                 # compare the built engines with the float reference
"""
import argparse
import ctypes
import ctypes.util
import glob
import hashlib
import json
from pathlib import Path
import sys
import time
import zlib

import numpy as np

HERE = Path(__file__).resolve().parent


def load_manifest(root):
    manifest = json.loads((root / "manifest.json").read_text(encoding="utf-8"))
    for model in manifest["models"]:
        if hashlib.sha256((root / model["onnx"]).read_bytes()).hexdigest() != model["onnx_sha256"]:
            raise ValueError(f"{model['onnx']}: checksum does not match manifest.json")
    return manifest


def to_float(batch):
    """Bundled inputs are stored as uint8; the models take float32 in [0, 1]."""
    return np.ascontiguousarray(batch, dtype=np.float32) / np.float32(255)


def select(manifest, wanted):
    models = manifest["models"]
    if wanted:
        unknown = set(wanted) - {m["id"] for m in models}
        if unknown:
            raise SystemExit(f"Unknown model id(s): {sorted(unknown)}")
        return [m for m in models if m["id"] in wanted]
    return [m for m in models if m.get("role") == "primary"]


# ---------------------------------------------------------------------------
# Comparison with the float reference (pure numpy; shared by every backend)
# ---------------------------------------------------------------------------

def compare_segmenter(expected, observed):
    """Mask overlap (IoU) between float and engine masks, per reference batch."""
    overlaps = []
    for want, got in zip(expected, observed):
        a, b = np.asarray(want) > 0, np.asarray(got).reshape(want.shape) > 0
        union = int((a | b).sum())
        overlaps.append(1.0 if union == 0 else float((a & b).sum() / union))
    return {"cases": len(overlaps), "mask_iou_min": min(overlaps),
            "mask_iou_mean": float(np.mean(overlaps)),
            "passed": bool(np.mean(overlaps) >= 0.95 and min(overlaps) >= 0.90)}


def compare_locator(expected, observed, threshold):
    """Same dart/no-dart decision, and the same peak cell where both see a dart."""
    same, shifts, gaps = 0, [], []
    for want, got in zip(expected, observed):
        want = np.asarray(want, dtype=np.float32)
        got = np.asarray(got, dtype=np.float32).reshape(want.shape)
        found = [float(h.max()) >= threshold for h in (want, got)]
        same += found[0] == found[1]
        gaps.append(abs(float(want.max()) - float(got.max())))
        if all(found):
            peaks = [np.unravel_index(int(h.argmax()), h.shape[-2:]) for h in (want, got)]
            shifts.append(float(np.hypot(peaks[0][0] - peaks[1][0], peaks[0][1] - peaks[1][1])))
    worst = max(shifts) if shifts else 0.0
    return {"cases": len(expected), "same_decision": same, "cases_with_dart_in_both": len(shifts),
            "peak_shift_cells_max": worst, "peak_logit_gap_max": max(gaps),
            "passed": bool(same == len(expected) and worst <= 2.0)}


def compare(model, expected, observed):
    if len(expected) == 0 or len(expected) != len(observed):
        return {"passed": False, "error": "Missing reference cases or outputs"}
    for want, got in zip(expected, observed):
        if (np.shape(want) != np.shape(got)
                or not np.isfinite(want).all() or not np.isfinite(got).all()):
            return {"passed": False, "error": "Output shape mismatch or non-finite values"}
    if model["task"] == "seg":
        return compare_segmenter(expected, observed)
    return compare_locator(expected, observed, model["decision"]["peak_logit_threshold"])


# ---------------------------------------------------------------------------
# CUDA memory through libcudart (avoids a pycuda / cuda-python dependency)
# ---------------------------------------------------------------------------

class Cuda:
    def __init__(self):
        candidates = ["libcudart.so"] + sorted(glob.glob("/usr/local/cuda*/lib64/libcudart.so*"), reverse=True)
        found = ctypes.util.find_library("cudart")
        if found:
            candidates.append(found)
        self.lib = None
        for name in candidates:
            try:
                self.lib = ctypes.CDLL(name)
                break
            except OSError:
                continue
        if self.lib is None:
            raise SystemExit("libcudart.so not found. Add CUDA's lib64 folder to LD_LIBRARY_PATH.")
        self.lib.cudaMalloc.argtypes = [ctypes.POINTER(ctypes.c_void_p), ctypes.c_size_t]
        self.lib.cudaMemcpy.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int]
        self.lib.cudaFree.argtypes = [ctypes.c_void_p]
        for call in (self.lib.cudaMalloc, self.lib.cudaMemcpy, self.lib.cudaFree):
            call.restype = ctypes.c_int

    def check(self, status, what):
        if status != 0:
            raise RuntimeError(f"{what} failed with CUDA error {status}")

    def alloc(self, nbytes):
        pointer = ctypes.c_void_p()
        self.check(self.lib.cudaMalloc(ctypes.byref(pointer), nbytes), "cudaMalloc")
        return pointer.value

    def upload(self, pointer, array):
        self.check(self.lib.cudaMemcpy(pointer, array.ctypes.data, array.nbytes, 1), "cudaMemcpy to device")

    def download(self, pointer, array):
        self.check(self.lib.cudaMemcpy(array.ctypes.data, pointer, array.nbytes, 2), "cudaMemcpy to host")

    def free(self, pointer):
        self.lib.cudaFree(pointer)


# ---------------------------------------------------------------------------
# Build
# ---------------------------------------------------------------------------

def engine_path(out, model, precision):
    return out / f"{model['id']}.{precision}.plan"


def receipt_path(path):
    return path.with_suffix(path.suffix + ".verified")


def write_receipt(path, onnx):
    """Content identity for the C++ loader; not a signature or trust boundary."""
    receipt_path(path).write_text(
        "TREBLOTRON_VERIFIED_ENGINE_V1\n"
        f"{zlib.crc32(onnx.read_bytes()):08x} {zlib.crc32(path.read_bytes()):08x}\n",
        encoding="ascii")


def build_engine(root, out, model, precision, workspace_mb, recalibrate):
    import tensorrt as trt
    logger = trt.Logger(trt.Logger.WARNING)
    builder = trt.Builder(logger)
    major = int(trt.__version__.split(".")[0])
    if major != 10:
        raise RuntimeError("This calibrated deployment targets TensorRT 10.x (tested on 10.3). "
                           "TensorRT 11 removed the implicit INT8 calibration API.")
    network = builder.create_network(0)  # TensorRT 10: explicit batch, weak typing
    parser = trt.OnnxParser(network, logger)
    if not parser.parse_from_file(str(root / model["onnx"])):
        errors = "\n".join(str(parser.get_error(i)) for i in range(parser.num_errors))
        raise RuntimeError(f"ONNX parse failed for {model['id']}:\n{errors}")
    config = builder.create_builder_config()
    config.set_memory_pool_limit(trt.MemoryPoolType.WORKSPACE, workspace_mb << 20)
    config.set_flag(trt.BuilderFlag.FP16)
    cuda = pointer = None
    if precision == "int8":
        config.set_flag(trt.BuilderFlag.INT8)
        cache = out / f"{model['id']}.calib.cache"
        cache_identity_path = cache.with_suffix(".cache.json")
        identity = {"onnx_sha256": model["onnx_sha256"],
                    "calibration_sha256": hashlib.sha256((root / model["calibration"]).read_bytes()).hexdigest(),
                    "tensorrt": trt.__version__, "method": "minmax"}
        try:
            cached_identity = json.loads(cache_identity_path.read_text())
        except (OSError, ValueError):
            cached_identity = None
        if cache.exists() and (recalibrate or cached_identity != identity):
            cache.unlink()
        batches = np.load(root / model["calibration"])["inputs"]
        if batches.dtype != np.uint8 or list(batches.shape[1:]) != model["input_shape"] or not len(batches):
            raise ValueError(f"Invalid calibration input shape/dtype for {model['id']}")
        cuda = Cuda()
        pointer = cuda.alloc(int(np.prod(model["input_shape"])) * 4)

        # Min/max ranges, not entropy: a dart is a few bright cells in a mostly
        # empty map, and entropy calibration clips exactly those rare peaks.
        class Calibrator(trt.IInt8MinMaxCalibrator):
            def __init__(self):
                trt.IInt8MinMaxCalibrator.__init__(self)
                self.index = 0

            def get_batch_size(self):
                return int(model["input_shape"][0])

            def get_batch(self, names):
                if list(names) != [model["input_name"]]:
                    raise ValueError(f"Unexpected calibration inputs: {names}")
                if self.index >= len(batches):
                    return None
                cuda.upload(pointer, to_float(batches[self.index]))
                self.index += 1
                return [pointer]

            def read_calibration_cache(self):
                return cache.read_bytes() if cache.exists() else None

            def write_calibration_cache(self, data):
                cache.write_bytes(bytes(data))
                cache_identity_path.write_text(json.dumps(identity, indent=2) + "\n")

        calibrator = Calibrator()
        config.int8_calibrator = calibrator
        source = "existing cache" if cache.exists() else f"{len(batches)} bundled real inputs"
        print(f"  calibration: {source}", flush=True)
    started = time.time()
    try:
        plan = builder.build_serialized_network(network, config)
    finally:
        if cuda is not None:
            cuda.free(pointer)
    if plan is None:
        raise RuntimeError(f"TensorRT could not build {model['id']} ({precision})")
    path = engine_path(out, model, precision)
    path.write_bytes(bytes(plan))
    print(f"  wrote {path.relative_to(root)} in {time.time() - started:.0f} s", flush=True)
    return path


def command_build(args):
    root = args.bundle.resolve()
    manifest = load_manifest(root)
    out = root / "engines"
    out.mkdir(exist_ok=True)
    for model in select(manifest, args.models):
        path = engine_path(out, model, args.precision)
        if path.exists() and not args.rebuild:
            print(f"{model['id']}: {path.name} exists, keeping it (--rebuild to replace)")
            continue
        receipt_path(path).unlink(missing_ok=True)
        print(f"{model['id']}: building {args.precision} engine", flush=True)
        build_engine(root, out, model, args.precision, args.workspace_mb, args.recalibrate)
    print("\nNext: python3 engines.py verify" + ("" if args.precision == "int8" else " --precision fp16"))


# ---------------------------------------------------------------------------
# Verify
# ---------------------------------------------------------------------------

def run_tensorrt(path, model, inputs):
    import tensorrt as trt
    logger = trt.Logger(trt.Logger.WARNING)
    runtime = trt.Runtime(logger)
    engine = runtime.deserialize_cuda_engine(path.read_bytes())
    if engine is None:
        raise RuntimeError(f"Cannot deserialize {path}; rebuild on this device/runtime")
    context = engine.create_execution_context()
    if context is None:
        raise RuntimeError(f"Cannot create execution context for {path}")
    cuda = Cuda()
    names = [engine.get_tensor_name(i) for i in range(engine.num_io_tensors)]
    if set(names) != {model["input_name"], model["output_name"]}:
        raise ValueError(f"Unexpected engine I/O: {names}")
    for direction in ("input", "output"):
        name = model[f"{direction}_name"]
        if (list(engine.get_tensor_shape(name)) != model[f"{direction}_shape"]
                or engine.get_tensor_dtype(name) != trt.float32
                or engine.get_tensor_format(name) != trt.TensorFormat.LINEAR
                or engine.get_tensor_location(name) != trt.TensorLocation.DEVICE
                or engine.get_tensor_mode(name) != (trt.TensorIOMode.INPUT if direction == "input" else trt.TensorIOMode.OUTPUT)):
            raise ValueError(f"{name}: runtime requires the declared shape and linear FP32 device I/O")
    buffers, pointers = {}, []
    outputs = []
    try:
        for name in names:
            shape = tuple(engine.get_tensor_shape(name))
            buffers[name] = np.zeros(shape, dtype=np.float32)
            pointers.append(cuda.alloc(buffers[name].nbytes))
        for batch in inputs:
            cuda.upload(pointers[names.index(model["input_name"])], to_float(batch))
            if not context.execute_v2(pointers):
                raise RuntimeError(f"Inference failed for {path.name}")
            result = buffers[model["output_name"]]
            cuda.download(pointers[names.index(model["output_name"])], result)
            outputs.append(result.copy())
    finally:
        for pointer in pointers:
            cuda.free(pointer)
    return outputs


def run_onnxruntime(root, model, inputs):
    import onnxruntime as ort
    session = ort.InferenceSession(str(root / model["onnx"]), providers=["CPUExecutionProvider"])
    return [session.run([model["output_name"]], {model["input_name"]: to_float(batch)})[0]
            for batch in inputs]


def command_verify(args):
    root = args.bundle.resolve()
    manifest = load_manifest(root)
    results, failed = [], False
    for model in select(manifest, args.models):
        path = engine_path(root / "engines", model, args.precision)
        if args.backend == "tensorrt":
            # A failed or interrupted verification must not leave an old pass.
            receipt_path(path).unlink(missing_ok=True)
        reference = np.load(root / model["reference"])
        inputs, expected = reference["inputs"], reference["expected"]
        if inputs.dtype != np.uint8 or list(inputs.shape[1:]) != model["input_shape"]:
            raise ValueError(f"Invalid reference input shape/dtype for {model['id']}")
        if args.backend == "onnxruntime":
            observed, what = run_onnxruntime(root, model, inputs), "onnx (cpu)"
        else:
            path = engine_path(root / "engines", model, args.precision)
            if not path.exists():
                raise SystemExit(f"{path} not found. Run: python3 engines.py build --precision {args.precision}")
            observed, what = run_tensorrt(path, model, inputs), path.name
        row = dict(model=model["id"], ran=what, **compare(model, expected, observed))
        if args.backend == "tensorrt" and row["passed"]:
            write_receipt(path, root / model["onnx"])
        failed |= not row["passed"]
        results.append(row)
        print(f"{model['id']} [{what}]: {'PASS' if row['passed'] else 'FAIL'}")
        for key, value in row.items():
            if key not in ("model", "ran", "passed"):
                print(f"    {key}: {value:.4f}" if isinstance(value, float) else f"    {key}: {value}")
    if args.backend == "tensorrt":
        out = root / "engines" / f"verify-{args.precision}.json"
        out.write_text(json.dumps(results, indent=2) + "\n", encoding="utf-8")
        print(f"\nWrote {out.relative_to(root)}")
    if failed:
        print("\nFAILED: at least one engine disagrees with the float reference. Do not ship it;"
              "\nuse the fp16 engine for that model instead.")
    return 1 if failed else 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--bundle", type=Path, default=HERE, help="Extracted bundle folder (default: this folder).")
    commands = parser.add_subparsers(dest="command", required=True)
    build = commands.add_parser("build", help="Build TensorRT engines into ./engines.")
    build.add_argument("--precision", choices=["int8", "fp16"], default="int8")
    build.add_argument("--models", nargs="*", help="Model ids (default: the primary pair).")
    build.add_argument("--rebuild", action="store_true", help="Replace an engine that already exists.")
    build.add_argument("--recalibrate", action="store_true", help="Ignore an existing calibration cache.")
    build.add_argument("--workspace-mb", type=int, default=2048)
    verify = commands.add_parser("verify", help="Compare engines with the bundled float reference.")
    verify.add_argument("--precision", choices=["int8", "fp16"], default="int8")
    verify.add_argument("--models", nargs="*", help="Model ids (default: the primary pair).")
    verify.add_argument("--backend", choices=["tensorrt", "onnxruntime"], default="tensorrt",
                        help="onnxruntime checks the ONNX files themselves on any machine.")
    args = parser.parse_args(argv)
    return command_build(args) if args.command == "build" else command_verify(args)


if __name__ == "__main__":
    sys.exit(main() or 0)
