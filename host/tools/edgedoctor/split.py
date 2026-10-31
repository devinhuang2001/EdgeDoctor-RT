"""Leakage-safe recording-level E1 and leave-one-load-out splits."""

from __future__ import annotations

from collections import defaultdict
import hashlib
from typing import Iterable

from .manifest import ManifestError, Recording


def _rank(recording: Recording, seed: int) -> bytes:
    value = f"{seed}:{recording.recording_key}".encode("utf-8")
    return hashlib.sha256(value).digest()


def recording_split(
    recordings: Iterable[Recording], seed: int = 20260828
) -> dict[str, list[Recording]]:
    """Stratify by fault/load and never split a recording across partitions."""

    groups: dict[tuple[int, int], list[Recording]] = defaultdict(list)
    for recording in recordings:
        groups[(recording.label, recording.load_nm)].append(recording)
    if not groups:
        raise ManifestError("cannot split an empty recording list")

    result = {"train": [], "val": [], "test": []}
    for group in groups.values():
        ordered = sorted(group, key=lambda item: _rank(item, seed))
        count = len(ordered)
        if count < 3:
            raise ManifestError(
                "each fault/load stratum needs at least three recordings for E1"
            )
        test_count = max(1, round(count * 0.15))
        val_count = max(1, round(count * 0.15))
        if test_count + val_count >= count:
            test_count = val_count = 1
        train_count = count - val_count - test_count
        result["train"].extend(ordered[:train_count])
        result["val"].extend(ordered[train_count : train_count + val_count])
        result["test"].extend(ordered[train_count + val_count :])
    _assert_disjoint(result)
    return {name: sorted(items, key=lambda item: item.recording_key) for name, items in result.items()}


def lolo_splits(
    recordings: Iterable[Recording], seed: int = 20260828
) -> dict[str, dict[str, list[Recording]]]:
    items = list(recordings)
    available = {recording.load_nm for recording in items}
    if available != {0, 2, 4}:
        raise ManifestError(f"LOLO requires loads 0, 2, and 4; found {sorted(available)}")
    folds: dict[str, dict[str, list[Recording]]] = {}
    for held_out in (0, 2, 4):
        test = [recording for recording in items if recording.load_nm == held_out]
        development = [recording for recording in items if recording.load_nm != held_out]
        by_label: dict[int, list[Recording]] = defaultdict(list)
        for recording in development:
            by_label[recording.label].append(recording)
        train: list[Recording] = []
        val: list[Recording] = []
        for group in by_label.values():
            ordered = sorted(group, key=lambda item: _rank(item, seed + held_out))
            if len(ordered) < 2:
                raise ManifestError("LOLO development label needs at least two recordings")
            val_count = max(1, round(len(ordered) * 0.15))
            train.extend(ordered[:-val_count])
            val.extend(ordered[-val_count:])
        fold = {"train": train, "val": val, "test": test}
        _assert_disjoint(fold)
        folds[f"test_load_{held_out}nm"] = fold
    return folds


def _assert_disjoint(split: dict[str, list[Recording]]) -> None:
    ids = {name: {item.recording_id for item in items} for name, items in split.items()}
    names = list(ids)
    for index, left in enumerate(names):
        for right in names[index + 1 :]:
            overlap = ids[left] & ids[right]
            if overlap:
                raise ManifestError(f"recording leakage between {left}/{right}: {overlap}")

