# Verification matrix

Status values are `PASS`, `FAIL`, and `MISSING_EVIDENCE`. Only evidence produced
by the named gate can establish `PASS`.

| Scope | Requirement | Gate |
|---|---|---|
| Data | version/license/hash/parser/sync/leakage/round-trip | `scripts/test.ps1` plus pinned real-dataset manifest |
| ML | AUC >= 0.90, E1 Macro-F1 >= 0.85, LOLO >= 0.75 | signed evaluation JSON generated from held-out manifests |
| Quantization | INT8 degradation <= 2 percentage points | FP32/INT8 paired evaluation JSON |
| Firmware DSP | Python/M55 audio and vibration parity | board-exported golden vector report |
| Inference | Reference/CMSIS-NN/U55 parity | same-input board parity CSV |
| Runtime | 30-minute replay, no growth, bounded queues | board stability JSON and serial log |
| Performance | >=200 warm samples, P50/P95, coverage >=95% | profiler CSV and Vela compile report |
| GUI | four 800x480 pages, responsive touch, real stats | four screenshots plus 100-switch board test |
| Desktop dashboard | four routes, nine UI states, frozen asset bindings, 3D resource release, security and a11y | `scripts/test-ui.ps1` with evidence under `benchmark/results/desktop-dashboard/{run_id}/` |
| IPC/P1 | 10,000 ordered messages and correct slot state | M33/M55 stress report |

## Current implementation evidence (2026-09-12)

| Area | Current result | Evidence |
|---|---|---|
| Portable contracts, quality, baseline, alarm, MQTT, voice | PASS | `scripts/test-common-c.ps1` |
| Server ingest, trends, evidence, commands, notification adapter | PASS | `scripts/test.ps1` (`28 passed`) |
| Firmware build | PASS | `scripts/build-firmware.ps1` (`scons: done building targets`) |
| E84 programming | PASS | OpenOCD `** Verified OK **` |
| Board IMU capture/quality smoke test | PASS | COM21 run: `drops=0/gaps=0/bus_errors=0/queue_high=0`, measured 208.15 Hz |
| Board MQTT/telemetry to a live broker | PASS | 120 s acceptance run: `connected=1 rejected=0` for the whole window; events and telemetry ingested into SQLite on the host |
| Board continuous IMU stability | PASS (120 s) | `benchmark/results/v2/board-stability.json` (`PASS_STABILITY`, `failure_reasons=[]`); the 8-hour gate remains missing |
| Unattended bring-up | PASS | Boot log: Wi-Fi manager loads the saved profile, associates, takes DHCP, and the MQTT worker starts without operator input |
| Server-to-device command round trip | PASS | `POST /api/v1/events/{id}/ack` settled as `APPLIED`; the server event advanced revision 2 -> 3 with `ack=ACKNOWLEDGED` |
| Device evidence capture and upload | PASS | `POST /api/v1/events/{id}/evidence-request` -> `APPLIED`; the stored EDV2 unpacks with a valid magic/CRC, 2080 samples (1040 pre + 1040 post), static Z axis reads 1.004 g |
| Computer notification delivery | PASS (2026-09-14) | `benchmark/results/v2/computer-notification-20260914.json`: real ntfy publish plus computer subscription received identical title/body/id |
| 8-hour stability | EXCLUDED_BY_REQUEST | User explicitly excluded this gate on 2026-09-14 |
| Field five-class / audio fusion accuracy | MISSING_EVIDENCE | Requires synchronized, labeled field recordings; no such data is available |
| Inference backend (CMSIS-NN/U55) | PASS (2026-09-12) | `benchmark/results/v2/board-inference-20260912.json`: both backends returned all 200 vectors, max error 1 LSB, class mismatches 0; Vela report shows 2/2 operators on U55 |
| Audio fusion model | MISSING_EVIDENCE | No synchronized, labeled device audio exists; current model is explicitly vibration-only and reports audio score unavailable |
| IPC ordered stress | PASS (host) | `scripts/test-common-c.ps1`: 10,000 publish/acquire/release cycles with frame and score integrity checks |
| UI page rendering / navigation | PASS (2026-09-14) | `scripts/ui-preview/render.py` rendered four pages at 480x800 with layout bounds checks; COM21 `edui_stress stress 100` completed 100 real LVGL owner-thread switches in 128 ms |
| Journal recovery / boot durability | PASS (host fault injection) | `test_journal_recovery.c`: partial/torn tail, write failure and durable boot counter cases passed |
| Desktop dashboard (spec 01 v0.1) | PASS 28 / FAIL 0 / MISSING_EVIDENCE 1 / OUT_OF_SCOPE 1 (2026-09-15) | `benchmark/results/desktop-dashboard/accept-20260915/`; report `docs/desktop-dashboard-acceptance-2026-09-15.md` |
| Desktop dashboard GPU first-frame gate | MISSING_EVIDENCE | Headless software rendering only; `metrics-3d06.json` records P95 499 ms but is not hardware-accelerated acceptance |
| Desktop dashboard LAN walkthrough | NOT_RUN | Requires a real bound device, live MQTT broker and site network |
| Desktop dashboard EDV2 spectrum view | OUT_OF_SCOPE | Server analysis endpoint returns `available:false`; the UI shows an explicit unavailable state (`per-case/DD-UI-05-*`) |

Host unit tests prove portable algorithms and packet safety. They do not replace
hardware, NPU, accuracy, or stability evidence.

## Known hardware/vendor defects observed (2026-09-12)

| Symptom | Evidence | Status |
|---|---|---|
| `ping <host>` from the shell hard-faults the board | `lwip_netconn_do_writemore` (`api_msg.c:2038`) resolved with `arm-none-eabi-addr2line`; PRECISERR at BFAR `0xFFFFFF8C` | Vendor lwIP path, not reproduced after reboot; avoid `ping` in acceptance runs |
| Wi-Fi credentials were not persisted by `wifi join` | Boot log showed `No saved config found` and reverted to AP mode | Fixed: `edwifi save <ssid> <password>` writes the profile the BSP replays at boot |

Host unit tests prove portable algorithms and packet safety. They do not replace
hardware, NPU, accuracy, or stability evidence.
