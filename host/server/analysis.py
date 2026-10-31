"""Versioned EDV2 XYZ analysis. No inferred sensor scale or fault diagnosis."""
from __future__ import annotations

import cmath
import math
import struct

from .evidence import unpack

PARSER_VERSION = "edv2-xyz-v1"


def _fft(values: list[float], size: int) -> list[complex]:
    out = [complex(value) for value in values] + [0j] * (size - len(values))
    target = 0
    for index in range(1, size):
        bit = size >> 1
        while target & bit:
            target ^= bit
            bit >>= 1
        target ^= bit
        if index < target:
            out[index], out[target] = out[target], out[index]
    width = 2
    while width <= size:
        step = cmath.exp(-2j * math.pi / width)
        for start in range(0, size, width):
            phase = 1 + 0j
            for offset in range(width // 2):
                left, right = start + offset, start + offset + width // 2
                odd = phase * out[right]
                out[right], out[left] = out[left] - odd, out[left] + odd
                phase *= step
        width *= 2
    return out


def analyze(blob: bytes, event_id: str) -> dict:
    metadata, payload = unpack(blob)  # Both CRCs are checked again on every read.
    if not isinstance(metadata, dict) or metadata.get("event_id") != event_id:
        raise ValueError("evidence event identity mismatch")
    if metadata.get("encoding") != "xyz_int16_le":
        raise ValueError("evidence lacks a supported ADC encoding")
    if metadata.get("complete") is not True or metadata.get("gaps"):
        raise ValueError("evidence is incomplete or contains sample gaps")
    rate, scale = metadata.get("sample_rate_hz"), metadata.get("sensitivity_g_per_adc")
    for value in (rate, scale):
        if isinstance(value, bool) or not isinstance(value, (float, int)) or not math.isfinite(value) or value <= 0:
            raise ValueError("evidence sample rate or sensitivity is invalid")
    if rate > 32000 or scale > 1 or len(payload) % 6 or not 4 <= len(payload) // 6 <= 2730:
        raise ValueError("invalid XYZ sample payload")
    count = len(payload) // 6
    axes = {axis: [] for axis in "xyz"}
    for sample in struct.iter_unpack("<hhh", payload):
        for axis, value in zip("xyz", sample):
            axes[axis].append(value * scale)
    times = [index / rate for index in range(count)]
    size = 1 << (count - 1).bit_length()
    frequencies = [index * rate / size for index in range(size // 2 + 1)]
    window = [0.5 - 0.5 * math.cos(2 * math.pi * index / (count - 1)) for index in range(count)]
    gain = sum(window)
    spectrum, envelope, statistics, peaks = {}, {}, {}, []
    for axis, values in axes.items():
        mean = math.fsum(values) / count
        centered = [value - mean for value in values]
        variance = sum(value * value for value in centered) / count
        rms, peak = math.sqrt(variance), max(abs(value) for value in centered)
        statistics[axis] = {"mean_g": mean, "rms_ac_g": rms, "peak_ac_g": peak,
                            "crest_factor": peak / rms if rms else None,
                            "kurtosis": sum(value ** 4 for value in centered) / count / variance ** 2 if variance else None}
        transform = _fft([value * weight for value, weight in zip(centered, window)], size)
        amplitudes = [abs(value) / gain * (1 if index in (0, size // 2) else 2)
                      for index, value in enumerate(transform[:size // 2 + 1])]
        spectrum[f"{axis}_g"] = amplitudes
        # Rectified and smoothed AC amplitude, explicitly not a Hilbert envelope.
        prefix = [0.0]
        for value in centered:
            prefix.append(prefix[-1] + abs(value))
        half = max(1, round(rate * 0.05))
        envelope[f"{axis}_g"] = [(prefix[min(count, i + half + 1)] - prefix[max(0, i - half)]) /
                                 (min(count, i + half + 1) - max(0, i - half)) for i in range(count)]
        candidates = [i for i in range(1, len(amplitudes) - 1)
                      if amplitudes[i] > 1e-12 and amplitudes[i] > amplitudes[i - 1] and amplitudes[i] >= amplitudes[i + 1]]
        for index in sorted(candidates, key=lambda i: amplitudes[i], reverse=True)[:5]:
            peaks.append({"axis": axis.upper(), "frequency_hz": frequencies[index], "amplitude_g": amplitudes[index]})
    return {"event_id": event_id, "available": True, "parser_version": PARSER_VERSION,
            "crc_ok": True, "unit": "g", "sample_rate_hz": rate,
            "sample_rate_kind": metadata.get("sample_rate_kind", "declared"), "sample_count": count,
            "waveform": {"time_s": times, **{f"{axis}_g": values for axis, values in axes.items()}},
            "spectrum": {"frequency_hz": frequencies, "window": "Hann", "dc_removed": True, **spectrum},
            "envelope": {"time_s": times, "method": "rectified_ac_moving_mean", **envelope},
            "statistics": statistics, "dominant_peaks": sorted(peaks, key=lambda p: p["amplitude_g"], reverse=True)[:10]}
