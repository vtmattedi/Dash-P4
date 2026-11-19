"""What the PC is doing, for the panel's PC page: the open windows (to switch to one), notification counts read off their
titles, and vitals (CPU, memory, battery, disk, NVIDIA GPU when there is one). Standard library only (ctypes).

Every function is safe to call from a worker thread (asyncio.to_thread)."""

from __future__ import annotations

import ctypes
import json
import logging
import re
import shutil
import subprocess
import threading
import time
from ctypes import wintypes

from . import windows

log = logging.getLogger("dashlink")

_user32 = windows._user32
_kernel32 = windows._kernel32
_dwmapi = ctypes.WinDLL("dwmapi")

_WNDENUMPROC = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
_user32.EnumWindows.argtypes = [_WNDENUMPROC, wintypes.LPARAM]
_user32.EnumWindows.restype = wintypes.BOOL
_user32.IsWindowVisible.argtypes = [wintypes.HWND]
_user32.IsWindowVisible.restype = wintypes.BOOL
_user32.IsIconic.argtypes = [wintypes.HWND]
_user32.IsIconic.restype = wintypes.BOOL
_user32.IsWindow.argtypes = [wintypes.HWND]
_user32.IsWindow.restype = wintypes.BOOL
_user32.ShowWindow.argtypes = [wintypes.HWND, ctypes.c_int]
_user32.ShowWindow.restype = wintypes.BOOL
_user32.SetForegroundWindow.argtypes = [wintypes.HWND]
_user32.SetForegroundWindow.restype = wintypes.BOOL
_user32.BringWindowToTop.argtypes = [wintypes.HWND]
_user32.GetWindow.argtypes = [wintypes.HWND, wintypes.UINT]
_user32.GetWindow.restype = wintypes.HWND
_user32.GetWindowLongW.argtypes = [wintypes.HWND, ctypes.c_int]
_user32.GetWindowLongW.restype = ctypes.c_long
_user32.GetClassNameW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
_user32.GetClassNameW.restype = ctypes.c_int
_dwmapi.DwmGetWindowAttribute.argtypes = [wintypes.HWND, wintypes.DWORD, ctypes.c_void_p, wintypes.DWORD]
_dwmapi.DwmGetWindowAttribute.restype = ctypes.c_long

_GW_OWNER = 4
_GWL_EXSTYLE = -20
_WS_EX_TOOLWINDOW = 0x80
_WS_EX_NOACTIVATE = 0x08000000
_DWMWA_CLOAKED = 14
_SW_RESTORE = 9
_VK_MENU = 0x12
_KEYEVENTF_KEYUP = 0x0002

MAX_WINDOWS = 16
EDITORS = {"VS Code", "Cursor"}  # listed first: switching between editor windows is what the page is for
MAX_TITLE = 80

# Executable -> the name the panel shows.
_LABELS = {
    "code.exe": "VS Code",
    "code - insiders.exe": "VS Code",
    "cursor.exe": "Cursor",
    "chrome.exe": "Chrome",
    "msedge.exe": "Edge",
    "firefox.exe": "Firefox",
    "brave.exe": "Brave",
    "slack.exe": "Slack",
    "discord.exe": "Discord",
    "ms-teams.exe": "Teams",
    "teams.exe": "Teams",
    "outlook.exe": "Outlook",
    "olk.exe": "Outlook",
    "spotify.exe": "Spotify",
    "windowsterminal.exe": "Terminal",
    "explorer.exe": "Explorer",
    "linear.exe": "Linear",
    "notion.exe": "Notion",
    "zoom.exe": "Zoom",
    "whatsapp.exe": "WhatsApp",
    "telegram.exe": "Telegram",
}
_SKIP_CLASSES = {"Progman", "WorkerW", "Shell_TrayWnd", "Windows.UI.Core.CoreWindow"}
_SKIP_TITLES = {"Program Manager", "Settings", "Microsoft Text Input Application", "NVIDIA GeForce Overlay"}

_names: dict[int, str] = {}  # pid -> executable name (a pid is reused rarely and the label only changes then)


def _exe(pid: int) -> str:
    name = _names.get(pid)
    if name is None:
        name = windows._process_name(pid)
        if name:
            _names[pid] = name
        if len(_names) > 512:
            _names.clear()
    return name


def label_for(exe: str) -> str:
    key = exe.lower()
    if key in _LABELS:
        return _LABELS[key]
    stem = exe[:-4] if key.endswith(".exe") else exe
    return stem[:1].upper() + stem[1:] if stem else "App"


def workspace(label: str, title: str) -> str:
    """What identifies a window within its app. VS Code titles read 'file - folder - Visual Studio Code' (or just
    'folder - Visual Studio Code' with nothing open): the folder is the second to last part."""
    parts = [p.strip() for p in title.split(" - ")]
    if label in ("VS Code", "Cursor") and len(parts) >= 2:
        return parts[-2] if len(parts) >= 3 else parts[0]
    return title


def _cloaked(hwnd) -> bool:
    value = wintypes.DWORD(0)
    if _dwmapi.DwmGetWindowAttribute(hwnd, _DWMWA_CLOAKED, ctypes.byref(value), ctypes.sizeof(value)) == 0:
        return value.value != 0
    return False


def list_windows() -> list[dict]:
    """Top-level windows a person would call 'open': visible, titled, not a tool window, not cloaked (virtual-desktop
    ghosts, background UWP shells). {"id": hwnd, "app": label, "title": text}, the focused one first."""
    found: list[dict] = []
    focus = _user32.GetForegroundWindow()
    own = ctypes.windll.kernel32.GetCurrentProcessId()

    @_WNDENUMPROC
    def visit(hwnd, _):
        try:
            if not _user32.IsWindowVisible(hwnd) or _user32.GetWindow(hwnd, _GW_OWNER):
                return True
            ex = _user32.GetWindowLongW(hwnd, _GWL_EXSTYLE)
            if ex & (_WS_EX_TOOLWINDOW | _WS_EX_NOACTIVATE) or _cloaked(hwnd):
                return True
            length = _user32.GetWindowTextLengthW(hwnd)
            if length <= 0:
                return True
            buffer = ctypes.create_unicode_buffer(length + 1)
            _user32.GetWindowTextW(hwnd, buffer, length + 1)
            title = buffer.value
            cls = ctypes.create_unicode_buffer(64)
            _user32.GetClassNameW(hwnd, cls, 64)
            if cls.value in _SKIP_CLASSES or title in _SKIP_TITLES:
                return True
            pid = wintypes.DWORD(0)
            _user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
            if pid.value == own:
                return True
            exe = _exe(pid.value)
            if not exe:
                return True
            label = label_for(exe)
            found.append({"id": int(hwnd), "app": label, "title": title, "exe": exe, "focus": hwnd == focus})
        except Exception:  # one odd window must not hide the rest
            pass
        return True

    _user32.EnumWindows(visit, 0)
    found.sort(key=lambda w: not w["focus"])  # stable: the focused window first, the rest in z-order
    return found


def focus_window(hwnd: int) -> bool:
    """Bring a window to the front. Only a window that is in the current list: the panel names one, it never injects an
    arbitrary handle."""
    if not any(w["id"] == hwnd for w in list_windows()):
        return False
    handle = wintypes.HWND(hwnd)
    if not _user32.IsWindow(handle):
        return False
    if _user32.IsIconic(handle):
        _user32.ShowWindow(handle, _SW_RESTORE)
    # Windows refuses SetForegroundWindow from a background process unless it just saw input: an Alt tap counts.
    _user32.keybd_event(_VK_MENU, 0, 0, 0)
    _user32.keybd_event(_VK_MENU, 0, _KEYEVENTF_KEYUP, 0)
    _user32.BringWindowToTop(handle)
    return bool(_user32.SetForegroundWindow(handle))


# ------------------------------------------------------------------------------------------------------- badges

_UNREAD = [
    re.compile(r"^\((\d{1,3})\+?\)"),            # "(3) Slack", "(2) Chat | Microsoft Teams", "(1) Discord"
    re.compile(r"^\[(\d{1,3})\]"),                # "[2] Telegram"
    re.compile(r"\((\d{1,4})\)\s*[-|]"),          # "Inbox (12) - me@mail.com"
    re.compile(r"(\d{1,4}) unread", re.I),        # "Inbox - 5 unread"
]
_BADGE_APPS = {"Slack", "Discord", "Teams", "Outlook", "WhatsApp", "Telegram", "Chrome", "Edge", "Firefox", "Brave", "Linear"}
_BROWSERS = {"Chrome", "Edge", "Firefox", "Brave"}


def _site(title: str) -> str:
    """A browser tab's site, from its title ('Inbox (3) - Gmail - Google Chrome' -> 'Gmail')."""
    parts = [p.strip() for p in title.split(" - ")]
    return parts[-2] if len(parts) >= 3 else (parts[0] if parts else "")


def badges(window_list: list[dict]) -> list[dict]:
    """Unread counts the apps put in their window titles: {"app": name, "count": n}. Slack, Discord, Teams, Outlook and the
    web apps in a browser tab's title do this; Windows itself offers no way to read another app's taskbar badge."""
    totals: dict[str, int] = {}
    for w in window_list:
        if w["app"] not in _BADGE_APPS:
            continue
        for pattern in _UNREAD:
            m = pattern.search(w["title"])
            if m:
                count = int(m.group(1))
                if count > 0:
                    name = _site(w["title"]) if w["app"] in _BROWSERS else w["app"]
                    totals[name or w["app"]] = max(totals.get(name or w["app"], 0), count)
                break
    return [{"app": name[:20], "count": count} for name, count in sorted(totals.items(), key=lambda kv: -kv[1])][:6]


# ------------------------------------------------------------------------------------------------------- vitals


class _FILETIME(ctypes.Structure):
    _fields_ = [("lo", wintypes.DWORD), ("hi", wintypes.DWORD)]


class _MemoryStatus(ctypes.Structure):
    _fields_ = [
        ("length", wintypes.DWORD), ("load", wintypes.DWORD), ("total", ctypes.c_ulonglong), ("avail", ctypes.c_ulonglong),
        ("totalPage", ctypes.c_ulonglong), ("availPage", ctypes.c_ulonglong), ("totalVirt", ctypes.c_ulonglong),
        ("availVirt", ctypes.c_ulonglong), ("availExt", ctypes.c_ulonglong),
    ]


class _PowerStatus(ctypes.Structure):
    _fields_ = [
        ("ac", ctypes.c_ubyte), ("flag", ctypes.c_ubyte), ("percent", ctypes.c_ubyte), ("saver", ctypes.c_ubyte),
        ("lifeTime", wintypes.DWORD), ("fullLifeTime", wintypes.DWORD),
    ]


_kernel32.GetSystemTimes.argtypes = [ctypes.POINTER(_FILETIME)] * 3
_kernel32.GetSystemTimes.restype = wintypes.BOOL
_kernel32.GlobalMemoryStatusEx.argtypes = [ctypes.POINTER(_MemoryStatus)]
_kernel32.GlobalMemoryStatusEx.restype = wintypes.BOOL
_kernel32.GetSystemPowerStatus.argtypes = [ctypes.POINTER(_PowerStatus)]
_kernel32.GetSystemPowerStatus.restype = wintypes.BOOL


_TEMP_SCRIPT = r"""
$o = @{}
foreach ($ns in 'root/LibreHardwareMonitor', 'root/OpenHardwareMonitor') {
  try {
    $s = Get-CimInstance -Namespace $ns -ClassName Sensor -ErrorAction Stop | Where-Object { $_.SensorType -eq 'Temperature' -and $_.Name -match 'CPU' }
    if ($s) { $o.cpu = [math]::Round(($s | Measure-Object Value -Maximum).Maximum); break }
  } catch {}
}
try {
  $z = (Get-Counter '\Thermal Zone Information(*)\Temperature' -ErrorAction Stop).CounterSamples | ForEach-Object { $_.CookedValue - 273.15 }
  if ($z) { $o.zone = [math]::Round(($z | Measure-Object -Maximum).Maximum) }
} catch {}
$o | ConvertTo-Json -Compress
"""


def _ticks(ft: _FILETIME) -> int:
    return (ft.hi << 32) | ft.lo


class Vitals:
    """CPU load needs two readings, so this keeps the previous one. Not thread-safe; one caller (the publisher)."""

    def __init__(self, disk: str = "C:\\") -> None:
        self._disk = disk
        self._last: tuple[int, int] | None = None
        self._gpu: dict | None = None
        self._gpu_at = 0.0
        self._gpu_lock = threading.Lock()
        self._has_gpu = shutil.which("nvidia-smi") is not None
        self._temps: dict = {}
        self._temps_at = 0.0

    def _cpu(self) -> int | None:
        idle, kernel, user = _FILETIME(), _FILETIME(), _FILETIME()
        if not _kernel32.GetSystemTimes(ctypes.byref(idle), ctypes.byref(kernel), ctypes.byref(user)):
            return None
        idle_t, busy_t = _ticks(idle), _ticks(kernel) + _ticks(user)  # kernel time includes idle
        last, self._last = self._last, (idle_t, busy_t)
        if last is None:
            return None
        total = busy_t - last[1]
        return None if total <= 0 else max(0, min(100, round(100 * (total - (idle_t - last[0])) / total)))

    def _read_gpu(self) -> None:
        try:
            out = subprocess.run(
                ["nvidia-smi", "--query-gpu=utilization.gpu,temperature.gpu", "--format=csv,noheader,nounits"],
                capture_output=True, text=True, timeout=3, creationflags=0x08000000,  # CREATE_NO_WINDOW
            ).stdout.strip().splitlines()
            load, temp = [int(v) for v in out[0].split(",")]
            gpu = {"load": load, "temp": temp}
        except (OSError, ValueError, IndexError, subprocess.SubprocessError):
            gpu = None
            self._has_gpu = False
        with self._gpu_lock:
            self._gpu = gpu

    def _read_temps(self) -> None:
        """The CPU temperature (LibreHardwareMonitor / OpenHardwareMonitor, when one runs) and the hottest ACPI thermal zone.
        Windows has no built-in CPU die temperature: the ACPI zones are chassis sensors that barely follow the load."""
        try:
            out = subprocess.run(["powershell.exe", "-NoLogo", "-NoProfile", "-NonInteractive", "-Command", _TEMP_SCRIPT],
                                 capture_output=True, text=True, timeout=15, creationflags=0x08000000).stdout.strip()
            data = json.loads(out) if out else {}
        except (OSError, ValueError, subprocess.SubprocessError):
            data = {}
        with self._gpu_lock:
            self._temps = data if isinstance(data, dict) else {}

    def read(self) -> dict:
        """{"cpu": %, "ram": % used, "ram_free": GB, "disk": % used, "battery": % or absent, "charging": bool, "gpu": %,
        "gpu_temp": C, and "cpu_temp": C (a hardware monitor runs) or "temp": C (the hottest ACPI zone)}."""
        out: dict = {}
        cpu = self._cpu()
        if cpu is not None:
            out["cpu"] = cpu
        mem = _MemoryStatus()
        mem.length = ctypes.sizeof(mem)
        if _kernel32.GlobalMemoryStatusEx(ctypes.byref(mem)):
            out["ram"] = int(mem.load)
            out["ram_free"] = round(mem.avail / (1 << 30), 1)  # GB
        try:
            usage = shutil.disk_usage(self._disk)
            out["disk"] = round(100 * usage.used / usage.total)
        except OSError:
            pass
        power = _PowerStatus()
        if _kernel32.GetSystemPowerStatus(ctypes.byref(power)) and power.percent <= 100 and power.flag != 128:  # 128: no battery
            out["battery"] = int(power.percent)
            out["charging"] = bool(power.ac == 1)
        if self._has_gpu and time.monotonic() - self._gpu_at > 5:
            self._gpu_at = time.monotonic()
            threading.Thread(target=self._read_gpu, daemon=True).start()  # nvidia-smi takes ~100 ms: never block the loop
        if time.monotonic() - self._temps_at > 10:
            self._temps_at = time.monotonic()
            threading.Thread(target=self._read_temps, daemon=True).start()
        with self._gpu_lock:
            if self._temps.get("cpu") is not None:
                out["cpu_temp"] = int(self._temps["cpu"])
            elif self._temps.get("zone") is not None:
                out["temp"] = int(self._temps["zone"])  # not the CPU: shown as "Temp"
            if self._gpu:
                out["gpu"], out["gpu_temp"] = self._gpu["load"], self._gpu["temp"]
        return out


class WindowsPcProvider:
    """pc.snapshot() -> {"windows": [...], "badges": [...], "vitals": {...}} (async); pc.focus(id) -> bool (async)."""

    def __init__(self) -> None:
        self._vitals = Vitals()

    async def snapshot(self) -> dict:
        import asyncio

        def run() -> dict:
            window_list = list_windows()
            ordered = sorted(window_list, key=lambda w: w["app"] not in EDITORS)  # stable: z-order kept inside each group
            shown = [{"id": w["id"], "app": w["app"], "title": w["title"].strip()[:MAX_TITLE], "ws": workspace(w["app"], w["title"].strip())[:MAX_TITLE]}
                     for w in ordered[:MAX_WINDOWS]]
            return {"windows": shown, "badges": badges(window_list), "vitals": self._vitals.read()}

        return await asyncio.to_thread(run)

    async def focus(self, window_id: int) -> bool:
        import asyncio

        return await asyncio.to_thread(focus_window, window_id)


def create_pc_provider():
    return WindowsPcProvider()
