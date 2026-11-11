"""Windows adapters, standard library only: the focused window through user32/kernel32 (ctypes) and media control
through the virtual media keys."""

from __future__ import annotations

import ctypes
import logging
import os
from ctypes import wintypes

log = logging.getLogger("dashlink")

_user32 = ctypes.WinDLL("user32", use_last_error=True)
_kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)

_user32.GetForegroundWindow.restype = wintypes.HWND
_user32.GetWindowTextLengthW.argtypes = [wintypes.HWND]
_user32.GetWindowTextLengthW.restype = ctypes.c_int
_user32.GetWindowTextW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
_user32.GetWindowTextW.restype = ctypes.c_int
_user32.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
_user32.GetWindowThreadProcessId.restype = wintypes.DWORD
_kernel32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
_kernel32.OpenProcess.restype = wintypes.HANDLE
_kernel32.QueryFullProcessImageNameW.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.LPWSTR, ctypes.POINTER(wintypes.DWORD)]
_kernel32.QueryFullProcessImageNameW.restype = wintypes.BOOL
_kernel32.CloseHandle.argtypes = [wintypes.HANDLE]

PROCESS_QUERY_LIMITED_INFORMATION = 0x1000


def _process_name(pid: int) -> str:
    handle = _kernel32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
    if not handle:
        return ""
    try:
        size = wintypes.DWORD(1024)
        buffer = ctypes.create_unicode_buffer(size.value)
        if _kernel32.QueryFullProcessImageNameW(handle, 0, buffer, ctypes.byref(size)):
            return os.path.basename(buffer.value)
        return ""
    finally:
        _kernel32.CloseHandle(handle)


def foreground_window() -> dict | None:
    """The focused top-level window: title, executable name, process id. None when there is none (lock screen,
    desktop switch) or it has no title."""
    hwnd = _user32.GetForegroundWindow()
    if not hwnd:
        return None
    length = _user32.GetWindowTextLengthW(hwnd)
    buffer = ctypes.create_unicode_buffer(length + 1)
    _user32.GetWindowTextW(hwnd, buffer, length + 1)
    pid = wintypes.DWORD(0)
    _user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
    title = buffer.value
    app = _process_name(pid.value)
    if not title and not app:
        return None
    return {"title": title, "app": app, "pid": int(pid.value)}


class WindowsWindowProvider:
    async def get(self) -> dict | None:
        return foreground_window()


# --------------------------------------------------------------------------------------------------------- media

_VK = {"play_pause": 0xB3, "next": 0xB0, "prev": 0xB1, "stop": 0xB2}
_KEYEVENTF_KEYUP = 0x0002


def send_media_key(command: str) -> bool:
    """Press the virtual media key. Works for whichever app Windows routes media keys to; no feedback."""
    code = _VK.get(command)
    if code is None:
        return False
    _user32.keybd_event(code, 0, 0, 0)
    _user32.keybd_event(code, 0, _KEYEVENTF_KEYUP, 0)
    return True


class MediaKeysOnly:
    """Control without metadata."""

    async def get(self) -> dict | None:
        return None

    async def control(self, command: str) -> bool:
        return send_media_key(command)


def create_media_provider():
    """Control through the media keys. (What is playing is not reported in v0.)"""
    return MediaKeysOnly()
