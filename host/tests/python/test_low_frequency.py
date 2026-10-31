import numpy as np
import unittest

from edgedoctor.low_frequency import compute_low_frequency_features


class LowFrequencyTests(unittest.TestCase):
    def test_sine_rms_and_band_match_contract(self):
        n = 208
        t = np.arange(n, dtype=np.float32) / 208.0
        xyz = np.zeros((n, 3), dtype=np.float32)
        xyz[:, 0] = 0.1 * np.sin(2 * np.pi * 10 * t)
        result = compute_low_frequency_features(xyz, 208)
        self.assertTrue(0.069 < result["values"][0] < 0.072)
        self.assertGreater(result["band_energy_g2"][1], result["band_energy_g2"][0])
        self.assertEqual(result["values"].shape, (15,))

    def test_invalid_input_rejected(self):
        with self.assertRaises(ValueError):
            compute_low_frequency_features(np.zeros((32, 3)), 208)
