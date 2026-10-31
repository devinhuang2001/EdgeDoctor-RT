"""Run a disposable, plaintext MQTT broker for LAN development.

This helper is intentionally separate from the production Mosquitto compose
deployment. It is useful on a workstation where Docker is unavailable. Do not
expose this broker to an untrusted network; production uses Mosquitto with TLS,
authentication, ACLs, and persistence.
"""

from __future__ import annotations

import argparse
import asyncio
import logging
from pathlib import Path

from amqtt.broker import Broker


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="EdgeDoctor development MQTT broker")
    parser.add_argument("--host", default="0.0.0.0", help="bind address (default: all interfaces)")
    parser.add_argument("--port", type=int, default=1883, help="plaintext MQTT port")
    parser.add_argument("--data-dir", type=Path, default=Path(".mqtt-dev"))
    return parser.parse_args()


async def run(host: str, port: int, data_dir: Path) -> None:
    data_dir.mkdir(parents=True, exist_ok=True)
    config = {
        "listeners": {"default": {"type": "tcp", "bind": f"{host}:{port}"}},
        "sys_interval": 20,
        "auth": {"allow-anonymous": True},
        "topic-check": {"enabled": False},
    }
    broker = Broker(config)
    await broker.start()
    logging.getLogger(__name__).warning(
        "development MQTT broker listening on %s:%d (anonymous/plaintext; do not use in production)",
        host,
        port,
    )
    try:
        await asyncio.Event().wait()
    finally:
        await broker.shutdown()


def main() -> None:
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    args = parse_args()
    if not 1 <= args.port <= 65535:
        raise SystemExit("--port must be between 1 and 65535")
    try:
        asyncio.run(run(args.host, args.port, args.data_dir))
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
