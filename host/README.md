# EdgeDoctor-RT

EdgeDoctor-RT is a reproducible audio/vibration fault-diagnosis project for the
Infineon PSoC Edge E84 Edgi-Talk board and RT-Thread. It is an application layer
on top of the official `Edgi_Talk_M55_XiaoZhi` v1.4.0 baseline; vendor startup,
linker, HAL, generated configuration, and Wi-Fi resources remain untouched.

The canonical benchmark path is:

```text
KAIST recording -> validated manifest -> recording-level split -> EdgePack
-> 20 ms replay -> board DSP -> INT8 fusion -> result snapshot -> GUI/CSV
```

KAIST replay remains an independent benchmark path. The board application also
starts a live LSM6DS3TR-C low-frequency monitor at boot; live IMU snapshots and
KAIST classification results are kept separate and never mixed in accuracy
metrics.

## Repository status

The implementation is organized so data and algorithm code can be tested on a
Windows host before it is integrated into the board BSP. Hardware-only evidence
(U55/Vela reports, 30-minute replay, screenshots, and IPC stress logs) is stored
under `benchmark/results/` when produced; no placeholder result is treated as a
pass.

## Quick verification

```powershell
cd EdgeDoctor-RT
.\scripts\test.ps1
```

The test runner builds the C core with warnings as errors and runs the Python
data, split, preprocessing, and EdgePack tests.

## Desktop digital-twin console

`ui/desktop/` implements the desktop console specified in
`docs/specs/desktop-dashboard-v1/`. It is a Vite + React + TypeScript + Three.js
application whose build output is served from `server/web-v2/` at
`/dashboard-v2/`; the legacy `/dashboard/` stays untouched and the root redirect
still points at it until the switch is reviewed.

```powershell
cd EdgeDoctor-RT\ui\desktop
npm ci
npm run build          # validates asset bindings, lints, type-checks, then builds
npm test               # unit tests for schema, trends, spectrum rules and status mapping
```

Acceptance for this console (browser, visual, security and accessibility layers):

```powershell
cd EdgeDoctor-RT
powershell -ExecutionPolicy Bypass -File .\scripts\test-ui.ps1
```

Evidence (manifest, per-case logs, HAR, screenshots, metrics, checksums) is
written to `benchmark/results/desktop-dashboard/{run_id}/`. The 2026-09-15 run is
recorded in `docs/desktop-dashboard-acceptance-2026-09-15.md`; its GPU first-frame
gate and the EDV2 spectrum branch are reported as `MISSING_EVIDENCE` and
`OUT_OF_SCOPE` rather than as passes.

## Data preparation

1. Obtain KAIST dataset version 6 (DOI `10.17632/ztmf3m7h5x.6`) under CC BY 4.0.
2. Keep the downloaded data outside Git, for example under `data/raw/kaist-v6`.
3. Generate a manifest, verify synchronized audio/vibration pairs, create the
   recording-level E1 and LOLO split manifests, then convert recordings to
   `.edgp` files. The commands are documented in `docs/data-pipeline.md`.

## Acceptance policy

Requirements and evidence gates are tracked in `docs/verification-matrix.md`.
Host tests, board tests, ML quality gates, and NPU compile evidence are reported
separately. A missing board or dataset is reported as missing evidence, never as
a synthetic pass.


## Board IMU quick check

The board-mounted LSM6DS3TR-C is configured for 208 Hz and ±4g. The E84
integration uses a bounded direct-output worker by default because this board's
FIFO can wedge the I²C state machine after an overrun; the FIFO implementation
remains available for a verified board variant. `edgeimu start` is idempotent;
it does not create a second producer. `edgeimu sample` reads the latest monitor
snapshot, while `edgeimu info` reports sample counters, measured rate, FIFO gaps,
and bus errors. `edgeimu cal selftest` starts or reports the 416-sample
stationary gravity/RMS check; `edgeimu cal` reports its current state.

Start a healthy baseline with `edgeimu baseline start`, inspect progress with
`edgeimu baseline status`, and request a queued commit with `edgeimu baseline
commit` after 60 valid windows. Baselines are accepted only when schema, CRC,
sensor profile, operating profile, and asset identity match. `edgeimu baseline
load` queues a monitor-owned reload after reboot.

For a stationary board-only acceptance run, use `edgeimu baseline start idle`;
this explicitly permits idle windows and does not change the default quality
gate. The autonomous serial acceptance script is:

```powershell
python .\scripts\test-board-v2.py --port COM21 --seconds 60
```

It starts capture once and then only reads diagnostics. A PASS requires growing
sample counters, valid quality, measured rate near 208 Hz, no known gaps or bus
errors, and at least one connected MQTT observation. Missing or interrupted
evidence remains `MISSING_EVIDENCE`.
