# Four-tab UI refresh

Implemented the supplied light UI references for the 480×800 board. The existing six-item dark/red bottom navigation remains 64 px high. No startup, linker, HAL, generated configuration or Wi-Fi resources were changed.

## Changes

- `firmware/m55/ui/edgedoctor_ui.c`: common light-page layout with fixed header and vertically scrollable body; bounded wrapped labels; event filters, severity badges, detail and queued acknowledgement; 1h/24h runtime trend aggregation; device/baseline details, progress and actions; connection and event receipt status; local alarm demo start/stop. Old classifier widgets are shown only for replay/demo.
- `firmware/m55/ui/edgedoctor_ui_owner.c`: load the already bundled Chinese TTF once at 16/22/28 px. Allocation failures are logged and available Chinese font sizes reused.
- `firmware/m55/runtime/ed_monitor_app.[ch]`: nonblocking fixed-size copy of retained event slots and baseline state. No filesystem operation runs in this reader.
- `firmware/m55/protocol/ed_mqtt_client.[ch]`: read-only transport metadata and an event-receipt counter independent of telemetry PUBACKs.
- `tests/c/test_mqtt_contract.c`: verify transport metadata, event receipt identity, duplicate handling and separation from telemetry acknowledgement.
- `scripts/ui-preview/`: host build/render of the production LVGL UI using the BSP LVGL and bundled font. Hardware data is unavailable or explicitly supplied by test fixtures. This is not a photograph of the board.

## Verification

- `scripts/build-firmware.ps1`: successful build/link. Final incremental build has no C compiler warnings; existing RWX LOAD-segment linker warning remains. Earlier full build also emitted existing vendor warnings; none were suppressed.
- `scripts/test-common-c.ps1`: core, monitor, features, window, baseline, IMU profile, MQTT and voice checks.
- `python scripts/ui-preview/render.py`: renders empty and populated states; checks horizontal bounds, event-detail selection, failed-ack retry, fixed 736..799 navigation bounds, and 200 page changes with stable 334-object count.
- Preview files: `build/ui-preview/tab-2..5.png` (unavailable state) and `tab-2..5-fixture.png` (test data). Layout assertions are in `build/ui-preview/layout.txt`.

## Deliberate limits

- Events are the monitor's eight retained slots, not an unlimited archive. Pending means active OR unacknowledged.
- Trend memory covers up to 24 hours of the current run; it is cleared on reboot. Minute peaks are aggregated into 60 plot points; summary values describe observed valid windows, not guaranteed continuous wall-clock coverage.
- No sample image dates, device IDs, IPs or online states were copied into production as live data. Wi-Fi status uses the existing read-only state API; broker name is the last bound transport. Event-receipt count does not imply all events or a phone received a notification.
- Maintenance records and evidence playback do not yet have connected page services; the pages explain this instead of presenting false success.
- Firmware has not been flashed. Physical touch, panel rendering, memory under combined audio/sensor load and long-duration operation still require board validation.
