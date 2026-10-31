import pytest
from server.evidence import pack, unpack

def test_round_trip_and_crc():
    blob = pack({"event_id":"e1","complete":True}, b"0123")
    assert unpack(blob) == ({"event_id":"e1","complete":True}, b"0123")
    broken = bytearray(blob); broken[-1] ^= 1
    with pytest.raises(ValueError): unpack(bytes(broken))

def test_truncation_rejected():
    with pytest.raises(ValueError): unpack(pack({}, b"x")[:-1])

def test_upload_is_bound_to_event(tmp_path, monkeypatch):
    import server.app as module
    from fastapi.testclient import TestClient
    monkeypatch.setattr(module, "DB_PATH", tmp_path / "x.db")
    client = TestClient(module.app)
    event={"schema_version":1,"device_id":"ed84-001","event_id":"e3","revision":1,"source":"LIVE_IMU","condition":"ACTIVE","ack":"UNACKNOWLEDGED"}
    assert client.post('/api/v1/events',json=event).status_code==200
    blob=pack({"event_id":"e3"},b"raw")
    assert client.post('/api/v1/events/e3/evidence',files={"upload":("x.edv2",blob)}).status_code==201
    assert client.get('/api/v1/evidence/e3/download').status_code==200
    metadata = client.get('/api/v1/events/e3/evidence').json()
    assert metadata["event_id"] == "e3" and metadata["crc_ok"] is True

def test_download_requires_token_when_configured(tmp_path, monkeypatch):
    import server.app as module
    from fastapi.testclient import TestClient
    monkeypatch.setattr(module, "DB_PATH", tmp_path / "x.db")
    monkeypatch.setenv("EDGEDOCTOR_API_TOKEN", "secret")
    client = TestClient(module.app)
    event={"schema_version":1,"device_id":"ed84-001","event_id":"e-token","revision":1,"source":"LIVE_IMU","condition":"ACTIVE","ack":"UNACKNOWLEDGED"}
    headers={"X-Edgedoctor-Token":"secret"}
    client.post('/api/v1/events',json=event,headers=headers)
    blob=pack({"event_id":"e-token"},b"raw")
    client.post('/api/v1/events/e-token/evidence',files={"upload":("x.edv2",blob)},headers=headers)
    assert client.get('/api/v1/evidence/e-token/download').status_code == 401
    assert client.get('/api/v1/evidence/e-token/download',headers=headers).status_code == 200

def test_mqtt_chunks_reassemble_only_when_complete(tmp_path, monkeypatch):
    import server.app as module
    monkeypatch.setattr(module, "DB_PATH", tmp_path / "x.db")
    module.init_db(module.db())
    from fastapi.testclient import TestClient
    client = TestClient(module.app)
    client.post('/api/v1/events', json={"schema_version":1,"device_id":"ed84-001","event_id":"e-chunk","revision":1,"source":"LIVE_IMU","condition":"ACTIVE","ack":"UNACKNOWLEDGED"})
    challenge = client.post('/api/v1/events/e-chunk/evidence-request').json()["challenge"]
    blob = pack({"event_id": "e-chunk", "challenge":challenge, "complete": True}, b"raw-evidence")
    split = max(1, len(blob) // 2)
    first = module.ingest_evidence_chunk("ed84-001", "e-chunk", 0, 2, blob[:split])
    assert first["status"] == "partial"
    second = module.ingest_evidence_chunk("ed84-001", "e-chunk", 1, 2, blob[split:])
    assert second["status"] == "stored" and second["bytes"] == len(blob) and second["challenge"] == challenge
    assert module.evidence_metadata("e-chunk")["crc_ok"] is True
