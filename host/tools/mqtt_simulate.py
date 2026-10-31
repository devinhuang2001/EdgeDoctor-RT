"""Publish contract-valid EdgeDoctor events for LAN integration testing."""
from __future__ import annotations
import argparse, json, os, time

def main() -> int:
    import paho.mqtt.publish as publish
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default=os.getenv("EDGEDOCTOR_MQTT_HOST", "127.0.0.1"))
    parser.add_argument("--port", type=int, default=int(os.getenv("EDGEDOCTOR_MQTT_PORT", "8883")))
    parser.add_argument("--topic", default=os.getenv("EDGEDOCTOR_EVENTS_TOPIC", "edgedoctor/site/device/events"))
    parser.add_argument("--device", default="ed84-001")
    parser.add_argument("--mode", choices=("healthy", "alarm", "recovered"), default="alarm")
    parser.add_argument("--revision", type=int, default=1)
    args = parser.parse_args()
    active = args.mode != "recovered"
    payload = {"schema_version": 1, "device_id": args.device, "event_id": f"sim-{int(time.time())}",
               "revision": args.revision, "source": "LIVE_IMU", "condition": "ACTIVE" if active else "RECOVERED",
               "ack": "UNACKNOWLEDGED", "severity": "ALARM" if args.mode == "alarm" else "WARNING",
               "diagnosis": "UNKNOWN_ANOMALY" if args.mode == "alarm" else "WITHIN_BASELINE",
               "quality": "VALID", "snapshot_id": int(time.time() * 1000)}
    auth = {"username": os.getenv("EDGEDOCTOR_MQTT_USER"), "password": os.getenv("EDGEDOCTOR_MQTT_PASSWORD")}
    tls = {"ca_certs": os.getenv("EDGEDOCTOR_MQTT_CA")} if os.getenv("EDGEDOCTOR_MQTT_CA") else None
    publish.single(args.topic, json.dumps(payload, separators=(",", ":")), hostname=args.host, port=args.port,
                   auth=auth if auth["username"] else None, tls=tls, qos=1)
    print(json.dumps(payload, ensure_ascii=False))
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
