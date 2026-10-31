from __future__ import annotations

import json
import math
import os
import re
import sqlite3
import time
from contextlib import asynccontextmanager
from pathlib import Path
from typing import Any

from fastapi import FastAPI, HTTPException, Query, BackgroundTasks
from fastapi.staticfiles import StaticFiles
from fastapi import UploadFile, File, Header
from fastapi.responses import StreamingResponse
from fastapi.responses import RedirectResponse
import asyncio
import threading
import secrets
import hashlib
from .evidence import unpack
from .analysis import analyze
from pydantic import BaseModel, Field
from .notify import publish_ntfy

DB_PATH = Path(os.environ.get("EDGEDOCTOR_DB", Path(__file__).with_name("edgedoctor.db")))
# 服务端读取构建期生成的副本；源码树里的冻结配置只作为开发期回退。
_SERVER_BINDINGS_COPY = Path(__file__).with_name("asset-bindings.json")
_SOURCE_BINDINGS = Path(__file__).parents[1] / "ui" / "desktop" / "src" / "config" / "asset-bindings.json"
ASSET_BINDINGS_PATH = Path(
    os.environ.get("EDGEDOCTOR_BINDINGS")
    or (_SERVER_BINDINGS_COPY if _SERVER_BINDINGS_COPY.exists() else _SOURCE_BINDINGS)
)
ONLINE_RECEIPT_MAX_AGE_MS = 30_000

# 进程级常驻连接：让 WAL 在整个进程生命周期内始终存在。
#
# WAL 模式下，最后一个连接关闭时 SQLite 会 checkpoint 并把 -wal / -shm 删掉。
# 本服务是"每个请求新建连接、用完即关"（sync 端点还跑在线程池里，天然并发），
# 于是就存在这个竞态窗口：一条连接正在关闭并删除 WAL，另一条连接刚打开准备写入——
# 写入会以 "attempt to write a readonly database" 这类瞬时错误失败，
# 在对外的表现就是 POST /api/v1/events 偶发 500（验收里 DD-API-06 播种失败）。
# 常驻一条连接可以让 WAL 不被回收，从根上消除这个竞态。
_keepalive_conn: sqlite3.Connection | None = None


@asynccontextmanager
async def lifespan(_: FastAPI):
    global _keepalive_conn
    DB_PATH.parent.mkdir(parents=True, exist_ok=True)
    conn = db(); init_db(conn)
    _keepalive_conn = conn
    try:
        yield
    finally:
        _keepalive_conn = None
        conn.close()

app = FastAPI(title="EdgeDoctor LAN service", version="1.0", lifespan=lifespan)
app.mount("/dashboard", StaticFiles(directory=Path(__file__).with_name("web"), html=True), name="dashboard")
app.mount("/dashboard-v2", StaticFiles(directory=Path(__file__).with_name("web-v2"), html=True), name="dashboard-v2")

@app.get("/", include_in_schema=False)
def home() -> RedirectResponse:
    return RedirectResponse("/dashboard/")

def auth(x_edgedoctor_token: str | None) -> None:
    expected = os.environ.get("EDGEDOCTOR_API_TOKEN")
    if expected and x_edgedoctor_token != expected: raise HTTPException(401, detail="authentication required")


def db() -> sqlite3.Connection:
    # 每个请求都新建连接，WAL 下读写可以并发，但换日志模式与写入仍要抢锁。
    # 给足 busy_timeout，避免并发请求把瞬时锁冲突暴露成 500。
    conn = sqlite3.connect(DB_PATH, timeout=15)
    conn.row_factory = sqlite3.Row
    conn.execute("PRAGMA busy_timeout=15000")
    conn.execute("PRAGMA journal_mode=WAL")
    return conn


def init_db(conn: sqlite3.Connection) -> None:
    conn.executescript("""
    CREATE TABLE IF NOT EXISTS event_revisions(
      device_id TEXT NOT NULL, event_id TEXT NOT NULL, revision INTEGER NOT NULL,
      payload TEXT NOT NULL, received_ms INTEGER NOT NULL,
      PRIMARY KEY(device_id,event_id,revision));
    CREATE TABLE IF NOT EXISTS events(
      device_id TEXT NOT NULL, event_id TEXT NOT NULL, revision INTEGER NOT NULL,
      payload TEXT NOT NULL, updated_ms INTEGER NOT NULL,
      PRIMARY KEY(device_id,event_id));
    CREATE TABLE IF NOT EXISTS commands(
      command_id TEXT PRIMARY KEY, device_id TEXT NOT NULL, event_id TEXT NOT NULL,
      payload TEXT NOT NULL, status TEXT NOT NULL, created_ms INTEGER NOT NULL);
    CREATE TABLE IF NOT EXISTS telemetry(
      device_id TEXT NOT NULL, mono_ms INTEGER NOT NULL, payload TEXT NOT NULL,
      PRIMARY KEY(device_id,mono_ms));
    CREATE TABLE IF NOT EXISTS telemetry_receipts(
      device_id TEXT NOT NULL, mono_ms INTEGER NOT NULL, received_ms INTEGER NOT NULL,
      PRIMARY KEY(device_id,mono_ms));
    CREATE TABLE IF NOT EXISTS evidence(
      event_id TEXT PRIMARY KEY, device_id TEXT NOT NULL, path TEXT NOT NULL,
      bytes INTEGER NOT NULL, crc_ok INTEGER NOT NULL, created_ms INTEGER NOT NULL);
    CREATE TABLE IF NOT EXISTS evidence_chunks(
      event_id TEXT NOT NULL, device_id TEXT NOT NULL, chunk_index INTEGER NOT NULL,
      chunk_total INTEGER NOT NULL, payload BLOB NOT NULL, created_ms INTEGER NOT NULL,
      PRIMARY KEY(event_id, chunk_index));
    CREATE TABLE IF NOT EXISTS audit_log(
      id INTEGER PRIMARY KEY AUTOINCREMENT, kind TEXT NOT NULL, payload TEXT NOT NULL,
      created_ms INTEGER NOT NULL);
    CREATE TABLE IF NOT EXISTS maintenance(
      id INTEGER PRIMARY KEY AUTOINCREMENT, asset_id TEXT NOT NULL,
      operator TEXT NOT NULL, note TEXT NOT NULL, created_ms INTEGER NOT NULL);
    """)
    conn.commit()


_DEVICE_ID_PATTERN = re.compile(r"^[a-z0-9_-]{1,32}$")
_TELEMETRY_NUMERIC_FIELDS = ("timestamp_ms", "rms_vector_g", "anomaly_score")


def validate_telemetry(payload: Any) -> tuple[str, int]:
    """Rigorous telemetry contract.

    Rejects values that would be persisted and later break the dashboard
    (wrong types, negative counters, NaN/Infinity, oversized payloads), while
    staying forward compatible with unknown extra fields.
    """
    if not isinstance(payload, dict):
        raise HTTPException(422, detail="invalid telemetry")
    try:
        if len(json.dumps(payload)) > 4096:
            raise HTTPException(422, detail="invalid telemetry")
    except (TypeError, ValueError) as exc:
        raise HTTPException(422, detail="invalid telemetry") from exc
    device = payload.get("device_id")
    mono = payload.get("mono_ms")
    if not isinstance(device, str) or not _DEVICE_ID_PATTERN.match(device):
        raise HTTPException(422, detail="invalid telemetry")
    if not isinstance(mono, int) or isinstance(mono, bool) or mono < 0:
        raise HTTPException(422, detail="invalid telemetry")
    for field in _TELEMETRY_NUMERIC_FIELDS:
        value = payload.get(field)
        if value is None:
            continue
        if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value):
            raise HTTPException(422, detail="invalid telemetry")
    alarm = payload.get("alarm")
    if alarm is not None and not isinstance(alarm, bool):
        raise HTTPException(422, detail="invalid telemetry")
    return device, mono


def asset_bindings() -> list[dict[str, Any]]:
    try:
        payload = json.loads(ASSET_BINDINGS_PATH.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise HTTPException(503, detail="asset binding configuration unavailable") from exc
    if not isinstance(payload, dict) or payload.get("schema_version") != 1 or not isinstance(payload.get("assets"), list):
        raise HTTPException(503, detail="asset binding configuration invalid")
    required = {"asset_id", "device_id", "scene_instance", "zone", "detail_model", "sensor_node", "display_name_zh", "binding_state"}
    seen_ids: set[str] = set()
    seen_instances: set[str] = set()
    result: list[dict[str, Any]] = []
    for binding in payload["assets"]:
        if not isinstance(binding, dict) or required - binding.keys():
            raise HTTPException(503, detail="asset binding configuration invalid")
        asset_id, instance, state = binding["asset_id"], binding["scene_instance"], binding["binding_state"]
        if (not isinstance(asset_id, str) or not asset_id or not isinstance(instance, str) or not instance
                or state not in {"verified", "provisional", "unbound"}
                or asset_id in seen_ids or instance in seen_instances
                or (state == "verified" and not isinstance(binding["device_id"], str))):
            raise HTTPException(503, detail="asset binding configuration invalid")
        seen_ids.add(asset_id); seen_instances.add(instance); result.append(binding)
    return result


def public_binding(binding: dict[str, Any]) -> dict[str, Any]:
    return {
        key: binding.get(key)
        for key in ("asset_id", "device_id", "scene_instance", "zone", "detail_model", "sensor_node", "display_name_zh", "binding_state")
    }


class Event(BaseModel):
    schema_version: int = Field(ge=1)
    device_id: str = Field(min_length=1, max_length=32, pattern=r"^[a-z0-9_-]+$")
    event_id: str = Field(min_length=1, max_length=96)
    revision: int = Field(ge=1)
    source: str = Field(pattern=r"^(LIVE_IMU|SENSOR|STORAGE|TREND)$")
    condition: str = Field(pattern=r"^(ACTIVE|RECOVERED)$")
    ack: str = Field(pattern=r"^(UNACKNOWLEDGED|ACKNOWLEDGED)$")
    severity: str = Field(default="WARNING", pattern=r"^(WARNING|ALARM)$")
    diagnosis: str = Field(default="UNKNOWN", pattern=r"^(NO_BASELINE|BASELINE_MISMATCH|WITHIN_BASELINE|UNKNOWN_ANOMALY|KNOWN_CLASS|UNCERTAIN|UNAVAILABLE|UNKNOWN)$")
    quality: str = Field(default="VALID", pattern=r"^(WARMUP|VALID|STALE|DISCONNECTED|SATURATED|GAPPED|CONFIG_ERROR)$")
    snapshot_id: int | None = None
    peak_deviation: float | None = None


class Ack(BaseModel):
    actor: str = Field(min_length=1, max_length=48)
    expected_revision: int = Field(ge=1)

class CommandResult(BaseModel):
    # PENDING is the device's immediate answer to an accepted acknowledgement
    # that still has a two-phase journal commit ahead of it; the final
    # APPLIED/REJECTED follows once that commit completes. Rejecting PENDING
    # here dropped the receipt and left the command pending forever.
    status: str = Field(pattern=r"^(PENDING|APPLIED|REJECTED|EXPIRED)$")
    detail: str = Field(default="", max_length=500)

class VoiceToolCall(BaseModel):
    tool: str = Field(min_length=1, max_length=48)
    device_id: str = Field(min_length=1, max_length=32)
    limit: int = Field(default=5, ge=1, le=20)


@app.get("/health/live")
def live() -> dict[str, str]:
    return {"status": "live"}


@app.get("/health/ready")
def ready() -> dict[str, str]:
    try:
        conn = db(); result = conn.execute("PRAGMA integrity_check").fetchone()[0]; conn.close()
        if result != "ok": raise sqlite3.DatabaseError(result)
        return {"status": "ready"}
    except sqlite3.Error as exc:
        raise HTTPException(503, detail=str(exc))


def _notify_alarm(event: Event) -> None:
    base, topic, token = os.environ.get("EDGEDOCTOR_NTFY_URL"), os.environ.get("EDGEDOCTOR_NTFY_TOPIC"), os.environ.get("EDGEDOCTOR_NTFY_TOKEN", "")
    if not base or not topic: return
    title = "EdgeDoctor 严重报警" if event.severity == "ALARM" else "EdgeDoctor 预警"
    result = publish_ntfy(base, topic, title, f"设备 {event.device_id}：{event.diagnosis}，事件 {event.event_id}", token)
    conn = db(); init_db(conn); conn.execute("INSERT INTO audit_log(kind,payload,created_ms) VALUES(?,?,?)", ("notification", json.dumps({"event_id": event.event_id, "channel": "ntfy", "result": result}, separators=(",", ":")), int(time.time() * 1000))); conn.commit(); conn.close()

@app.get("/api/v1/notifications/status")
def notification_status(x_edgedoctor_token: str | None = Header(default=None)) -> dict[str, Any]:
    """Expose configuration state without returning the topic or bearer token."""
    auth(x_edgedoctor_token)
    return {"configured": bool(os.environ.get("EDGEDOCTOR_NTFY_URL") and os.environ.get("EDGEDOCTOR_NTFY_TOPIC")),
            "channel": "ntfy", "delivery_semantics": "CHANNEL_ACCEPTED"}

@app.post("/api/v1/notifications/test")
def notification_test(x_edgedoctor_token: str | None = Header(default=None)) -> dict[str, str]:
    """Send an operator initiated test notification; acceptance is not phone display."""
    auth(x_edgedoctor_token)
    base, topic, token = os.environ.get("EDGEDOCTOR_NTFY_URL", ""), os.environ.get("EDGEDOCTOR_NTFY_TOPIC", ""), os.environ.get("EDGEDOCTOR_NTFY_TOKEN", "")
    if not base or not topic: raise HTTPException(503, detail="notification channel is not configured")
    result = publish_ntfy(base, topic, "EdgeDoctor 测试通知", "通知通道已收到测试请求", token)
    return {"channel": "ntfy", "status": result}

@app.post("/api/v1/events")
def ingest(event: Event, x_edgedoctor_token: str | None = Header(default=None), background_tasks: BackgroundTasks = None) -> dict[str, Any]:
    auth(x_edgedoctor_token)
    if event.source not in {"LIVE_IMU", "SENSOR", "STORAGE", "TREND"}:
        raise HTTPException(422, detail="invalid source")
    now = int(time.time() * 1000); payload = event.model_dump_json()
    conn = db(); init_db(conn)
    inserted = False
    try:
        inserted = conn.execute("INSERT OR IGNORE INTO event_revisions VALUES(?,?,?,?,?)", (event.device_id,event.event_id,event.revision,payload,now)).rowcount == 1
        row = conn.execute("SELECT revision FROM events WHERE device_id=? AND event_id=?", (event.device_id,event.event_id)).fetchone()
        if row is None or event.revision > row[0]:
            conn.execute("INSERT INTO events VALUES(?,?,?,?,?) ON CONFLICT(device_id,event_id) DO UPDATE SET revision=excluded.revision,payload=excluded.payload,updated_ms=excluded.updated_ms", (event.device_id,event.event_id,event.revision,payload,now))
        conn.execute("INSERT INTO audit_log(kind,payload,created_ms) VALUES(?,?,?)", ("event",payload,now)); conn.commit()
    except sqlite3.Error as exc:
        conn.rollback(); raise HTTPException(500, detail=str(exc))
    finally: conn.close()
    if inserted and event.condition == "ACTIVE" and event.ack == "UNACKNOWLEDGED":
        if background_tasks is not None: background_tasks.add_task(_notify_alarm, event)
        else: threading.Thread(target=_notify_alarm, args=(event,), daemon=True).start()
    return {"status":"persisted","event_id":event.event_id,"revision":event.revision,"server_received_ms":now}


@app.get("/api/v1/events")
def list_events(device_id: str | None = None, limit: int = Query(50, ge=1, le=50)) -> list[dict[str, Any]]:
    """Server-ordered events. Each item carries the server receipt time so the
    console can show when the event was observed instead of inferring it."""
    conn=db(); init_db(conn)
    # rowid 作为次关键字：同一毫秒内写入的多条事件也保持确定的“后写入在前”，
    # 否则列表顺序会随 SQLite 的计划变化，界面与验收都不可复现。
    rows=conn.execute("SELECT payload,updated_ms FROM events WHERE (? IS NULL OR device_id=?) ORDER BY updated_ms DESC, rowid DESC LIMIT ?",(device_id,device_id,limit)).fetchall(); conn.close()
    return [{**json.loads(r[0]), "updated_ms": r[1]} for r in rows]


@app.get("/api/v1/events/{event_id}")
def event_detail(event_id: str) -> dict[str, Any]:
    conn=db(); init_db(conn)
    rows=conn.execute("SELECT payload,received_ms FROM event_revisions WHERE event_id=? ORDER BY revision",(event_id,)).fetchall()
    updated = conn.execute("SELECT updated_ms FROM events WHERE event_id=?", (event_id,)).fetchone()
    conn.close()
    if not rows: raise HTTPException(404, detail="event not found")
    return {
        "event_id": event_id,
        "updated_ms": updated[0] if updated else None,
        "revisions": [{**json.loads(r[0]), "server_received_ms": r[1]} for r in rows],
    }


@app.get("/api/v1/events/{event_id}/analysis")
def event_analysis(event_id: str, x_edgedoctor_token: str | None = Header(default=None)) -> dict[str, Any]:
    """Only analyze complete, CRC-verified evidence with an explicit ADC scale."""
    auth(x_edgedoctor_token)
    conn = db(); init_db(conn)
    event = conn.execute("SELECT 1 FROM events WHERE event_id=?", (event_id,)).fetchone()
    evidence = conn.execute("SELECT path,crc_ok FROM evidence WHERE event_id=?", (event_id,)).fetchone()
    conn.close()
    if not event:
        raise HTTPException(404, detail="event not found")
    if not evidence or not evidence[1]:
        return {"event_id": event_id, "available": False, "reason": "verified evidence analysis is not available"}
    try:
        with Path(evidence[0]).open("rb") as source:
            blob = source.read(18501)
        if len(blob) > 18500:
            raise ValueError("evidence file exceeds size limit")
        return analyze(blob, event_id)
    except (OSError, ValueError, TypeError) as exc:
        reason = str(exc) if not isinstance(exc, OSError) else "evidence file cannot be read"
        return {"event_id": event_id, "available": False, "reason": reason}

@app.get("/api/v1/query/status")
def query_status(device_id: str = Query(..., min_length=1, max_length=32)) -> dict[str, Any]:
    """Read-only, voice-friendly snapshot summary; never invents a fault class."""
    now = int(time.time() * 1000)
    conn = db(); init_db(conn)
    event_row = conn.execute("SELECT payload,updated_ms FROM events WHERE device_id=? ORDER BY updated_ms DESC, rowid DESC LIMIT 1", (device_id,)).fetchone()
    telemetry_row = conn.execute("SELECT payload,mono_ms FROM telemetry WHERE device_id=? ORDER BY mono_ms DESC LIMIT 1", (device_id,)).fetchone()
    conn.close()
    event = json.loads(event_row[0]) if event_row else None
    age_ms = (now - event_row[1]) if event_row else None
    if not event:
        status, advice = "NO_DATA", "等待设备上传诊断快照"
    elif event.get("condition") == "ACTIVE":
        status = "ALARM" if event.get("severity") == "ALARM" else "WARNING"
        advice = "确认报警并检查安装、负载和传动部件"
    elif event.get("condition") == "RECOVERED":
        status, advice = "RECOVERED", "报警已恢复，建议查看留证并记录维修结果"
    else:
        status, advice = "UNKNOWN", "当前状态无法确定，请检查信号质量和基线"
    return {"device_id": device_id, "status": status, "event": event,
            "snapshot_age_ms": age_ms, "telemetry": json.loads(telemetry_row[0]) if telemetry_row else None,
            "advice": advice, "read_only": True}

@app.get("/api/v1/query/recent-alerts")
def query_recent_alerts(device_id: str = Query(..., min_length=1, max_length=32), limit: int = Query(5, ge=1, le=20)) -> dict[str, Any]:
    conn = db(); init_db(conn)
    rows = conn.execute("SELECT payload,updated_ms FROM events WHERE device_id=? ORDER BY updated_ms DESC, rowid DESC LIMIT ?", (device_id, limit)).fetchall(); conn.close()
    return {"device_id": device_id, "alerts": [{"event": json.loads(row[0]), "updated_ms": row[1]} for row in rows], "read_only": True}

@app.get("/api/v1/xiaozhi/tools")
def xiaozhi_tools() -> dict[str, Any]:
    return {"tools": [{"name":"get_status", "description":"读取设备当前诊断状态、最新遥测和处理建议"}, {"name":"get_recent_alerts", "description":"读取设备最近报警事件"}], "read_only": True}

@app.post("/api/v1/xiaozhi/execute")
def xiaozhi_execute(call: VoiceToolCall) -> dict[str, Any]:
    if call.tool == "get_status": return query_status(call.device_id)
    if call.tool == "get_recent_alerts": return query_recent_alerts(call.device_id, call.limit)
    raise HTTPException(404, detail="unknown XiaoZhi tool")

@app.post("/api/v1/events/{event_id}/evidence", status_code=201)
async def upload_evidence(event_id: str, upload: UploadFile = File(...), x_edgedoctor_token: str | None = Header(default=None)) -> dict[str, Any]:
    auth(x_edgedoctor_token)
    blob = await upload.read()
    if len(blob) > 16 * 1024: raise HTTPException(413, detail="evidence too large")
    try: metadata, _ = unpack(blob)
    except (ValueError, json.JSONDecodeError) as exc: raise HTTPException(422, detail=str(exc))
    if metadata.get("event_id") != event_id: raise HTTPException(422, detail="event binding mismatch")
    conn=db(); init_db(conn); row=conn.execute("SELECT device_id FROM events WHERE event_id=?",(event_id,)).fetchone()
    if not row: conn.close(); raise HTTPException(404, detail="event not found")
    root=DB_PATH.parent / "evidence"; root.mkdir(parents=True,exist_ok=True); path=root/(event_id.replace("/","_")+".edv2"); path.write_bytes(blob)
    conn.execute("INSERT OR REPLACE INTO evidence VALUES(?,?,?,?,?,?)",(event_id,row[0],str(path),len(blob),1,int(time.time()*1000))); conn.commit(); conn.close()
    return {"status":"stored","event_id":event_id,"bytes":len(blob)}

@app.get("/api/v1/evidence/{event_id}/download")
def download_evidence(event_id: str, x_edgedoctor_token: str | None = Header(default=None)):
    auth(x_edgedoctor_token)
    from fastapi.responses import FileResponse
    conn=db(); init_db(conn); row=conn.execute("SELECT path FROM evidence WHERE event_id=? AND crc_ok=1",(event_id,)).fetchone(); conn.close()
    if not row or not Path(row[0]).is_file(): raise HTTPException(404, detail="evidence not found")
    return FileResponse(row[0], media_type="application/octet-stream", filename=event_id+".edv2")

@app.get("/api/v1/events/{event_id}/evidence")
def evidence_metadata(event_id: str, x_edgedoctor_token: str | None = Header(default=None)) -> dict[str, Any]:
    auth(x_edgedoctor_token)
    conn = db(); init_db(conn)
    row = conn.execute("SELECT event_id,device_id,bytes,crc_ok,created_ms FROM evidence WHERE event_id=?", (event_id,)).fetchone()
    conn.close()
    if not row: raise HTTPException(404, detail="evidence not found")
    return {"event_id": row[0], "device_id": row[1], "bytes": row[2], "crc_ok": bool(row[3]), "created_ms": row[4], "download": f"/api/v1/evidence/{event_id}/download"}

def ingest_evidence_chunk(device_id: str, event_id: str, index: int, total: int, payload: bytes) -> dict[str, Any]:
    if not re.fullmatch(r"[A-Za-z0-9_.-]{1,47}", device_id) or not re.fullmatch(r"[A-Za-z0-9_.-]{1,95}", event_id) or total < 1 or total > 33 or index < 0 or index >= total or not 1 <= len(payload) <= 508:
        raise ValueError("invalid evidence chunk")
    conn = db(); init_db(conn); now = int(time.time() * 1000)
    try:
        if not conn.execute("SELECT 1 FROM events WHERE event_id=? AND device_id=?", (event_id, device_id)).fetchone():
            raise ValueError("unknown evidence event/device")
        previous = conn.execute("SELECT chunk_total,payload FROM evidence_chunks WHERE event_id=? AND device_id=? AND chunk_index=?", (event_id, device_id, index)).fetchone()
        if previous and (previous[0] != total or previous[1] != payload):
            raise ValueError("conflicting evidence chunk")
        totals = conn.execute("SELECT DISTINCT chunk_total FROM evidence_chunks WHERE event_id=? AND device_id=?", (event_id, device_id)).fetchall()
        if any(row[0] != total for row in totals):
            raise ValueError("inconsistent chunk total")
        conn.execute("INSERT OR IGNORE INTO evidence_chunks VALUES(?,?,?,?,?,?)", (event_id, device_id, index, total, payload, now))
        rows = conn.execute("SELECT chunk_index,payload FROM evidence_chunks WHERE event_id=? AND device_id=? ORDER BY chunk_index", (event_id, device_id)).fetchall()
        if len(rows) != total or not all(int(row[0]) == n for n, row in enumerate(rows)):
            conn.commit()
            return {"status":"partial", "event_id":event_id, "received":len(rows), "total":total}
        blob = b"".join(row[1] for row in rows)
        try:
            metadata, _ = unpack(blob)
            if len(blob) > 16384 or not isinstance(metadata, dict) or metadata.get("event_id") != event_id:
                raise ValueError("evidence identity/size invalid")
            challenge = metadata.get("challenge")
            requests = conn.execute("SELECT payload FROM commands WHERE device_id=? AND event_id=? AND status != 'EXPIRED'", (device_id, event_id)).fetchall()
            if not isinstance(challenge, str) or not any(json.loads(row[0]).get("action") == "request_evidence" and json.loads(row[0]).get("challenge") == challenge for row in requests):
                raise ValueError("evidence challenge mismatch")
        except (ValueError, TypeError):
            conn.execute("DELETE FROM evidence_chunks WHERE event_id=? AND device_id=?", (event_id, device_id)); conn.commit()
            raise
        root = DB_PATH.parent / "evidence"; root.mkdir(parents=True, exist_ok=True)
        path = root / (event_id + ".edv2")
        temporary = root / (event_id + ".edv2.tmp")
        with temporary.open("wb") as output:
            output.write(blob); output.flush(); os.fsync(output.fileno())
        os.replace(temporary, path)
        conn.execute("INSERT OR REPLACE INTO evidence VALUES(?,?,?,?,?,?)", (event_id, device_id, str(path), len(blob), 1, now))
        conn.execute("DELETE FROM evidence_chunks WHERE event_id=? AND device_id=?", (event_id, device_id)); conn.commit()
        return {"status":"stored", "event_id":event_id, "challenge":challenge, "bytes":len(blob), "sha256":hashlib.sha256(blob).hexdigest()}
    finally:
        conn.close()

@app.post("/api/v1/assets/{asset_id}/maintenance", status_code=201)
def maintenance(asset_id: str, body: dict[str, Any], x_edgedoctor_token: str | None = Header(default=None)) -> dict[str, Any]:
    auth(x_edgedoctor_token)
    operator=str(body.get("operator","")); note=str(body.get("note",""))
    if not asset_id or not operator or len(note)>2000: raise HTTPException(422, detail="invalid maintenance record")
    now=int(time.time()*1000); conn=db(); init_db(conn); cur=conn.execute("INSERT INTO maintenance(asset_id,operator,note,created_ms) VALUES(?,?,?,?)",(asset_id,operator,note,now)); conn.commit(); conn.close()
    return {"id":cur.lastrowid,"asset_id":asset_id,"created_ms":now}

@app.get("/api/v1/assets/{asset_id}/maintenance")
def maintenance_list(asset_id: str) -> list[dict[str, Any]]:
    conn=db(); init_db(conn); rows=conn.execute("SELECT id,asset_id,operator,note,created_ms FROM maintenance WHERE asset_id=? ORDER BY created_ms DESC",(asset_id,)).fetchall(); conn.close()
    return [dict(r) for r in rows]

@app.get("/api/v1/assets/{asset_id}/maintenance/compare")
def maintenance_compare(asset_id: str) -> dict[str, Any]:
    conn = db(); init_db(conn)
    m = conn.execute("SELECT created_ms FROM maintenance WHERE asset_id=? ORDER BY created_ms DESC LIMIT 1", (asset_id,)).fetchone()
    if not m: conn.close(); raise HTTPException(404, detail="maintenance record not found")
    pivot = int(m[0]); rows = conn.execute("SELECT payload FROM telemetry WHERE device_id=?", (asset_id,)).fetchall()
    events = conn.execute("SELECT payload,updated_ms FROM events WHERE device_id=?", (asset_id,)).fetchall(); conn.close()
    def summary(items: list[dict[str, Any]]) -> dict[str, Any]:
        rms = [float(x["rms_vector_g"]) for x in items if isinstance(x.get("rms_vector_g"), (int, float))]
        score = [float(x["anomaly_score"]) for x in items if isinstance(x.get("anomaly_score"), (int, float))]
        return {"samples": len(items), "rms_mean_g": sum(rms) / len(rms) if rms else None, "anomaly_mean": sum(score) / len(score) if score else None}
    before, after = [], []
    for row in rows:
        item = json.loads(row[0]); (after if int(item.get("timestamp_ms", item.get("mono_ms", 0))) >= pivot else before).append(item)
    before_events = sum(1 for payload, ts in events if ts < pivot and json.loads(payload).get("condition") == "ACTIVE")
    after_events = sum(1 for payload, ts in events if ts >= pivot and json.loads(payload).get("condition") == "ACTIVE")
    return {"asset_id": asset_id, "maintenance_ms": pivot, "before": summary(before), "after": summary(after), "alarm_count_before": before_events, "alarm_count_after": after_events}

@app.get("/api/v1/stream")
async def stream():
    async def events():
        last=0
        for _ in range(30):
            conn=db(); init_db(conn); rows=conn.execute("SELECT id,kind,payload,created_ms FROM audit_log WHERE id>? ORDER BY id LIMIT 50",(last,)).fetchall(); conn.close()
            for row in rows:
                # Unnamed SSE messages deliberately use EventSource.onmessage in the
                # dashboard; the payload still contains the audited event kind.
                last=row[0]; yield f"id: {row[0]}\ndata: {row[2]}\n\n"
            await asyncio.sleep(1)
    return StreamingResponse(events(), media_type="text/event-stream", headers={"Cache-Control":"no-cache","X-Accel-Buffering":"no"})


@app.post("/api/v1/events/{event_id}/ack", status_code=202)
def ack(event_id: str, request: Ack, x_edgedoctor_token: str | None = Header(default=None)) -> dict[str, Any]:
    auth(x_edgedoctor_token)
    conn=db(); init_db(conn); row=conn.execute("SELECT device_id,revision,payload FROM events WHERE event_id=?",(event_id,)).fetchone()
    if not row: conn.close(); raise HTTPException(404, detail="event not found")
    if row[1] != request.expected_revision: conn.close(); raise HTTPException(409, detail={"current_revision":row[1]})
    command_id=f"cmd-{int(time.time()*1000)}-{abs(hash((event_id,request.actor)))%1000000:06d}"
    payload=json.dumps({"action":"ack_event","event_id":event_id,"actor":request.actor,"expected_revision":request.expected_revision})
    conn.execute("INSERT INTO commands VALUES(?,?,?,?,?,?)",(command_id,row[0],event_id,payload,"PENDING",int(time.time()*1000))); conn.commit(); conn.close()
    return {"status":"pending","command_id":command_id,"event_id":event_id}

@app.post("/api/v1/events/{event_id}/evidence-request", status_code=202)
def evidence_request(event_id: str, x_edgedoctor_token: str | None = Header(default=None)) -> dict[str, Any]:
    """Queue a bounded pre/post raw-sample capture on the device."""
    auth(x_edgedoctor_token)
    conn = db(); init_db(conn)
    row = conn.execute("SELECT device_id,revision FROM events WHERE event_id=?", (event_id,)).fetchone()
    if not row:
        conn.close(); raise HTTPException(404, detail="event not found")
    challenge = secrets.token_hex(16)
    command_id = f"evidence-{int(time.time()*1000)}-{challenge[:8]}"
    payload = json.dumps({"action":"request_evidence", "event_id":event_id, "expected_revision":row[1], "challenge":challenge, "pre_ms":5000, "post_ms":5000}, separators=(",", ":"))
    conn.execute("INSERT INTO commands VALUES(?,?,?,?,?,?)", (command_id, row[0], event_id, payload, "PENDING", int(time.time()*1000)))
    conn.commit(); conn.close()
    return {"status":"pending", "command_id":command_id, "event_id":event_id, "challenge":challenge, "pre_ms":5000, "post_ms":5000}


@app.get("/api/v1/commands/{command_id}")
def command(command_id: str) -> dict[str, Any]:
    conn=db(); init_db(conn); row=conn.execute("SELECT command_id,status,payload FROM commands WHERE command_id=?",(command_id,)).fetchone(); conn.close()
    if not row: raise HTTPException(404, detail="command not found")
    return {"command_id":row[0],"status":row[1],"payload":json.loads(row[2])}

@app.get("/api/v1/commands")
def pending_commands(device_id: str = Query(..., min_length=1, max_length=32), status: str = Query("PENDING")) -> list[dict[str, Any]]:
    """Read the bounded command queue for a device-side MQTT poller."""
    if status not in {"PENDING", "APPLIED", "REJECTED", "EXPIRED"}:
        raise HTTPException(422, detail="invalid command status")
    conn = db(); init_db(conn)
    rows = conn.execute("SELECT command_id,event_id,payload,status,created_ms FROM commands WHERE device_id=? AND status=? ORDER BY created_ms LIMIT 20", (device_id, status)).fetchall()
    conn.close()
    return [{"command_id": r[0], "event_id": r[1], "payload": json.loads(r[2]), "status": r[3], "created_ms": r[4]} for r in rows]

@app.post("/api/v1/commands/{command_id}/result")
def command_result(command_id: str, result: CommandResult, x_edgedoctor_token: str | None = Header(default=None)) -> dict[str, Any]:
    """Apply a device receipt to a pending command; safe to retry with the same status."""
    auth(x_edgedoctor_token)
    conn = db(); init_db(conn)
    row = conn.execute("SELECT status FROM commands WHERE command_id=?", (command_id,)).fetchone()
    if not row:
        conn.close(); raise HTTPException(404, detail="command not found")
    current = row[0]
    now = int(time.time() * 1000)
    if result.status == "PENDING":
        # Not a terminal receipt: record that the device accepted the command
        # but leave its status alone for the final result to settle.
        conn.execute("INSERT INTO audit_log(kind,payload,created_ms) VALUES(?,?,?)", ("command_result", json.dumps({"command_id": command_id, "status": result.status, "detail": result.detail}, separators=(",", ":")), now))
        conn.commit(); conn.close()
        return {"command_id": command_id, "status": current, "detail": result.detail}
    if current not in {"PENDING", result.status}:
        conn.close(); raise HTTPException(409, detail={"current_status": current})
    conn.execute("UPDATE commands SET status=? WHERE command_id=?", (result.status, command_id))
    conn.execute("INSERT INTO audit_log(kind,payload,created_ms) VALUES(?,?,?)", ("command_result", json.dumps({"command_id": command_id, "status": result.status, "detail": result.detail}, separators=(",", ":")), now))
    conn.commit(); conn.close()
    return {"command_id": command_id, "status": result.status, "detail": result.detail}

@app.post("/api/v1/telemetry")
def telemetry(payload: dict[str, Any]) -> dict[str, str]:
    device, mono = validate_telemetry(payload)
    received_ms = int(time.time() * 1000)
    conn=db(); init_db(conn)
    conn.execute("INSERT OR REPLACE INTO telemetry VALUES(?,?,?)",(device,mono,json.dumps(payload,separators=(",",":"))))
    conn.execute("INSERT OR REPLACE INTO telemetry_receipts VALUES(?,?,?)", (device, mono, received_ms))
    conn.commit(); conn.close()
    return {"status":"persisted"}


@app.get("/api/v1/factory/summary")
def factory_summary() -> dict[str, Any]:
    now = int(time.time() * 1000)
    conn = db(); init_db(conn)
    assets: list[dict[str, Any]] = []
    counts = {"ONLINE": 0, "STALE": 0, "UNKNOWN": 0}
    for binding in asset_bindings():
        item = public_binding(binding)
        device_id = item["device_id"]
        received_ms = None
        if device_id:
            row = conn.execute(
                "SELECT MAX(received_ms) FROM telemetry_receipts WHERE device_id=?", (device_id,)
            ).fetchone()
            received_ms = row[0] if row and row[0] is not None else None
        # 在线状态只对已核验绑定成立；provisional / unbound 一律 UNKNOWN，不进入在线统计。
        if binding.get("binding_state") != "verified" or not device_id or received_ms is None:
            status = "UNKNOWN"
        elif now - int(received_ms) <= ONLINE_RECEIPT_MAX_AGE_MS:
            status = "ONLINE"
        else:
            status = "STALE"
        item["status"] = status
        item["last_telemetry_received_ms"] = received_ms
        assets.append(item)
        counts[status] += 1
    conn.close()
    return {"assets": assets, "counts": counts, "online_threshold_ms": ONLINE_RECEIPT_MAX_AGE_MS}


@app.get("/api/v1/assets/{asset_id}/binding")
def asset_binding(asset_id: str) -> dict[str, Any]:
    for binding in asset_bindings():
        if binding.get("asset_id") == asset_id:
            return public_binding(binding)
    raise HTTPException(404, detail="asset binding not found")

@app.get("/api/v1/trends")
def trends(device_id: str, limit: int = Query(2000, ge=1, le=2000)) -> list[dict[str, Any]]:
    conn=db(); init_db(conn); rows=conn.execute("SELECT payload FROM telemetry WHERE device_id=? ORDER BY mono_ms DESC LIMIT ?",(device_id,limit)).fetchall(); conn.close()
    return [json.loads(r[0]) for r in reversed(rows)]

@app.get("/api/v1/trends/minute")
def minute_trends(device_id: str, limit: int = Query(1440, ge=1, le=1440)) -> list[dict[str, Any]]:
    raw = trends(device_id, min(limit * 60, 2000))
    buckets: dict[int, dict[str, Any]] = {}
    for item in raw:
        stamp = int(item.get("timestamp_ms", item.get("mono_ms", 0)))
        key = stamp // 60000
        bucket = buckets.setdefault(key, {"minute": key, "samples": 0, "alarm_count": 0, "rms_sum": 0.0, "score_sum": 0.0, "rms_n": 0, "score_n": 0})
        bucket["samples"] += 1
        if item.get("condition") == "ACTIVE" or item.get("alarm") is True: bucket["alarm_count"] += 1
        if isinstance(item.get("rms_vector_g"), (int, float)): bucket["rms_sum"] += float(item["rms_vector_g"]); bucket["rms_n"] += 1
        if isinstance(item.get("anomaly_score"), (int, float)): bucket["score_sum"] += float(item["anomaly_score"]); bucket["score_n"] += 1
    result=[]
    for bucket in sorted(buckets.values(), key=lambda item: item["minute"]):
        result.append({"minute": bucket["minute"], "samples": bucket["samples"], "alarm_count": bucket["alarm_count"], "rms_mean_g": round(bucket["rms_sum"] / bucket["rms_n"], 6) if bucket["rms_n"] else None, "anomaly_mean": round(bucket["score_sum"] / bucket["score_n"], 6) if bucket["score_n"] else None})
    return result[-limit:]
