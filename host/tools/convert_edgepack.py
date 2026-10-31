"""Convert a validated synchronized recording manifest into EdgePack v1."""

from __future__ import annotations

import argparse
import csv
from dataclasses import asdict
from pathlib import Path
import re
import sys

from edgedoctor.edgepack import EdgePack, EdgePackError, EdgePackHeader
from edgedoctor.kaist import (
    KaistMatError,
    load_audio_recording,
    load_vibration_recording,
    validate_synchronized_pair,
)
from edgedoctor.manifest import ManifestError, Recording, read_manifest, sha256_file
from edgedoctor.preprocess import physical_to_int16, resample_float


def _severity_order(value: str) -> tuple[tuple[int, object], ...]:
    parts = re.findall(r"\d+(?:\.\d+)?|[^\d]+", value.lower())
    return tuple((0, float(part)) if re.fullmatch(r"\d+(?:\.\d+)?", part) else (1, part) for part in parts)


def severity_codes(recordings: list[Recording]) -> dict[tuple[str, str], int]:
    result: dict[tuple[str, str], int] = {}
    for fault in sorted({recording.fault for recording in recordings}):
        values = sorted(
            {recording.severity for recording in recordings if recording.fault == fault},
            key=_severity_order,
        )
        if fault == "normal":
            for value in values:
                result[(fault, value)] = 0
            continue
        if len(values) > 255:
            raise ManifestError(f"too many severity levels for {fault}")
        for code, value in enumerate(values, start=1):
            result[(fault, value)] = code
    return result


def convert_recording(
    recording: Recording,
    severity: int,
    destination: Path,
    audio_full_scale_pa: float,
    vibration_full_scale_g: float,
) -> dict[str, object]:
    validate_synchronized_pair(recording.audio_path, recording.vibration_path)
    audio_rate, audio = load_audio_recording(recording.audio_path)
    vibration_rate, vibration = load_vibration_recording(recording.vibration_path)
    duration_error = abs(len(audio) / audio_rate - len(vibration) / vibration_rate)
    if duration_error > 1.0 / min(audio_rate, vibration_rate):
        raise ManifestError(
            f"{recording.recording_key}: modality durations differ by {duration_error:.9f} s"
        )
    audio_resampled = resample_float(audio, audio_rate, 16000)
    vibration_resampled = resample_float(vibration, vibration_rate, 6400)
    audio_i16 = physical_to_int16(audio_resampled, audio_full_scale_pa)
    vibration_i16 = physical_to_int16(vibration_resampled, vibration_full_scale_g)
    if len(audio_i16) % 320 or len(vibration_i16) % 128:
        raise ManifestError(f"{recording.recording_key}: resampled duration is not 20 ms aligned")
    if len(audio_i16) // 320 != len(vibration_i16) // 128:
        raise ManifestError(f"{recording.recording_key}: resampled modalities are not synchronized")
    header = EdgePackHeader(
        recording_id=recording.recording_id,
        dataset_id=recording.dataset_id,
        label=recording.label,
        severity=severity,
        load_x10_nm=recording.load_nm * 10,
        chunk_ms=20,
        audio_rate=16000,
        vibration_rate=6400,
        vibration_channels=2,
        audio_samples=len(audio_i16),
        vibration_frames=len(vibration_i16),
    )
    destination.parent.mkdir(parents=True, exist_ok=True)
    EdgePack.create(header, audio_i16, vibration_i16).write(destination)
    return {
        **asdict(recording),
        "edgepack_path": str(destination.resolve()),
        "edgepack_sha256": sha256_file(destination),
        "audio_full_scale_pa": audio_full_scale_pa,
        "vibration_full_scale_g": vibration_full_scale_g,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--out-dir", type=Path, required=True)
    parser.add_argument("--audio-full-scale-pa", type=float, default=4.0)
    parser.add_argument("--vibration-full-scale-g", type=float, default=50.0)
    args = parser.parse_args()
    recordings = read_manifest(args.manifest)
    codes = severity_codes(recordings)
    rows = []
    for recording in recordings:
        rows.append(
            convert_recording(
                recording,
                codes[(recording.fault, recording.severity)],
                args.out_dir / f"{recording.recording_key}.edgp",
                args.audio_full_scale_pa,
                args.vibration_full_scale_g,
            )
        )
    if not rows:
        raise ManifestError("refusing to convert an empty manifest")
    manifest_path = args.out_dir / "edgepack_manifest.csv"
    with manifest_path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    print(f"converted {len(rows)} synchronized recordings -> {manifest_path}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, ManifestError, KaistMatError, EdgePackError) as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(1)
