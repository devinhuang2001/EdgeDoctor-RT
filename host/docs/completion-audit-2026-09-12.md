# Completion audit — 2026-09-12

The previous claim that all work was complete was not supported by the
requirement-level evidence. Completion remains unproven.

## New board evidence

- COM21 60-second capture: `benchmark/results/v2/board-audit-2026-09-12.json`.
  IMU samples progressed without reported drops, gaps or bus errors. MQTT
  never connected, so this is not a passing end-to-end stability run.
- Stationary calibration: `benchmark/results/v2/imu-cal-20260912.txt`.
  The running board reported PASS over 416 samples, gravity 1.0029 g and
  dynamic RMS 0.0015 g. This is evidence for the running image; its source
  revision has not been proven to match the current worktree.
- KitProg3 CMSIS-DAP is present. Wireless association to the user's network
  succeeded, but a usable DHCP address and broker connection remain unproven.

## Remaining scope

- Prove the newly pruned firmware links, then program and verify that image.
- Verify MQTT TLS, receipts and command acknowledgement on the board.
- Execute power-loss and long-duration gates rather than inferring them from
  host tests or fsync calls.
- Audit live audio integration against its actual input contract. The legacy
  fusion pipeline requires 16 kHz audio and 6.4 kHz vibration; the 208 Hz IMU
  stream cannot be represented as that vibration source without a different
  validated model/profile. V2 deliberately specifies low-frequency detection
  without forced five-class labels.
- Check every original milestone and V2 UI/data requirement against current
  behavior. Disabled UI placeholders do not prove implemented history or
  maintenance functionality.

The acceptance script now rejects an entirely missing/zero measured sample
rate. Its regression tests include a valid post-warmup measured-rate case.

## Inference backend evidence

- The exported INT8 vibration model is 4,824 bytes; held-out accuracy is
  0.970 versus FP32 0.965. Vela reports two NPU operators and zero CPU
  operators.
- `benchmark/results/v2/board-inference-20260912.json` records a real COM21
  run after programming the new image. CMSIS-NN and Ethos-U55 each returned
  200 vectors with maximum error 1 LSB and zero class mismatches.
- Audio fusion remains unavailable by design because no synchronized, labeled
  device recordings exist. The firmware does not copy vibration scores into
  the audio field.
