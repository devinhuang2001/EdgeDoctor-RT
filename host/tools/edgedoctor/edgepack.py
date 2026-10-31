"""Versioned, bounded, CRC-protected EdgePack v1 codec."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
import struct
import zlib

import numpy as np


MAGIC = b"EDGP"
VERSION = 1
HEADER = struct.Struct("<4sHHIHBBhHIIBBHIIII16s")
HEADER_BYTES = HEADER.size


class EdgePackError(ValueError):
    """Raised for invalid or corrupted EdgePack input."""


@dataclass(frozen=True)
class EdgePackHeader:
    recording_id: int
    dataset_id: int
    label: int
    severity: int
    load_x10_nm: int
    chunk_ms: int
    audio_rate: int
    vibration_rate: int
    vibration_channels: int
    audio_samples: int
    vibration_frames: int
    payload_bytes: int = 0
    payload_crc32: int = 0

    def validate(self) -> None:
        if not 0 <= self.label <= 4:
            raise EdgePackError("label must be in range 0..4")
        if not 0 <= self.severity <= 255:
            raise EdgePackError("severity must fit uint8")
        if self.chunk_ms <= 0 or 1000 % self.chunk_ms:
            raise EdgePackError("chunk_ms must be a positive divisor of 1000")
        if self.audio_rate <= 0 or self.vibration_rate <= 0:
            raise EdgePackError("sample rates must be positive")
        if (self.audio_rate * self.chunk_ms) % 1000:
            raise EdgePackError("audio chunk has a fractional sample count")
        if (self.vibration_rate * self.chunk_ms) % 1000:
            raise EdgePackError("vibration chunk has a fractional frame count")
        if not 1 <= self.vibration_channels <= 4:
            raise EdgePackError("vibration channel count must be in range 1..4")
        audio_per_chunk = self.audio_rate * self.chunk_ms // 1000
        vibration_per_chunk = self.vibration_rate * self.chunk_ms // 1000
        if self.audio_samples <= 0 or self.audio_samples % audio_per_chunk:
            raise EdgePackError("audio sample count is not a whole number of chunks")
        chunks = self.audio_samples // audio_per_chunk
        if self.vibration_frames != chunks * vibration_per_chunk:
            raise EdgePackError("audio and vibration durations are not synchronized")
        expected = 2 * (
            self.audio_samples + self.vibration_frames * self.vibration_channels
        )
        if self.payload_bytes and self.payload_bytes != expected:
            raise EdgePackError("payload byte count does not match header dimensions")


@dataclass(frozen=True)
class EdgePack:
    header: EdgePackHeader
    audio: np.ndarray
    vibration: np.ndarray

    @staticmethod
    def create(
        header: EdgePackHeader, audio: np.ndarray, vibration: np.ndarray
    ) -> "EdgePack":
        audio_i16 = np.ascontiguousarray(audio, dtype="<i2").reshape(-1)
        vibration_i16 = np.ascontiguousarray(vibration, dtype="<i2")
        if vibration_i16.ndim != 2:
            raise EdgePackError("vibration must have shape [frames, channels]")
        if len(audio_i16) != header.audio_samples:
            raise EdgePackError("audio array length differs from header")
        if vibration_i16.shape != (
            header.vibration_frames,
            header.vibration_channels,
        ):
            raise EdgePackError("vibration array shape differs from header")
        header.validate()
        return EdgePack(header, audio_i16, vibration_i16)

    def _payload(self) -> bytes:
        h = self.header
        audio_chunk = h.audio_rate * h.chunk_ms // 1000
        vibration_chunk = h.vibration_rate * h.chunk_ms // 1000
        chunks = h.audio_samples // audio_chunk
        parts: list[bytes] = []
        for index in range(chunks):
            a0 = index * audio_chunk
            v0 = index * vibration_chunk
            parts.append(self.audio[a0 : a0 + audio_chunk].astype("<i2", copy=False).tobytes())
            parts.append(
                self.vibration[v0 : v0 + vibration_chunk]
                .astype("<i2", copy=False)
                .tobytes()
            )
        return b"".join(parts)

    def to_bytes(self) -> bytes:
        payload = self._payload()
        crc = zlib.crc32(payload) & 0xFFFFFFFF
        h = self.header
        packed = HEADER.pack(
            MAGIC,
            VERSION,
            HEADER_BYTES,
            h.recording_id,
            h.dataset_id,
            h.label,
            h.severity,
            h.load_x10_nm,
            h.chunk_ms,
            h.audio_rate,
            h.vibration_rate,
            h.vibration_channels,
            2,
            0,
            h.audio_samples,
            h.vibration_frames,
            len(payload),
            crc,
            b"\x00" * 16,
        )
        return packed + payload

    def write(self, path: str | Path) -> None:
        destination = Path(path)
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(self.to_bytes())

    @staticmethod
    def from_bytes(data: bytes) -> "EdgePack":
        if len(data) < HEADER_BYTES:
            raise EdgePackError("file is shorter than the EdgePack header")
        fields = HEADER.unpack_from(data)
        if fields[0] != MAGIC:
            raise EdgePackError("invalid EdgePack magic")
        if fields[1] != VERSION or fields[2] != HEADER_BYTES:
            raise EdgePackError("unsupported EdgePack version/header size")
        if fields[12] != 2:
            raise EdgePackError("only int16 EdgePack payloads are supported")
        header = EdgePackHeader(
            recording_id=fields[3],
            dataset_id=fields[4],
            label=fields[5],
            severity=fields[6],
            load_x10_nm=fields[7],
            chunk_ms=fields[8],
            audio_rate=fields[9],
            vibration_rate=fields[10],
            vibration_channels=fields[11],
            audio_samples=fields[14],
            vibration_frames=fields[15],
            payload_bytes=fields[16],
            payload_crc32=fields[17],
        )
        header.validate()
        if len(data) != HEADER_BYTES + header.payload_bytes:
            raise EdgePackError("file length differs from declared payload length")
        payload = data[HEADER_BYTES:]
        if (zlib.crc32(payload) & 0xFFFFFFFF) != header.payload_crc32:
            raise EdgePackError("payload CRC32 mismatch")

        audio = np.empty(header.audio_samples, dtype="<i2")
        vibration = np.empty(
            (header.vibration_frames, header.vibration_channels), dtype="<i2"
        )
        audio_chunk = header.audio_rate * header.chunk_ms // 1000
        vibration_chunk = header.vibration_rate * header.chunk_ms // 1000
        chunk_bytes = 2 * (
            audio_chunk + vibration_chunk * header.vibration_channels
        )
        offset = 0
        for index in range(header.audio_samples // audio_chunk):
            block = payload[offset : offset + chunk_bytes]
            a_bytes = audio_chunk * 2
            a0 = index * audio_chunk
            v0 = index * vibration_chunk
            audio[a0 : a0 + audio_chunk] = np.frombuffer(
                block[:a_bytes], dtype="<i2"
            )
            vibration[v0 : v0 + vibration_chunk] = np.frombuffer(
                block[a_bytes:], dtype="<i2"
            ).reshape(vibration_chunk, header.vibration_channels)
            offset += chunk_bytes
        return EdgePack(header, audio, vibration)

    @staticmethod
    def read(path: str | Path) -> "EdgePack":
        return EdgePack.from_bytes(Path(path).read_bytes())

