import importlib.util
from pathlib import Path
import unittest
import numpy as np

source = Path(__file__).resolve().parents[2] / "scripts/test-board-inference.py"
spec = importlib.util.spec_from_file_location("board_inference", source)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class InferenceAcceptanceTests(unittest.TestCase):
    def setUp(self):
        self.expected = np.asarray([[5, 2, -1, -2, 0], [-3, 4, 1, 0, -1]], dtype=np.int8)
        self.raw = "".join(
            f"ednn_csv,{name},{i}," + ",".join(map(str, row)) + "\n"
            for name in ("tflm-cmsis-nn", "ethos-u55-128")
            for i, row in enumerate(self.expected))

    def test_missing_backend_fails(self):
        self.assertEqual(module.evaluate("", self.expected)["status"], "FAIL")

    def test_complete_outputs_pass(self):
        self.assertEqual(module.evaluate(self.raw, self.expected)["status"], "PASS")

    def test_duplicate_or_corrupted_output_fails(self):
        self.assertEqual(module.evaluate(self.raw + self.raw, self.expected)["status"], "FAIL")
        changed = self.raw.replace("5,2,-1,-2,0", "1,2,-1,-2,0", 1)
        self.assertEqual(module.evaluate(changed, self.expected)["status"], "FAIL")


if __name__ == "__main__":
    unittest.main()
