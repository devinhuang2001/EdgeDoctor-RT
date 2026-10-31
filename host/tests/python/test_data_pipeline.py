from __future__ import annotations

from dataclasses import replace
from pathlib import Path
import tempfile
import unittest

import numpy as np
from scipy.io import savemat

from edgedoctor.edgepack import EdgePack, EdgePackError, EdgePackHeader, HEADER_BYTES
from edgedoctor.kaist import (
    KaistMatError,
    load_audio_recording,
    load_mat_signals,
    load_vibration_recording,
)
from edgedoctor.manifest import (
    LABELS,
    ManifestError,
    Recording,
    pair_recordings,
    parse_kaist_name,
)
from edgedoctor.preprocess import (
    audio_logmel,
    physical_to_int16,
    resample_float,
    resample_int16,
    vibration_log_spectrum,
    vibration_statistics,
)
from edgedoctor.split import lolo_splits, recording_split


class ManifestTests(unittest.TestCase):
    def test_parse_all_fault_names(self) -> None:
        cases = {
            "0Nm_normal_none.mat": (0, "normal"),
            "2Nm_bearing_inner_race_07.mat": (2, "bearing_inner"),
            "0Nm_BPFI_03.mat": (0, "bearing_inner"),
            "4Nm_bearing_outer_race_09.mat": (4, "bearing_outer"),
            "0Nm_BPFO_10.mat": (0, "bearing_outer"),
            "2Nm_shaft_misalignment_10.mat": (2, "misalignment"),
            "0Nm_Misalign_03.mat": (0, "misalignment"),
            "4Nm_rotor_unbalance_15.mat": (4, "unbalance"),
            "2Nm_Unbalalnce_0583mg.mat": (2, "unbalance"),
        }
        for name, expected in cases.items():
            with self.subTest(name=name):
                parsed = parse_kaist_name(name)
                self.assertEqual((parsed.load_nm, parsed.fault), expected)

    def test_rejects_ambiguous_or_unknown_name(self) -> None:
        for name in ("normal.mat", "0Nm_2Nm_normal.mat", "8Nm_normal.mat", "0Nm_magic.mat"):
            with self.subTest(name=name), self.assertRaises(ManifestError):
                parse_kaist_name(name)

    def test_pairing_is_one_to_one_and_hashes_files(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            audio = root / "audio" / "0Nm_normal_none.mat"
            vibration = root / "vibration" / audio.name
            audio.parent.mkdir()
            vibration.parent.mkdir()
            metadata = {
                "function_record": {
                    "TL_export_properties_annotation": {
                        "absolute_time": "2026-08-30 12:00:00 ms 123.000"
                    }
                }
            }
            savemat(audio, {"Signal": metadata})
            savemat(vibration, {"Signal": metadata})
            pair = pair_recordings([audio], [vibration])
            self.assertEqual(len(pair), 1)
            self.assertEqual(pair[0].label, LABELS["normal"])
            self.assertEqual(len(pair[0].audio_sha256), 64)
            with self.assertRaises(ManifestError):
                pair_recordings([audio], [])

    def test_pairing_rejects_condition_match_without_time_sync(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            audio = root / "audio" / "0Nm_normal_none.mat"
            vibration = root / "vibration" / audio.name
            audio.parent.mkdir()
            vibration.parent.mkdir()
            def metadata(timestamp: str) -> dict[str, object]:
                return {
                    "Signal": {
                        "function_record": {
                            "TL_export_properties_annotation": {"absolute_time": timestamp}
                        }
                    }
                }
            savemat(audio, metadata("2026-08-30 12:00:00 ms 000.000"))
            savemat(vibration, metadata("2026-08-30 12:00:01 ms 000.000"))
            with self.assertRaisesRegex(ManifestError, "not synchronized"):
                pair_recordings([audio], [vibration])


class KaistMatTests(unittest.TestCase):
    @staticmethod
    def _signal(samples: np.ndarray, increment: float) -> dict[str, object]:
        return {
            "x_values": {
                "start_value": 0.0,
                "increment": increment,
                "number_of_values": samples.size,
            },
            "y_values": {"values": samples},
        }

    def test_reads_real_nested_layout_and_sample_rate(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "0Nm_Normal.mat"
            samples = np.linspace(-1.0, 1.0, 512, dtype=np.float64)
            savemat(path, {"Signal": self._signal(samples, 1.0 / 51200.0)})
            rate, actual = load_audio_recording(path)
            self.assertEqual(rate, 51200)
            np.testing.assert_array_equal(actual, samples)

    def test_exposes_multiple_channels_but_audio_rejects_ambiguity(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "signals.mat"
            samples = np.arange(16, dtype=np.float64)
            savemat(
                path,
                {
                    "Channel_A": self._signal(samples, 1.0 / 25600.0),
                    "Channel_B": self._signal(-samples, 1.0 / 25600.0),
                },
            )
            signals = load_mat_signals(path)
            self.assertEqual([signal.name for signal in signals], ["Channel_A", "Channel_B"])
            with self.assertRaisesRegex(KaistMatError, "exactly one"):
                load_audio_recording(path)

    def test_rejects_nonfinite_samples(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "bad.mat"
            savemat(path, {"Signal": self._signal(np.array([0.0, np.nan]), 0.001)})
            with self.assertRaisesRegex(KaistMatError, "NaN"):
                load_mat_signals(path)

    def test_selects_housing_a_xy_and_applies_g_conversion(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "0Nm_Normal.mat"
            raw = np.arange(32, dtype=np.float64).reshape(8, 4)
            signal = self._signal(raw, 1.0 / 25600.0)
            signal["y_values"]["quantity"] = {
                "label": "g",
                "unit_transformation": {"factor": 0.5, "offset": 1.0},
            }
            savemat(path, {"Signal": signal})
            rate, vibration = load_vibration_recording(path)
            self.assertEqual(rate, 25600)
            np.testing.assert_array_equal(vibration, raw[:, :2] * 0.5 + 1.0)


def _recordings(per_stratum: int = 7) -> list[Recording]:
    items = []
    identifier = 1
    for load in (0, 2, 4):
        for label, fault in enumerate(LABELS):
            for repetition in range(per_stratum):
                key = f"{load}nm_{fault}_{repetition:02d}"
                items.append(
                    Recording(
                        recording_id=identifier,
                        recording_key=key,
                        load_nm=load,
                        fault=fault,
                        label=label,
                        severity=str(repetition),
                        audio_path=f"a/{key}.mat",
                        vibration_path=f"v/{key}.mat",
                    )
                )
                identifier += 1
    return items


class SplitTests(unittest.TestCase):
    def test_e1_is_deterministic_disjoint_and_complete(self) -> None:
        recordings = _recordings()
        first = recording_split(recordings)
        second = recording_split(list(reversed(recordings)))
        self.assertEqual(
            {name: [item.recording_id for item in values] for name, values in first.items()},
            {name: [item.recording_id for item in values] for name, values in second.items()},
        )
        sets = [{item.recording_id for item in first[name]} for name in ("train", "val", "test")]
        self.assertFalse(sets[0] & sets[1] or sets[0] & sets[2] or sets[1] & sets[2])
        self.assertEqual(len(set.union(*sets)), len(recordings))

    def test_lolo_holds_out_only_requested_load(self) -> None:
        folds = lolo_splits(_recordings())
        for held_out in (0, 2, 4):
            fold = folds[f"test_load_{held_out}nm"]
            self.assertEqual({item.load_nm for item in fold["test"]}, {held_out})
            self.assertNotIn(held_out, {item.load_nm for item in fold["train"]})


class EdgePackTests(unittest.TestCase):
    def setUp(self) -> None:
        self.audio = np.arange(16000, dtype=np.int16) - 8000
        self.vibration = np.arange(6400 * 2, dtype=np.int16).reshape(6400, 2) - 6400
        self.header = EdgePackHeader(
            recording_id=123,
            dataset_id=1,
            label=4,
            severity=2,
            load_x10_nm=40,
            chunk_ms=20,
            audio_rate=16000,
            vibration_rate=6400,
            vibration_channels=2,
            audio_samples=len(self.audio),
            vibration_frames=len(self.vibration),
        )

    def test_round_trip_is_bit_exact_and_synchronized(self) -> None:
        encoded = EdgePack.create(self.header, self.audio, self.vibration).to_bytes()
        self.assertEqual(len(encoded), HEADER_BYTES + 2 * (16000 + 6400 * 2))
        decoded = EdgePack.from_bytes(encoded)
        np.testing.assert_array_equal(decoded.audio, self.audio)
        np.testing.assert_array_equal(decoded.vibration, self.vibration)

    def test_corruption_and_truncation_are_rejected(self) -> None:
        encoded = bytearray(EdgePack.create(self.header, self.audio, self.vibration).to_bytes())
        encoded[-1] ^= 0x80
        with self.assertRaisesRegex(EdgePackError, "CRC32"):
            EdgePack.from_bytes(bytes(encoded))
        with self.assertRaises(EdgePackError):
            EdgePack.from_bytes(bytes(encoded[:-1]))

    def test_duration_mismatch_is_rejected(self) -> None:
        bad = replace(self.header, vibration_frames=6399)
        with self.assertRaisesRegex(EdgePackError, "synchronized"):
            EdgePack.create(bad, self.audio, self.vibration[:-1])


class PreprocessTests(unittest.TestCase):
    def test_reference_feature_shapes_and_finiteness(self) -> None:
        t_audio = np.arange(16000) / 16000.0
        audio = np.rint(12000 * np.sin(2 * np.pi * 1000 * t_audio)).astype(np.int16)
        t_vibration = np.arange(6400) / 6400.0
        vibration = np.stack(
            [
                np.rint(9000 * np.sin(2 * np.pi * 200 * t_vibration)),
                np.rint(7000 * np.sin(2 * np.pi * 400 * t_vibration)),
            ],
            axis=1,
        ).astype(np.int16)
        audio_feature = audio_logmel(audio)
        vibration_feature = vibration_log_spectrum(vibration)
        stats = vibration_statistics(vibration)
        self.assertEqual(audio_feature.shape, (40, 98))
        self.assertEqual(vibration_feature.shape, (64, 49, 2))
        self.assertEqual(stats.shape, (12,))
        self.assertTrue(np.isfinite(audio_feature).all())
        self.assertTrue(np.isfinite(vibration_feature).all())
        self.assertTrue(np.isfinite(stats).all())

    def test_resample_preserves_duration_and_tone(self) -> None:
        source_rate = 25600
        target_rate = 6400
        time = np.arange(source_rate) / source_rate
        source = np.rint(10000 * np.sin(2 * np.pi * 321 * time)).astype(np.int16)
        target = resample_int16(source, source_rate, target_rate)
        self.assertEqual(len(target), target_rate)
        peak = np.argmax(np.abs(np.fft.rfft(target)))
        self.assertLessEqual(abs(peak - 321), 1)

    def test_physical_quantization_is_explicit_and_rejects_clipping(self) -> None:
        values = np.array([-4.0, 0.0, 4.0])
        np.testing.assert_array_equal(
            physical_to_int16(values, 4.0), np.array([-32767, 0, 32767], dtype=np.int16)
        )
        with self.assertRaisesRegex(ValueError, "exceeds"):
            physical_to_int16(np.array([4.1]), 4.0)
        two_channel = np.stack([values, -values], axis=1)
        same = resample_float(two_channel, 16000, 16000)
        np.testing.assert_array_equal(same, two_channel)


if __name__ == "__main__":
    unittest.main()
