"""A tiny local control socket, so `python -m dashlink push-fw` can use the link a running dashlink already holds (the
panel takes one PC at a time). Localhost only; one JSON line in, JSON lines out:

    -> {"cmd":"push","path":"...firmware.bin","target":"p4","force":false}
    <- {"progress":42,"text":"sending"}   (any number)
    <- {"ok":true,"msg":"updated: ..."}   (the last line)
"""

from __future__ import annotations

import asyncio
import json
import logging

from .firmware import FirmwareError, load_image

log = logging.getLogger("dashlink")

DEFAULT_CONTROL_PORT = 7341
LOCALHOST = "127.0.0.1"


async def serve(link, port: int = DEFAULT_CONTROL_PORT):
    """Start the control socket for `link`. Returns the server, or None when the port is taken (logged, not fatal)."""
    busy = asyncio.Lock()

    async def handle(reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        async def reply(body: dict) -> None:
            writer.write(json.dumps(body).encode("utf-8") + b"\n")
            await writer.drain()

        try:
            request = json.loads(await asyncio.wait_for(reader.readline(), 5))
            if request.get("cmd") != "push":
                await reply({"ok": False, "msg": "unknown command"})
                return
            if busy.locked():
                await reply({"ok": False, "msg": "an update is already running"})
                return
            async with busy:
                async def progress(percent: int, text: str) -> None:
                    await reply({"progress": percent, "text": text})

                loop = asyncio.get_running_loop()

                def report(percent: int, text: str) -> None:
                    loop.create_task(progress(percent, text))

                image = load_image(request.get("path", ""), request.get("target", "p4"))
                message = await link.push_firmware(image, progress=report, force=bool(request.get("force")))
                await reply({"ok": True, "msg": message})
        except FirmwareError as error:
            await _quiet(reply, {"ok": False, "msg": str(error)})
        except (ValueError, asyncio.TimeoutError, ConnectionError, OSError):
            pass  # a bad or vanished client: nothing to tell it
        finally:
            writer.close()

    try:
        return await asyncio.start_server(handle, LOCALHOST, port)
    except OSError as error:
        log.warning("control socket on %s:%d not available (%s): push-fw will use its own link", LOCALHOST, port, error)
        return None


async def _quiet(reply, body: dict) -> None:
    try:
        await reply(body)
    except (ConnectionError, OSError):
        pass


async def request_push(port: int, path: str, target: str, force: bool, on_progress):
    """Ask a running dashlink to push. Returns (ok, message), or None when no dashlink is listening."""
    try:
        reader, writer = await asyncio.wait_for(asyncio.open_connection(LOCALHOST, port), 1.0)
    except (OSError, asyncio.TimeoutError):
        return None
    try:
        writer.write(json.dumps({"cmd": "push", "path": path, "target": target, "force": force}).encode("utf-8") + b"\n")
        await writer.drain()
        while True:
            line = await reader.readline()
            if not line:
                return False, "dashlink closed the control connection"
            message = json.loads(line)
            if "progress" in message:
                on_progress(message["progress"], message.get("text", ""))
            else:
                return bool(message.get("ok")), str(message.get("msg", ""))
    finally:
        writer.close()
