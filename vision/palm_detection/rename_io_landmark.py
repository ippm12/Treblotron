"""
Rename MediaPipe hand-landmark ONNX I/O tensors to canonical names the
C++ runtime expects. tf2onnx emits generic `input_1` / `Identity{,_1,_2,_3}`
names; we rewrite those to stable shape-bearing names so the C++ binding
code does not depend on the exporter's suffix scheme.

Usage:
    python rename_io_landmark.py [path/to/hand_landmarks_detector.onnx]
"""

from __future__ import annotations
import sys
from pathlib import Path

import onnx


CANONICAL_INPUT = "input"

OUTPUT_RENAMES = {
    "Identity":   "landmarks",
    "Identity_1": "presence",
    "Identity_2": "handedness",
    "Identity_3": "world_landmarks",
}

EXPECTED_SHAPES = {
    "landmarks":       [1, 63],
    "presence":        [1, 1],
    "handedness":      [1, 1],
    "world_landmarks": [1, 63],
}


def rename(path: Path) -> None:
    model = onnx.load(str(path))
    g = model.graph

    renames: dict[str, str] = {}

    if len(g.input) != 1:
        raise SystemExit(f"Expected 1 input tensor, found {len(g.input)}")
    if g.input[0].name != CANONICAL_INPUT:
        renames[g.input[0].name] = CANONICAL_INPUT

    if len(g.output) != 4:
        raise SystemExit(f"Expected 4 output tensors, found {len(g.output)}")

    for out in g.output:
        target = OUTPUT_RENAMES.get(out.name)
        if target is None and out.name in OUTPUT_RENAMES.values():
            target = out.name
        if target is None:
            raise SystemExit(
                f"Output {out.name!r} not in expected set "
                f"{sorted(OUTPUT_RENAMES)} — re-export may have changed.")
        dims = [d.dim_value for d in out.type.tensor_type.shape.dim]
        if dims != EXPECTED_SHAPES[target]:
            raise SystemExit(
                f"Output {out.name!r} (→ {target!r}) has shape {dims}, "
                f"expected {EXPECTED_SHAPES[target]}")
        if out.name != target:
            renames[out.name] = target

    if not renames:
        print(f"{path}: I/O names already canonical — nothing to do.")
        return

    print(f"{path}: applying renames:")
    for old, new in renames.items():
        print(f"  {old!r} -> {new!r}")

    for tensor in list(g.input) + list(g.output):
        if tensor.name in renames:
            tensor.name = renames[tensor.name]
    for node in g.node:
        for i, name in enumerate(node.input):
            if name in renames:
                node.input[i] = renames[name]
        for i, name in enumerate(node.output):
            if name in renames:
                node.output[i] = renames[name]

    onnx.checker.check_model(model)
    onnx.save(model, str(path))
    print(f"{path}: saved.")


def main() -> None:
    arg = (Path(sys.argv[1]) if len(sys.argv) > 1
           else Path("hand_landmarks_detector.onnx"))
    if not arg.exists():
        raise SystemExit(f"{arg}: file not found")
    rename(arg)


if __name__ == "__main__":
    main()
