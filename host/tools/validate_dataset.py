"""Validate extracted KAIST modality pairs and write the canonical manifest."""

from __future__ import annotations

import argparse
from pathlib import Path
import sys

from edgedoctor.manifest import ManifestError, pair_recordings, write_manifest


def _files(root: Path) -> list[Path]:
    return sorted(path for path in root.rglob("*.mat") if path.is_file())


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--audio-dir", type=Path)
    parser.add_argument("--vibration-dir", type=Path)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    audio_root = args.audio_dir or args.root / "audio"
    vibration_root = args.vibration_dir or args.root / "vibration"
    if not audio_root.is_dir() or not vibration_root.is_dir():
        raise ManifestError(
            f"expected extracted audio/vibration directories; "
            f"audio={audio_root}, vibration={vibration_root}"
        )
    audio = _files(audio_root)
    vibration = _files(vibration_root)
    if not audio or not vibration:
        raise ManifestError("no .mat recordings found in one or both modalities")
    recordings = pair_recordings(audio, vibration)
    write_manifest(recordings, args.out)
    print(f"validated {len(recordings)} synchronized recordings -> {args.out}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ManifestError) as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(1)

