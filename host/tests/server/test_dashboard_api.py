"""电脑版控制台只读接口的服务端验收测试（规格 02 的 DD-API-01 / DD-API-02 / DD-API-07）。

这些用例只证明服务端行为，不能证明板端、真实留证或现场网络表现。
"""

import json
from pathlib import Path

import pytest
from fastapi.testclient import TestClient


def _client(tmp_path, monkeypatch=None, bindings: dict | None = None):
    import server.app as module

    if monkeypatch is not None:
        monkeypatch.setattr(module, "DB_PATH", tmp_path / "dashboard.db")
    else:
        module.DB_PATH = tmp_path / "dashboard.db"
    if bindings is not None:
        path = tmp_path / "bindings.json"
        path.write_text(json.dumps(bindings, ensure_ascii=False), encoding="utf-8")
        monkeypatch.setattr(module, "ASSET_BINDINGS_PATH", path)
    return TestClient(module.app)


def _binding(asset_id: str, state: str, device_id: str | None, instance: str, zone: str = "B") -> dict:
    return {
        "asset_id": asset_id,
        "device_id": device_id,
        "scene_instance": instance,
        "zone": zone,
        "detail_model": None,
        "sensor_node": None,
        "display_name_zh": f"测试资产 {asset_id}",
        "binding_state": state,
    }


def _bindings(*assets: dict) -> dict:
    return {"schema_version": 1, "zones": [{"zone": "B", "scene_root": "ZONE_B", "display_name_zh": "旋转设备"}], "assets": list(assets)}


# --- DD-API-01：空库、仅遥测、活动事件、恢复未确认、过期事件下的状态语义 -------------------

def test_factory_summary_distinguishes_online_stale_and_unbound_assets(tmp_path, monkeypatch):
    import server.app as module

    now = 1_700_000_000_000
    monkeypatch.setattr(module.time, "time", lambda: now / 1000)
    client = _client(tmp_path, monkeypatch, _bindings(
        _binding("motor-01", "verified", "ed84-001", "MOTOR_01"),
        _binding("zone-a-cnc", "unbound", None, "CNC_MACHINE_01", zone="A"),
    ))

    assert client.post(
        "/api/v1/telemetry",
        json={"device_id": "ed84-001", "mono_ms": 7, "rms_vector_g": 0.012},
    ).status_code == 200

    summary = client.get("/api/v1/factory/summary").json()
    motor = next(asset for asset in summary["assets"] if asset["asset_id"] == "motor-01")
    unbound = next(asset for asset in summary["assets"] if asset["asset_id"] == "zone-a-cnc")

    assert motor["status"] == "ONLINE"
    assert motor["last_telemetry_received_ms"] == now
    assert unbound["status"] == "UNKNOWN"
    assert unbound["last_telemetry_received_ms"] is None
    assert summary["counts"] == {"ONLINE": 1, "STALE": 0, "UNKNOWN": 1}
    assert summary["online_threshold_ms"] == 30_000


def test_factory_summary_marks_bound_asset_stale_after_receipt_threshold(tmp_path, monkeypatch):
    import server.app as module

    now = [1_700_000_000_000]
    monkeypatch.setattr(module.time, "time", lambda: now[0] / 1000)
    client = _client(tmp_path, monkeypatch, _bindings(_binding("motor-01", "verified", "ed84-001", "MOTOR_01")))
    client.post("/api/v1/telemetry", json={"device_id": "ed84-001", "mono_ms": 1})

    for delta, expected in ((29_000, "ONLINE"), (30_000, "ONLINE"), (30_001, "STALE")):
        now[0] = 1_700_000_000_000 + delta
        motor = client.get("/api/v1/factory/summary").json()["assets"][0]
        assert motor["status"] == expected, f"阈值 ±1 ms 处状态应为 {expected}"


def test_unverified_bindings_never_count_as_online(tmp_path, monkeypatch):
    """DD-API-07：只有 verified 绑定进入在线统计；provisional / unbound 一律 UNKNOWN。"""
    import server.app as module

    now = 1_700_000_000_000
    monkeypatch.setattr(module.time, "time", lambda: now / 1000)
    client = _client(tmp_path, monkeypatch, _bindings(
        _binding("motor-01", "verified", "ed84-001", "MOTOR_01", zone="B"),
        _binding("spare-motor-01", "provisional", "ed84-002", "SPARE_MOTOR_01", zone="E"),
        _binding("cnc-01", "unbound", None, "CNC_MACHINE_01", zone="A"),
    ))
    # 三个设备都在阈值内上报过
    for device in ("ed84-001", "ed84-002"):
        assert client.post("/api/v1/telemetry", json={"device_id": device, "mono_ms": 1}).status_code == 200

    summary = client.get("/api/v1/factory/summary").json()
    statuses = {asset["asset_id"]: asset["status"] for asset in summary["assets"]}
    assert statuses == {"motor-01": "ONLINE", "spare-motor-01": "UNKNOWN", "cnc-01": "UNKNOWN"}
    assert summary["counts"] == {"ONLINE": 1, "STALE": 0, "UNKNOWN": 2}


def test_empty_database_reports_no_fabricated_health(tmp_path, monkeypatch):
    import server.app as module

    monkeypatch.setattr(module.time, "time", lambda: 1_700_000_000)
    client = _client(tmp_path, monkeypatch, _bindings(_binding("motor-01", "verified", "ed84-001", "MOTOR_01")))

    snapshot = client.get("/api/v1/query/status", params={"device_id": "ed84-001"}).json()
    assert snapshot["status"] in {"NO_DATA", "UNKNOWN"}
    assert "rms_vector_g" not in json.dumps(snapshot)

    summary = client.get("/api/v1/factory/summary").json()
    assert summary["counts"] == {"ONLINE": 0, "STALE": 0, "UNKNOWN": 1}


# --- DD-API-02：乱序 revision、缺字段、非法数值、未知枚举 ------------------------------------

@pytest.mark.parametrize(
    "payload",
    [
        {},
        {"schema_version": 1, "device_id": "ed84-001"},
        {"schema_version": 1, "device_id": "ed84-001", "event_id": "e1", "revision": 0, "source": "LIVE_IMU", "condition": "ACTIVE", "ack": "UNACKNOWLEDGED"},
        {"schema_version": 1, "device_id": "ed84-001", "event_id": "e1", "revision": 1, "source": "UNKNOWN_SOURCE", "condition": "ACTIVE", "ack": "UNACKNOWLEDGED"},
        {"schema_version": 1, "device_id": "ed84-001", "event_id": "e1", "revision": 1, "source": "LIVE_IMU", "condition": "MAYBE", "ack": "UNACKNOWLEDGED"},
        {"schema_version": 1, "device_id": "ed84-001", "event_id": "e1", "revision": 1, "source": "LIVE_IMU", "condition": "ACTIVE", "ack": "ACKED"},
        {"schema_version": 1, "device_id": "ED84 001", "event_id": "e1", "revision": 1, "source": "LIVE_IMU", "condition": "ACTIVE", "ack": "UNACKNOWLEDGED"},
    ],
)
def test_event_schema_rejects_invalid_payloads(tmp_path, monkeypatch, payload):
    client = _client(tmp_path, monkeypatch, _bindings())
    assert client.post("/api/v1/events", json=payload).status_code == 422


def test_event_detail_lists_server_side_revisions_in_order(tmp_path, monkeypatch):
    client = _client(tmp_path, monkeypatch, _bindings())
    for revision, condition in ((1, "ACTIVE"), (2, "ACTIVE"), (3, "RECOVERED")):
        body = {
            "schema_version": 1, "device_id": "ed84-001", "event_id": "evt-1",
            "revision": revision, "source": "LIVE_IMU", "condition": condition, "ack": "UNACKNOWLEDGED",
        }
        assert client.post("/api/v1/events", json=body).status_code == 200

    detail = client.get("/api/v1/events/evt-1").json()
    assert [item["revision"] for item in detail["revisions"]] == [1, 2, 3]
    assert detail["revisions"][-1]["condition"] == "RECOVERED"
    assert client.get("/api/v1/events/missing").status_code == 404


def test_telemetry_rejects_illegal_values(tmp_path, monkeypatch):
    client = _client(tmp_path, monkeypatch, _bindings())
    for body in (
        {"device_id": "", "mono_ms": 1},
        {"device_id": "ed84-001", "mono_ms": -1},
        {"device_id": "ed84-001", "mono_ms": 1.5},
        {"device_id": "ed84-001", "mono_ms": 1, "rms_vector_g": "high"},
        {"device_id": "ed84-001", "mono_ms": 1, "anomaly_score": None, "rms_vector_g": True},
        {"device_id": "ed84-001", "mono_ms": 1, "alarm": "yes"},
        {"device_id": "ED84 001", "mono_ms": 1},
        {"device_id": "ed84-001", "mono_ms": 1, "pad": "x" * 5000},
    ):
        assert client.post("/api/v1/telemetry", json=body).status_code == 422, body


def test_telemetry_rejects_non_finite_numbers_that_would_break_the_browser(tmp_path, monkeypatch):
    """NaN/Infinity 会被 SQLite 原样存下，再以非法 JSON 返回浏览器，必须在入口拒绝。"""
    client = _client(tmp_path, monkeypatch, _bindings())
    for value in (float("nan"), float("inf"), float("-inf")):
        response = client.post(
            "/api/v1/telemetry",
            content=json.dumps({"device_id": "ed84-001", "mono_ms": 1, "rms_vector_g": value}),
            headers={"Content-Type": "application/json"},
        )
        assert response.status_code == 422, value


# --- 绑定与证据接口的边界 ----------------------------------------------------------------

def test_asset_binding_exposes_no_mqtt_credentials_or_topics(tmp_path, monkeypatch):
    client = _client(tmp_path, monkeypatch)

    response = client.get("/api/v1/assets/motor-01/binding")

    assert response.status_code == 200
    body = response.json()
    assert body["asset_id"] == "motor-01"
    assert body["device_id"] == "ed84-001"
    assert body["scene_instance"] == "MOTOR_01"
    for forbidden in ("topic", "password", "username", "token", "secret", "broker"):
        assert forbidden not in json.dumps(body).lower()
    assert client.get("/api/v1/assets/missing/binding").status_code == 404


def test_shipped_binding_config_declares_six_zones_and_one_verified_device(tmp_path, monkeypatch):
    client = _client(tmp_path, monkeypatch)

    summary = client.get("/api/v1/factory/summary").json()
    assets = summary["assets"]
    assert len(assets) == 6
    assert {asset["zone"] for asset in assets} == {"A", "B", "C", "D", "E", "F"}
    verified = [asset for asset in assets if asset["binding_state"] == "verified"]
    assert [asset["asset_id"] for asset in verified] == ["motor-01"]
    assert all(asset["device_id"] for asset in verified)
    assert len({asset["scene_instance"] for asset in assets}) == len(assets)
    assert sum(summary["counts"].values()) == len(assets)


def test_binding_configuration_errors_surface_as_service_unavailable(tmp_path, monkeypatch):
    import server.app as module

    client = _client(tmp_path, monkeypatch, _bindings(
        _binding("motor-01", "verified", "ed84-001", "MOTOR_01"),
        _binding("motor-01", "verified", "ed84-002", "MOTOR_01"),
    ))
    assert client.get("/api/v1/factory/summary").status_code == 503

    bad = tmp_path / "bad.json"
    bad.write_text("[]", encoding="utf-8")
    monkeypatch.setattr(module, "ASSET_BINDINGS_PATH", bad)
    assert client.get("/api/v1/factory/summary").status_code == 503


def test_server_side_binding_copy_matches_the_frozen_source(tmp_path, monkeypatch):
    """构建期副本必须与冻结源码一致，否则部署读到的是过期绑定。"""
    repo_root = Path(__file__).resolve().parents[2]
    source = json.loads((repo_root / "ui" / "desktop" / "src" / "config" / "asset-bindings.json").read_text(encoding="utf-8"))
    copy = json.loads((repo_root / "server" / "asset-bindings.json").read_text(encoding="utf-8"))
    assert copy == source


# --- 信号分析接口的边界（规格 §7 / DD-UI-05） ---------------------------------------------

def test_event_analysis_is_explicitly_unavailable_without_a_verified_parser(tmp_path, monkeypatch):
    client = _client(tmp_path, monkeypatch, _bindings())
    event = {
        "schema_version": 1, "device_id": "ed84-001", "event_id": "analysis-e1",
        "revision": 1, "source": "LIVE_IMU", "condition": "ACTIVE", "ack": "UNACKNOWLEDGED",
    }
    assert client.post("/api/v1/events", json=event).status_code == 200

    response = client.get("/api/v1/events/analysis-e1/analysis")

    assert response.status_code == 200
    body = response.json()
    assert body["available"] is False
    assert body["event_id"] == "analysis-e1"
    assert isinstance(body["reason"], str) and body["reason"]
    assert client.get("/api/v1/events/missing/analysis").status_code == 404


def test_evidence_endpoint_requires_a_persisted_evidence_record(tmp_path, monkeypatch):
    client = _client(tmp_path, monkeypatch, _bindings())
    assert client.get("/api/v1/events/no-evidence/evidence").status_code == 404
