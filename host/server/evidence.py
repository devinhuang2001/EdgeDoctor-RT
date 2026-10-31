"""EDV2 evidence framing: explicit little-endian fields and CRCs."""
from __future__ import annotations
import json, struct, zlib

HEADER = struct.Struct("<4sHHIII")
FIRMWARE_HEADER = struct.Struct("<4sHHIIII")
MAGIC = b"EDV2"
MAX_FILE = 16 * 1024

def pack(metadata: dict, payload: bytes) -> bytes:
    meta = json.dumps(metadata, separators=(",", ":"), ensure_ascii=False).encode()
    if len(meta) > 2048 or len(payload) > MAX_FILE: raise ValueError("evidence too large")
    head = HEADER.pack(MAGIC, 2, HEADER.size, len(meta), len(payload), zlib.crc32(meta) & 0xffffffff)
    return head + meta + payload + struct.pack("<I", zlib.crc32(payload) & 0xffffffff)

def unpack(blob: bytes) -> tuple[dict, bytes]:
    if len(blob) < HEADER.size + 4: raise ValueError("truncated evidence")
    magic, version, header_bytes = struct.unpack_from("<4sHH", blob)
    if magic != MAGIC or version != 2 or header_bytes not in (HEADER.size, FIRMWARE_HEADER.size): raise ValueError("invalid evidence header")
    if header_bytes == FIRMWARE_HEADER.size:
        if len(blob) < FIRMWARE_HEADER.size + 4: raise ValueError("truncated evidence")
        _, _, _, meta_len, payload_len, meta_crc, payload_crc = FIRMWARE_HEADER.unpack_from(blob)
        header_size = FIRMWARE_HEADER.size
    else:
        _, _, _, meta_len, payload_len, meta_crc = HEADER.unpack_from(blob)
        payload_crc = None
        header_size = HEADER.size
    if meta_len > 2048 or payload_len > MAX_FILE: raise ValueError("invalid evidence header")
    end_meta = header_size + meta_len; end_payload = end_meta + payload_len
    if end_payload + 4 != len(blob): raise ValueError("truncated evidence")
    meta_raw, payload = blob[header_size:end_meta], blob[end_meta:end_payload]
    trailer_crc = struct.unpack_from("<I", blob, end_payload)[0]
    if zlib.crc32(meta_raw) & 0xffffffff != meta_crc or zlib.crc32(payload) & 0xffffffff != trailer_crc: raise ValueError("evidence CRC mismatch")
    if payload_crc is not None and zlib.crc32(payload) & 0xffffffff != payload_crc: raise ValueError("evidence CRC mismatch")
    return json.loads(meta_raw), payload
