"""Firmware push: the PC streams an image, the (fake) panel checks and 'restarts', the PC verifies the new build."""

from __future__ import annotations

import asyncio
import hashlib
import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from dashlink import protocol  # noqa: E402
from dashlink.client import Link  # noqa: E402
from dashlink.fakepanel import FakePanel  # noqa: E402
from dashlink.firmware import APP_DESC_MAGIC, APP_DESC_OFFSET, FirmwareError, parse_image  # noqa: E402


def make_image(elf: bytes, chip: int = 0x12, size: int = 40_000, version: str = "1.2.3") -> bytes:
    data = bytearray(size)
    data[0] = 0xE9
    struct.pack_into("<H", data, 12, chip)
    struct.pack_into("<I", data, APP_DESC_OFFSET, APP_DESC_MAGIC)
    data[APP_DESC_OFFSET + 16 : APP_DESC_OFFSET + 16 + len(version)] = version.encode()
    data[APP_DESC_OFFSET + 144 : APP_DESC_OFFSET + 176] = elf
    for i in range(APP_DESC_OFFSET + 176, size):  # something that is not all zeros, so a short copy shows
        data[i] = i % 251
    return bytes(data)


class Image(unittest.TestCase):
    def test_reads_identity(self):
        image = parse_image(make_image(b"\xab" * 32), "p4")
        self.assertEqual(image.elf_sha256, "ab" * 32)
        self.assertEqual(image.version, "1.2.3")
        self.assertEqual(image.sha256, hashlib.sha256(image.data).hexdigest())

    def test_refuses_what_is_not_for_this_target(self):
        with self.assertRaises(FirmwareError):
            parse_image(make_image(b"\0" * 32, chip=0x0D), "p4")  # a C6 image pushed as P4
        with self.assertRaises(FirmwareError):
            parse_image(make_image(b"\0" * 32), "c6")
        with self.assertRaises(FirmwareError):
            parse_image(b"<html>not firmware</html>" * 100, "p4")
        with self.assertRaises(FirmwareError):
            parse_image(b"\xe9" * 10, "p4")



class Framing(unittest.IsolatedAsyncioTestCase):
    async def test_data_frame_is_raw(self):
        frame = protocol.encode_data(b"\x00\xff{}")
        self.assertEqual(frame, (5).to_bytes(4, "little") + b"D\x00\xff{}")
        reader = asyncio.StreamReader()
        reader.feed_data(frame)
        self.assertEqual(await protocol.read_frame(reader), (b"D", {"data": b"\x00\xff{}"}))


class Push(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.panel = FakePanel("secret", heartbeat_s=0.5)
        self.panel.elf = "11" * 32
        await self.panel.start()
        self.link = Link("127.0.0.1", "secret", "desk", port=self.panel.port, connect_timeout_s=1.0)
        self.task = asyncio.create_task(self.link.run_forever())
        await asyncio.wait_for(self.panel.connected.wait(), 3)
        self.steps: list[tuple[int, str]] = []

    async def asyncTearDown(self):
        self.task.cancel()
        await asyncio.gather(self.task, return_exceptions=True)
        await self.panel.stop()

    def progress(self, percent: int, text: str) -> None:
        self.steps.append((percent, text))

    async def test_pushes_and_confirms_the_new_build(self):
        image = parse_image(make_image(b"\x22" * 32, size=100_000), "p4")
        message = await self.link.push_firmware(image, progress=self.progress, restart_timeout_s=10)
        self.assertIn("updated", message)
        self.assertEqual(self.panel.updates, [image.data])  # every byte, in order
        self.assertEqual(self.steps[0][0], 0)
        self.assertEqual(self.steps[-1][0], 100)
        self.assertEqual(self.link.panel["elf"], "22" * 32)  # it reconnected to the "restarted" panel

    async def test_the_same_build_is_not_pushed_again(self):
        image = parse_image(make_image(b"\x11" * 32), "p4")
        self.assertIn("already runs", await self.link.push_firmware(image))
        self.assertEqual(self.panel.updates, [])
        await self.link.push_firmware(image, force=True, restart_timeout_s=10)
        self.assertEqual(len(self.panel.updates), 1)

    async def test_a_refusal_is_reported(self):
        self.panel.refuse_update = "an update is already running"
        with self.assertRaisesRegex(FirmwareError, "already running"):
            await self.link.push_firmware(parse_image(make_image(b"\x22" * 32), "p4"))

    async def test_a_rolled_back_build_is_an_error(self):
        self.panel.update_boots = False
        with self.assertRaisesRegex(FirmwareError, "rolled back"):
            await self.link.push_firmware(parse_image(make_image(b"\x22" * 32), "p4"), restart_timeout_s=10)

    async def test_a_panel_that_never_returns_times_out(self):
        await self.panel.stop()  # nothing to reconnect to after the update
        with self.assertRaises(FirmwareError):
            await self.link.push_firmware(parse_image(make_image(b"\x22" * 32), "p4"), restart_timeout_s=1)


if __name__ == "__main__":
    unittest.main()
