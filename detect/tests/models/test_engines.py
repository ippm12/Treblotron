"""Run with a Python environment containing numpy; TensorRT is not required."""
import argparse
import importlib.util
import json
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

import numpy as np

SCRIPT = Path(__file__).resolve().parents[2] / 'models/regnetx_fast/engines.py'
spec = importlib.util.spec_from_file_location('engines', SCRIPT)
engines = importlib.util.module_from_spec(spec)
spec.loader.exec_module(engines)


class VerificationTests(unittest.TestCase):
    def test_tensor_rt_builder_uses_graph_precision(self):
        # This fake deliberately has no precision flags or calibrator API.
        # A regression to weak typing must fail even without a GPU in CI.
        with tempfile.TemporaryDirectory() as folder:
            source, path = Path(folder) / 'typed.onnx', Path(folder) / 'test.plan'
            parsed = []
            config = SimpleNamespace(set_memory_pool_limit=lambda *_: None)
            builder = SimpleNamespace(create_network=lambda flags: object(),
                                      create_builder_config=lambda: config,
                                      build_serialized_network=lambda *_: b'plan')
            class Logger:
                WARNING = 1
                def __init__(self, *_): pass
            trt = SimpleNamespace(Logger=Logger, Builder=lambda _: builder,
                                  OnnxParser=lambda *_: SimpleNamespace(
                                      parse_from_file=lambda name: parsed.append(name) or True),
                                  MemoryPoolType=SimpleNamespace(WORKSPACE=1))
            engines.build_engine(source, path, 32, trt)
            self.assertEqual(parsed, [str(source)])
            self.assertEqual(path.read_bytes(), b'plan')

    def test_missing_nonfinite_or_wrong_shape_outputs_fail(self):
        model = {'task': 'seg'}
        expected = [np.ones((1, 1, 2, 2), dtype=np.float32)]
        for observed in ([], [np.full((1, 1, 2, 2), np.nan)], [np.ones((2, 2))]):
            self.assertFalse(engines.compare(model, expected, observed)['passed'])

    def test_locator_checks_absent_cases_and_peak_movement(self):
        expected = np.full((2, 1, 6, 6), -1, dtype=np.float32)
        expected[0, 0, 0, 0] = 1
        model = {'task': 'ar', 'decision': {'peak_logit_threshold': .200670695}}
        self.assertTrue(engines.compare(model, expected, expected)['passed'])
        changed = expected.copy()
        changed[1, 0, 0, 0] = 1
        self.assertFalse(engines.compare(model, expected, changed)['passed'])
        changed = expected.copy()
        changed[0, 0, 0, 0], changed[0, 0, 5, 5] = -1, 1
        self.assertFalse(engines.compare(model, expected, changed)['passed'])

    def test_failed_verification_revokes_previous_receipt(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / 'engines').mkdir()
            (root / 'model.onnx').write_bytes(b'model')
            model = {'id': 'seg', 'role': 'primary', 'task': 'seg', 'onnx': 'model.onnx',
                     'reference': 'reference.npz', 'input_shape': [1, 1, 2, 2]}
            inputs = np.ones((1, 1, 1, 2, 2), dtype=np.uint8)
            expected = np.ones_like(inputs, dtype=np.float32)
            np.savez(root / 'reference.npz', inputs=inputs, expected=expected)
            plan = root / 'engines/seg.int8.plan'
            plan.write_bytes(b'engine')
            args = argparse.Namespace(bundle=root, models=None, precision='int8', backend='tensorrt')
            with patch.object(engines, 'load_manifest', return_value={'models': [model]}):
                with patch.object(engines, 'run_tensorrt', return_value=expected):
                    self.assertEqual(engines.command_verify(args), 0)
                    self.assertTrue(engines.receipt_path(plan).exists())
                with patch.object(engines, 'run_tensorrt', return_value=-expected):
                    self.assertEqual(engines.command_verify(args), 1)
                    self.assertFalse(engines.receipt_path(plan).exists())
                engines.receipt_path(plan).write_text('old receipt')
                with patch.object(engines, 'run_tensorrt', side_effect=RuntimeError('incompatible engine')):
                    with self.assertRaises(RuntimeError):
                        engines.command_verify(args)
                    self.assertFalse(engines.receipt_path(plan).exists())

    def test_prepared_onnx_check_does_not_approve_an_engine(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / 'engines').mkdir()
            model = {'id': 'seg', 'role': 'primary', 'task': 'seg',
                     'reference': 'reference.npz', 'input_shape': [1, 1, 2, 2]}
            inputs = np.ones((1, 1, 1, 2, 2), dtype=np.uint8)
            expected = inputs.astype(np.float32)
            np.savez(root / 'reference.npz', inputs=inputs, expected=expected)
            args = argparse.Namespace(bundle=root, models=None, precision='int8', backend='prepared')
            with patch.object(engines, 'load_manifest', return_value={'models': [model]}), \
                 patch.object(engines, 'run_onnxruntime', return_value=expected):
                self.assertEqual(engines.command_verify(args), 0)
            self.assertFalse(list((root / 'engines').glob('*.verified')))


if __name__ == '__main__':
    unittest.main()
