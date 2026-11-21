"""CPU export checks; install the bundle's requirements-export.txt first."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

import numpy as np
import onnx
import onnxruntime as ort
from onnx import helper, numpy_helper, TensorProto

SCRIPT = Path(__file__).resolve().parents[2] / 'models/regnetx_fast/prepare_onnx.py'
spec = importlib.util.spec_from_file_location('prepare_onnx', SCRIPT)
prepare_onnx = importlib.util.module_from_spec(spec)
spec.loader.exec_module(prepare_onnx)


class ExportTests(unittest.TestCase):
    def test_precision_exports_preserve_io_and_invalidate_changed_calibration(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / 'source.onnx'
            shape = [1, 1, 2, 2]
            graph = helper.make_graph(
                [helper.make_node('Conv', ['input', 'weight'], ['logits'], name='conv')], 'tiny',
                [helper.make_tensor_value_info('input', TensorProto.FLOAT, shape)],
                [helper.make_tensor_value_info('logits', TensorProto.FLOAT, shape)],
                [numpy_helper.from_array(np.ones((1, 1, 1, 1), dtype=np.float32), 'weight')])
            onnx.save(helper.make_model(graph, opset_imports=[helper.make_opsetid('', 17)], ir_version=8), source)
            original = source.read_bytes()
            batches = np.array([[[[[0, 85], [170, 255]]]]], dtype=np.uint8)
            np.savez(root / 'calibration.npz', inputs=batches)
            model = {'id': 'tiny', 'onnx': 'source.onnx', 'calibration': 'calibration.npz',
                     'input_name': 'input', 'input_shape': shape}
            for precision in ('fp16', 'int8'):
                path = prepare_onnx.prepare(root, root / 'engines', model, precision)
                graph = onnx.load(path)
                onnx.checker.check_model(graph, full_check=True)
                self.assertEqual(graph.graph.input[0].type.tensor_type.elem_type, TensorProto.FLOAT)
                self.assertEqual(graph.graph.output[0].type.tensor_type.elem_type, TensorProto.FLOAT)
                self.assertEqual(any(n.op_type == 'QuantizeLinear' for n in graph.graph.node), precision == 'int8')
                if precision == 'int8':
                    self.assertFalse(any(v.data_type == TensorProto.FLOAT16 for v in graph.graph.initializer))
                    types = {v.name: v.data_type for v in graph.graph.initializer}
                    for node in graph.graph.node:
                        if node.op_type in ('QuantizeLinear', 'DequantizeLinear'):
                            self.assertEqual(types[node.input[1]], TensorProto.FLOAT)
                session = ort.InferenceSession(str(path), providers=['CPUExecutionProvider'])
                values = batches[0].astype(np.float32) / 255
                np.testing.assert_allclose(session.run(None, {'input': values})[0], values, atol=.01)
                stamp = path.stat().st_mtime_ns
                prepare_onnx.prepare(root, root / 'engines', model, precision)
                self.assertEqual(path.stat().st_mtime_ns, stamp)
            self.assertEqual(source.read_bytes(), original)
            metadata_path = path.with_suffix('.onnx.json')
            previous = json.loads(metadata_path.read_text())
            np.savez(root / 'calibration.npz', inputs=batches // 2)
            prepare_onnx.prepare(root, root / 'engines', model, 'int8')
            self.assertNotEqual(json.loads(metadata_path.read_text())['recipe'], previous['recipe'])


if __name__ == '__main__':
    unittest.main()
