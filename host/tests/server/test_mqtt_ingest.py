import os
import json
from types import SimpleNamespace
from fastapi.testclient import TestClient


def test_mqtt_command_receipt_with_api_token(tmp_path, monkeypatch):
    import server.app as app
    import server.mqtt_ingest as bridge

    monkeypatch.setattr(app, "DB_PATH", tmp_path / "receipt.db")
    monkeypatch.setenv("EDGEDOCTOR_API_TOKEN", "test-token")
    client = TestClient(app.app)
    headers = {"X-Edgedoctor-Token": "test-token"}
    event = {"schema_version": 2, "device_id": "ed84-001", "event_id": "ack-test",
             "revision": 1, "source": "LIVE_IMU", "condition": "ACTIVE",
             "ack": "UNACKNOWLEDGED"}
    assert client.post("/api/v1/events", json=event, headers=headers).status_code == 200
    response = client.post("/api/v1/events/ack-test/ack",
                           json={"actor": "operator", "expected_revision": 1}, headers=headers)
    command_id = response.json()["command_id"]
    for status in ("PENDING", "APPLIED", "APPLIED"):
        bridge.on_message(None, None, SimpleNamespace(
            topic="edgedoctor/site_1/ed84-001/command-results",
            payload=json.dumps({"device_id": "ed84-001", "command_id": command_id,
                                "status": status}).encode()))
        assert client.get(f"/api/v1/commands/{command_id}").json()["status"] == status
    assert client.post(f"/api/v1/commands/{command_id}/result",
                       json={"status": "APPLIED"}).status_code == 401


def test_mqtt_run_allows_explicit_plaintext_dev_mode(monkeypatch):
    import server.mqtt_ingest as module

    calls = []

    class FakeClient:
        def __init__(self, *args, **kwargs):
            calls.append(("client", args, kwargs))

        def username_pw_set(self, *args):
            calls.append(("auth", args))

        def tls_set(self, **kwargs):
            calls.append(("tls", kwargs))

        def connect(self, *args):
            calls.append(("connect", args))

        def loop_forever(self, **kwargs):
            calls.append(("loop", kwargs))

        def reconnect_delay_set(self, **kwargs):
            calls.append(("retry", kwargs))

    monkeypatch.setattr(module.mqtt, "Client", FakeClient)
    monkeypatch.setenv("EDGEDOCTOR_CLIENT_ID", "test")
    monkeypatch.setenv("EDGEDOCTOR_EVENTS_TOPIC", "events")
    monkeypatch.setenv("EDGEDOCTOR_MQTT_HOST", "127.0.0.1")
    monkeypatch.setenv("EDGEDOCTOR_MQTT_PORT", "1883")
    monkeypatch.setenv("EDGEDOCTOR_MQTT_TLS", "0")
    monkeypatch.setenv("EDGEDOCTOR_MQTT_USER", "u")
    monkeypatch.setenv("EDGEDOCTOR_MQTT_PASSWORD", "p")
    module.run()
    assert not any(item[0] == "tls" for item in calls)
    assert any(item[0] == "connect" for item in calls)


def test_mqtt_message_boundary_accepts_event_and_rejects_invalid(monkeypatch):
    import server.mqtt_ingest as module

    accepted = []
    monkeypatch.setattr(module, "ingest", lambda event, token: accepted.append(event))
    monkeypatch.delenv("EDGEDOCTOR_API_TOKEN", raising=False)

    class Message:
        topic = "edgedoctor/site_1/ed84-001/events"
        payload = b'{"schema_version":2,"device_id":"ed84-001","event_id":"e1","revision":1,"source":"LIVE_IMU","condition":"ACTIVE","ack":"UNACKNOWLEDGED","severity":"ALARM"}'

    module.on_message(None, None, Message())
    assert len(accepted) == 1 and accepted[0].event_id == "e1"

    class InvalidMessage:
        payload = b"not-json"

    module.on_message(None, None, InvalidMessage())
    assert len(accepted) == 1


def test_mqtt_event_publishes_persistence_receipt(monkeypatch):
    import server.mqtt_ingest as module

    published = []
    monkeypatch.setattr(module, "ingest", lambda event, token: {
        "status": "persisted",
        "event_id": event.event_id,
        "revision": event.revision,
    })

    class Client:
        def publish(self, topic, payload, qos, retain):
            published.append((topic, payload, qos, retain))

    class Message:
        topic = "edgedoctor/site_1/ed84-001/events"
        payload = b'{"schema_version":2,"device_id":"ed84-001","event_id":"e2","revision":3,"source":"LIVE_IMU","condition":"RECOVERED","ack":"UNACKNOWLEDGED","severity":"ALARM"}'

    module.on_message(Client(), None, Message())
    assert len(published) == 1
    assert published[0][0] == "edgedoctor/site_1/ed84-001/event_receipts"
    assert '"status":"persisted"' in published[0][1]
    assert published[0][2:] == (1, False)


def test_mqtt_topic_binding_rejects_cross_device_and_malformed_topics():
    import server.mqtt_ingest as module

    assert module._topic_allowed("edgedoctor/site_1/ed84-001/events", "ed84-001")
    assert not module._topic_allowed("edgedoctor/site_1/other/events", "ed84-001")
    assert not module._topic_allowed("edgedoctor/site_1/ed84-001/state", "ed84-001")
    assert not module._topic_allowed("edgedoctor/site_1/ed84-001/events/extra", "ed84-001")

def test_mqtt_command_site_is_configurable(monkeypatch):
    import server.mqtt_ingest as module

    monkeypatch.setenv("EDGEDOCTOR_SITE_ID", "plant_7")
    assert module._configured_site_id() == "plant_7"
    monkeypatch.delenv("EDGEDOCTOR_SITE_ID")
    assert module._configured_site_id() == "site"

def test_mqtt_message_boundary_persists_telemetry(monkeypatch):
    import server.mqtt_ingest as module
    accepted = []
    monkeypatch.setattr(module, "telemetry", lambda payload: accepted.append(payload))

    class Message:
        topic = "edgedoctor/site_1/ed84-001/telemetry"
        payload = b'{"device_id":"ed84-001","mono_ms":123,"rms_vector_g":0.12,"anomaly_score":1.5,"alarm":false}'

    module.on_message(None, None, Message())
    assert accepted == [{"device_id": "ed84-001", "mono_ms": 123, "rms_vector_g": 0.12, "anomaly_score": 1.5, "alarm": False}]

