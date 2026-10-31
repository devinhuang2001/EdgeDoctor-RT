import math
import struct

import pytest

from server.analysis import analyze
from server.evidence import pack


def evidence(event_id="analysis-sine", **overrides):
    metadata = {"event_id": event_id, "complete": True, "encoding": "xyz_int16_le",
                "sensitivity_g_per_adc": 0.000122, "sample_rate_hz": 208, **overrides}
    # One second, exactly 13 cycles; axis Z has a real DC acceleration.
    payload = b"".join(struct.pack("<hhh", round(1000 * math.sin(2 * math.pi * 13 * i / 208)), 0, 8000) for i in range(208))
    return pack(metadata, payload)


def test_xyz_fft_uses_declared_scale_and_rate():
    result = analyze(evidence(), "analysis-sine")
    assert result["sample_count"] == 208
    peak = result["dominant_peaks"][0]
    assert peak["axis"] == "X"
    assert peak["frequency_hz"] == pytest.approx(13, abs=208 / 256)
    assert peak["amplitude_g"] == pytest.approx(0.122, rel=0.06)
    assert result["spectrum"]["frequency_hz"][-1] == 104
    assert result["statistics"]["x"]["rms_ac_g"] == pytest.approx(0.122 / math.sqrt(2), rel=0.001)
    assert result["statistics"]["z"]["rms_ac_g"] < 1e-12
    assert result["waveform"]["z_g"][0] == pytest.approx(0.976)
    assert result["waveform"]["time_s"][-1] == 207 / 208
    assert result["envelope"]["method"] == "rectified_ac_moving_mean"


@pytest.mark.parametrize("overrides", [{"encoding": None}, {"complete": False}, {"sample_rate_hz": 0},
                                       {"sample_rate_hz": True}, {"sensitivity_g_per_adc": float("nan")}, {"gaps": [[1, 3]]}])
def test_invalid_contract_not_analyzed(overrides):
    with pytest.raises(ValueError):
        analyze(evidence(**overrides), "analysis-sine")


def test_crc_identity_and_alignment_rejected():
    blob = evidence()
    with pytest.raises(ValueError): analyze(blob, "other")
    with pytest.raises(ValueError): analyze(blob[:-1] + bytes([blob[-1] ^ 1]), "analysis-sine")
    with pytest.raises(ValueError): analyze(pack({"event_id": "x", "complete": True, "encoding": "xyz_int16_le", "sample_rate_hz": 208, "sensitivity_g_per_adc": 0.000122}, b"x"), "x")


def test_analysis_api_revalidates_file_after_upload(tmp_path, monkeypatch):
    from fastapi.testclient import TestClient
    import server.app as module
    monkeypatch.setattr(module, "DB_PATH", tmp_path / "analysis.db")
    client = TestClient(module.app)
    client.post("/api/v1/events", json={"schema_version": 1, "device_id": "ed84-001", "event_id": "analysis-sine", "revision": 1,
                                      "source": "LIVE_IMU", "condition": "ACTIVE", "ack": "UNACKNOWLEDGED"})
    assert client.post("/api/v1/events/analysis-sine/evidence", files={"upload": ("sine.edv2", evidence())}).status_code == 201
    assert client.get("/api/v1/events/analysis-sine/analysis").json()["available"] is True
    conn = module.db()
    path = conn.execute("select path from evidence").fetchone()[0]
    conn.close()
    from pathlib import Path
    Path(path).write_bytes(b"corrupt")
    response = client.get("/api/v1/events/analysis-sine/analysis").json()
    assert response["available"] is False and response["reason"]
