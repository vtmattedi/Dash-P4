"""python -m dashlink --host <panel ip> --token <pairing token>          run the link
python -m dashlink push-fw [--file firmware.bin] [--target p4|c6]   push a firmware build to the panel"""

from __future__ import annotations

import argparse
import asyncio
import logging
import os
import functools
import platform
import sys
from pathlib import Path

from . import __version__, protocol
from .actions import ActionRunner
from .browsers import open_in_browser
from .client import Link
from .control import DEFAULT_CONTROL_PORT, request_push, serve
from .firmware import TARGETS, FirmwareError, default_path, load_image
from .providers import create_call_provider, create_providers


def _common(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--host", required=True, help="the panel's IP address (shown in its Settings)")
    parser.add_argument("--port", type=int, default=protocol.DEFAULT_PORT)
    parser.add_argument("--token", default=os.environ.get("DASHLINK_TOKEN"), help="pairing token (or set DASHLINK_TOKEN)")
    parser.add_argument("--name", default=platform.node() or "pc", help="how this PC is named on the panel")
    parser.add_argument("--control-port", type=int, default=DEFAULT_CONTROL_PORT, help="localhost port push-fw uses to reach a running dashlink")
    parser.add_argument("-v", "--verbose", action="store_true")


def main() -> None:
    if len(sys.argv) > 1 and sys.argv[1] == "push-fw":
        sys.exit(push_main(sys.argv[2:]))

    parser = argparse.ArgumentParser(prog="dashlink", description="Link this PC to a Dash-P4 panel on the local network")
    _common(parser)
    parser.add_argument("--allow-host", action="append", default=[], metavar="DOMAIN",
                        help="only open links on this domain (and its subdomains); repeatable. Default: any http/https link")
    parser.add_argument("--no-linear-app", action="store_true",
                        help="open Linear links in the browser even when the Linear desktop app is installed")
    parser.add_argument("--version", action="version", version=f"dashlink {__version__}")
    args = parser.parse_args()
    if not args.token:
        parser.error("a pairing token is required (--token or DASHLINK_TOKEN)")

    logging.basicConfig(level=logging.DEBUG if args.verbose else logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    window, media, display = create_providers()
    call = create_call_provider()
    runner = ActionRunner(media=media, display=display, call=call, allow_hosts=args.allow_host,
                          opener=functools.partial(open_in_browser, linear_app=not args.no_linear_app))
    link = Link(args.host, args.token, args.name, window=window, media=media, display=display, call=call, runner=runner, port=args.port)

    async def run() -> None:
        server = await serve(link, args.control_port)
        try:
            await link.run_forever()
        finally:
            if server:
                server.close()

    try:
        asyncio.run(run())
    except KeyboardInterrupt:
        pass


def push_main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(prog="dashlink push-fw", description="Push a firmware build to the panel over the link; the panel restarts into it")
    _common(parser)
    parser.add_argument("--file", type=Path, default=None, help=f"the image (default: the target's PlatformIO build, e.g. {default_path()})")
    parser.add_argument("--target", choices=TARGETS, default="p4", help="p4 (the main firmware), c6 (the Wi-Fi co-processor) or dashboard (the handheld panel: open Settings > Update from PC first, and use its --host and token)")
    parser.add_argument("--force", action="store_true", help="push even when the panel already runs this build")
    args = parser.parse_args(argv)
    if not args.token:
        parser.error("a pairing token is required (--token or DASHLINK_TOKEN)")
    logging.basicConfig(level=logging.DEBUG if args.verbose else logging.WARNING, format="%(asctime)s %(levelname)s %(message)s")

    path = (args.file or default_path(target=args.target)).resolve()
    try:
        image = load_image(path, args.target)  # checked here too, so a wrong file fails before any connection
    except FirmwareError as error:
        print(f"error: {error}", file=sys.stderr)
        return 2
    print(f"{path}  ({image.size // 1024} KB, {image.target.upper()}, firmware {image.version}, sha256 {image.sha256[:12]}...)")

    last = [-10]

    def show(percent: int, text: str) -> None:
        if percent - last[0] >= 5 or percent >= 100 or percent == 0:
            last[0] = percent
            print(f"  {percent:3d}%  {text}", flush=True)

    result = asyncio.run(_push(args, path, image, show))
    print(("OK: " if result[0] else "FAILED: ") + result[1])
    return 0 if result[0] else 1


async def _push(args, path: Path, image, show) -> tuple[bool, str]:
    # A running dashlink holds the link to Dash-P4, so it can only push to Dash-P4 (p4, c6). The Dashboard is its own
    # panel with its own address and token: always a direct connection.
    delegated = None if args.target == "dashboard" else await request_push(args.control_port, str(path), args.target, args.force, show)
    if delegated is not None:
        return delegated  # a running dashlink did it over its own link
    link = Link(args.host, args.token, args.name, port=args.port)
    session = asyncio.create_task(link.run_forever())
    try:
        return True, await link.push_firmware(image, progress=show, force=args.force)
    except FirmwareError as error:
        return False, str(error)
    finally:
        session.cancel()
        await asyncio.gather(session, return_exceptions=True)


if __name__ == "__main__":
    main()
