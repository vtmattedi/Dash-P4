"""A stand-in for the panel: the server side of PROTOCOL.md in about a screen of Python. It is the test double for the
PC app and the executable reference for the firmware (what to send, what to check). Run it by hand to try the PC app
without the board:

    python -m dashlink.fakepanel --token secret
    python -m dashlink --host 127.0.0.1 --token secret
"""

from __future__ import annotations

import argparse
import asyncio
import logging
import secrets

from . import protocol

log = logging.getLogger("fakepanel")


class FakePanel:
    def __init__(self, token: str, heartbeat_s: float = 5.0, device: str = "fake-panel", fw: str = "0.0-test") -> None:
        self.token, self.heartbeat_s, self.device, self.fw = token, heartbeat_s, device, fw
        self.states: list[dict] = []     # every STATE frame received, in order
        self.results: list[dict] = []    # every RESULT frame received
        self.hello: dict = {}
        self.pings = 0
        self.rejected = 0
        self.connected = asyncio.Event()
        self._server: asyncio.base_events.Server | None = None
        self._writer: asyncio.StreamWriter | None = None
        self._next_action = 1

    @property
    def port(self) -> int:
        return self._server.sockets[0].getsockname()[1]

    async def start(self, host: str = "127.0.0.1", port: int = 0) -> None:
        self._server = await asyncio.start_server(self._serve, host, port)

    async def stop(self) -> None:
        if self._writer:
            self._writer.close()
        if self._server:
            self._server.close()
            await self._server.wait_closed()

    async def send_action(self, name: str, args: dict | None = None) -> int:
        """Ask the PC to do something; the RESULT lands in `results`."""
        action_id = self._next_action
        self._next_action += 1
        assert self._writer is not None, "no PC is connected"
        self._writer.write(protocol.encode(protocol.ACTION, {"id": action_id, "name": name, "args": args or {}}))
        await self._writer.drain()
        return action_id

    async def _serve(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        nonce = secrets.token_hex(16)
        try:
            writer.write(protocol.encode(protocol.CHALLENGE, {"v": protocol.VERSION, "nonce": nonce, "device": self.device, "fw": self.fw}))
            kind, hello = await asyncio.wait_for(protocol.read_frame(reader), 5)
            if kind != protocol.HELLO or not protocol.verify_proof(self.token, nonce, str(hello.get("name", "")), hello.get("auth", "")):
                self.rejected += 1
                writer.write(protocol.encode(protocol.ERROR, {"code": "auth", "msg": "bad pairing token"}))
                await writer.drain()
                return
            self.hello = hello
            self._writer = writer
            writer.write(protocol.encode(protocol.WELCOME, {"heartbeat_s": self.heartbeat_s}))
            await writer.drain()
            self.connected.set()
            log.info("%s (%s) connected, caps %s", hello.get("name"), hello.get("os"), hello.get("caps"))
            while True:
                kind, body = await asyncio.wait_for(protocol.read_frame(reader), self.heartbeat_s * 3)
                if kind == protocol.STATE:
                    self.states.append(body)
                    log.info("state %s", body)
                elif kind == protocol.RESULT:
                    self.results.append(body)
                    log.info("result %s", body)
                elif kind == protocol.PING:
                    self.pings += 1
                    writer.write(protocol.encode(protocol.PONG, {"t": body.get("t", 0)}))
                    await writer.drain()
        except (asyncio.IncompleteReadError, ConnectionError, asyncio.TimeoutError, protocol.ProtocolError) as error:
            log.info("PC left: %s", error.__class__.__name__)
        finally:
            self.connected.clear()
            if self._writer is writer:
                self._writer = None
            writer.close()


async def _main(args: argparse.Namespace) -> None:
    panel = FakePanel(args.token, heartbeat_s=args.heartbeat)
    await panel.start(args.host, args.port)
    log.info("fake panel listening on %s:%d", args.host, panel.port)
    if args.send_url or args.send_media:
        await panel.connected.wait()
        await asyncio.sleep(1)
        if args.send_url:
            await panel.send_action("open_url", {"url": args.send_url})
        if args.send_media:
            await panel.send_action("media", {"cmd": args.send_media})
    await asyncio.Event().wait()


def main() -> None:
    parser = argparse.ArgumentParser(description="A fake Dash-P4 panel for trying the PC app")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=protocol.DEFAULT_PORT)
    parser.add_argument("--token", required=True)
    parser.add_argument("--heartbeat", type=float, default=5.0)
    parser.add_argument("--send-url", help="once a PC connects, ask it to open this URL")
    parser.add_argument("--send-media", choices=["play_pause", "next", "prev", "stop"], help="once a PC connects, send this media command")
    args = parser.parse_args()
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(name)s %(message)s")
    try:
        asyncio.run(_main(args))
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
