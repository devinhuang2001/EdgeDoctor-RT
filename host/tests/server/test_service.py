import json
from fastapi.testclient import TestClient


def test_ingest_dedup_and_revision(tmp_path, monkeypatch):
    import server.app as module
    monkeypatch.setattr(module, "DB_PATH", tmp_path / "x.db")
    client = TestClient(module.app)
    e = {"schema_version":1,"device_id":"ed84-001","event_id":"e1","revision":1,"source":"LIVE_IMU","condition":"ACTIVE","ack":"UNACKNOWLEDGED"}
    assert client.post("/api/v1/events", json=e).status_code == 200
    assert client.post("/api/v1/events", json=e).json()["status"] == "persisted"
    e["revision"] = 2; e["condition"] = "RECOVERED"
    assert client.post("/api/v1/events", json=e).status_code == 200
    assert len(client.get("/api/v1/events/e1").json()["revisions"]) == 2


def test_ack_is_pending_and_revision_conflict(tmp_path, monkeypatch):
    import server.app as module
    monkeypatch.setattr(module, "DB_PATH", tmp_path / "x.db")
    client = TestClient(module.app)
    e = {"schema_version":1,"device_id":"ed84-001","event_id":"e2","revision":1,"source":"LIVE_IMU","condition":"ACTIVE","ack":"UNACKNOWLEDGED"}
    client.post("/api/v1/events", json=e)
    assert client.post("/api/v1/events/e2/ack", json={"actor":"op","expected_revision":1}).status_code == 202
    assert client.post("/api/v1/events/e2/ack", json={"actor":"op","expected_revision":9}).status_code == 409

def test_command_receipt_closes_ack_and_is_idempotent(tmp_path, monkeypatch):
    import server.app as module
    monkeypatch.setattr(module, "DB_PATH", tmp_path / "x.db")
    client = TestClient(module.app)
    e = {"schema_version":1,"device_id":"ed84-001","event_id":"e3","revision":1,"source":"LIVE_IMU","condition":"ACTIVE","ack":"UNACKNOWLEDGED"}
    client.post("/api/v1/events", json=e)
    created = client.post("/api/v1/events/e3/ack", json={"actor":"op","expected_revision":1}).json()
    command_id = created["command_id"]
    result = client.post(f"/api/v1/commands/{command_id}/result", json={"status":"APPLIED","detail":"device confirmed"})
    assert result.status_code == 200
    assert client.get(f"/api/v1/commands/{command_id}").json()["status"] == "APPLIED"
    assert client.post(f"/api/v1/commands/{command_id}/result", json={"status":"APPLIED"}).status_code == 200

def test_mutations_require_configured_token(tmp_path, monkeypatch):
    import server.app as module
    monkeypatch.setattr(module, "DB_PATH", tmp_path / "x.db")
    monkeypatch.setenv("EDGEDOCTOR_API_TOKEN", "secret")
    client = TestClient(module.app)
    e = {"schema_version":1,"device_id":"ed84-001","event_id":"e4","revision":1,"source":"LIVE_IMU","condition":"ACTIVE","ack":"UNACKNOWLEDGED"}
    assert client.post("/api/v1/events", json=e).status_code == 401
    assert client.post("/api/v1/events", json=e, headers={"X-Edgedoctor-Token":"secret"}).status_code == 200

def test_voice_query_is_read_only_and_truthful(tmp_path, monkeypatch):
    import server.app as module
    monkeypatch.setattr(module, "DB_PATH", tmp_path / "x.db")
    client = TestClient(module.app)
    initial = client.get("/api/v1/query/status?device_id=ed84-001").json()
    assert initial["status"] == "NO_DATA" and initial["read_only"] is True
    e = {"schema_version":1,"device_id":"ed84-001","event_id":"e5","revision":1,"source":"LIVE_IMU","condition":"ACTIVE","ack":"UNACKNOWLEDGED"}
    client.post("/api/v1/events", json=e)
    result = client.get("/api/v1/query/status?device_id=ed84-001").json()
    assert result["status"] == "WARNING" and "概率" not in result["advice"]

def test_alarm_notification_runs_after_persist(tmp_path, monkeypatch):
    import server.app as module
    monkeypatch.setattr(module, "DB_PATH", tmp_path / "x.db")
    calls = []
    monkeypatch.setenv("EDGEDOCTOR_NTFY_URL", "https://ntfy.sh")
    monkeypatch.setenv("EDGEDOCTOR_NTFY_TOPIC", "edge-test")
    monkeypatch.setattr(module, "publish_ntfy", lambda *args: calls.append(args) or "CHANNEL_ACCEPTED")
    client = TestClient(module.app)
    e = {"schema_version":1,"device_id":"ed84-001","event_id":"e6","revision":1,"source":"LIVE_IMU","condition":"ACTIVE","ack":"UNACKNOWLEDGED"}
    assert client.post("/api/v1/events", json=e).status_code == 200
    assert calls and calls[0][1] == "edge-test" and calls[0][2] == "EdgeDoctor 预警"

def test_duplicate_revision_does_not_repeat_notification(tmp_path, monkeypatch):
    import server.app as module
    monkeypatch.setattr(module, "DB_PATH", tmp_path / "x.db")
    monkeypatch.setenv("EDGEDOCTOR_NTFY_URL", "https://ntfy.sh")
    monkeypatch.setenv("EDGEDOCTOR_NTFY_TOPIC", "edge-test")
    calls=[]; monkeypatch.setattr(module, "publish_ntfy", lambda *args: calls.append(args) or "CHANNEL_ACCEPTED")
    client=TestClient(module.app)
    e={"schema_version":1,"device_id":"ed84-001","event_id":"e-dup","revision":1,"source":"LIVE_IMU","condition":"ACTIVE","ack":"UNACKNOWLEDGED"}
    client.post("/api/v1/events",json=e); client.post("/api/v1/events",json=e)
    assert len(calls) == 1

def test_event_contract_preserves_diagnostic_fields(tmp_path, monkeypatch):
    import server.app as module
    monkeypatch.setattr(module, "DB_PATH", tmp_path / "x.db")
    client=TestClient(module.app)
    e={"schema_version":1,"device_id":"ed84-001","event_id":"e-fields","revision":1,"source":"LIVE_IMU","condition":"ACTIVE","ack":"UNACKNOWLEDGED","severity":"ALARM","diagnosis":"UNKNOWN_ANOMALY","quality":"VALID","snapshot_id":42,"peak_deviation":12.5}
    client.post("/api/v1/events",json=e)
    stored=client.get("/api/v1/events/e-fields").json()["revisions"][0]
    assert stored["severity"] == "ALARM" and stored["snapshot_id"] == 42 and stored["peak_deviation"] == 12.5

def test_event_contract_rejects_unknown_enum(tmp_path, monkeypatch):
    import server.app as module
    monkeypatch.setattr(module, "DB_PATH", tmp_path / "x.db")
    client=TestClient(module.app)
    e={"schema_version":1,"device_id":"ed84-001","event_id":"e-bad","revision":1,"source":"LIVE_IMU","condition":"ACTIVE","ack":"UNACKNOWLEDGED","severity":"CRITICAL"}
    assert client.post("/api/v1/events",json=e).status_code == 422

def test_recent_alert_query_is_bounded(tmp_path, monkeypatch):
    import server.app as module
    monkeypatch.setattr(module, "DB_PATH", tmp_path / "x.db")
    client=TestClient(module.app)
    for n in range(1, 4):
        client.post("/api/v1/events",json={"schema_version":1,"device_id":"ed84-001","event_id":f"e-{n}","revision":1,"source":"LIVE_IMU","condition":"ACTIVE","ack":"UNACKNOWLEDGED"})
    result=client.get("/api/v1/query/recent-alerts?device_id=ed84-001&limit=2").json()
    assert len(result["alerts"]) == 2 and result["read_only"] is True


def test_maintenance_compare_splits_history(tmp_path, monkeypatch):
    import server.app as module
    monkeypatch.setattr(module, "DB_PATH", tmp_path / "x.db")
    client = TestClient(module.app)
    client.post("/api/v1/telemetry", json={"device_id":"motor-1","mono_ms":1,"timestamp_ms":100,"rms_vector_g":0.2,"anomaly_score":2})
    client.post("/api/v1/telemetry", json={"device_id":"motor-1","mono_ms":2,"timestamp_ms":300,"rms_vector_g":0.1,"anomaly_score":1})
    client.post("/api/v1/assets/motor-1/maintenance", json={"operator":"op","note":"replace bearing"})
    # force a deterministic pivot for the endpoint's time split
    import sqlite3
    conn=sqlite3.connect(tmp_path / "x.db"); conn.execute("UPDATE maintenance SET created_ms=200 WHERE asset_id='motor-1'"); conn.commit(); conn.close()
    result=client.get("/api/v1/assets/motor-1/maintenance/compare").json()
    assert result["before"]["samples"] == 1 and result["after"]["samples"] == 1

def test_minute_trends_aggregate_metrics(tmp_path, monkeypatch):
    import server.app as module
    monkeypatch.setattr(module, "DB_PATH", tmp_path / "x.db")
    client=TestClient(module.app)
    client.post("/api/v1/telemetry", json={"device_id":"ed84-001","mono_ms":60001,"timestamp_ms":60001,"rms_vector_g":0.2,"anomaly_score":4,"alarm":True})
    client.post("/api/v1/telemetry", json={"device_id":"ed84-001","mono_ms":60002,"timestamp_ms":60002,"rms_vector_g":0.4,"anomaly_score":6})
    result=client.get("/api/v1/trends/minute?device_id=ed84-001").json()
    assert result[-1]["samples"] == 2 and result[-1]["alarm_count"] == 1 and result[-1]["rms_mean_g"] == 0.3

def test_telemetry_is_bounded_and_ordered(tmp_path, monkeypatch):
    import server.app as module
    monkeypatch.setattr(module, "DB_PATH", tmp_path / "x.db")
    client = TestClient(module.app)
    for n in (2, 1):
        assert client.post("/api/v1/telemetry", json={"device_id":"ed84-001","mono_ms":n,"rms_vector_g":0.1}).status_code == 200
    assert [x["mono_ms"] for x in client.get("/api/v1/trends?device_id=ed84-001").json()] == [1, 2]

def test_maintenance_is_auditable(tmp_path, monkeypatch):
    import server.app as module
    monkeypatch.setattr(module, "DB_PATH", tmp_path / "x.db")
    client = TestClient(module.app)
    r=client.post('/api/v1/assets/motor-1/maintenance',json={"operator":"op","note":"tightened mount"})
    assert r.status_code==201
    assert client.get('/api/v1/assets/motor-1/maintenance').json()[0]["operator"]=="op"
