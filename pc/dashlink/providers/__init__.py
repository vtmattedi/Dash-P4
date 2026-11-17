"""Platform adapters. Each platform supplies a window provider and a media provider with the same small interface:

    window.get()            -> {"title": str, "app": str, "pid": int} or None     (async)
    media.control(command)  -> True if it was delivered   (command: see actions.py)
    display.state           -> "on", "off", "dim" or None (not known yet)    (a plain attribute, read often)
    display.wake()          -> True if it was done                            (async)
    display.sleep()         -> True if the monitors were told to turn off     (async)
    display.suspend()       -> True if the PC is about to go to sleep         (async)
    call.get()              -> {"active": True, "app": str, "muted": bool} while an app records the microphone, else None (async)
    call.mic(mode)          -> True if the default microphone was muted / unmuted / toggled (async)

v0 scope: Windows only, no now-playing metadata, no encryption. Linux/KDE (a KWin script over D-Bus, MPRIS) comes later;
until then a Linux run only links, pings and runs actions.
"""

from __future__ import annotations

import logging
import sys

log = logging.getLogger("dashlink")


def create_providers():
    """Return (window, media, display) providers; each may be None when the platform cannot supply it."""
    if sys.platform == "win32":
        from . import windows

        return windows.WindowsWindowProvider(), windows.create_media_provider(), windows.create_display_provider()
    log.warning("no window/media/display provider for %s yet: linking only", sys.platform)
    return None, None, None


def create_call_provider():
    """The call/microphone provider (call.get() -> {"active","app","muted"} or None; call.mic(mode)), or None."""
    if sys.platform == "win32":
        from . import windows_audio

        return windows_audio.create_call_provider()
    return None
