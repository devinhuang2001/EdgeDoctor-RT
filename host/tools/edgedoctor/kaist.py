"""Strict readers for the nested MATLAB signal records used by KAIST v6."""

from __future__ import annotations

from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
import re
from typing import Iterator

import numpy as np
from scipy.io import loadmat


class KaistMatError(ValueError):
    """Raised when a MATLAB recording cannot be interpreted unambiguously."""


@dataclass(frozen=True)
class MatSignal:
    name: str
    sample_rate: int
    values: np.ndarray
    unit: str


def _fields(value: object) -> tuple[str, ...]:
    names = getattr(value, "_fieldnames", ())
    return tuple(names) if names else ()


def _walk(value: object, name: str, seen: set[int]) -> Iterator[tuple[str, object]]:
    identity = id(value)
    if identity in seen:
        return
    seen.add(identity)

    fields = _fields(value)
    if "x_values" in fields and "y_values" in fields:
        yield name, value
        return
    for field in fields:
        yield from _walk(getattr(value, field), f"{name}.{field}", seen)
    if isinstance(value, np.ndarray) and value.dtype == object:
        for index, item in enumerate(value.flat):
            yield from _walk(item, f"{name}[{index}]", seen)


def _sample_rate(signal: object, name: str) -> int:
    x_values = getattr(signal, "x_values")
    if "increment" not in _fields(x_values):
        raise KaistMatError(f"{name}: x_values.increment is missing")
    increment_values = np.asarray(getattr(x_values, "increment"), dtype=np.float64).reshape(-1)
    if increment_values.size != 1 or not np.isfinite(increment_values[0]):
        raise KaistMatError(f"{name}: sample interval must be one finite value")
    increment = float(increment_values[0])
    if increment <= 0.0:
        raise KaistMatError(f"{name}: sample interval must be positive")
    rate = 1.0 / increment
    rounded = int(round(rate))
    if rounded <= 0 or not np.isclose(rate, rounded, rtol=1e-7, atol=1e-7):
        raise KaistMatError(f"{name}: non-integral sample rate {rate:.12g} Hz")
    return rounded


def _text(value: object) -> str:
    array = np.asarray(value)
    if array.dtype.kind in "US":
        return "".join(str(item) for item in array.reshape(-1))
    return str(value)


def _samples(signal: object, name: str) -> tuple[np.ndarray, str]:
    y_values = getattr(signal, "y_values")
    if "values" not in _fields(y_values):
        raise KaistMatError(f"{name}: y_values.values is missing")
    samples = np.asarray(getattr(y_values, "values"), dtype=np.float64).squeeze()
    if samples.ndim not in (1, 2) or samples.size == 0:
        raise KaistMatError(f"{name}: expected a non-empty signal vector or matrix")
    if not np.isfinite(samples).all():
        raise KaistMatError(f"{name}: signal contains NaN or infinity")
    unit = ""
    if "quantity" in _fields(y_values):
        quantity = getattr(y_values, "quantity")
        if "label" in _fields(quantity):
            unit = _text(getattr(quantity, "label"))
        if "unit_transformation" in _fields(quantity):
            transform = getattr(quantity, "unit_transformation")
            factor = float(np.asarray(getattr(transform, "factor", 1.0)).reshape(-1)[0])
            offset = float(np.asarray(getattr(transform, "offset", 0.0)).reshape(-1)[0])
            if not np.isfinite((factor, offset)).all() or factor == 0.0:
                raise KaistMatError(f"{name}: invalid unit transformation")
            samples = samples * factor + offset
    return np.ascontiguousarray(samples), unit


def load_mat_signals(path: str | Path) -> tuple[MatSignal, ...]:
    """Read every explicit x/y signal record without guessing channel meaning."""

    source = Path(path)
    try:
        document = loadmat(source, squeeze_me=True, struct_as_record=False)
    except (OSError, ValueError, TypeError) as error:
        raise KaistMatError(f"cannot read {source}: {error}") from error

    found: list[MatSignal] = []
    seen: set[int] = set()
    for root_name, root_value in document.items():
        if root_name.startswith("__"):
            continue
        for name, signal in _walk(root_value, root_name, seen):
            values, unit = _samples(signal, name)
            found.append(MatSignal(name, _sample_rate(signal, name), values, unit))
    if not found:
        raise KaistMatError(f"{source}: no x_values/y_values signal records found")
    return tuple(found)


def _string_items(value: object) -> list[str]:
    array = np.asarray(value)
    if array.dtype == object:
        result: list[str] = []
        for item in array.reshape(-1):
            result.extend(_string_items(item))
        return result
    if array.dtype.kind in "US":
        return ["".join(str(item) for item in array.reshape(-1))]
    return []


def load_acquisition_times(path: str | Path) -> tuple[datetime, ...]:
    """Read Test.Lab absolute acquisition times from a KAIST MAT file."""

    source = Path(path)
    try:
        document = loadmat(source, squeeze_me=True, struct_as_record=False)
    except (OSError, ValueError, TypeError) as error:
        raise KaistMatError(f"cannot read {source}: {error}") from error
    texts: list[str] = []
    for root_name, root in document.items():
        if root_name.startswith("__") or "function_record" not in _fields(root):
            continue
        function_record = getattr(root, "function_record")
        if "TL_export_properties_annotation" not in _fields(function_record):
            continue
        annotation = getattr(function_record, "TL_export_properties_annotation")
        if "absolute_time" in _fields(annotation):
            texts.extend(_string_items(getattr(annotation, "absolute_time")))
    parsed: list[datetime] = []
    for value in texts:
        match = re.fullmatch(
            r"\s*(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2})\s+ms\s+(\d+(?:\.\d+)?)\s*",
            value,
        )
        if not match:
            raise KaistMatError(f"{source}: invalid acquisition time {value!r}")
        base = datetime.strptime(match.group(1), "%Y-%m-%d %H:%M:%S")
        parsed.append(base.replace(microsecond=round(float(match.group(2)) * 1000.0)))
    if not parsed:
        raise KaistMatError(f"{source}: acquisition time metadata is missing")
    return tuple(parsed)


def validate_synchronized_pair(
    audio_path: str | Path, vibration_path: str | Path, tolerance_ms: float = 20.0
) -> None:
    """Reject condition-matched files that were not acquired at the same time."""

    audio_times = load_acquisition_times(audio_path)
    vibration_times = load_acquisition_times(vibration_path)
    audio_unique = set(audio_times)
    vibration_unique = set(vibration_times)
    if len(audio_unique) != 1 or len(vibration_unique) != 1:
        raise KaistMatError("each modality must have one common acquisition start time")
    difference_ms = abs((next(iter(audio_unique)) - next(iter(vibration_unique))).total_seconds()) * 1000.0
    if difference_ms > tolerance_ms:
        raise KaistMatError(
            f"modalities are not synchronized: start-time difference is {difference_ms:.3f} ms"
        )


def load_audio_recording(path: str | Path) -> tuple[int, np.ndarray]:
    """Load a KAIST acoustic file, requiring exactly one recorded channel."""

    signals = load_mat_signals(path)
    if len(signals) != 1:
        names = ", ".join(signal.name for signal in signals)
        raise KaistMatError(f"audio recording must contain exactly one signal; found {names}")
    signal = signals[0]
    values = signal.values
    if values.ndim == 2:
        if values.shape[1] != 1:
            raise KaistMatError(f"audio recording has {values.shape[1]} sample columns")
        values = values[:, 0]
    if signal.sample_rate != 51200:
        raise KaistMatError(f"unexpected KAIST acoustic rate {signal.sample_rate} Hz")
    if signal.unit and signal.unit.lower() != "pa":
        raise KaistMatError(f"unexpected KAIST acoustic unit {signal.unit!r}")
    return signal.sample_rate, values


def load_vibration_recording(path: str | Path) -> tuple[int, np.ndarray]:
    """Load housing-A x/y channels using the publisher's declared column order.

    The MAT payload omits a time column because its start and increment are stored
    separately. Its four sample columns are housing-A x/y then housing-B x/y.
    """

    signals = load_mat_signals(path)
    if len(signals) != 1:
        names = ", ".join(signal.name for signal in signals)
        raise KaistMatError(f"vibration recording must contain one matrix; found {names}")
    signal = signals[0]
    if signal.values.ndim != 2 or signal.values.shape[1] != 4:
        raise KaistMatError(
            f"vibration recording must have four sample columns; got {signal.values.shape}"
        )
    if signal.sample_rate != 25600:
        raise KaistMatError(f"unexpected KAIST vibration rate {signal.sample_rate} Hz")
    if signal.unit and signal.unit.lower() != "g":
        raise KaistMatError(f"unexpected KAIST vibration unit {signal.unit!r}")
    return signal.sample_rate, np.ascontiguousarray(signal.values[:, :2])
