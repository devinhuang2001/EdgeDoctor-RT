"""KAIST recording metadata and strict synchronized-pair validation."""

from __future__ import annotations

from dataclasses import asdict, dataclass
from pathlib import Path
import csv
import hashlib
import re
from typing import Iterable

from .kaist import KaistMatError, validate_synchronized_pair


LABELS = {
    "normal": 0,
    "bearing_inner": 1,
    "bearing_outer": 2,
    "misalignment": 3,
    "unbalance": 4,
}

_ALIASES = {
    "normal": "normal",
    "healthy": "normal",
    "bearinginnerrace": "bearing_inner",
    "bpfi": "bearing_inner",
    "inner": "bearing_inner",
    "innerrace": "bearing_inner",
    "bearingouterrace": "bearing_outer",
    "bpfo": "bearing_outer",
    "outer": "bearing_outer",
    "outerrace": "bearing_outer",
    "misalignment": "misalignment",
    "misalign": "misalignment",
    "shaftmisalignment": "misalignment",
    "unbalance": "unbalance",
    "unbalalnce": "unbalance",
    "rotorunbalance": "unbalance",
    "imbalance": "unbalance",
}


class ManifestError(ValueError):
    """Raised when source metadata is unsafe or ambiguous."""


@dataclass(frozen=True)
class ParsedName:
    recording_key: str
    load_nm: int
    fault: str
    severity: str


@dataclass(frozen=True)
class Recording:
    recording_id: int
    recording_key: str
    load_nm: int
    fault: str
    label: int
    severity: str
    audio_path: str
    vibration_path: str
    audio_sha256: str = ""
    vibration_sha256: str = ""
    dataset_id: int = 1
    dataset_version: str = "10.17632/ztmf3m7h5x.6"
    license: str = "CC BY 4.0"


def _token(value: str) -> str:
    return re.sub(r"[^a-z0-9]+", "", value.lower())


def _fault_from_stem(stem: str) -> tuple[str, str]:
    tokens = [part for part in re.split(r"[_\-\s]+", stem) if part]
    normalized = [_token(part) for part in tokens]
    for width in (3, 2, 1):
        for start in range(len(normalized) - width + 1):
            candidate = "".join(normalized[start : start + width])
            if candidate in _ALIASES:
                used = set(range(start, start + width))
                severity = "_".join(
                    part
                    for index, part in enumerate(tokens)
                    if index not in used and not re.fullmatch(r"\d+nm", part.lower())
                )
                return _ALIASES[candidate], severity or "none"
    raise ManifestError(f"unknown fault label in filename: {stem}")


def parse_kaist_name(path: str | Path) -> ParsedName:
    """Parse `<load>Nm_<fault>_<severity>.mat` without guessing a load."""

    stem = Path(path).stem
    load_matches = re.findall(r"(?<!\d)(\d+)\s*nm(?![a-z])", stem, re.IGNORECASE)
    if len(load_matches) != 1:
        raise ManifestError(f"expected exactly one load token in filename: {stem}")
    load_nm = int(load_matches[0])
    if load_nm not in (0, 2, 4):
        raise ManifestError(f"unsupported KAIST load {load_nm} Nm in filename: {stem}")
    fault, severity = _fault_from_stem(stem)
    recording_key = re.sub(r"[^a-z0-9]+", "_", stem.lower()).strip("_")
    return ParsedName(recording_key, load_nm, fault, severity)


def sha256_file(path: str | Path, block_bytes: int = 1024 * 1024) -> str:
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        while block := stream.read(block_bytes):
            digest.update(block)
    return digest.hexdigest()


def stable_recording_id(key: str) -> int:
    value = int.from_bytes(hashlib.sha256(key.encode("utf-8")).digest()[:4], "little")
    return value or 1


def pair_recordings(audio_files: Iterable[Path], vibration_files: Iterable[Path]) -> list[Recording]:
    """Return one-to-one modality pairs and reject duplicates or missing peers."""

    def index(files: Iterable[Path], modality: str) -> dict[str, tuple[Path, ParsedName]]:
        result: dict[str, tuple[Path, ParsedName]] = {}
        for file_path in files:
            parsed = parse_kaist_name(file_path)
            if parsed.recording_key in result:
                raise ManifestError(f"duplicate {modality} recording key: {parsed.recording_key}")
            result[parsed.recording_key] = (file_path, parsed)
        return result

    audio = index(audio_files, "audio")
    vibration = index(vibration_files, "vibration")
    if set(audio) != set(vibration):
        missing_audio = sorted(set(vibration) - set(audio))
        missing_vibration = sorted(set(audio) - set(vibration))
        raise ManifestError(
            f"unpaired modalities; missing audio={missing_audio}, "
            f"missing vibration={missing_vibration}"
        )

    recordings: list[Recording] = []
    seen_ids: set[int] = set()
    for key in sorted(audio):
        audio_path, a = audio[key]
        vibration_path, v = vibration[key]
        if (a.load_nm, a.fault, a.severity) != (v.load_nm, v.fault, v.severity):
            raise ManifestError(f"metadata mismatch for synchronized pair: {key}")
        try:
            validate_synchronized_pair(audio_path, vibration_path)
        except KaistMatError as error:
            raise ManifestError(f"invalid synchronized pair {key}: {error}") from error
        recording_id = stable_recording_id(key)
        if recording_id in seen_ids:
            raise ManifestError(f"recording ID hash collision: {key}")
        seen_ids.add(recording_id)
        recordings.append(
            Recording(
                recording_id=recording_id,
                recording_key=key,
                load_nm=a.load_nm,
                fault=a.fault,
                label=LABELS[a.fault],
                severity=a.severity,
                audio_path=str(audio_path.resolve()),
                vibration_path=str(vibration_path.resolve()),
                audio_sha256=sha256_file(audio_path),
                vibration_sha256=sha256_file(vibration_path),
            )
        )
    return recordings


def write_manifest(recordings: Iterable[Recording], path: str | Path) -> None:
    rows = [asdict(recording) for recording in recordings]
    if not rows:
        raise ManifestError("refusing to write an empty manifest")
    destination = Path(path)
    destination.parent.mkdir(parents=True, exist_ok=True)
    with destination.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)


def read_manifest(path: str | Path) -> list[Recording]:
    with Path(path).open(newline="", encoding="utf-8") as stream:
        rows = list(csv.DictReader(stream))
    recordings = []
    for row in rows:
        row["recording_id"] = int(row["recording_id"])
        row["load_nm"] = int(row["load_nm"])
        row["label"] = int(row["label"])
        row["dataset_id"] = int(row.get("dataset_id", 1))
        recordings.append(Recording(**row))
    return recordings
