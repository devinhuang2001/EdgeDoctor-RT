"""Reference preprocessing shared by training, parity tests, and conversion."""

from __future__ import annotations

import numpy as np
from scipy.signal import get_window, resample_poly


def resample_float(samples: np.ndarray, source_rate: int, target_rate: int) -> np.ndarray:
    """Polyphase-resample physical-unit samples without implicit quantization."""

    if source_rate <= 0 or target_rate <= 0:
        raise ValueError("sample rates must be positive")
    values = np.asarray(samples, dtype=np.float64)
    if values.ndim not in (1, 2):
        raise ValueError("samples must be one- or two-dimensional")
    if source_rate == target_rate:
        return np.ascontiguousarray(values)
    gcd = int(np.gcd(source_rate, target_rate))
    converted = resample_poly(values, target_rate // gcd, source_rate // gcd, axis=0)
    return np.ascontiguousarray(converted)


def physical_to_int16(samples: np.ndarray, full_scale: float) -> np.ndarray:
    """Map a declared symmetric physical range to int16 and reject clipping."""

    values = np.asarray(samples, dtype=np.float64)
    if not np.isfinite(full_scale) or full_scale <= 0.0:
        raise ValueError("full_scale must be finite and positive")
    if values.size == 0 or not np.isfinite(values).all():
        raise ValueError("samples must be non-empty and finite")
    peak = float(np.max(np.abs(values)))
    if peak > full_scale * (1.0 + 1e-12):
        raise ValueError(
            f"signal peak {peak:.9g} exceeds declared full scale {full_scale:.9g}"
        )
    return np.rint(values * (32767.0 / full_scale)).astype(np.int16)


def resample_int16(samples: np.ndarray, source_rate: int, target_rate: int) -> np.ndarray:
    if source_rate <= 0 or target_rate <= 0:
        raise ValueError("sample rates must be positive")
    values = np.asarray(samples)
    if values.ndim not in (1, 2):
        raise ValueError("samples must be one- or two-dimensional")
    converted = resample_float(values, source_rate, target_rate)
    return np.rint(np.clip(converted, -32768, 32767)).astype(np.int16)


def _frames(signal: np.ndarray, frame_length: int, hop_length: int) -> np.ndarray:
    if signal.ndim != 1 or len(signal) < frame_length:
        raise ValueError("signal is shorter than one analysis frame")
    count = 1 + (len(signal) - frame_length) // hop_length
    shape = (count, frame_length)
    strides = (signal.strides[0] * hop_length, signal.strides[0])
    return np.lib.stride_tricks.as_strided(signal, shape=shape, strides=strides).copy()


def _hz_to_mel(value: np.ndarray | float) -> np.ndarray:
    return 2595.0 * np.log10(1.0 + np.asarray(value) / 700.0)


def _mel_to_hz(value: np.ndarray | float) -> np.ndarray:
    return 700.0 * (np.power(10.0, np.asarray(value) / 2595.0) - 1.0)


def mel_filterbank(sample_rate: int, fft_size: int, bins: int) -> np.ndarray:
    mel_points = np.linspace(_hz_to_mel(0), _hz_to_mel(sample_rate / 2), bins + 2)
    hz_points = _mel_to_hz(mel_points)
    fft_bins = np.floor((fft_size + 1) * hz_points / sample_rate).astype(int)
    fft_bins = np.clip(fft_bins, 0, fft_size // 2)
    filters = np.zeros((bins, fft_size // 2 + 1), dtype=np.float32)
    for index in range(bins):
        left, center, right = fft_bins[index : index + 3]
        if center <= left:
            center = min(left + 1, fft_size // 2)
        if right <= center:
            right = min(center + 1, fft_size // 2)
        if center > left:
            filters[index, left:center] = np.arange(left, center) - left
            filters[index, left:center] /= center - left
        if right > center:
            filters[index, center:right] = right - np.arange(center, right)
            filters[index, center:right] /= right - center
    return filters


def audio_logmel(
    audio: np.ndarray,
    sample_rate: int = 16000,
    frame_ms: int = 25,
    hop_ms: int = 10,
    fft_size: int = 512,
    mel_bins: int = 40,
    log_floor: float = 1e-6,
) -> np.ndarray:
    values = np.asarray(audio, dtype=np.float32).reshape(-1) / 32768.0
    frame_length = sample_rate * frame_ms // 1000
    hop_length = sample_rate * hop_ms // 1000
    framed = _frames(values, frame_length, hop_length)
    framed *= get_window("hann", frame_length, fftbins=True).astype(np.float32)
    spectrum = np.abs(np.fft.rfft(framed, n=fft_size, axis=1)) ** 2
    mel = spectrum @ mel_filterbank(sample_rate, fft_size, mel_bins).T
    return np.log(np.maximum(mel, log_floor)).T.astype(np.float32)


def _log_band_matrix(sample_rate: int, fft_size: int, bands: int) -> np.ndarray:
    max_bin = fft_size // 2
    edges = np.geomspace(1.0, max_bin + 1.0, bands + 1)
    result = np.zeros((bands, max_bin + 1), dtype=np.float32)
    for index in range(bands):
        left = max(1, int(np.floor(edges[index])))
        right = min(max_bin + 1, int(np.ceil(edges[index + 1])))
        if right <= left:
            right = min(max_bin + 1, left + 1)
        result[index, left:right] = 1.0 / max(1, right - left)
    return result


def vibration_log_spectrum(
    vibration: np.ndarray,
    sample_rate: int = 6400,
    frame_ms: int = 40,
    hop_ms: int = 20,
    fft_size: int = 256,
    bands: int = 64,
    log_floor: float = 1e-6,
) -> np.ndarray:
    values = np.asarray(vibration, dtype=np.float32)
    if values.ndim != 2:
        raise ValueError("vibration must have shape [samples, channels]")
    frame_length = sample_rate * frame_ms // 1000
    hop_length = sample_rate * hop_ms // 1000
    window = get_window("hann", frame_length, fftbins=True).astype(np.float32)
    band_matrix = _log_band_matrix(sample_rate, fft_size, bands)
    channel_features = []
    for channel in range(values.shape[1]):
        framed = _frames(values[:, channel] / 32768.0, frame_length, hop_length)
        power = np.abs(np.fft.rfft(framed * window, n=fft_size, axis=1)) ** 2
        compressed = power @ band_matrix.T
        channel_features.append(np.log(np.maximum(compressed, log_floor)).T)
    return np.stack(channel_features, axis=-1).astype(np.float32)


def vibration_statistics(vibration: np.ndarray, bands: int = 8) -> np.ndarray:
    values = np.asarray(vibration, dtype=np.float64)
    if values.ndim != 2:
        raise ValueError("vibration must have shape [samples, channels]")
    magnitude = np.sqrt(np.mean(values * values, axis=1))
    rms = np.sqrt(np.mean(magnitude * magnitude))
    peak = np.max(np.abs(magnitude))
    crest = peak / max(rms, 1e-12)
    centered = magnitude - np.mean(magnitude)
    variance = np.mean(centered * centered)
    kurtosis = np.mean(centered**4) / max(variance * variance, 1e-12)
    power = np.abs(np.fft.rfft(magnitude)) ** 2
    chunks = np.array_split(power[1:], bands)
    energies = [float(np.sum(chunk)) for chunk in chunks]
    total = max(sum(energies), 1e-12)
    return np.asarray([rms, peak, crest, kurtosis] + [value / total for value in energies], dtype=np.float32)
