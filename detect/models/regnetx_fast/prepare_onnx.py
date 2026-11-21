"""Portable precision exports for TensorRT 11; never modifies the source ONNX.

Uses CPU ONNX Runtime for min/max calibration, so preparation can also run on
a workstation. TensorRT plans still have to be built and verified on the GPU.
"""
import hashlib
import json
from pathlib import Path
import tempfile

import numpy as np


def sha256(path):
    with Path(path).open('rb') as stream:
        digest = hashlib.sha256()
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
        return digest.hexdigest()


def prepare(root, out, model, precision, rebuild=False):
    try:
        import onnx
        import onnxruntime as ort
        from onnxruntime.quantization import (
            CalibrationDataReader, CalibrationMethod, QuantFormat, QuantType, quantize_static,
        )
        from onnxruntime.quantization.shape_inference import quant_pre_process
        from onnxruntime.transformers.float16 import convert_float_to_float16
        from onnxruntime.transformers.onnx_model import OnnxModel
    except ImportError as error:
        raise RuntimeError('TensorRT 11 export needs ONNX and ONNX Runtime. Install '
                           'requirements-export.txt in a Python environment that can also '
                           'import the JetPack tensorrt module.') from error

    source = root / model['onnx']
    recipe = {'version': 2, 'precision': precision, 'source_sha256': sha256(source),
              'onnx': onnx.__version__, 'onnxruntime': ort.__version__,
              'numpy': np.__version__}
    if precision == 'int8':
        recipe['calibration_sha256'] = sha256(root / model['calibration'])
    path = out / f"{model['id']}.{precision}.onnx"
    identity_path = path.with_suffix('.onnx.json')
    try:
        previous = json.loads(identity_path.read_text())
        if (not rebuild and previous['recipe'] == recipe and path.is_file()
                and previous['onnx_sha256'] == sha256(path)):
            print(f'  keeping prepared {path.name}', flush=True)
            return path
    except (OSError, ValueError, KeyError):
        pass

    identity_path.unlink(missing_ok=True)
    out.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='prepare-', dir=out) as temporary:
        temporary = Path(temporary)
        graph = onnx.load(str(source))
        if precision == 'int8':
            input_path = temporary / 'source.onnx'
            onnx.save(graph, str(input_path))
            preprocessed = temporary / 'preprocessed.onnx'
            # Basic constant folding resolves initializer aliases in the locator.
            quant_pre_process(str(input_path), str(preprocessed), skip_symbolic_shape=True)
            with np.load(root / model['calibration']) as data:
                batches = data['inputs']
            if (batches.dtype != np.uint8 or not len(batches)
                    or list(batches.shape[1:]) != model['input_shape']):
                raise ValueError(f"Invalid calibration inputs for {model['id']}")

            class Reader(CalibrationDataReader):
                def __init__(self):
                    self.set_range(0, len(batches))

                def __len__(self):
                    return len(batches)

                def set_range(self, start_index, end_index):
                    self.index, self.end = start_index, end_index

                def get_next(self):
                    if self.index >= self.end:
                        return None
                    batch = np.ascontiguousarray(batches[self.index], dtype=np.float32) / np.float32(255)
                    self.index += 1
                    print(f'  calibration {self.index}/{len(batches)}', flush=True)
                    return {model['input_name']: batch}

            quantized = temporary / 'qdq.onnx'
            quantize_static(
                str(preprocessed), str(quantized), Reader(), quant_format=QuantFormat.QDQ,
                op_types_to_quantize=['Conv'], per_channel=True,
                activation_type=QuantType.QInt8, weight_type=QuantType.QInt8,
                calibrate_method=CalibrationMethod.MinMax,
                calibration_providers=['CPUExecutionProvider'],
                extra_options={'ActivationSymmetric': True, 'WeightSymmetric': True,
                               'CalibTensorRangeSymmetric': True, 'CalibStridedMinMax': 1,
                               'QuantizeBias': False,
                               'OpTypesToExcludeOutputQuantization': ['Conv']})
            graph = onnx.load(str(quantized))

        if precision == 'fp16':
            # Strong typing replaces the removed FP16 builder flag. The
            # converter appends I/O Casts, so restore topological order.
            graph = convert_float_to_float16(graph, keep_io_types=True)
            OnnxModel(graph).topological_sort()
        # INT8 keeps FP32 scales and surrounding operations. Converting its
        # Q/DQ graph to FP16 lost locator detections on TensorRT 11.4.1 even
        # though unfused CPU evaluation passed. Verify the actual GPU engine.
        onnx.checker.check_model(graph, full_check=True)
        for value in list(graph.graph.input) + list(graph.graph.output):
            if value.type.tensor_type.elem_type != onnx.TensorProto.FLOAT:
                raise ValueError(f'{value.name}: prepared graph must expose FP32 I/O')
        if precision == 'int8' and not any(n.op_type == 'QuantizeLinear' for n in graph.graph.node):
            raise ValueError('INT8 export produced no explicit quantization nodes')
        candidate = temporary / 'prepared.onnx'
        onnx.save(graph, str(candidate))
        candidate.replace(path)

    identity_path.write_text(json.dumps({'recipe': recipe, 'onnx_sha256': sha256(path)}, indent=2) + '\n')
    print(f'  wrote {path.name} (explicit {precision}, FP32 I/O)', flush=True)
    return path
