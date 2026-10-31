# EdgeDoctor LAN service

This service stores versioned event revisions before returning the application receipt. It exposes health, event history, durable command queue, and a Paho MQTT 3.1.1 bridge. SQLite WAL is the default single-host store. Compose runs the API and MQTT ingest as separate services so message consumption remains independent of HTTP serving.

Run the API from the repository root after installing `server/requirements.txt`:

```powershell
$env:PYTHONPATH='EdgeDoctor-RT'
uvicorn server.app:app --host 127.0.0.1 --port 8080
```

The MQTT bridge requires `EDGEDOCTOR_MQTT_CA`, credentials, topic, and client ID environment variables. No credentials are included in the repository.
For a loopback development broker only, set `EDGEDOCTOR_MQTT_TLS=0`; production
configurations leave this unset so TLS remains mandatory.

Before starting Compose, run `powershell -ExecutionPolicy Bypass -File ..\scripts\mqtt-provision.ps1 -User edgedoctor` to create the local Mosquitto password file. The generated `server/passwd` is intentionally untracked.
Set `EDGEDOCTOR_MQTT_USER` and `EDGEDOCTOR_MQTT_PASSWORD` in the shell that runs Compose so the Broker health check can authenticate.
可复制 `.env.example` 为 `.env` 后填写现场凭据；`.env` 已被忽略，不会进入版本库。

`notify.publish_ntfy` only reports channel acceptance; it never claims that a phone displayed the notification. Use HTTPS and a protected topic in deployment.

## API quick reference

- `POST /api/v1/events` — ingest a versioned event; duplicate revisions are idempotent.
- `POST /api/v1/events/{event_id}/ack` — create a durable remote acknowledgement command.
- `POST /api/v1/commands/{command_id}/result` — apply the device receipt (`APPLIED`, `REJECTED`, or `EXPIRED`).
- `POST /api/v1/telemetry`, `GET /api/v1/trends/minute` — store and aggregate trend data.
- `GET /api/v1/query/status` and `GET /api/v1/query/recent-alerts` — read-only XiaoZhi/voice responses.
- `GET /api/v1/notifications/status`, `POST /api/v1/notifications/test` — check and test the configured ntfy channel.
- `POST/GET /api/v1/assets/{asset_id}/maintenance` and `/maintenance/compare` — maintenance history and before/after metrics.
- `POST/GET /api/v1/events/{event_id}/evidence` — EDV2 evidence upload/download; protected when an API token is configured.
- `POST /api/v1/events/{event_id}/evidence-request` — queue a 5 s pre/5 s post raw-sample capture challenge for the device.

All mutation requests accept `X-Edgedoctor-Token` when `EDGEDOCTOR_API_TOKEN` is set. Alarm events with `ACTIVE` + `UNACKNOWLEDGED` are sent asynchronously to ntfy when `EDGEDOCTOR_NTFY_URL` and `EDGEDOCTOR_NTFY_TOPIC` are configured.

Run the complete host regression with `powershell -ExecutionPolicy Bypass -File ..\scripts\test.ps1`; build the board image with `..\scripts\build-firmware.ps1`.

For LAN integration without a board, use `python ..\tools\mqtt_simulate.py --mode alarm` (requires the pinned `paho-mqtt` dependency). This validates routing and service behavior only; it is not hardware acceptance.

If Docker is unavailable, start a disposable plaintext broker for local/LAN
development with `python ..\tools\mqtt_dev_broker.py --port 1883`, then run the
ingest service with `EDGEDOCTOR_MQTT_TLS=0` and `EDGEDOCTOR_MQTT_PORT=1883`.
This broker allows anonymous clients and must never be exposed as a production
endpoint; production deployment remains the TLS Mosquitto compose stack above.

Windows one-command local demo (broker + API + MQTT ingest):
`powershell -ExecutionPolicy Bypass -File ..\scripts\pc-demo-up.ps1`.
It serves the dashboard at `http://127.0.0.1:8080/dashboard/` and stores its
SQLite database and logs below `EdgeDoctor-RT\tmp\pc-demo`.

For a connected board, one command provisions Wi-Fi, broker settings, and the
background MQTT worker:
`powershell -ExecutionPolicy Bypass -File ..\scripts\provision-board-mqtt.ps1 -Ssid <SSID> -Password <PASSWORD> -BrokerHost <LAN-IP> -BrokerPort 1883`.

For a password-protected broker, add `-MqttUser <USER> -MqttPassword <PASSWORD>`.
The board also accepts `edmqtt stop`, `edmqtt auth <user> <password>`, then
`edmqtt start`. Run `auth` after `config`, which resets credentials. Existing
credential-free configuration files remain readable. Reboot after changing
the device ID so the monitor and transport adopt the same identity.
The BSP shell limits each command to 79 bytes; provisioning rejects longer
credential commands rather than silently truncating them.

Event PUBACKs only mark broker delivery: unreceipted revisions stay queued,
retry after 10 seconds, and are retried immediately after reconnecting. Only
the service's `persisted` receipt removes a revision from the event outbox.

Run the board stability evidence collector for a complete eight-hour gate with
`python ..\scripts\test-board-v2.py --port COM21 --seconds 28800`; the script
returns `PASS_LONG_STABILITY` only when samples and MQTT states remain valid,
the queue stays bounded, and no messages are rejected.
