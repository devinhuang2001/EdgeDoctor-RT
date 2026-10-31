import importlib.util
import unittest
from pathlib import Path


def load_script():
    path = Path(__file__).parents[2] / "scripts" / "test-board-v2.py"
    spec = importlib.util.spec_from_file_location("test_board_v2", path)
    module = importlib.util.module_from_spec(spec)
    assert spec and spec.loader
    spec.loader.exec_module(module)
    return module


class BoardAcceptanceTests(unittest.TestCase):
    def setUp(self):
        self.module = load_script()

    def sample(self, count, quality=1, **extra):
        return {"samples": count, "quality": quality, "drops": 0,
                "gaps": 0, "bus_errors": 0, "measured_millihz": 208000,
                **extra}

    def mqtt(self, connected=1):
        return {"connected": connected, "queued": 0, "sent": 1,
                "persisted": 1, "rejected": 0}

    def test_single_snapshot_cannot_pass(self):
        ok, reasons = self.module.evaluate_stability(
            [self.sample(208)], [self.mqtt()], [], 30, 30)
        self.assertFalse(ok)
        self.assertIn("fewer_than_two_snapshots", reasons)

    def test_missing_measured_rate_cannot_pass(self):
        ok, reasons = self.module.evaluate_stability(
            [self.sample(208, measured_millihz=0),
             self.sample(416, measured_millihz=0)], [self.mqtt()], [], 30, 30)
        self.assertFalse(ok)
        self.assertIn("missing_measured_rate", reasons)

    def test_measured_rate_after_warmup_can_pass(self):
        ok, reasons = self.module.evaluate_stability(
            [self.sample(208, measured_millihz=0), self.sample(416)],
            [self.mqtt()], [], 30, 30)
        self.assertTrue(ok, reasons)

    def test_disconnected_mqtt_cannot_pass(self):
        ok, reasons = self.module.evaluate_stability(
            [self.sample(208), self.sample(416)], [self.mqtt(0)], [], 30, 30)
        self.assertFalse(ok)
        self.assertIn("mqtt_never_connected", reasons)

    def test_transient_disconnect_and_overflow_cannot_be_hidden_by_recovery(self):
        bad = {**self.mqtt(0), "inbox_dropped": 1}
        ok, reasons = self.module.evaluate_stability([self.sample(208), self.sample(416)], [bad, self.mqtt()], [], 30, 30)
        self.assertFalse(ok)
        self.assertIn("mqtt_disconnected_during_capture", reasons)
        self.assertIn("mqtt_inbox_overflow", reasons)

    def test_mid_capture_reset_cannot_pass(self):
        ok, reasons = self.module.evaluate_stability([self.sample(208), self.sample(10), self.sample(416)], [self.mqtt()], [], 30, 30)
        self.assertFalse(ok)
        self.assertIn("sample_counter_reset_or_stalled", reasons)

    def test_heap_growth_or_missing_evidence_cannot_pass_long_run(self):
        self.assertEqual(self.module.parse_heap('heap 924592 660840 307904'), {"heap": 616688})
        self.assertIn("missing_long_heap_evidence", self.module.evaluate_heap([], 28800))
        steady = [{"elapsed_seconds": n * 60, "pools": {"heap": 100000}} for n in range(481)]
        self.assertEqual(self.module.evaluate_heap(steady, 28800), [])
        growing = [{"elapsed_seconds": n * 60, "pools": {"heap": 100000 + n * 20}} for n in range(481)]
        self.assertIn("heap_growth_exceeds_4KiB:heap", self.module.evaluate_heap(growing, 28800))
        self.assertIn("continued_heap_growth:heap", self.module.evaluate_heap(growing, 28800))

    def test_warmup_quality_is_not_sufficient_evidence(self):
        ok, reasons = self.module.evaluate_stability(
            [self.sample(208, quality=0), self.sample(416, quality=0)],
            [self.mqtt(), self.mqtt()], [], 30, 30)
        self.assertFalse(ok)
        self.assertIn("quality_never_valid", reasons)

    def test_counter_must_increase(self):
        ok, reasons = self.module.evaluate_stability(
            [self.sample(208), self.sample(208)], [self.mqtt(), self.mqtt()], [], 30, 30)
        self.assertFalse(ok)
        self.assertIn("sample_counter_not_increasing", reasons)

    def test_serial_interruption_cannot_pass(self):
        ok, reasons = self.module.evaluate_stability(
            [self.sample(208), self.sample(416)], [self.mqtt(), self.mqtt()],
            ["serial lost"], 30, 30)
        self.assertFalse(ok)
        self.assertIn("serial_or_capture_error", reasons)

    def test_short_runtime_cannot_pass(self):
        ok, reasons = self.module.evaluate_stability(
            [self.sample(208), self.sample(416)], [self.mqtt(), self.mqtt()], [], 30, 5)
        self.assertFalse(ok)
        self.assertIn("insufficient_runtime", reasons)

    def test_parse_live_nan_snapshot_and_rate_fields(self):
        text = (
            "edgeimu sample\\n"
            "edgeimu: quality=1 rms_x=0.1 baseline=0 deviation=nan\\n"
            "edgeimu info\\n"
            "edgeimu: running=1 nominal_hz=208 measured_millihz=204944 "
            "samples=5664 drops=0 gaps=0 bus_errors=0 queue_high=0\\n"
            "edmqtt info\\n"
            "edmqtt: connected=0 queued=0 sent=0 persisted=0 rejected=0\\n"
        )
        stats, sample, mqtt = self.module.parse_capture(text)
        self.assertEqual(stats["measured_millihz"], 204944)
        self.assertEqual(sample["quality"], 1)
        self.assertTrue(sample["deviation"] != sample["deviation"])
        self.assertEqual(mqtt["connected"], 0)


if __name__ == "__main__":
    unittest.main()
