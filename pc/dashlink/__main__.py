"""python -m dashlink --host <panel ip> --token <pairing token>"""

from __future__ import annotations

import argparse
import asyncio
import logging
import os
import functools
import platform

from . import __version__, protocol
from .actions import ActionRunner
from .browsers import open_in_browser
from .client import Link
from .providers import create_providers


def main() -> None:
    parser = argparse.ArgumentParser(prog="dashlink", description="Link this PC to a Dash-P4 panel on the local network")
    parser.add_argument("--host", required=True, help="the panel's IP address (shown in its Settings)")
    parser.add_argument("--port", type=int, default=protocol.DEFAULT_PORT)
    parser.add_argument("--token", default=os.environ.get("DASHLINK_TOKEN"), help="pairing token (or set DASHLINK_TOKEN)")
    parser.add_argument("--name", default=platform.node() or "pc", help="how this PC is named on the panel")
    parser.add_argument("--allow-host", action="append", default=[], metavar="DOMAIN",
                        help="only open links on this domain (and its subdomains); repeatable. Default: any http/https link")
    parser.add_argument("--no-linear-app", action="store_true",
                        help="open Linear links in the browser even when the Linear desktop app is installed")
    parser.add_argument("-v", "--verbose", action="store_true")
    parser.add_argument("--version", action="version", version=f"dashlink {__version__}")
    args = parser.parse_args()
    if not args.token:
        parser.error("a pairing token is required (--token or DASHLINK_TOKEN)")

    logging.basicConfig(level=logging.DEBUG if args.verbose else logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    window, media = create_providers()
    runner = ActionRunner(media=media, allow_hosts=args.allow_host,
                          opener=functools.partial(open_in_browser, linear_app=not args.no_linear_app))
    link = Link(args.host, args.token, args.name, window=window, media=media, runner=runner, port=args.port)
    try:
        asyncio.run(link.run_forever())
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
