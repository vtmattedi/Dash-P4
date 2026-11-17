"""The microphone and "am I in a call" on Windows, standard library only (ctypes over the Core Audio COM interfaces).

- mute state / mute / unmute / toggle: the default *communications* capture device's endpoint mute flag. It is the one
  signal every call app shares (Meet in a browser, Discord, Teams, Slack huddles): muting it silences the microphone for all
  of them, and it can be read back, which no app but Teams and Discord offers for its own mute button.
- in a call: some process has an *active* capture session on that device, i.e. it is recording from the microphone.
  The process name becomes the label (Discord, Teams, Slack; a browser is "Meet" when the focused window says so).

Every function initialises COM for the calling thread, so they can run on any worker thread (asyncio.to_thread)."""

from __future__ import annotations

import ctypes
import logging
import os
import threading
from ctypes import POINTER, byref, c_int, c_long, c_uint, c_void_p, wintypes

from . import windows

log = logging.getLogger("dashlink")

_ole32 = ctypes.WinDLL("ole32")
_ole32.CoInitializeEx.argtypes = [c_void_p, wintypes.DWORD]
_ole32.CoInitializeEx.restype = c_long
_ole32.CoCreateInstance.argtypes = [c_void_p, c_void_p, wintypes.DWORD, c_void_p, POINTER(c_void_p)]
_ole32.CoCreateInstance.restype = c_long
_ole32.CoUninitialize.argtypes = []

COINIT_MULTITHREADED = 0
CLSCTX_ALL = 23
E_CAPTURE = 1
ROLE_COMMUNICATIONS = 2
SESSION_ACTIVE = 1


class _GUID(ctypes.Structure):
    _fields_ = [("a", ctypes.c_uint32), ("b", ctypes.c_uint16), ("c", ctypes.c_uint16), ("d", ctypes.c_ubyte * 8)]


def _guid(text: str) -> _GUID:
    import uuid

    u = uuid.UUID(text)
    g = _GUID()
    g.a, g.b, g.c = u.time_low, u.time_mid, u.time_hi_version
    for i, byte in enumerate(u.bytes[8:]):
        g.d[i] = byte
    return g


CLSID_MMDeviceEnumerator = _guid("BCDE0395-E52F-467C-8E3D-C4579291692E")
IID_IMMDeviceEnumerator = _guid("A95664D2-9614-4F35-A746-DE8DB63617E6")
IID_IAudioEndpointVolume = _guid("5CDF2C82-841E-4546-9722-0CF74078229A")
IID_IAudioSessionManager2 = _guid("77AA99A0-1BD6-484F-8BC7-2C654C9A9B6F")
IID_IAudioSessionControl2 = _guid("BFB7FF88-7239-4FC9-8FA2-07C950BE9C6D")


class _Com:
    """One COM object: call vtable slots by index; released when dropped."""

    def __init__(self, pointer: c_void_p) -> None:
        self.p = pointer

    def _slot(self, index: int, restype, *argtypes):
        vtable = ctypes.cast(self.p, POINTER(POINTER(c_void_p))).contents
        return ctypes.WINFUNCTYPE(restype, c_void_p, *argtypes)(vtable[index])

    def call(self, index: int, *argtypes_and_args):
        argtypes = [t for t, _ in argtypes_and_args]
        args = [a for _, a in argtypes_and_args]
        result = self._slot(index, c_long, *argtypes)(self.p, *args)
        if result < 0:
            raise OSError(f"COM call {index} failed: 0x{result & 0xFFFFFFFF:08X}")

    def query(self, iid: _GUID) -> "_Com":
        out = c_void_p()
        self.call(0, (c_void_p, byref(iid)), (POINTER(c_void_p), byref(out)))
        return _Com(out)

    def __del__(self) -> None:
        try:
            if self.p:
                self._slot(2, wintypes.ULONG)(self.p)  # Release
        except Exception:
            pass


_thread = threading.local()


class _ComThread:
    """COM is initialised once per worker thread and stays so: the COM objects of a call are released by the garbage
    collector, possibly after the call returned, and releasing one after CoUninitialize is an access violation."""

    def __enter__(self):
        if not getattr(_thread, "ready", False):
            _ole32.CoInitializeEx(None, COINIT_MULTITHREADED)
            _thread.ready = True
        return self

    def __exit__(self, *exc) -> None:
        pass


def _capture_device() -> _Com:
    enumerator = c_void_p()
    hr = _ole32.CoCreateInstance(byref(CLSID_MMDeviceEnumerator), None, CLSCTX_ALL, byref(IID_IMMDeviceEnumerator), byref(enumerator))
    if hr < 0:
        raise OSError(f"no audio device enumerator: 0x{hr & 0xFFFFFFFF:08X}")
    enum = _Com(enumerator)
    device = c_void_p()
    enum.call(4, (c_int, E_CAPTURE), (c_int, ROLE_COMMUNICATIONS), (POINTER(c_void_p), byref(device)))  # GetDefaultAudioEndpoint
    return _Com(device)


def _activate(device: _Com, iid: _GUID) -> _Com:
    out = c_void_p()
    device.call(3, (c_void_p, byref(iid)), (wintypes.DWORD, CLSCTX_ALL), (c_void_p, None), (POINTER(c_void_p), byref(out)))  # Activate
    return _Com(out)


def mic_muted() -> bool | None:
    """True / False, or None when there is no capture device."""
    try:
        with _ComThread():
            volume = _activate(_capture_device(), IID_IAudioEndpointVolume)
            muted = wintypes.BOOL()
            volume.call(15, (POINTER(wintypes.BOOL), byref(muted)))  # GetMute
            return bool(muted.value)
    except OSError as error:
        log.debug("microphone state: %s", error)
        return None


def set_mic_muted(muted: bool) -> bool:
    try:
        with _ComThread():
            volume = _activate(_capture_device(), IID_IAudioEndpointVolume)
            volume.call(14, (wintypes.BOOL, int(muted)), (c_void_p, None))  # SetMute
            return True
    except OSError as error:
        log.warning("cannot set the microphone mute: %s", error)
        return False


def capture_processes() -> list[int]:
    """Process ids with an active capture session on the communications microphone."""
    pids: list[int] = []
    try:
        with _ComThread():
            manager = _activate(_capture_device(), IID_IAudioSessionManager2)
            sessions = c_void_p()
            manager.call(5, (POINTER(c_void_p), byref(sessions)))  # GetSessionEnumerator
            enum = _Com(sessions)
            count = c_int()
            enum.call(3, (POINTER(c_int), byref(count)))  # GetCount
            for i in range(count.value):
                control = c_void_p()
                enum.call(4, (c_int, i), (POINTER(c_void_p), byref(control)))  # GetSession
                session = _Com(control)
                state = c_int()
                session.call(3, (POINTER(c_int), byref(state)))  # GetState
                if state.value != SESSION_ACTIVE:
                    continue
                pid = wintypes.DWORD()
                session.query(IID_IAudioSessionControl2).call(14, (POINTER(wintypes.DWORD), byref(pid)))  # GetProcessId
                if pid.value:
                    pids.append(int(pid.value))
    except OSError as error:
        log.debug("capture sessions: %s", error)
    return pids


# Executable -> label. A browser is labelled from the focused window (Meet runs in a tab).
_APPS = {
    "discord.exe": "Discord",
    "ms-teams.exe": "Teams",
    "teams.exe": "Teams",
    "slack.exe": "Slack",
    "zoom.exe": "Zoom",
}
_BROWSERS = {"chrome.exe", "msedge.exe", "firefox.exe", "brave.exe", "opera.exe", "vivaldi.exe"}
# Things that hold the microphone open all day without being a call.
_IGNORED = {"audiodg.exe", "nvbroadcast.container.exe", "voicemeeter.exe", "voicemeeterpro.exe", "systemsettings.exe"}


def label_for(exe: str, focused_title: str = "") -> str:
    name = exe.lower()
    if name in _APPS:
        return _APPS[name]
    if name in _BROWSERS:
        return "Meet" if "meet" in focused_title.lower() else "Browser"
    return os.path.splitext(exe)[0] or "App"


def current_call() -> dict | None:
    """{"active": True, "app": label, "muted": bool} while an app is recording from the microphone, else None."""
    pids = capture_processes()
    apps = [(pid, windows._process_name(pid)) for pid in pids]
    apps = [(pid, exe) for pid, exe in apps if exe and exe.lower() not in _IGNORED]
    if not apps:
        return None
    focus = windows.foreground_window() or {}
    # Prefer the focused app, then a known call app, then whatever else.
    def rank(item):
        pid, exe = item
        return (0 if pid == focus.get("pid") else 1, 0 if exe.lower() in _APPS else 1)

    pid, exe = sorted(apps, key=rank)[0]
    return {"active": True, "app": label_for(exe, focus.get("title", "")), "muted": bool(mic_muted())}


class WindowsCallProvider:
    """call.get() -> the call state or None (async); call.mic(mode) -> True if the microphone was changed (async)."""

    async def get(self) -> dict | None:
        import asyncio

        return await asyncio.to_thread(current_call)

    async def mic(self, mode: str) -> bool:
        import asyncio

        def run() -> bool:
            current = mic_muted()
            if current is None:
                return False
            target = {"mute": True, "unmute": False, "toggle": not current}[mode]
            return set_mic_muted(target)

        return await asyncio.to_thread(run)


def create_call_provider():
    return WindowsCallProvider()
