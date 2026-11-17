"""Framing and authentication of the DashLink protocol (see PROTOCOL.md). No I/O policy here, only the wire format."""

from __future__ import annotations

import asyncio
import hashlib
import hmac
import json
import struct

VERSION = 0
DEFAULT_PORT = 7340
MAX_FRAME = 16 * 1024

CHALLENGE = b"C"
HELLO = b"H"
WELCOME = b"W"
ERROR = b"E"
PING = b"P"
PONG = b"O"
STATE = b"S"
ACTION = b"A"
RESULT = b"R"
UPDATE = b"U"  # PC -> panel: a firmware image follows (see PROTOCOL.md, "Firmware push")
DATA = b"D"    # PC -> panel: raw bytes of that image (the payload is not JSON)
STATUS = b"T"  # panel -> PC: how the update is going

MAX_CHUNK = MAX_FRAME - 1  # the most bytes one DATA frame carries


def encode_data(chunk: bytes) -> bytes:
    """A DATA frame: the payload is the raw bytes, not JSON."""
    if not 0 < len(chunk) <= MAX_CHUNK:
        raise ProtocolError(f"a data chunk is 1..{MAX_CHUNK} bytes, got {len(chunk)}")
    return struct.pack("<I", len(chunk) + 1) + DATA + chunk


class ProtocolError(Exception):
    """The peer sent something that is not DashLink v0."""


def encode(kind: bytes, body: dict | None = None) -> bytes:
    """One frame: u32 little-endian length, the type letter, then the JSON object (if any)."""
    if len(kind) != 1:
        raise ValueError("a frame type is exactly one byte")
    payload = kind
    if body is not None:
        payload += json.dumps(body, separators=(",", ":"), ensure_ascii=False).encode("utf-8")
    if len(payload) > MAX_FRAME:
        raise ProtocolError(f"frame of {len(payload)} bytes is over the {MAX_FRAME} limit")
    return struct.pack("<I", len(payload)) + payload


async def read_frame(reader: asyncio.StreamReader) -> tuple[bytes, dict]:
    """Read one frame. Raises ProtocolError on a bad length or body, asyncio.IncompleteReadError on EOF."""
    (length,) = struct.unpack("<I", await reader.readexactly(4))
    if length < 1 or length > MAX_FRAME:
        raise ProtocolError(f"frame length {length} is outside 1..{MAX_FRAME}")
    data = await reader.readexactly(length)
    kind, rest = data[:1], data[1:]
    if not rest:
        return kind, {}
    if kind == DATA:
        return kind, {"data": rest}
    try:
        body = json.loads(rest.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise ProtocolError(f"frame {kind!r} has a malformed body: {error}") from error
    if not isinstance(body, dict):
        raise ProtocolError(f"frame {kind!r} body is not a JSON object")
    return kind, body


def auth_proof(token: str, nonce: str, name: str) -> str:
    """hex(HMAC-SHA256(key=token, message=nonce + "|" + name)). The panel computes the same and compares."""
    return hmac.new(token.encode("utf-8"), f"{nonce}|{name}".encode("utf-8"), hashlib.sha256).hexdigest()


def verify_proof(token: str, nonce: str, name: str, proof: str) -> bool:
    return hmac.compare_digest(auth_proof(token, nonce, name), str(proof))
