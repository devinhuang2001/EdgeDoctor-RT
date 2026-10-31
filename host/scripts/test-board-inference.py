"""Capture real CMSIS-NN/U55 outputs and compare every held-out golden vector."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import time

import numpy as np


def evaluate(raw: str, expected: np.ndarray) -> dict:
    results = {}
    pattern = r"ednn_csv,([^,\r\n]+),(\d+),(-?\d+),(-?\d+),(-?\d+),(-?\d+),(-?\d+)"
    for name in ("tflm-cmsis-nn", "ethos-u55-128"):
        seen = {}
        duplicate = False
        for match in re.finditer(pattern, raw):
            if match[1] != name:
                continue
            index = int(match[2])
            duplicate |= index in seen
            seen[index] = [int(match[k]) for k in range(3, 8)]
        complete = set(seen) == set(range(len(expected))) and not duplicate
        error = None
        mismatches = None
        if complete:
            actual = np.asarray([seen[i] for i in range(len(expected))], dtype=np.int32)
            error = int(np.max(np.abs(actual - expected.astype(np.int32))))
            mismatches = int(np.sum(np.argmax(actual, axis=1) != np.argmax(expected, axis=1)))
        results[name] = {"vectors": len(seen), "complete": complete,
                         "maximum_lsb_error": error, "class_mismatches": mismatches,
                         "pass": complete and error <= 1 and mismatches == 0}
    return {"status": "PASS" if all(row["pass"] for row in results.values()) else "FAIL",
            "backends": results}


def main() -> int:
    import serial
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="COM21")
    parser.add_argument("--golden", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    with serial.Serial(args.port, 115200, timeout=0.2) as handle:
        handle.reset_input_buffer()
        handle.write(b"edgeai parity\n")
        deadline = time.monotonic() + 45
        chunks = []
        while time.monotonic() < deadline:
            chunks.append(handle.read(4096))
            if b"msh >" in b"".join(chunks):
                break
    raw = b"".join(chunks).decode("utf-8", errors="replace")
    expected = np.load(args.golden, allow_pickle=False)["outputs"][:200]
    report = evaluate(raw, expected)
    report["golden_sha256"] = hashlib.sha256(args.golden.read_bytes()).hexdigest()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.with_suffix(".serial.txt").write_text(raw, encoding="utf-8")
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))
    return 0 if report["status"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
