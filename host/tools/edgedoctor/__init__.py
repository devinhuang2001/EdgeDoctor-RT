"""EdgeDoctor host-side data and reference-DSP package."""

from .edgepack import EdgePack, EdgePackHeader, EdgePackError
from .kaist import (
    KaistMatError,
    MatSignal,
    load_audio_recording,
    load_acquisition_times,
    load_mat_signals,
    load_vibration_recording,
    validate_synchronized_pair,
)
from .manifest import Recording, parse_kaist_name

__all__ = [
    "EdgePack",
    "EdgePackError",
    "EdgePackHeader",
    "KaistMatError",
    "MatSignal",
    "Recording",
    "load_audio_recording",
    "load_acquisition_times",
    "load_mat_signals",
    "load_vibration_recording",
    "validate_synchronized_pair",
    "parse_kaist_name",
]
