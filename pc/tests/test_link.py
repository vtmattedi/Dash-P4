"""Run from the pc/ folder:  python -m unittest discover -s tests -v"""

from __future__ import annotations

import asyncio
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from dashlink import protocol  # noqa: E402
from dashlink.actions import ActionRunner, validate_url  # noqa: E402
from dashlink.client import Link, LinkError  # noqa: E402
from dashlink.fakepanel import FakePanel  # noqa: E402


class StubWindow:
    def __init__(self) -> None:
        self.value: dict | None = {"title": "Editor", "app": "code.exe", "pid": 1}

    async def get(self):
        return self.value


class StubMedia:
    def __init__(self) -> None:
        self.commands: list[str] = []

    async def control(self, command: str) -> bool:
        self.commands.append(command)
        return True


class StubCall:
    def __init__(self) -> None:
        self.call = None
        self.modes: list[str] = []

    async def get(self):
        return self.call

    async def mic(self, mode: str) -> bool:
        self.modes.append(mode)
        return True


class StubDisplay:
    def __init__(self) -> None:
        self.state: str | None = None
        self.wakes = 0
        self.sleeps = 0
        self.suspends = 0

    async def wake(self) -> bool:
        self.wakes += 1
        return True

    async def sleep(self) -> bool:
        self.sleeps += 1
        return True

    async def suspend(self) -> bool:
        self.suspends += 1
        return True


async def eventually(predicate, timeout: float = 3.0) -> None:
    deadline = asyncio.get_running_loop().time() + timeout
    while not predicate():
        if asyncio.get_running_loop().time() > deadline:
            raise AssertionError("condition not met in time")
        await asyncio.sleep(0.02)


class Framing(unittest.IsolatedAsyncioTestCase):
    async def test_round_trip(self):
        reader = asyncio.StreamReader()
        reader.feed_data(protocol.encode(protocol.STATE, {"window": {"title": "Ação — não"}}))
        reader.feed_data(protocol.encode(protocol.PING))
        self.assertEqual(await protocol.read_frame(reader), (b"S", {"window": {"title": "Ação — não"}}))
        self.assertEqual(await protocol.read_frame(reader), (b"P", {}))

    async def test_rejects_bad_frames(self):
        for raw in (b"\x00\x00\x00\x00", (protocol.MAX_FRAME + 1).to_bytes(4, "little") + b"S", b"\x03\x00\x00\x00S[]", b"\x04\x00\x00\x00S{x"):
            reader = asyncio.StreamReader()
            reader.feed_data(raw)
            reader.feed_eof()  # a frame that promises more than it has must fail, not wait
            with self.assertRaises((protocol.ProtocolError, asyncio.IncompleteReadError)):
                await protocol.read_frame(reader)

    def test_proof_depends_on_everything(self):
        base = protocol.auth_proof("secret", "abc", "pc")
        self.assertTrue(protocol.verify_proof("secret", "abc", "pc", base))
        self.assertFalse(protocol.verify_proof("other", "abc", "pc", base))
        self.assertFalse(protocol.verify_proof("secret", "abd", "pc", base))
        self.assertFalse(protocol.verify_proof("secret", "abc", "pc2", base))
        # a fixed vector, so the firmware can check its HMAC-SHA256 against it
        self.assertEqual(
            protocol.auth_proof("secret", "00112233445566778899aabbccddeeff", "desk"),
            "cece2869e341f73926cc74026d3de54f331328e7e2c4edc526df9c694e8f046c",
        )


class Urls(unittest.TestCase):
    def test_accepts_and_rejects(self):
        self.assertEqual(validate_url("https://example.com/a?b=1"), "https://example.com/a?b=1")
        for bad in ("", None, "javascript:alert(1)", "file:///c:/x", "ftp://x.org", "https://", "https://a b.com", "https://a.com/\n", "x" * 3000):
            with self.assertRaises(ValueError, msg=repr(bad)[:40]):
                validate_url(bad)

    def test_allowlist(self):
        validate_url("https://github.com/x", ["github.com"])
        validate_url("https://gist.github.com/x", ["github.com"])
        with self.assertRaises(ValueError):
            validate_url("https://evilgithub.com/x", ["github.com"])


class Runner(unittest.IsolatedAsyncioTestCase):
    async def test_open_url_is_rate_limited(self):
        opened: list[str] = []
        now = [0.0]
        runner = ActionRunner(opener=lambda url, browser: opened.append(url), clock=lambda: now[0], url_limit=(2, 60.0))
        self.assertEqual(await runner.run("open_url", {"url": "https://a.com"}), (True, ""))
        self.assertEqual(await runner.run("open_url", {"url": "https://b.com"}), (True, ""))
        ok, msg = await runner.run("open_url", {"url": "https://c.com"})
        self.assertFalse(ok)
        now[0] = 61.0
        self.assertEqual(await runner.run("open_url", {"url": "https://c.com"}), (True, ""))
        self.assertEqual(opened, ["https://a.com", "https://b.com", "https://c.com"])

    async def test_unknown_and_bad_arguments(self):
        runner = ActionRunner(opener=lambda url, browser: None)
        self.assertEqual(await runner.run("format_disk", {}), (False, "unknown action"))
        self.assertFalse((await runner.run("open_url", {"url": "javascript:1"}))[0])
        self.assertFalse((await runner.run("media", {"cmd": "volume_up"}))[0])


class Session(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.panel = FakePanel("secret", heartbeat_s=0.3)
        await self.panel.start()
        self.window, self.media = StubWindow(), StubMedia()
        self.opened: list[str] = []
        self.browsers: list[str] = []
        self.runner = ActionRunner(media=self.media, opener=self._open)
        self.link = Link("127.0.0.1", "secret", "desk", window=self.window, media=self.media, runner=self.runner,
                         port=self.panel.port, window_poll_s=0.05, connect_timeout_s=1.0)
        self.task: asyncio.Task | None = None

    def _open(self, url: str, browser: str):
        self.opened.append(url)
        self.browsers.append(browser)
        return "Chrome not found: used the default browser" if browser == "chrome" and url.endswith("/nochrome") else None

    async def asyncTearDown(self):
        if self.task:
            self.task.cancel()
            await asyncio.gather(self.task, return_exceptions=True)
        await self.panel.stop()

    async def start(self):
        self.task = asyncio.create_task(self.link.run_forever())
        await asyncio.wait_for(self.panel.connected.wait(), 3)

    async def test_pairs_and_publishes_state(self):
        await self.start()
        self.assertEqual(self.panel.hello["name"], "desk")
        self.assertIn("open_url", self.panel.hello["caps"])
        await eventually(lambda: self.panel.states)
        self.assertEqual(self.panel.states[0], {"window": {"title": "Editor", "app": "code.exe", "pid": 1}})

    async def test_only_changes_are_sent(self):
        await self.start()
        await eventually(lambda: self.panel.states)
        count = len(self.panel.states)
        await asyncio.sleep(0.3)
        self.assertEqual(len(self.panel.states), count)  # nothing changed, nothing sent
        self.window.value = {"title": "Browser", "app": "chrome.exe", "pid": 2}
        await eventually(lambda: len(self.panel.states) == count + 1)
        self.assertEqual(self.panel.states[-1]["window"]["title"], "Browser")
        self.window.value = None  # nothing focused: reported as null, once
        await eventually(lambda: len(self.panel.states) == count + 2)
        self.assertEqual(self.panel.states[-1], {"window": None})

    async def test_titles_are_clipped(self):
        self.window.value = {"title": "x" * 500, "app": "a.exe", "pid": 3}
        await self.start()
        await eventually(lambda: self.panel.states)
        self.assertEqual(len(self.panel.states[0]["window"]["title"]), 200)

    async def test_actions_and_results(self):
        await self.start()
        first = await self.panel.send_action("open_url", {"url": "https://example.com/x"})
        second = await self.panel.send_action("media", {"cmd": "next"})
        third = await self.panel.send_action("rm_rf", {})
        await eventually(lambda: len(self.panel.results) == 3)
        by_id = {r["id"]: r for r in self.panel.results}
        self.assertTrue(by_id[first]["ok"] and by_id[second]["ok"])
        self.assertFalse(by_id[third]["ok"])
        self.assertEqual(self.opened, ["https://example.com/x"])
        self.assertEqual(self.media.commands, ["next"])

    async def test_open_url_in_chrome_and_the_note_comes_back(self):
        await self.start()
        first = await self.panel.send_action("open_url", {"url": "https://example.com/meet", "browser": "chrome"})
        second = await self.panel.send_action("open_url", {"url": "https://example.com/nochrome", "browser": "chrome"})
        third = await self.panel.send_action("open_url", {"url": "https://example.com/x", "browser": "netscape"})
        await eventually(lambda: len(self.panel.results) == 3)
        by_id = {r["id"]: r for r in self.panel.results}
        self.assertEqual(self.browsers, ["chrome", "chrome"])
        self.assertTrue(by_id[first]["ok"] and by_id[first]["msg"] == "")
        self.assertTrue(by_id[second]["ok"] and "default browser" in by_id[second]["msg"])
        self.assertFalse(by_id[third]["ok"])  # an unknown browser is refused, nothing is opened

    async def test_monitor_state_is_reported_when_known_and_on_change(self):
        display = StubDisplay()
        self.link.display = display
        self.link.runner.display = display
        await self.start()
        await eventually(lambda: self.panel.states)
        self.assertNotIn("display", self.panel.states[0])  # not known yet: not reported
        self.assertIn("display", self.panel.hello["caps"])
        self.assertIn("wake_display", self.panel.hello["caps"])
        display.state = "off"
        await eventually(lambda: any(s.get("display") == "off" for s in self.panel.states))
        off = next(s for s in self.panel.states if s.get("display") == "off")
        self.assertEqual(off, {"display": "off"})  # only the key that changed
        display.state = "on"
        await eventually(lambda: any(s.get("display") == "on" for s in self.panel.states))

    async def test_call_state_is_reported_on_change(self):
        call = StubCall()
        self.link.call = call
        self.link.runner.call = call
        await self.start()
        self.assertIn("call", self.panel.hello["caps"])
        self.assertIn("mic", self.panel.hello["caps"])
        call.call = {"active": True, "app": "Meet", "muted": False}
        await eventually(lambda: any(s.get("call") for s in self.panel.states))
        call.call = {"active": True, "app": "Meet", "muted": True}
        await eventually(lambda: any((s.get("call") or {}).get("muted") for s in self.panel.states))
        call.call = None
        await eventually(lambda: any("call" in s and s["call"] is None for s in self.panel.states[1:]))

    async def test_mic_action(self):
        call = StubCall()
        self.link.call = call
        self.link.runner.call = call
        await self.start()
        action = await self.panel.send_action("mic", {"mode": "toggle"})
        await eventually(lambda: self.panel.results)
        self.assertTrue(self.panel.results[0]["ok"] and self.panel.results[0]["id"] == action)
        self.assertEqual(call.modes, ["toggle"])
        await self.panel.send_action("mic", {"mode": "format c:"})
        await eventually(lambda: len(self.panel.results) == 2)
        self.assertFalse(self.panel.results[1]["ok"])  # only the named modes

    async def test_wake_display_action(self):
        display = StubDisplay()
        self.link.display = display
        self.link.runner.display = display
        await self.start()
        action = await self.panel.send_action("wake_display")
        await eventually(lambda: self.panel.results)
        self.assertTrue(self.panel.results[0]["ok"] and self.panel.results[0]["id"] == action)
        self.assertEqual(display.wakes, 1)

    async def test_sleep_display_action(self):
        display = StubDisplay()
        self.link.display = display
        self.link.runner.display = display
        await self.start()
        self.assertIn("sleep_display", self.panel.hello["caps"])
        action = await self.panel.send_action("sleep_display")
        await eventually(lambda: self.panel.results)
        self.assertTrue(self.panel.results[0]["ok"] and self.panel.results[0]["id"] == action)
        self.assertEqual(display.sleeps, 1)

    async def test_sleep_pc_action(self):
        display = StubDisplay()
        self.link.display = display
        self.link.runner.display = display
        await self.start()
        self.assertIn("sleep_pc", self.panel.hello["caps"])
        action = await self.panel.send_action("sleep_pc")
        await eventually(lambda: self.panel.results)
        self.assertTrue(self.panel.results[0]["ok"] and self.panel.results[0]["id"] == action)
        self.assertEqual(display.suspends, 1)
        self.assertEqual(display.sleeps, 0)  # sleeping the PC is not the monitors-off action

    async def test_wake_display_without_a_display_provider(self):
        await self.start()  # no display provider on this PC
        await self.panel.send_action("wake_display")
        await eventually(lambda: self.panel.results)
        self.assertFalse(self.panel.results[0]["ok"])
        self.assertNotIn("wake_display", self.panel.hello["caps"])

    async def test_heartbeat_keeps_it_alive(self):
        await self.start()
        await eventually(lambda: self.panel.pings >= 2, timeout=3)

    async def test_wrong_token_is_refused(self):
        bad = Link("127.0.0.1", "wrong", "desk", port=self.panel.port, connect_timeout_s=1.0)
        with self.assertRaises(LinkError):
            await bad.run_once()
        self.assertEqual(self.panel.rejected, 1)

    async def test_reconnects_after_the_panel_restarts(self):
        await self.start()
        port = self.panel.port
        await self.panel.stop()
        await asyncio.sleep(0.2)
        self.panel = FakePanel("secret", heartbeat_s=0.3)
        await self.panel.start(port=port)
        self.link.connected.clear()
        await asyncio.wait_for(self.panel.connected.wait(), 6)  # run_forever retries (1 s delay)


if __name__ == "__main__":
    unittest.main()
