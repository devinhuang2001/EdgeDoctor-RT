import json
from types import SimpleNamespace

import pytest
from fastapi.testclient import TestClient

from server.evidence import pack


def setup_capture(tmp_path, monkeypatch):
    import server.app as app
    monkeypatch.setattr(app, "DB_PATH", tmp_path / "receipt.db")
    client = TestClient(app.app)
    client.post('/api/v1/events', json={"schema_version": 1, "device_id": "ed84-001", "event_id": "receipt-1", "revision": 1,
                                      "source": "LIVE_IMU", "condition": "ACTIVE", "ack": "UNACKNOWLEDGED"})
    challenge = client.post('/api/v1/events/receipt-1/evidence-request').json()['challenge']
    return app, challenge


def test_lost_subscriber_and_receipt_replayed_without_false_storage(tmp_path, monkeypatch):
    app, challenge = setup_capture(tmp_path, monkeypatch)
    from server.mqtt_ingest import on_message
    receipts = []
    class Broker:
        def publish(self, topic, payload, **options): receipts.append((topic, json.loads(payload), options))
    blob = pack({"event_id": "receipt-1", "challenge": challenge, "complete": True}, bytes(500))
    pieces = [blob[:300], blob[300:]]
    def send(index):
        on_message(Broker(), None, SimpleNamespace(topic='edgedoctor/site_1/ed84-001/evidence/receipt-1/chunks',
                   payload=b'\x00\x02' + index.to_bytes(2, 'big') + pieces[index]))
    send(0)  # Subscriber missed the last chunk.
    assert receipts == []
    send(0)  # Retransmission is idempotent.
    assert receipts == []
    send(1)
    assert receipts[0][0].endswith('/evidence_receipts')
    assert receipts[0][1]['challenge'] == challenge and receipts[0][1]['bytes'] == len(blob)
    assert receipts[0][2] == {'qos': 1, 'retain': False}
    send(0); send(1)  # Device missed stored receipt: resending produces another receipt.
    assert receipts[0][1]['sha256'] == receipts[1][1]['sha256']


@pytest.mark.parametrize('kind', ['crc', 'identity', 'challenge'])
def test_bad_evidence_never_receives_storage_receipt(tmp_path, monkeypatch, kind):
    app, challenge = setup_capture(tmp_path, monkeypatch)
    blob = pack({'event_id': 'other' if kind == 'identity' else 'receipt-1', 'challenge': 'wrong' if kind == 'challenge' else challenge}, b'raw')
    if kind == 'crc': blob = blob[:-1] + bytes([blob[-1] ^ 1])
    with pytest.raises(ValueError): app.ingest_evidence_chunk('ed84-001', 'receipt-1', 0, 1, blob)
    assert app.db().execute('select count(*) from evidence').fetchone()[0] == 0


def test_conflicting_chunk_does_not_overwrite_valid_fragment(tmp_path, monkeypatch):
    app, _ = setup_capture(tmp_path, monkeypatch)
    app.ingest_evidence_chunk('ed84-001', 'receipt-1', 0, 2, b'one')
    with pytest.raises(ValueError): app.ingest_evidence_chunk('ed84-001', 'receipt-1', 0, 2, b'two')
    assert app.db().execute('select payload from evidence_chunks').fetchone()[0] == b'one'


def test_analysis_requires_same_auth_as_raw_evidence(tmp_path, monkeypatch):
    setup_capture(tmp_path, monkeypatch)
    import server.app as app
    client = TestClient(app.app)
    monkeypatch.setenv('EDGEDOCTOR_API_TOKEN', 'test-receipt-token')
    assert client.get('/api/v1/events/receipt-1/analysis').status_code == 401
    assert client.get('/api/v1/events/receipt-1/analysis', headers={'X-Edgedoctor-Token': 'test-receipt-token'}).status_code == 200
