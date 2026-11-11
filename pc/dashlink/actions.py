"""What the panel is allowed to ask the PC to do. The panel sends an action NAME and arguments; the code that runs is
here, never a command from the wire."""

from __future__ import annotations

import time
import urllib.parse
import webbrowser
from collections import deque
from typing import Callable, Iterable

MAX_URL = 2048
MEDIA_COMMANDS = ("play_pause", "next", "prev", "stop")


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
    """Runs the named actions. `media` is a media provider (or None); `opener` and `clock` can be replaced in tests."""

    def __init__(
        self,
        media=None,
        allow_hosts: Iterable[str] | None = None,
        opener: Callable[[str], object] = webbrowser.open,
        clock: Callable[[], float] = time.monotonic,
        url_limit: tuple[int, float] = (5, 60.0),
    ) -> None:
        self.media = media
        self.allow_hosts = list(allow_hosts or [])
        self.opener = opener
        self.clock = clock
        self._url_count, self._url_window = url_limit
        self._url_times: deque[float] = deque()

    def capabilities(self) -> list[str]:
        caps = ["open_url"]
        if self.media is not None:
            caps.append("media")
        return caps

    async def run(self, name: object, args: object) -> tuple[bool, str]:
        args = args if isinstance(args, dict) else {}
        try:
            if name == "open_url":
                return self._open_url(args)
            if name == "media":
                return await self._media(args)
        except ValueError as error:
            return False, str(error)
        return False, "unknown action"

    def _open_url(self, args: dict) -> tuple[bool, str]:
        url = validate_url(args.get("url"), self.allow_hosts)
        now = self.clock()
        while self._url_times and now - self._url_times[0] > self._url_window:
            self._url_times.popleft()
        if len(self._url_times) >= self._url_count:
            return False, "too many links opened; slow down"
        self._url_times.append(now)
        self.opener(url)
        return True, ""

    async def _media(self, args: dict) -> tuple[bool, str]:
        command = args.get("cmd")
        if command not in MEDIA_COMMANDS:
            raise ValueError("unknown media command")
        if self.media is None:
            return False, "no media control on this PC"
        done = await self.media.control(command)
        return (True, "") if done else (False, "nothing is playing")
