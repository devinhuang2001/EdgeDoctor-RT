"""Download the pinned public KAIST modalities with resume and SHA-256 checks."""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import sys
import urllib.request
import zipfile


FILES = {
    "audio": {
        "name": "acoustic.zip",
        "bytes": 47_205_730,
        "sha256": "5dd1371ccec00729498e2d151df13593f6400fdced8895481ff6a8a945a5962f",
        "url": "https://data.mendeley.com/public-files/datasets/ztmf3m7h5x/files/99f03ffd-04f9-4676-932a-4312284cbc18/file_downloaded",
    },
    "vibration": {
        "name": "vibration.zip",
        "bytes": 2_659_778_162,
        "sha256": "df73fa80371a2ad3c983825044447b7772fcefd1db2ac409b2b298b533e4659f",
        "url": "https://data.mendeley.com/public-files/datasets/ztmf3m7h5x/files/ee98c5d9-1052-4448-84b2-ed57711b658d/file_downloaded",
    },
}


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while block := stream.read(4 * 1024 * 1024):
            digest.update(block)
    return digest.hexdigest()


def download(spec: dict[str, object], destination: Path) -> None:
    partial = destination.with_suffix(destination.suffix + ".partial")
    start = partial.stat().st_size if partial.exists() else 0
    headers = {
        "Accept": "application/octet-stream,*/*",
        "Referer": "https://data.mendeley.com/datasets/ztmf3m7h5x/6",
        "User-Agent": "Mozilla/5.0 EdgeDoctor-RT dataset downloader",
    }
    if start:
        headers["Range"] = f"bytes={start}-"
    request = urllib.request.Request(str(spec["url"]), headers=headers)
    with urllib.request.urlopen(request, timeout=60) as response:
        if start and response.status != 206:
            start = 0
            partial.unlink(missing_ok=True)
        mode = "ab" if start else "wb"
        total = int(spec["bytes"])
        copied = start
        with partial.open(mode) as output:
            while block := response.read(4 * 1024 * 1024):
                output.write(block)
                copied += len(block)
                print(f"{destination.name}: {copied}/{total} bytes", flush=True)
    if partial.stat().st_size != int(spec["bytes"]):
        raise RuntimeError(
            f"size mismatch for {destination.name}: "
            f"{partial.stat().st_size} != {spec['bytes']}"
        )
    digest = sha256(partial)
    if digest != spec["sha256"]:
        raise RuntimeError(f"SHA-256 mismatch for {destination.name}: {digest}")
    partial.replace(destination)


def extract(archive: Path, destination: Path) -> None:
    destination.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(archive) as source:
        root = destination.resolve()
        for member in source.infolist():
            target = (destination / member.filename).resolve()
            if root not in target.parents and target != root:
                raise RuntimeError(f"unsafe zip member: {member.filename}")
        source.extractall(destination)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", type=Path, default=Path("data/raw/kaist-v6"))
    parser.add_argument("--only", nargs="+", choices=sorted(FILES), default=sorted(FILES))
    parser.add_argument("--extract", action="store_true")
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    for modality in args.only:
        spec = FILES[modality]
        archive = args.out / str(spec["name"])
        if archive.exists() and archive.stat().st_size == int(spec["bytes"]):
            if sha256(archive) != spec["sha256"]:
                raise RuntimeError(f"existing {archive} has the wrong SHA-256")
        else:
            download(spec, archive)
        if args.extract:
            extract(archive, args.out / modality)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, zipfile.BadZipFile) as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(1)
