#!/usr/bin/env python3
"""Run an explicitly requested KAIST replay against a frozen reference.

The script is intentionally conservative: missing serial output, a hash
mismatch, an interrupted replay, or an incomplete frame count is evidence
failure, never an accuracy pass.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

FINISHED = re.compile(r"edgeai replay finished: status=(?P<status>-?\d+)")
FRAME_COUNT = re.compile(r"frames[=:\s]+(?P<count>\d+)")


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def validate_reference(edgp: Path, reference: dict[str, Any]) -> list[str]:
    errors: list[str] = []
    expected_pack = reference.get("edgp_sha256")
    if expected_pack and expected_pack != sha256(edgp):
        errors.append("edgp_sha256_mismatch")
    if not reference.get("model_sha256"):
        errors.append("reference_missing_model_sha256")
    if not reference.get("expected_frames"):
        errors.append("reference_missing_expected_frames")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", required=True)
    parser.add_argument("--edgp", type=Path, required=True)
    parser.add_argument("--reference-json", type=Path, required=True)
    parser.add_argument("--timeout", type=int, default=600)
    parser.add_argument("--output", type=Path,
                        default=Path("benchmark/results/v2/replay-reference.json"))
    args = parser.parse_args()
    errors: list[str] = []
    reference: dict[str, Any] = {}
    if not args.edgp.is_file():
        errors.append("missing_edgp")
    if not args.reference_json.is_file():
        errors.append("missing_reference_json")
    if not errors:
        try:
            reference = json.loads(args.reference_json.read_text(encoding="utf-8"))
            errors.extend(validate_reference(args.edgp, reference))
        except (OSError, ValueError) as exc:
            errors.append(f"invalid_reference:{type(exc).__name__}")

    output_text = ""
    started = time.monotonic()
    try:
        import serial  # type: ignore
        if not errors:
            with serial.Serial(args.port, 115200, timeout=0.2) as handle:
                handle.write((f"edgeai replay {args.edgp.as_posix()} fast\r\n").encode())
                deadline = started + args.timeout
                while time.monotonic() < deadline:
                    output_text += handle.read(8192).decode(errors="replace")
                    if FINISHED.search(output_text):
                        break
                if not FINISHED.search(output_text):
                    errors.append("replay_timeout_or_interruption")
    except ImportError as exc:
        errors.append(f"missing_dependency:{exc}")
    except Exception as exc:
        errors.append(f"serial_error:{type(exc).__name__}:{exc}")

    finished = FINISHED.search(output_text)
    frame_match = FRAME_COUNT.search(output_text)
    observed_status = int(finished.group("status")) if finished else None
    observed_frames = int(frame_match.group("count")) if frame_match else None
    expected_frames = reference.get("expected_frames")
    if finished and observed_status != 0:
        errors.append("board_replay_failed")
    if expected_frames and observed_frames is not None and observed_frames != expected_frames:
        errors.append("frame_count_mismatch")
    if expected_frames and observed_frames is None:
        errors.append("missing_frame_count")

    result = {
        "schema_version": 1,
        "generated_at": datetime.now(timezone.utc).isoformat(),
        "port": args.port,
        "edgp": str(args.edgp),
        "edgp_sha256": sha256(args.edgp) if args.edgp.is_file() else None,
        "reference": reference,
        "observed_status": observed_status,
        "observed_frames": observed_frames,
        "errors": errors,
        "serial_output_tail": output_text[-4096:],
        "status": "PASS_REFERENCE_REPLAY" if not errors else "MISSING_EVIDENCE",
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(args.output)
    return 0 if not errors else 1


if __name__ == "__main__":
    raise SystemExit(main())
