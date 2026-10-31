"""Reference implementation for the 208 Hz live IMU feature contract."""
from __future__ import annotations

import numpy as np

BAND_EDGES_HZ = np.array([5, 8, 11, 14, 18, 23, 29, 35, 40], dtype=np.float32)


def compute_low_frequency_features(xyz: np.ndarray, sample_hz: int) -> dict[str, np.ndarray | float]:
    samples = np.asarray(xyz, dtype=np.float32)
    if samples.ndim != 2 or samples.shape[1] != 3 or not 33 <= samples.shape[0] <= 256:
        raise ValueError("xyz must contain 33..256 samples of three axes")
    if sample_hz <= 0 or not np.isfinite(samples).all():
        raise ValueError("invalid sample rate or non-finite sample")
    dynamic = samples - samples.mean(axis=0, keepdims=True)
    rms_axis = np.sqrt(np.mean(dynamic * dynamic, axis=0))
    vector_rms = float(np.sqrt(np.mean(np.sum(dynamic * dynamic, axis=1))))
    vector_peak = float(np.max(np.linalg.norm(samples, axis=1)))
    crest = vector_peak / max(vector_rms, 0.001)
    second = np.mean(dynamic * dynamic, axis=0)
    fourth = np.mean(dynamic**4, axis=0)
    kurtosis = np.divide(fourth, second**2, out=np.zeros(3, dtype=np.float32), where=second > 1e-8)
    window = np.hanning(samples.shape[0]).astype(np.float32)
    padded = np.zeros((256, 3), dtype=np.float32)
    padded[: samples.shape[0]] = dynamic * window[:, None]
    spectrum = np.fft.rfft(padded, axis=0)
    frequencies = np.fft.rfftfreq(256, 1.0 / sample_hz)
    band_energy = np.array(
        [np.sum(np.abs(spectrum[(frequencies >= lo) & (frequencies < hi)]) ** 2) / samples.shape[0] ** 2
         for lo, hi in zip(BAND_EDGES_HZ[:-1], BAND_EDGES_HZ[1:])], dtype=np.float32
    )
    return {
        "values": np.concatenate((rms_axis, [vector_rms, vector_peak, crest, float(kurtosis.max())], band_energy)),
        "raw_mean_g": samples.mean(axis=0),
        "band_energy_g2": band_energy,
        "kurtosis_axis": kurtosis,
    }
