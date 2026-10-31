"""Send one file to the RT-Thread ``ry`` command using YMODEM-1K.

This intentionally implements only the bounded single-file subset needed by
the board verification flow.  Every packet is CRC checked and retried; a
failed transfer exits non-zero instead of leaving the test to guess whether
the SD-card fixture is complete.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import posixpath
import time

import serial


SOH = 0x01
STX = 0x02
EOT = 0x04
ACK = 0x06
NAK = 0x15
CAN = 0x18
CRC_REQUEST = ord("C")


class TransferError(RuntimeError):
    pass


def crc16_xmodem(data: bytes) -> int:
    crc = 0
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def packet(marker: int, number: int, payload: bytes) -> bytes:
    crc = crc16_xmodem(payload)
    return bytes((marker, number & 0xFF, 0xFF - (number & 0xFF))) + payload + crc.to_bytes(2, "big")


def wait_for(port: serial.Serial, accepted: set[int], timeout: float) -> int:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        value = port.read(1)
        if not value:
            continue
        byte = value[0]
        if byte == CAN:
            raise TransferError("receiver cancelled the transfer")
        if byte in accepted:
            return byte
    expected = ", ".join(f"0x{x:02x}" for x in sorted(accepted))
    raise TransferError(f"timeout waiting for {expected}")


def send_with_ack(port: serial.Serial, frame: bytes, retries: int = 10) -> None:
    for _ in range(retries):
        port.write(frame)
        response = wait_for(port, {ACK, NAK}, 5.0)
        if response == ACK:
            return
    raise TransferError("packet retry limit exceeded")


def transfer(port_name: str, source: Path, destination: str, baud: int) -> None:
    content = source.read_bytes()
    remote_name = posixpath.basename(destination).encode("ascii")
    remote_directory = posixpath.dirname(destination) or "/"
    if len(remote_name) + len(str(len(content))) + 2 > 128:
        raise TransferError("YMODEM metadata exceeds block zero")

    with serial.Serial(port_name, baud, timeout=0.1, write_timeout=5.0) as port:
        port.reset_input_buffer()
        # RT-Thread's ry command takes a destination directory.  Its YMODEM
        # begin callback appends the filename carried by block zero.
        port.write(f"ry {remote_directory}\r\n".encode("ascii"))
        wait_for(port, {CRC_REQUEST}, 10.0)

        metadata = remote_name + b"\0" + str(len(content)).encode("ascii") + b"\0"
        send_with_ack(port, packet(SOH, 0, metadata.ljust(128, b"\0")))
        wait_for(port, {CRC_REQUEST}, 5.0)

        total = (len(content) + 1023) // 1024
        for index in range(total):
            payload = content[index * 1024 : (index + 1) * 1024].ljust(1024, b"\x1a")
            send_with_ack(port, packet(STX, index + 1, payload))
            print(f"\rYMODEM {index + 1}/{total}", end="", flush=True)

        port.write(bytes((EOT,)))
        response = wait_for(port, {ACK, NAK}, 5.0)
        if response == NAK:
            port.write(bytes((EOT,)))
            wait_for(port, {ACK}, 5.0)
        wait_for(port, {CRC_REQUEST}, 5.0)
        send_with_ack(port, packet(SOH, 0, bytes(128)))
        print(f"\nYMODEM PASS: {len(content)} bytes -> {destination}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path)
    parser.add_argument("destination")
    parser.add_argument("--port", default="COM21")
    parser.add_argument("--baud", type=int, default=115200)
    args = parser.parse_args()
    transfer(args.port, args.source, args.destination, args.baud)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
