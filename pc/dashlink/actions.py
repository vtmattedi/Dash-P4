"""What the panel is allowed to ask the PC to do. The panel sends an action NAME and arguments; the code that runs is
here, never a command from the wire."""

from __future__ import annotations

import time
import urllib.parse
from collections import deque
from typing import Callable, Iterable

from .browsers import BROWSERS, open_in_browser

MAX_URL = 2048
MEDIA_COMMANDS = ("play_pause", "next", "prev", "stop")
MIC_MODES = ("mute", "unmute", "toggle")


def validate_url(url: object, allow_hosts: Iterable[str] | None = None) -> str:
    """Return the URL if it is safe to hand to a browser, else raise ValueError."""
    if not isinstance(url, str) or not url:
        raise ValueError("url missing")
    if len(url) > MAX_URL:
        raise ValueError("url too long")
    if any(c.isspace() or ord(c) < 0x20 or ord(c) == 0x7F for c in url):
        raise ValueError("url has whitespace or control characters")
    parts = urllib.parse.urlsplit(url)
    if parts.scheme not in ("http", "https"):
        raise ValueError("only http and https links are opened")
    if not parts.hostname:
        raise ValueError("url has no host")
    allowed = [h.lower() for h in (allow_hosts or [])]
    if allowed:
        host = parts.hostname.lower()
        if not any(host == a or host.endswith("." + a) for a in allowed):
            raise ValueError("host is not on the allowlist")
    return url


class ActionRunner:
    """Runs the named actions. `media` and `display` are providers (or None); `opener` and `clock` can be replaced in tests."""

    def __init__(
        self,
        media=None,
        display=None,
        call=None,
        pc=None,
        launchers=None,
        recent=None,
        allow_hosts: Iterable[str] | None = None,
        opener: Callable[[str, str], object] = open_in_browser,
        clock: Callable[[], float] = time.monotonic,
        url_limit: tuple[int, float] = (5, 60.0),
    ) -> None:
        self.media = media
        self.display = display
        self.call = call
        self.pc = pc
        self.launchers = launchers
        self.recent = recent
        self.allow_hosts = list(allow_hosts or [])
        self.opener = opener
        self.clock = clock
        self._url_count, self._url_window = url_limit
        self._url_times: deque[float] = deque()

    def capabilities(self) -> list[str]:
        caps = ["open_url"]
        if self.media is not None:
            caps.append("media")
        if self.call is not None:
            caps.append("call")
            caps.append("mic")
        if self.pc is not None:
            caps += ["windows", "focus", "vitals"]
        if self.launchers is not None:
            caps.append("launch")
        if self.recent is not None:
            caps.append("recent")
        if self.display is not None:
            caps.append("wake_display")
            caps.append("sleep_display")
            caps.append("sleep_pc")
        return caps

    async def run(self, name: object, args: object) -> tuple[bool, str]:
        args = args if isinstance(args, dict) else {}
        try:
            if name == "open_url":
                return self._open_url(args)
            if name == "media":
                return await self._media(args)
            if name == "mic":
                return await self._mic(args)
            if name == "focus":
                return await self._focus(args)
            if name == "launch":
                return self._launch(args)
            if name == "open_recent":
                return self.recent.open(args.get("name")) if self.recent is not None else (False, "no VS Code list on this PC")
            if name == "wake_display":
                return await self._wake_display()
            if name == "sleep_display":
                return await self._sleep_display()
            if name == "sleep_pc":
                return await self._sleep_pc()
        except ValueError as error:
            return False, str(error)
        return False, "unknown action"

    def _open_url(self, args: dict) -> tuple[bool, str]:
        url = validate_url(args.get("url"), self.allow_hosts)
        browser = args.get("browser", "default")
        if browser not in BROWSERS:
            raise ValueError("unknown browser")
        now = self.clock()
        while self._url_times and now - self._url_times[0] > self._url_window:
            self._url_times.popleft()
        if len(self._url_times) >= self._url_count:
            return False, "too many links opened; slow down"
        self._url_times.append(now)
        note = self.opener(url, browser)
        return True, note if isinstance(note, str) else ""

    async def _media(self, args: dict) -> tuple[bool, str]:
        command = args.get("cmd")
        if command not in MEDIA_COMMANDS:
            raise ValueError("unknown media command")
        if self.media is None:
            return False, "no media control on this PC"
        done = await self.media.control(command)
        return (True, "") if done else (False, "nothing is playing")

    async def _mic(self, args: dict) -> tuple[bool, str]:
        mode = args.get("mode")
        if mode not in MIC_MODES:
            raise ValueError("unknown microphone mode")
        if self.call is None:
            return False, "no microphone control on this PC"
        done = await self.call.mic(mode)
        return (True, "") if done else (False, "could not change the microphone")

    async def _focus(self, args: dict) -> tuple[bool, str]:
        window_id = args.get("id")
        if not isinstance(window_id, int) or isinstance(window_id, bool):
            raise ValueError("window id missing")
        if self.pc is None:
            return False, "no window control on this PC"
        done = await self.pc.focus(window_id)
        return (True, "") if done else (False, "that window is gone")

    def _launch(self, args: dict) -> tuple[bool, str]:
        if self.launchers is None:
            return False, "no launchers on this PC"
        return self.launchers.launch(args.get("name"), args.get("item"))

    async def _wake_display(self) -> tuple[bool, str]:
        if self.display is None:
            return False, "no display control on this PC"
        done = await self.display.wake()
        return (True, "") if done else (False, "could not wake the monitor")

    async def _sleep_display(self) -> tuple[bool, str]:
        if self.display is None:
            return False, "no display control on this PC"
        done = await self.display.sleep()
        return (True, "") if done else (False, "could not turn the monitor off")

    async def _sleep_pc(self) -> tuple[bool, str]:
        if self.display is None:
            return False, "no power control on this PC"
        done = await self.display.suspend()
        return (True, "") if done else (False, "could not put the PC to sleep")
