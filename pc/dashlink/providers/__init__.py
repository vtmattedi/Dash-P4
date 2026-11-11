"""Platform adapters. Each platform supplies a window provider and a media provider with the same small interface:

    window.get()            -> {"title": str, "app": str, "pid": int} or None     (async)
    media.control(command)  -> True if it was delivered   (command: see actions.py)

v0 scope: Windows only, no now-playing metadata, no encryption. Linux/KDE (a KWin script over D-Bus, MPRIS) comes later;
until then a Linux run only links, pings and runs actions.
"""

from __future__ import annotations

import logging
import sys

log = logging.getLogger("dashlink")


def create_providers():
    """Return (window_provider, media_provider); either may be None when the platform cannot supply it."""
    if sys.platform == "win32":
        from . import windows

        return windows.WindowsWindowProvider(), windows.create_media_provider()
    log.warning("no window/media provider for %s yet: linking only", sys.platform)
    return None, None
