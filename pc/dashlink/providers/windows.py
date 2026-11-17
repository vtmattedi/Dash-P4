"""Windows adapters, standard library only: the focused window through user32/kernel32 (ctypes) and media control
through the virtual media keys."""

from __future__ import annotations

import ctypes
import logging
import os
import threading
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


# ------------------------------------------------------------------------------------------------------- display

# The monitor's power state comes from Windows as a power-setting notification: GUID_CONSOLE_DISPLAY_STATE, value 0 off,
# 1 on, 2 dimmed. It needs a window to receive it, so a thread owns a message-only window and runs its message loop.
_WM_POWERBROADCAST = 0x0218
_PBT_POWERSETTINGCHANGE = 0x8013
_HWND_MESSAGE = wintypes.HWND(-3)
_DEVICE_NOTIFY_WINDOW_HANDLE = 0
_DISPLAY_STATES = {0: "off", 1: "on", 2: "dim"}

_LRESULT = ctypes.c_ssize_t
_WNDPROC = ctypes.WINFUNCTYPE(_LRESULT, wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM)


class _GUID(ctypes.Structure):
    _fields_ = [("Data1", wintypes.DWORD), ("Data2", wintypes.WORD), ("Data3", wintypes.WORD), ("Data4", ctypes.c_ubyte * 8)]


# {6FE69556-704A-47A0-8F24-C28D936FDA47}
_GUID_CONSOLE_DISPLAY_STATE = _GUID(0x6FE69556, 0x704A, 0x47A0, (ctypes.c_ubyte * 8)(0x8F, 0x24, 0xC2, 0x8D, 0x93, 0x6F, 0xDA, 0x47))


class _PowerBroadcastSetting(ctypes.Structure):
    _fields_ = [("PowerSetting", _GUID), ("DataLength", wintypes.DWORD), ("Data", ctypes.c_ubyte * 1)]


class _WndClass(ctypes.Structure):
    _fields_ = [
        ("style", wintypes.UINT), ("lpfnWndProc", _WNDPROC), ("cbClsExtra", ctypes.c_int), ("cbWndExtra", ctypes.c_int),
        ("hInstance", wintypes.HINSTANCE), ("hIcon", wintypes.HANDLE), ("hCursor", wintypes.HANDLE),
        ("hbrBackground", wintypes.HANDLE), ("lpszMenuName", wintypes.LPCWSTR), ("lpszClassName", wintypes.LPCWSTR),
    ]


_user32.DefWindowProcW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
_user32.DefWindowProcW.restype = _LRESULT
_user32.RegisterClassW.argtypes = [ctypes.POINTER(_WndClass)]
_user32.RegisterClassW.restype = wintypes.ATOM
_user32.CreateWindowExW.argtypes = [wintypes.DWORD, wintypes.LPCWSTR, wintypes.LPCWSTR, wintypes.DWORD, ctypes.c_int, ctypes.c_int,
                                    ctypes.c_int, ctypes.c_int, wintypes.HWND, wintypes.HMENU, wintypes.HINSTANCE, wintypes.LPVOID]
_user32.CreateWindowExW.restype = wintypes.HWND
_user32.RegisterPowerSettingNotification.argtypes = [wintypes.HANDLE, ctypes.POINTER(_GUID), wintypes.DWORD]
_user32.RegisterPowerSettingNotification.restype = wintypes.HANDLE
_user32.GetMessageW.argtypes = [ctypes.POINTER(wintypes.MSG), wintypes.HWND, wintypes.UINT, wintypes.UINT]
_user32.GetMessageW.restype = ctypes.c_int
_user32.DispatchMessageW.argtypes = [ctypes.POINTER(wintypes.MSG)]
_kernel32.GetModuleHandleW.argtypes = [wintypes.LPCWSTR]
_kernel32.GetModuleHandleW.restype = wintypes.HINSTANCE

_powrprof = ctypes.WinDLL("powrprof", use_last_error=True)
_powrprof.SetSuspendState.argtypes = [wintypes.BOOLEAN, wintypes.BOOLEAN, wintypes.BOOLEAN]
_powrprof.SetSuspendState.restype = wintypes.BOOLEAN

_MOUSEEVENTF_MOVE = 0x0001
_HWND_BROADCAST = wintypes.HWND(0xFFFF)
_WM_SYSCOMMAND = 0x0112
_SC_MONITORPOWER = 0xF170
_MONITOR_OFF = 2


class WindowsDisplayProvider:
    """The monitor's state ("on", "off" or "dim"; None until Windows has said) and a way to wake it.

    Only the monitor: sleep and hibernate are not reported, the link just goes away with the PC."""

    def __init__(self) -> None:
        self.state: str | None = None
        self._thread = threading.Thread(target=self._run, name="dashlink-display", daemon=True)
        self._thread.start()

    def _run(self) -> None:
        def on_message(hwnd, message, wparam, lparam):
            if message == _WM_POWERBROADCAST and wparam == _PBT_POWERSETTINGCHANGE and lparam:
                setting = ctypes.cast(lparam, ctypes.POINTER(_PowerBroadcastSetting)).contents
                if bytes(setting.PowerSetting) == bytes(_GUID_CONSOLE_DISPLAY_STATE):
                    value = _DISPLAY_STATES.get(setting.Data[0])
                    if value:
                        self.state = value
                        log.debug("monitor %s", value)
                return 1
            return _user32.DefWindowProcW(hwnd, message, wparam, lparam)

        proc = _WNDPROC(on_message)  # kept alive for the life of the thread
        instance = _kernel32.GetModuleHandleW(None)
        cls = _WndClass()
        cls.lpfnWndProc = proc
        cls.hInstance = instance
        cls.lpszClassName = "DashlinkDisplayWatcher"
        if not _user32.RegisterClassW(ctypes.byref(cls)):
            log.warning("monitor watcher: cannot register a window class (error %d)", ctypes.get_last_error())
            return
        hwnd = _user32.CreateWindowExW(0, cls.lpszClassName, "dashlink", 0, 0, 0, 0, 0, _HWND_MESSAGE, None, instance, None)
        if not hwnd:
            log.warning("monitor watcher: cannot create its window (error %d)", ctypes.get_last_error())
            return
        # Windows sends the current state right after this call.
        if not _user32.RegisterPowerSettingNotification(hwnd, ctypes.byref(_GUID_CONSOLE_DISPLAY_STATE), _DEVICE_NOTIFY_WINDOW_HANDLE):
            log.warning("monitor watcher: cannot register for display power (error %d)", ctypes.get_last_error())
            return
        message = wintypes.MSG()
        while _user32.GetMessageW(ctypes.byref(message), None, 0, 0) > 0:
            _user32.DispatchMessageW(ctypes.byref(message))

    async def wake(self) -> bool:
        """Turn a blanked monitor back on: a one-pixel mouse nudge there and back, which Windows treats as user input."""
        _user32.mouse_event(_MOUSEEVENTF_MOVE, 1, 0, 0, 0)
        _user32.mouse_event(_MOUSEEVENTF_MOVE, 0xFFFFFFFF, 0, 0, 0)  # dx = -1
        return True

    async def sleep(self) -> bool:
        """Turn the monitors off (what a screen-off utility does). Any mouse or key input turns them back on."""
        _user32.PostMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
        _user32.PostMessageW.restype = wintypes.BOOL
        return bool(_user32.PostMessageW(_HWND_BROADCAST, _WM_SYSCOMMAND, _SC_MONITORPOWER, _MONITOR_OFF))

    async def suspend(self, delay_s: float = 1.0) -> bool:
        """Put the PC to sleep (suspend, not hibernate), `delay_s` from now.

        Delayed and on its own thread: SetSuspendState only returns when the PC wakes up again, and the panel is owed
        the action's result before the link goes down with the PC."""
        def go() -> None:
            if not _powrprof.SetSuspendState(False, False, False):
                log.warning("could not put the PC to sleep (error %d)", ctypes.get_last_error())

        timer = threading.Timer(delay_s, go)
        timer.daemon = True
        timer.start()
        return True


def create_display_provider():
    return WindowsDisplayProvider()
