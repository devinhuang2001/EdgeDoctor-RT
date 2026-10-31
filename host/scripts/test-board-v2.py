#!/usr/bin/env python3
"""Run a bounded autonomous live-IMU acceptance capture.

The board is started once. Subsequent reads are diagnostics only; the script
never sends ``sample`` as a producer command and cannot manufacture evidence.
"""
from __future__ import annotations

import argparse
import json
import re
import math
import statistics
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

INFO = re.compile(r"connected=(?P<connected>\d+) queued=(?P<queued>\d+) sent=(?P<sent>\d+) persisted=(?P<persisted>\d+) rejected=(?P<rejected>\d+)(?: inbox_dropped=(?P<inbox_dropped>\d+))?")
STATS = re.compile(r"running=(?P<running>\d+) nominal_hz=(?P<nominal_hz>\d+) measured_millihz=(?P<measured_millihz>\d+) samples=(?P<samples>\d+) drops=(?P<drops>\d+) gaps=(?P<gaps>\d+) bus_errors=(?P<bus_errors>\d+) queue_high=(?P<queue_high>\d+)")
SAMPLE = re.compile(r"quality=(?P<quality>\d+).*baseline=(?P<baseline>\d+) deviation=(?P<deviation>(?:[-+0-9.eE]+|nan))")
PROMPT = b"msh />"


def evaluate_stability(samples: list[dict[str, Any]], mqtt_states: list[dict[str, Any]], errors: list[str], requested_seconds: int, elapsed_seconds: float) -> tuple[bool, list[str]]:
    reasons: list[str] = []
    if errors:
        reasons.append("serial_or_capture_error")
    if elapsed_seconds + 1.0 < requested_seconds:
        reasons.append("insufficient_runtime")
    if len(samples) < 2:
        reasons.append("fewer_than_two_snapshots")
    else:
        counters = [item.get("samples") for item in samples]
        if any(not isinstance(value, int) for value in counters):
            reasons.append("missing_sample_counter")
        elif counters[-1] <= counters[0]:
            reasons.append("sample_counter_not_increasing")
        elif any(right <= left for left, right in zip(counters, counters[1:])):
            reasons.append("sample_counter_reset_or_stalled")
        if any(item.get("quality") in {3, 5, 6} for item in samples):
            reasons.append("quality_disconnected_gapped_or_config_error")
        if not any(item.get("quality") == 1 for item in samples):
            reasons.append("quality_never_valid")
        if any(item.get("drops", 0) or item.get("gaps", 0) or item.get("bus_errors", 0) for item in samples):
            reasons.append("known_capture_errors")
        measured = [item.get("measured_millihz", 0) for item in samples]
        measured = [value for value in measured if isinstance(value, int) and value]
        if not measured:
            reasons.append("missing_measured_rate")
        elif any(value < 197000 or value > 219000 for value in measured):
            reasons.append("measured_rate_outside_tolerance")
    if not mqtt_states:
        reasons.append("missing_mqtt_snapshots")
    elif not any(item.get("connected") == 1 for item in mqtt_states):
        reasons.append("mqtt_never_connected")
    elif any(item.get("connected") != 1 for item in mqtt_states):
        reasons.append("mqtt_disconnected_during_capture")
    if any(item.get("rejected", 0) for item in mqtt_states):
        reasons.append("mqtt_rejected_messages")
    if any(item.get("inbox_dropped", 0) for item in mqtt_states):
        reasons.append("mqtt_inbox_overflow")
    return not reasons, reasons


def parse_capture(text: str) -> tuple[dict[str, Any] | None, dict[str, Any] | None, dict[str, Any] | None]:
    stats_match = STATS.search(text)
    sample_match = SAMPLE.search(text)
    info_match = INFO.search(text)
    stats = None
    sample = None
    mqtt = None
    if stats_match:
        stats = {key: int(value) for key, value in stats_match.groupdict().items()}
        if sample_match:
            sample = {"quality": int(sample_match.group("quality")), "baseline_windows": int(sample_match.group("baseline")), "deviation": float(sample_match.group("deviation")), **stats}
    if info_match:
        mqtt = {key: int(value) for key, value in info_match.groupdict().items() if value is not None}
    return stats, sample, mqtt


def parse_heap(text: str) -> dict[str, int]:
    pools = {}
    for line in text.splitlines():
        fields = line.split()
        if len(fields) == 4 and fields[0] in {"heap", "hyperam"} and all(value.isdigit() for value in fields[1:]):
            pools[fields[0]] = int(fields[1]) - int(fields[3])
    return pools


def evaluate_heap(samples: list[dict[str, Any]], elapsed: float) -> list[str]:
    if not samples or samples[0]["elapsed_seconds"] > 120 or samples[-1]["elapsed_seconds"] < elapsed - 120:
        return ["missing_long_heap_evidence"]
    first = [item for item in samples if item["elapsed_seconds"] <= 3600]
    last = [item for item in samples if item["elapsed_seconds"] >= elapsed - 3600]
    if len(first) < 30 or len(last) < 30:
        return ["insufficient_hourly_heap_samples"]
    if any(not item.get("pools") or set(item["pools"]) != set(samples[0]["pools"]) for item in samples):
        return ["inconsistent_heap_pools"]
    reasons = []
    for pool in samples[0]["pools"]:
        growth = statistics.median(item["pools"][pool] for item in last) - statistics.median(item["pools"][pool] for item in first)
        if growth > 4096:
            reasons.append(f"heap_growth_exceeds_4KiB:{pool}")
        if all(right["pools"][pool] >= left["pools"][pool] for left, right in zip(last, last[1:])) and last[-1]["pools"][pool] - last[0]["pools"][pool] > 256:
            reasons.append(f"continued_heap_growth:{pool}")
    return reasons


def send_command(handle: Any, command: str, timeout: float = 3.0) -> str:
    """Send one command and wait for its prompt before issuing the next."""
    handle.reset_input_buffer()
    handle.write((command + "\r\n").encode("ascii"))
    handle.flush()
    end = time.monotonic() + timeout
    payload = bytearray()
    echoed = False
    while time.monotonic() < end:
        chunk = handle.read(4096)
        if chunk:
            payload.extend(chunk)
            echoed = echoed or command.encode("ascii") in payload
            if echoed and PROMPT in payload:
                break
    return payload.decode(errors="replace")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", default="COM21")
    parser.add_argument("--seconds", type=int, default=60)
    parser.add_argument("--interval", type=float, default=5.0)
    parser.add_argument("--output", type=Path, default=Path("benchmark/results/v2/board-stability.json"))
    args = parser.parse_args()
    if not 10 <= args.seconds <= 28800:
        parser.error("--seconds must be between 10 and 28800")
    if not 0.2 <= args.interval <= 60:
        parser.error("--interval must be between 0.2 and 60 seconds")
    try:
        import serial  # type: ignore
    except ImportError as exc:
        parser.error(str(exc))

    started = time.monotonic()
    samples: list[dict[str, Any]] = []
    states: list[dict[str, Any]] = []
    errors: list[str] = []
    heap_samples: list[dict[str, Any]] = []
    next_heap_at = 0.0
    checkpoint = args.output.with_suffix(args.output.suffix + ".partial")
    try:
        handle = serial.Serial(port=None, baudrate=115200, timeout=0.2)
        handle.dtr = False; handle.rts = False; handle.port = args.port
        handle.open()
        with handle:
            time.sleep(3.0)
            handle.read(16384)
            for command in ("edgeimu start", "edmqtt start"):
                send_command(handle, command)
            while time.monotonic() - started < args.seconds:
                # ``edgeimu sample`` is a read-only latest-snapshot query;
                # it does not create a producer or block the sensor worker.
                text = send_command(handle, "edgeimu sample")
                text += send_command(handle, "edgeimu info")
                text += send_command(handle, "edmqtt info")
                _stats, sample, mqtt = parse_capture(text)
                if sample:
                    if not math.isfinite(sample["deviation"]): sample["deviation"] = None
                    samples.append(sample)
                if mqtt:
                    states.append(mqtt)
                elapsed_now = time.monotonic() - started
                if elapsed_now >= next_heap_at:
                    heap_samples.append({"elapsed_seconds": elapsed_now, "pools": parse_heap(send_command(handle, "free"))})
                    next_heap_at = elapsed_now + 60
                checkpoint.parent.mkdir(parents=True, exist_ok=True)
                checkpoint.write_text(json.dumps({"schema_version": 2, "status": "IN_PROGRESS", "elapsed_seconds": round(time.monotonic() - started, 2), "samples": samples, "mqtt_states": states, "heap_samples": heap_samples, "errors": errors}, ensure_ascii=False), encoding="utf-8")
                time.sleep(min(args.interval, max(0.0, args.seconds - (time.monotonic() - started))))
    except Exception as exc:
        errors.append(f"{type(exc).__name__}: {exc}")

    elapsed = time.monotonic() - started
    passed, reasons = evaluate_stability(samples, states, errors, args.seconds, elapsed)
    if args.seconds >= 28800:
        reasons += evaluate_heap(heap_samples, elapsed)
        passed = not reasons
    result = {"schema_version": 2, "generated_at": datetime.now(timezone.utc).isoformat(), "port": args.port, "requested_seconds": args.seconds, "elapsed_seconds": round(elapsed, 2), "samples": samples, "mqtt_states": states, "heap_samples": heap_samples, "errors": errors, "failure_reasons": reasons, "scope": "live IMU, MQTT, heap; physical voice playback and power-cut tests are separate", "status": ("PASS_LONG_STABILITY" if args.seconds >= 28800 else "PASS_STABILITY" if args.seconds >= 30 else "PASS_SMOKE") if passed else "MISSING_EVIDENCE"}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    if passed:
        checkpoint.unlink(missing_ok=True)
    print(args.output)
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
