"""Create E1 and three leave-one-load-out recording manifests."""

from __future__ import annotations

import argparse
from dataclasses import asdict
from pathlib import Path
import csv
import sys

from edgedoctor.manifest import ManifestError, read_manifest
from edgedoctor.split import lolo_splits, recording_split


def write_split(path: Path, split: dict[str, list[object]]) -> None:
    rows = []
    for partition, recordings in split.items():
        for recording in recordings:
            row = {"partition": partition, **asdict(recording)}
            rows.append(row)
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--out-dir", type=Path, required=True)
    parser.add_argument("--seed", type=int, default=20260828)
    args = parser.parse_args()
    recordings = read_manifest(args.input)
    write_split(args.out_dir / "e1.csv", recording_split(recordings, args.seed))
    for name, fold in lolo_splits(recordings, args.seed).items():
        write_split(args.out_dir / f"lolo_{name}.csv", fold)
    print(f"wrote E1 and three LOLO manifests to {args.out_dir}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ManifestError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(1)

