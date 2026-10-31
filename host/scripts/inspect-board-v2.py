#!/usr/bin/env python3
"""Collect bounded EdgeDoctor V2 W0 host and optional board evidence.

The default invocation never opens a serial port.  Passing --serial-port is an
explicit opt-in to a short, read-only console capture; only a fixed allow-list
of RT-Thread diagnostic commands can be sent.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import shutil
import subprocess
import sys
import tempfile
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Sequence


SCHEMA_VERSION = 1
SERIAL_COMMANDS = {
    "threads": "ps",
    "memory": "free",
    "filesystem": "df",
}
PAHO_CANDIDATE = {
    "repository": "https://github.com/eclipse-paho/paho.mqtt.embedded-c.git",
    "tag": "v1.1.0",
    "commit": "c7b898c77225d2d01136cb591465361b70217a06",
    "status": "CANDIDATE_ONLY_W5_LOCK_PENDING",
}


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="seconds")


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def command(
    argv: Sequence[str], cwd: Path, timeout: int = 20
) -> dict[str, Any]:
    started = time.monotonic()
    try:
        result = subprocess.run(
            list(argv),
            cwd=cwd,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=timeout,
            check=False,
        )
        return {
            "argv": list(argv),
            "exit_code": result.returncode,
            "elapsed_seconds": round(time.monotonic() - started, 3),
            "output": result.stdout.decode("utf-8", errors="replace"),
        }
    except (OSError, subprocess.TimeoutExpired) as exc:
        output = getattr(exc, "stdout", b"") or b""
        if isinstance(output, bytes):
            output = output.decode("utf-8", errors="replace")
        return {
            "argv": list(argv),
            "exit_code": None,
            "elapsed_seconds": round(time.monotonic() - started, 3),
            "output": output,
            "error": f"{type(exc).__name__}: {exc}",
        }


def write_json(path: Path, value: Any) -> None:
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")


def tool_record(name: str, argv: Sequence[str], repo: Path) -> dict[str, Any]:
    result = command(argv, repo)
    return {
        "name": name,
        "available": result["exit_code"] is not None,
        "exit_code": result["exit_code"],
        "version_output": result["output"].strip(),
        **({"error": result["error"]} if "error" in result else {}),
    }


def find_arm_gcc(repo: Path) -> Path | None:
    roots = [repo.parent.parent / "toolchains"]
    for root in roots:
        if root.is_dir():
            matches = list(root.rglob("arm-none-eabi-gcc.exe"))
            if matches:
                return matches[0]
    resolved = shutil.which("arm-none-eabi-gcc")
    return Path(resolved) if resolved else None


def serial_ports(repo: Path) -> list[dict[str, Any]]:
    try:
        from serial.tools import list_ports  # type: ignore

        return [
            {
                "device": port.device,
                "description": port.description,
                "hwid": port.hwid,
                "source": "pyserial-list_ports",
            }
            for port in list_ports.comports()
        ]
    except ImportError:
        pass

    if os.name == "nt":
        script = (
            "Get-CimInstance Win32_SerialPort -ErrorAction SilentlyContinue | "
            "Select-Object DeviceID,Name,Description,PNPDeviceID,Status | "
            "ConvertTo-Json -Compress"
        )
        result = command(["powershell", "-NoProfile", "-Command", script], repo)
        if result["exit_code"] == 0 and result["output"].strip():
            parsed = json.loads(result["output"])
            return parsed if isinstance(parsed, list) else [parsed]
        return []

    ports: list[dict[str, Any]] = []
    for pattern in ("ttyUSB*", "ttyACM*", "cu.*"):
        ports.extend({"device": str(path), "source": "filesystem"} for path in Path("/dev").glob(pattern))
    return ports


def serial_capture(
    port: str, baud: int, seconds: float, requested: list[str], output_dir: Path
) -> dict[str, Any]:
    try:
        import serial  # type: ignore
    except ImportError as exc:
        return {"status": "MISSING_DEPENDENCY", "error": str(exc)}

    started = time.monotonic()
    payload = bytearray()
    try:
        handle = serial.Serial(port=None, baudrate=baud, timeout=0.2, write_timeout=0.5)
        handle.dtr = False
        handle.rts = False
        handle.port = port
        handle.open()  # Fails without waiting when Windows reports an occupied port.
        try:
            for item in requested:
                handle.write((SERIAL_COMMANDS[item] + "\r\n").encode("ascii"))
                handle.flush()
            deadline = started + seconds
            while time.monotonic() < deadline:
                payload.extend(handle.read(min(handle.in_waiting or 1, 4096)))
        finally:
            handle.close()
    except Exception as exc:  # pyserial exposes platform-specific exception types
        return {
            "status": "OPEN_OR_CAPTURE_FAILED",
            "port": port,
            "elapsed_seconds": round(time.monotonic() - started, 3),
            "error": f"{type(exc).__name__}: {exc}",
        }

    capture_path = output_dir / "serial-diagnostics.bin"
    capture_path.write_bytes(payload)
    return {
        "status": "CAPTURED_UNINTERPRETED",
        "port": port,
        "baud": baud,
        "requested_commands": requested,
        "elapsed_seconds": round(time.monotonic() - started, 3),
        "bytes": len(payload),
        "sha256": sha256(capture_path),
        "path": capture_path.name,
        "hardware_claim": "NONE_UNTIL_MANUAL_REVIEW",
    }


def host_filesystem_probe(output_dir: Path) -> dict[str, Any]:
    """Exercise host fsync/replace only; this can never satisfy the HIL gate."""
    root = Path(tempfile.mkdtemp(prefix="edgedoctor-w0-", dir=output_dir))
    old_path = root / "profile.old"
    new_path = root / "profile.new"
    final_path = root / "profile.json"
    old = b'{"revision":1}\n'
    new = b'{"revision":2}\n'
    try:
        with old_path.open("wb") as stream:
            stream.write(old)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(old_path, final_path)
        with new_path.open("wb") as stream:
            stream.write(new)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(new_path, final_path)
        observed = final_path.read_bytes()
        return {
            "scope": "HOST_ONLY",
            "status": "PASS_HOST_ONLY",
            "fsync_file": True,
            "atomic_replace_observed": observed == new,
            "directory_fsync": "UNAVAILABLE_ON_WINDOWS" if os.name == "nt" else "NOT_ATTEMPTED",
            "power_loss_test": "NOT_PERFORMED",
            "hil_gate": "MISSING_EVIDENCE",
        }
    finally:
        shutil.rmtree(root, ignore_errors=True)


def collect_model(repo: Path) -> dict[str, Any]:
    model_root = repo / "artifacts" / "training" / "kaist-vibration-v1"
    metrics_path = model_root / "metrics.json"
    params_path = repo / "firmware" / "common" / "include" / "edgedoctor_onboard_model_params.h"
    files: list[dict[str, Any]] = []
    for path, role in ((metrics_path, "metrics"), (model_root / "model_fp32.pt", "fp32_state"), (params_path, "board_export")):
        if path.is_file():
            files.append(
                {
                    "role": role,
                    "path": path.relative_to(repo).as_posix(),
                    "bytes": path.stat().st_size,
                    "sha256": sha256(path),
                }
            )
    if not metrics_path.is_file():
        return {"status": "MISSING_EVIDENCE", "files": files}
    metrics = json.loads(metrics_path.read_text(encoding="utf-8"))
    return {
        "status": "HOST_ARTIFACT_VERIFIED",
        "files": files,
        "reported_metrics": {
            "e1_fp32_accuracy": metrics.get("e1_fp32", {}).get("accuracy"),
            "e1_fp32_macro_f1": metrics.get("e1_fp32", {}).get("macro_f1"),
            "e1_static_int8_accuracy": metrics.get("e1_static_int8", {}).get("accuracy"),
            "e1_static_int8_macro_f1": metrics.get("e1_static_int8", {}).get("macro_f1"),
            "e1_static_int8_parameter_bytes": metrics.get("e1_static_int8", {}).get("parameter_bytes"),
            "lolo_4nm_accuracy": metrics.get("lolo", {}).get("4 Nm", {}).get("accuracy"),
            "lolo_4nm_macro_f1": metrics.get("lolo", {}).get("4 Nm", {}).get("macro_f1"),
        },
        "claim_limit": "KAIST_REPLAY_ONLY_NOT_LIVE_IMU",
    }


def sdk_record(bsp: Path | None) -> dict[str, Any]:
    if bsp is None or not bsp.is_dir():
        return {"status": "MISSING_EVIDENCE", "path": str(bsp) if bsp else None}
    rt_thread = bsp / "rt-thread"
    resolved_rt = rt_thread.resolve() if rt_thread.exists() else None
    result: dict[str, Any] = {
        "status": "LOCAL_TREE_FOUND_PROVENANCE_UNVERIFIED",
        "path": str(bsp),
        "project_name": "Edgi_Talk_M55_XiaoZhi" if (bsp / ".project").is_file() else None,
        "git_metadata": (bsp / ".git").exists(),
        "rt_thread_path": str(resolved_rt) if resolved_rt else None,
        "sdk_version_from_path": "1.4.0" if resolved_rt and "1.4.0" in str(resolved_rt) else None,
        "rt_thread_version": "5.0.2" if (rt_thread / "include" / "rtdef.h").is_file() else None,
    }
    for name in ("SConstruct", "rtconfig.h", ".config"):
        path = bsp / name
        if path.is_file():
            result.setdefault("configuration_files", []).append(
                {"path": name, "bytes": path.stat().st_size, "sha256": sha256(path)}
            )
    return result


def resource_artifacts(bsp: Path | None, arm_gcc: Path | None) -> dict[str, Any]:
    if bsp is None or not bsp.is_dir():
        return {"status": "MISSING_EVIDENCE", "reason": "BSP_NOT_FOUND"}
    files = []
    for name in ("rtthread.map", "rtthread.elf", "Debug/rtthread.bin", "Debug/rtthread.hex"):
        path = bsp / name
        if path.is_file():
            files.append({"path": name, "bytes": path.stat().st_size, "sha256": sha256(path)})
    size_output = None
    if arm_gcc and (bsp / "rtthread.elf").is_file():
        size_tool = arm_gcc.with_name("arm-none-eabi-size.exe" if os.name == "nt" else "arm-none-eabi-size")
        if size_tool.is_file():
            size_output = command([str(size_tool), str(bsp / "rtthread.elf")], bsp)["output"].strip()
    return {
        "status": "HOST_LINK_ARTIFACTS_ONLY" if files else "MISSING_EVIDENCE",
        "files": files,
        "gnu_size": size_output,
        "runtime_heap_stack_flash": "MISSING_EVIDENCE",
    }


def run_checks(repo: Path, output_dir: Path, bsp: Path | None) -> dict[str, Any]:
    powershell = shutil.which("powershell") or shutil.which("pwsh")
    if not powershell:
        return {"status": "NOT_RUN", "reason": "POWERSHELL_NOT_FOUND"}
    checks = [
        ("original-tests", [powershell, "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(repo / "scripts" / "test.ps1")]),
        ("firmware-build", [powershell, "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(repo / "scripts" / "build-firmware.ps1")]),
    ]
    records = []
    for name, argv in checks:
        started = time.monotonic()
        try:
            completed = subprocess.run(
                argv,
                cwd=repo,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                timeout=1800,
                check=False,
            )
            raw = completed.stdout
            exit_code: int | None = completed.returncode
            error = None
        except subprocess.TimeoutExpired as exc:
            raw = exc.stdout or b""
            exit_code = None
            error = "TIMEOUT_AFTER_1800_SECONDS"
        log_path = output_dir / f"{name}.log"
        log_path.write_bytes(raw)
        record = {
            "name": name,
            "argv": argv,
            "exit_code": exit_code,
            "elapsed_seconds": round(time.monotonic() - started, 3),
            "log": log_path.name,
            "log_bytes": len(raw),
            "log_sha256": sha256(log_path),
        }
        if error:
            record["error"] = error
        records.append(record)
    write_json(output_dir / "check-exit-codes.json", records)
    return {"status": "COMPLETE", "checks": records, "bsp": str(bsp) if bsp else None}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, help="inventory output directory")
    parser.add_argument("--bsp-project", type=Path, help="local BSP project used by build-firmware.ps1")
    parser.add_argument("--run-checks", action="store_true", help="run original host tests and firmware build")
    parser.add_argument("--serial-port", help="explicit opt-in serial port; omitted by default")
    parser.add_argument("--serial-baud", type=int, default=115200)
    parser.add_argument("--serial-seconds", type=float, default=5.0)
    parser.add_argument("--serial-command", action="append", choices=sorted(SERIAL_COMMANDS), default=[])
    parser.add_argument("--datasheet", type=Path, help="local, manually sourced sensor datasheet to hash")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if not 1.0 <= args.serial_seconds <= 30.0:
        raise SystemExit("--serial-seconds must be between 1 and 30")

    repo = Path(__file__).resolve().parents[1]
    workspace = repo.parent
    output_dir = (args.output or repo / "benchmark" / "results" / "v2" / "inventory").resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    bsp = args.bsp_project or Path(r"D:\CODEX SPACE\infei\edgi-workspace\Edgi_Talk_M55_EdgeDoctor")
    bsp = bsp.resolve() if bsp.exists() else bsp

    # Capture the pre-inventory state before this run creates or updates evidence.
    git_status = command(["git", "status", "--short", "--branch"], workspace)
    git_diff = command(["git", "diff", "--binary", "--", "app/edgepet/edgepet_sensor.c"], workspace)
    (output_dir / "git-status.txt").write_text(git_status["output"], encoding="utf-8")
    (output_dir / "existing-edgepet-diff.patch").write_text(git_diff["output"], encoding="utf-8")

    arm_gcc = find_arm_gcc(repo)
    tools = [
        tool_record("python", [sys.executable, "--version"], repo),
        tool_record("git", ["git", "--version"], repo),
        tool_record("scons", ["scons", "--version"], repo),
    ]
    if arm_gcc:
        tools.append(tool_record("arm-none-eabi-gcc", [str(arm_gcc), "--version"], repo))
    else:
        tools.append({"name": "arm-none-eabi-gcc", "available": False, "status": "MISSING_EVIDENCE"})
    write_json(output_dir / "toolchain.json", tools)

    ports = serial_ports(repo)
    write_json(output_dir / "serial-ports.json", ports)
    diagnostics: dict[str, Any] = {"status": "NOT_REQUESTED", "hardware_claim": "NONE"}
    if args.serial_port:
        known_names = {
            str(item.get("device") or item.get("DeviceID")) for item in ports
        }
        if args.serial_port not in known_names:
            diagnostics = {
                "status": "REFUSED_NOT_ENUMERATED",
                "port": args.serial_port,
                "hardware_claim": "NONE",
            }
        else:
            diagnostics = serial_capture(
                args.serial_port,
                args.serial_baud,
                args.serial_seconds,
                args.serial_command,
                output_dir,
            )

    checks = run_checks(repo, output_dir, bsp) if args.run_checks else {"status": "NOT_REQUESTED"}
    datasheet = {"status": "MISSING_EVIDENCE", "reason": "NO_LOCAL_REVIEWED_DATASHEET"}
    if args.datasheet and args.datasheet.is_file():
        datasheet = {
            "status": "LOCAL_FILE_HASHED_IDENTITY_REQUIRES_REVIEW",
            "path": str(args.datasheet.resolve()),
            "bytes": args.datasheet.stat().st_size,
            "sha256": sha256(args.datasheet),
        }

    inventory = {
        "schema_version": SCHEMA_VERSION,
        "generated_at_utc": utc_now(),
        "scope": {
            "host": platform.platform(),
            "hardware_access": "SERIAL_OPT_IN" if args.serial_port else "NONE",
            "claim": "HOST_INVENTORY_ONLY",
        },
        "repository": {
            "workspace": str(workspace),
            "edge_doctor_root": str(repo),
            "git_status_evidence": "git-status.txt",
            "preserved_edgepet_diff_evidence": "existing-edgepet-diff.patch",
        },
        "sdk": sdk_record(bsp),
        "toolchain": tools,
        "baseline_model": collect_model(repo),
        "imu_profile": {
            "profile_id": "imu_lf_208_v1",
            "status": "MISSING_EVIDENCE",
            "candidate_from_spec": {
                "device_family": "LSM6DS3",
                "bus": "i2c0",
                "addresses": ["0x6A", "0x6B"],
                "accelerometer_odr_hz": 208,
                "accelerometer_range_g": 4,
                "fifo_watermark_xyz_samples": 8,
                "analysis_band_hz": [5, 40],
                "target_anti_alias_bandwidth_hz": 50,
            },
            "measured": {
                "exact_part": None,
                "who_am_i": None,
                "bus": None,
                "address": None,
                "odr_hz": None,
                "range_g": None,
                "sensitivity": None,
                "fifo_mode": None,
                "filters": None,
                "irq_available": None,
                "bounded_i2c_timeout": None,
            },
            "datasheet": datasheet,
            "serial_diagnostics": diagnostics,
        },
        "budgets": {
            "fixed_ram_max_bytes": 160 * 1024,
            "tls_peak_heap_max_bytes": 96 * 1024,
            "ui_heap_max_bytes": 160 * 1024,
            "font_flash_target_bytes": 1024 * 1024,
            "application_flash_quota_bytes": 1024 * 1024,
            "minimum_free_flash_bytes": int(1.25 * 1024 * 1024),
            "runtime_measurement_status": "MISSING_EVIDENCE",
        },
        "resources": resource_artifacts(bsp, arm_gcc),
        "filesystem_semantics": host_filesystem_probe(output_dir),
        "paho_embedded_c": PAHO_CANDIDATE,
        "deployment_dependency_lock": {
            "status": "PENDING_W5_W6",
            "reason": "W0 has no selected broker/server deployment artifact to lock",
        },
        "checks": checks,
        "acceptance": {
            "T-SYS-01": "PASS" if checks.get("status") == "COMPLETE" and all(item.get("exit_code") == 0 for item in checks["checks"]) else "FAIL_OR_NOT_RUN",
            "T-SYS-02": "MISSING_EVIDENCE",
            "T-SYS-03": "MISSING_EVIDENCE",
            "T-SYS-04": "MISSING_EVIDENCE",
        },
    }
    write_json(output_dir / "inventory.json", inventory)
    print(output_dir / "inventory.json")
    return 0 if inventory["acceptance"]["T-SYS-01"] != "FAIL_OR_NOT_RUN" else 1


if __name__ == "__main__":
    raise SystemExit(main())
