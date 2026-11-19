"""The PC end of the link: connect to the panel, pair, keep the link alive, publish what the PC is doing, run the
actions the panel asks for. See ../PROTOCOL.md."""

from __future__ import annotations

import asyncio
import logging
import time

from . import __version__, protocol
from .actions import ActionRunner
from .firmware import ELF_TARGETS, FirmwareError, Image

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
        media=None,  # control (media keys); .get(), when it has one, is reported as state `media` (what is playing)
        call=None,  # microphone / call: .get() is reported as state `call`, .mic(mode) is an action
        display=None,  # the monitor: .state ("on"/"off"/"dim"/None) is reported, .wake() is an action
        pc=None,       # open windows, unread counts and vitals: .snapshot() is reported, .focus(id) is an action
        recent=None,  # recent.RecentFolders: .entries() is reported
        launchers=None,  # names the panel may start (launchers.Launchers): .names() is reported
        runner: ActionRunner | None = None,
        port: int = protocol.DEFAULT_PORT,
        window_poll_s: float = 0.5,
        connect_timeout_s: float = 5.0,
    ) -> None:
        self.host, self.port, self.token, self.name = host, port, token, name
        self.window, self.media, self.display, self.call = window, media, display, call
        self.pc, self.launchers, self.recent = pc, launchers, recent
        self.runner = runner or ActionRunner(media=media, display=display, call=call, pc=pc, launchers=launchers, recent=recent)
        self.window_poll_s = window_poll_s
        self.connect_timeout_s = connect_timeout_s
        self.connected = asyncio.Event()  # set while a session is up (tests and the CLI use it)
        self.panel: dict = {}             # the panel's challenge: device name, firmware, and `elf` (the build's identity)
        self.sessions = 0
        self._send_raw = None             # writes one encoded frame on the current session; None between sessions
        self._call_now = False             # a mic action just ran: read the call state again at once
        self._status: asyncio.Queue = asyncio.Queue()  # the panel's STATUS frames (firmware push progress)

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

        async def send_raw(frame: bytes) -> None:
            async with write_lock:
                writer.write(frame)
                await writer.drain()

        async def send(kind: bytes, body: dict | None = None) -> None:
            await send_raw(protocol.encode(kind, body))

        try:
            heartbeat = await self._pair(reader, send)
            self.sessions += 1
            self._send_raw = send_raw
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
            self._send_raw = None
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
        caps = (["window"] if self.window else []) + (["display"] if self.display else []) + (["call"] if self.call else []) + (["media_info"] if getattr(self.media, "get", None) else []) + (["badges"] if self.pc else []) + self.runner.capabilities()
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
            elif kind == protocol.STATUS:
                self._status.put_nowait(body)
            elif kind == protocol.ERROR:
                raise LinkError(f"the panel reported: {body.get('msg', body.get('code', '?'))}")
            # PONG and anything unknown: nothing to do, hearing it was the point

    async def _action(self, send, body: dict) -> None:
        ok, message = await self.runner.run(body.get("name"), body.get("args"))
        log.info("action %s -> %s %s", body.get("name"), "ok" if ok else "failed", message)
        if body.get("name") == "mic":
            self._call_now = True
        try:
            await send(protocol.RESULT, {"id": body.get("id"), "ok": ok, "msg": message})
        except (ConnectionError, OSError):
            pass  # the session is ending; the receive loop reports it

    # ------------------------------------------------------------------ firmware push

    async def push_firmware(self, image: Image, progress=None, force: bool = False, restart_timeout_s: float = 120.0) -> str:
        """Stream `image` to the panel over this link, wait for it to restart, and check it came back running this build.

        `progress(percent, text)` is called as it goes. Returns a one-line outcome; raises FirmwareError when the update
        did not happen or did not stick (refused, link lost, rolled back)."""
        note = progress or (lambda percent, text: None)
        try:
            await asyncio.wait_for(self.connected.wait(), self.connect_timeout_s * 3)
        except asyncio.TimeoutError as error:
            raise FirmwareError("the panel is not linked: check --host and --token (the Dashboard also needs update mode open: Settings > Update from PC)") from error
        if image.target in ELF_TARGETS and image.elf_sha256 and not force and self.panel.get("elf") == image.elf_sha256:
            return "the panel already runs this build (use --force to push it anyway)"
        send_raw, sessions_before = self._send_raw, self.sessions
        while not self._status.empty():
            self._status.get_nowait()

        async def next_status(timeout: float) -> dict:
            try:
                return await asyncio.wait_for(self._status.get(), timeout)
            except asyncio.TimeoutError as error:
                raise FirmwareError("the panel stopped answering during the update") from error

        try:
            await send_raw(protocol.encode(protocol.UPDATE, {"target": image.target, "size": image.size, "sha256": image.sha256}))
            reply = await next_status(15)
            if reply.get("state") != "ready":
                raise FirmwareError(f"the panel refused: {reply.get('msg', '?')}")
            note(0, f"sending {image.size // 1024} KB to the {image.target.upper()}")
            sent = 0
            while sent < image.size:
                chunk = image.data[sent : sent + 8192]
                await send_raw(protocol.encode_data(chunk))
                sent += len(chunk)
                note(sent * 100 // image.size, "sending")
                if not self._status.empty():
                    status = self._status.get_nowait()
                    if status.get("state") == "failed":
                        raise FirmwareError(f"the panel stopped the update: {status.get('msg', '?')}")
            note(100, "checking the image on the panel")
            while True:
                status = await next_status(90)
                if status.get("state") == "done":
                    break
                if status.get("state") == "failed":
                    raise FirmwareError(f"the panel rejected the image: {status.get('msg', '?')}")
        except (ConnectionError, OSError) as error:
            raise FirmwareError(f"the link dropped during the update ({error.__class__.__name__}); the panel keeps its old firmware") from error

        note(100, "written; the panel is restarting")
        deadline = asyncio.get_running_loop().time() + restart_timeout_s
        while not (self.sessions > sessions_before and self.connected.is_set()):
            if asyncio.get_running_loop().time() > deadline:
                raise FirmwareError(f"the panel did not come back within {restart_timeout_s:g} s")
            await asyncio.sleep(0.25)
        if image.target in ELF_TARGETS and image.elf_sha256 and self.panel.get("elf") != image.elf_sha256:
            raise FirmwareError("the panel is back but runs another build: the new image did not start and it rolled back")
        return f"updated: the panel runs firmware {self.panel.get('fw', '?')} again"

    async def _keepalive(self, send, heartbeat: float) -> None:
        while True:
            await asyncio.sleep(heartbeat)
            await send(protocol.PING, {"t": int(time.monotonic() * 1000)})

    async def _publish(self, send) -> None:
        """Send the focused window and the monitor state right away, then each when it changes (only the keys that did)."""
        last_window: object = object()  # nothing sent yet: the first reading always goes out
        last_display: object = object()
        last_call: object = object()
        last_media: object = object()
        last_launchers: object = object()
        last_recent: object = object()
        last_pc: dict = {}
        tick = 0
        while True:
            changed: dict = {}
            if self.window is not None:
                current = _clip_fields(await self.window.get())
                if current != last_window:
                    last_window = current
                    changed["window"] = current
            if self.display is not None:
                shown = self.display.state
                if shown is not None and shown != last_display:
                    last_display = shown
                    changed["display"] = shown
            if self.call is not None and (tick % 2 == 0 or self._call_now):  # the audio API is heavier than the window: about once a second
                self._call_now = False
                current = await self.call.get()
                if current != last_call:
                    last_call = current
                    changed["call"] = current
            if getattr(self.media, "get", None) is not None:
                current = await self.media.get()
                if current != last_media:
                    last_media = current
                    changed["media"] = current
            if self.pc is not None and tick % 4 == 0:  # windows and vitals: every two seconds, and only what changed
                for key, value in (await self.pc.snapshot()).items():
                    if last_pc.get(key, object()) != value:
                        last_pc[key] = value
                        changed[key] = value
            if self.launchers is not None and tick % 4 == 0:
                deck = self.launchers.deck()
                if deck != last_launchers:
                    last_launchers = deck
                    changed["launchers"] = deck
            if self.recent is not None and tick % 4 == 0:
                recent = self.recent.entries()
                if recent != last_recent:
                    last_recent = recent
                    changed["recent"] = recent
            tick += 1
            if changed:
                await send(protocol.STATE, changed)
            await asyncio.sleep(self.window_poll_s)


def _os_name() -> str:
    import sys

    return {"win32": "windows", "darwin": "macos"}.get(sys.platform, "linux" if sys.platform.startswith("linux") else sys.platform)
