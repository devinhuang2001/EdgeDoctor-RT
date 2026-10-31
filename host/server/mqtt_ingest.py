"""MQTT 3.1.1 bridge. The HTTP service remains the persistence authority."""
from __future__ import annotations

import json
import os
import logging
import sqlite3
import threading
import time
from typing import Any

import paho.mqtt.client as mqtt

from .app import DB_PATH, CommandResult, Event, command_result, ingest, ingest_evidence_chunk, telemetry

LOG = logging.getLogger("edgedoctor.mqtt")


def _configured_site_id() -> str:
    """Return the deployment site used in device command topics."""
    return os.environ.get("EDGEDOCTOR_SITE_ID", "site")


def _topic_allowed(topic: str, device_id: str, kind: str = "events") -> bool:
    parts = topic.split("/")
    return len(parts) == 4 and parts[0] == "edgedoctor" and parts[2] == device_id and parts[3] == kind


def on_message(client: mqtt.Client, __: Any, message: mqtt.MQTTMessage) -> None:
    try:
        topic = getattr(message, "topic", "")
        parts = topic.split("/")
        if len(parts) == 6 and parts[0] == "edgedoctor" and parts[3] == "evidence" and parts[5] == "chunks" and len(message.payload) >= 4:
            total = int.from_bytes(message.payload[0:2], "big")
            index = int.from_bytes(message.payload[2:4], "big")
            receipt = ingest_evidence_chunk(parts[2], parts[4], index, total, message.payload[4:])
            if receipt.get("status") == "stored":
                client.publish("/".join(parts[:3]) + "/evidence_receipts", json.dumps(receipt, separators=(",", ":")), qos=1, retain=False)
            return
        payload = json.loads(message.payload.decode("utf-8"))
        if topic.endswith("/events"):
            event = Event.model_validate(payload)
            if _topic_allowed(topic, event.device_id):
                receipt = ingest(event, os.environ.get("EDGEDOCTOR_API_TOKEN"))
                if receipt.get("status") == "persisted":
                    receipt_topic = topic.rsplit("/", 1)[0] + "/event_receipts"
                    client.publish(receipt_topic, json.dumps(receipt, separators=(",", ":")), qos=1, retain=False)
        elif topic.endswith("/telemetry"):
            device = str(payload.get("device_id", ""))
            if _topic_allowed(topic, device, "telemetry") and isinstance(payload.get("mono_ms"), int):
                telemetry(payload)
        elif topic.endswith("/command-results"):
            device = str(payload.get("device_id", ""))
            command_id = str(payload.get("command_id", ""))
            if command_id and _topic_allowed(topic, device, "command-results"):
                command_result(command_id, CommandResult.model_validate(payload),
                               os.environ.get("EDGEDOCTOR_API_TOKEN"))
        else:
            return
    except Exception:
        # Invalid MQTT payloads are rejected at the boundary and never stored.
        return

def on_connect(client: mqtt.Client, _: Any, __: Any, rc: int, ___: Any = None) -> None:
    if rc == 0:
        topic = os.environ.get("EDGEDOCTOR_EVENTS_TOPIC", "edgedoctor/+/+/#")
        result, _ = client.subscribe(topic, qos=1)
        LOG.info("mqtt connected; subscribed topic=%s result=%s", topic, result)
    else:
        LOG.error("mqtt connection failed rc=%s", rc)

def on_disconnect(_: mqtt.Client, __: Any, rc: int, ___: Any = None) -> None:
    if rc != 0: LOG.warning("mqtt disconnected rc=%s; paho will retry", rc)

# A device that never answers must not be republished to forever. Commands are
# immediate actions, so a pending one that outlives this window is retired
# rather than retried for the lifetime of the process.
COMMAND_EXPIRY_MS = 30 * 60 * 1000


def publish_pending_commands(client: mqtt.Client) -> None:
    """Republish pending commands on the device command topic until receipt."""
    while True:
        try:
            conn = sqlite3.connect(DB_PATH)
            conn.execute(
                "UPDATE commands SET status='EXPIRED' WHERE status='PENDING' AND created_ms < ?",
                (int(time.time() * 1000) - COMMAND_EXPIRY_MS,),
            )
            rows = conn.execute("SELECT command_id,device_id,payload FROM commands WHERE status='PENDING' ORDER BY created_ms LIMIT 20").fetchall()
            conn.commit()
            conn.close()
            for command_id, device_id, payload in rows:
                topic = f"edgedoctor/{_configured_site_id()}/{device_id}/commands"
                envelope = json.dumps({"command_id": command_id, **json.loads(payload)}, separators=(",", ":"))
                client.publish(topic, envelope, qos=1)
        except Exception as exc:
            LOG.debug("command publish cycle failed: %s", exc)
        time.sleep(2)


def run() -> None:
    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=os.environ["EDGEDOCTOR_CLIENT_ID"])
    user = os.environ.get("EDGEDOCTOR_MQTT_USER")
    password = os.environ.get("EDGEDOCTOR_MQTT_PASSWORD")
    if user is not None:
        client.username_pw_set(user, password or "")
    # TLS is mandatory by default. The explicit 0 value is intended only for
    # a loopback/LAN development broker and is useful for smoke tests.
    if os.environ.get("EDGEDOCTOR_MQTT_TLS", "1") != "0":
        client.tls_set(ca_certs=os.environ["EDGEDOCTOR_MQTT_CA"])
    client.on_message = on_message
    client.on_connect = on_connect
    client.on_disconnect = on_disconnect
    client.connect(os.environ.get("EDGEDOCTOR_MQTT_HOST", "127.0.0.1"), int(os.environ.get("EDGEDOCTOR_MQTT_PORT", "8883")), 30)
    client.reconnect_delay_set(min_delay=1, max_delay=60)
    threading.Thread(target=publish_pending_commands, args=(client,), name="edgedoctor-command-publisher", daemon=True).start()
    client.loop_forever(retry_first_connection=True)


if __name__ == "__main__":
    run()
