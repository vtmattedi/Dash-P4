"""A firmware image to push to the panel: checked here before a byte is sent, and the identity (the ELF sha256 inside the
app descriptor) the panel must report after it restarts."""

from __future__ import annotations

import hashlib
import struct
from dataclasses import dataclass
from pathlib import Path

ESP_IMAGE_MAGIC = 0xE9
APP_DESC_MAGIC = 0xABCD5432
APP_DESC_OFFSET = 24 + 8  # image header, then the first segment's header; the app descriptor starts the first segment
CHIP_IDS = {"p4": 0x12, "c6": 0x0D, "dashboard": 0x00}  # "dashboard" is the ESP32 handheld panel (devices/Dashboard)
TARGETS = tuple(CHIP_IDS)
# Targets whose challenge carries the build identity (`elf`), so the PC can tell "updated" from "rolled back".
ELF_TARGETS = ("p4", "dashboard")
DEFAULT_ENV = "jc1060p470c"
DASHBOARD_ENV = "esp32-3248S035C"


class FirmwareError(Exception):
    """The file is not an image this panel can run."""


@dataclass(frozen=True)
class Image:
    target: str
    data: bytes
    sha256: str                  # of the whole file
    elf_sha256: str | None       # the build's identity, as the panel reports it (P4 images only)
    version: str

    @property
    def size(self) -> int:
        return len(self.data)


def default_path(env: str = DEFAULT_ENV, target: str = "p4") -> Path:
    """The PlatformIO build output of this repository (pc/ sits inside the Dash-P4 project); for the Dashboard, the
    sibling project's."""
    if target == "dashboard":
        return Path(__file__).resolve().parents[3] / "Dashboard" / ".pio" / "build" / DASHBOARD_ENV / "firmware.bin"
    return Path(__file__).resolve().parents[2] / ".pio" / "build" / env / "firmware.bin"


def parse_image(data: bytes, target: str) -> Image:
    if target not in CHIP_IDS:
        raise FirmwareError(f"unknown target {target!r} (use one of {', '.join(TARGETS)})")
    if len(data) < APP_DESC_OFFSET + 256:
        raise FirmwareError("the file is too short to be a firmware image")
    if data[0] != ESP_IMAGE_MAGIC:
        raise FirmwareError("not an ESP application image (use firmware.bin from the build folder, not a factory image)")
    (chip_id,) = struct.unpack_from("<H", data, 12)
    if chip_id != CHIP_IDS[target]:
        raise FirmwareError(f"this image is for chip id {chip_id:#x}, not the {target.upper()} ({CHIP_IDS[target]:#x})")
    elf = version = None
    (magic,) = struct.unpack_from("<I", data, APP_DESC_OFFSET)
    if magic == APP_DESC_MAGIC:
        version = data[APP_DESC_OFFSET + 16 : APP_DESC_OFFSET + 48].split(b"\0", 1)[0].decode("ascii", "replace")
        elf = data[APP_DESC_OFFSET + 144 : APP_DESC_OFFSET + 176].hex()
    return Image(target, data, hashlib.sha256(data).hexdigest(), elf, version or "?")


def load_image(path: Path | str, target: str = "p4") -> Image:
    try:
        data = Path(path).read_bytes()
    except OSError as error:
        raise FirmwareError(f"cannot read {path}: {error}") from error
    return parse_image(data, target)
