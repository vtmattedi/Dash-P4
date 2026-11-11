"""The PC end of the link: connect to the panel, pair, keep the link alive, publish what the PC is doing, run the
actions the panel asks for. See ../PROTOCOL.md."""

from __future__ import annotations

import asyncio
import logging
import time

from . import __version__, protocol
from .actions import ActionRunner

log = logging.getLogger("dashlink")

MAX_TITLE = 200


def _clip(value: object) -> object:
    return value[:MAX_TITLE] if isinstance(value, str) else value


def _clip_fields(item: dict | None) -> dict | None:
    return None if item is None else {k: _clip(v) for k, v in item.items()}


class LinkError(Exception):
    """The session ended: refused, timed out, or the connection dropped."""


class Link:
    def __init__(
        self,
        host: str,
        token: str,
        name: str,
        window=None,
        media=None,  # control only (media keys); v0 does not report what is playing
        runner: ActionRunner | None = None,
        port: int = protocol.DEFAULT_PORT,
        window_poll_s: float = 0.5,
        connect_timeout_s: float = 5.0,
    ) -> None:
        self.host, self.port, self.token, self.name = host, port, token, name
        self.window, self.media = window, media
        self.runner = runner or ActionRunner(media=media)
        self.window_poll_s = window_poll_s
        self.connect_timeout_s = connect_timeout_s
        self.connected = asyncio.Event()  # set while a session is up (tests and the CLI use it)
        self.panel: dict = {}             # the panel's challenge: device name, firmware
        self.sessions = 0

    # ------------------------------------------------------------------ session

    async def run_forever(self) -> None:
        """Connect, and reconnect with a growing delay, until cancelled."""
        delay = 1.0
        while True:
            try:
                await self.run_once()
                delay = 1.0
            except LinkError as error:
                log.warning("link down: %s", error)
            except (OSError, asyncio.TimeoutError) as error:
                log.warning("cannot reach %s:%d: %s", self.host, self.port, error)
            self.connected.clear()
            await asyncio.sleep(delay)
            delay = min(delay * 2, 30.0)

    async def run_once(self) -> None:
        """One connection from connect to the end of the session. Raises LinkError (or OSError) when it ends."""
        reader, writer = await asyncio.wait_for(
            asyncio.open_connection(self.host, self.port), self.connect_timeout_s
        )
        write_lock = asyncio.Lock()

        async def send(kind: bytes, body: dict | None = None) -> None:
            async with write_lock:
                writer.write(protocol.encode(kind, body))
                await writer.drain()

        try:
            heartbeat = await self._pair(reader, send)
            self.sessions += 1
            self.connected.set()
            log.info("linked to %s (firmware %s)", self.panel.get("device"), self.panel.get("fw"))
            tasks = [
                asyncio.create_task(self._receive(reader, send, heartbeat)),
                asyncio.create_task(self._publish(send)),
                asyncio.create_task(self._keepalive(send, heartbeat)),
            ]
            try:
                done, _ = await asyncio.wait(tasks, return_when=asyncio.FIRST_EXCEPTION)
                for task in done:
                    error = task.exception()
                    if error is not None:
                        raise error  # whatever ended the session
            finally:
                # Also when this coroutine itself is cancelled: no task may outlive its session.
                for task in tasks:
                    task.cancel()
                await asyncio.gather(*tasks, return_exceptions=True)
        except (asyncio.IncompleteReadError, ConnectionError) as error:
            raise LinkError(f"connection closed ({error.__class__.__name__})") from error
        except protocol.ProtocolError as error:
            raise LinkError(f"protocol error: {error}") from error
        finally:
            self.connected.clear()
            writer.close()
            try:
                await writer.wait_closed()
            except OSError:
                pass

    async def _pair(self, reader, send) -> float:
        kind, challenge = await asyncio.wait_for(protocol.read_frame(reader), self.connect_timeout_s)
        if kind == protocol.ERROR:
            raise LinkError(f"refused: {challenge.get('msg', challenge.get('code', '?'))}")
        if kind != protocol.CHALLENGE or "nonce" not in challenge:
            raise LinkError("the panel did not start with a challenge")
        self.panel = challenge
        caps = (["window"] if self.window else []) + self.runner.capabilities()
        await send(
            protocol.HELLO,
            {
                "v": protocol.VERSION,
                "name": self.name,
                "os": _os_name(),
                "app": f"dashlink {__version__}",
                "caps": sorted(set(caps)),
                "auth": protocol.auth_proof(self.token, challenge["nonce"], self.name),
            },
        )
        kind, reply = await asyncio.wait_for(protocol.read_frame(reader), self.connect_timeout_s)
        if kind == protocol.ERROR:
            raise LinkError(f"refused: {reply.get('msg', reply.get('code', '?'))}")
        if kind != protocol.WELCOME:
            raise LinkError("the panel did not welcome us")
        return float(reply.get("heartbeat_s", 5))

    # ------------------------------------------------------------------ the three jobs of a session

    async def _receive(self, reader, send, heartbeat: float) -> None:
        """Answer pings and run actions. Ends the session when the panel is silent for 3 heartbeats."""
        while True:
            try:
                kind, body = await asyncio.wait_for(protocol.read_frame(reader), heartbeat * 3)
            except asyncio.TimeoutError as error:
                raise LinkError(f"the panel was silent for {heartbeat * 3:g} s") from error
            if kind == protocol.PING:
                await send(protocol.PONG, {"t": body.get("t", 0)})
            elif kind == protocol.ACTION:
                asyncio.create_task(self._action(send, body))
            elif kind == protocol.ERROR:
                raise LinkError(f"the panel reported: {body.get('msg', body.get('code', '?'))}")
            # PONG and anything unknown: nothing to do, hearing it was the point

    async def _action(self, send, body: dict) -> None:
        ok, message = await self.runner.run(body.get("name"), body.get("args"))
        log.info("action %s -> %s %s", body.get("name"), "ok" if ok else "failed", message)
        try:
            await send(protocol.RESULT, {"id": body.get("id"), "ok": ok, "msg": message})
        except (ConnectionError, OSError):
            pass  # the session is ending; the receive loop reports it

    async def _keepalive(self, send, heartbeat: float) -> None:
        while True:
            await asyncio.sleep(heartbeat)
            await send(protocol.PING, {"t": int(time.monotonic() * 1000)})

    async def _publish(self, send) -> None:
        """Send the focused window right away, then whenever it changes."""
        last: object = object()  # nothing sent yet: the first reading always goes out
        while True:
            if self.window is not None:
                current = _clip_fields(await self.window.get())
                if current != last:
                    last = current
                    await send(protocol.STATE, {"window": current})
            await asyncio.sleep(self.window_poll_s)


def _os_name() -> str:
    import sys

    return {"win32": "windows", "darwin": "macos"}.get(sys.platform, "linux" if sys.platform.startswith("linux") else sys.platform)
