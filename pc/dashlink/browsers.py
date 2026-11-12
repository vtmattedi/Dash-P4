"""Opening a link on this PC. The panel asks for "chrome" or "default" and sends plain https links; how each one is
opened is decided here: a Linear link goes to the Linear desktop app (linear://) when it is installed, anything else to the
browser."""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
import urllib.parse
import webbrowser

BROWSERS = ("default", "chrome")
LINEAR_HOST = "linear.app"


def linear_app_url(url: str) -> str | None:
    """https://linear.app/<workspace>/issue/ABC-1/slug  ->  linear://<workspace>/issue/ABC-1/slug, the form the Linear
    desktop app opens. None for anything that is not a Linear page on exactly linear.app."""
    parts = urllib.parse.urlsplit(url)
    if parts.scheme != "https" or (parts.hostname or "").lower() != LINEAR_HOST or not parts.path.strip("/"):
        return None
    rest = parts.path.lstrip("/")
    if parts.query:
        rest += "?" + parts.query
    if parts.fragment:
        rest += "#" + parts.fragment
    return "linear://" + rest


def linear_app_installed() -> bool:
    """Is something registered to open linear:// links?"""
    if sys.platform == "win32":
        import winreg

        try:
            with winreg.OpenKey(winreg.HKEY_CLASSES_ROOT, "linear") as key:
                winreg.QueryValueEx(key, "URL Protocol")
                return True
        except OSError:
            return False
    if sys.platform == "darwin":
        return os.path.isdir("/Applications/Linear.app")
    xdg = shutil.which("xdg-mime")
    if not xdg:
        return False
    try:
        done = subprocess.run([xdg, "query", "default", "x-scheme-handler/linear"], capture_output=True, text=True, timeout=5)
    except (OSError, subprocess.SubprocessError):
        return False
    return bool(done.stdout.strip())


def launch_scheme(url: str) -> None:
    """Hand a custom-scheme URL to whatever the OS registered for it (never through a shell)."""
    if sys.platform == "win32":
        os.startfile(url)  # noqa: S606 - the URL was built from a validated https link, scheme fixed to linear://
    elif sys.platform == "darwin":
        subprocess.Popen(["open", url], close_fds=True)
    else:
        subprocess.Popen(["xdg-open", url], close_fds=True)


def chrome_path() -> str | None:
    """Where Google Chrome is on this PC, or None."""
    if sys.platform == "win32":
        import winreg

        for root in (winreg.HKEY_LOCAL_MACHINE, winreg.HKEY_CURRENT_USER):
            try:
                with winreg.OpenKey(root, r"SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths\chrome.exe") as key:
                    path = winreg.QueryValue(key, None)
                    if path and os.path.exists(path):
                        return path
            except OSError:
                pass
        for variable in ("ProgramFiles", "ProgramFiles(x86)", "LocalAppData"):
            base = os.environ.get(variable)
            if base:
                path = os.path.join(base, "Google", "Chrome", "Application", "chrome.exe")
                if os.path.exists(path):
                    return path
        return None
    for name in ("google-chrome", "google-chrome-stable", "chromium", "chromium-browser"):
        found = shutil.which(name)
        if found:
            return found
    return None


def open_in_browser(url: str, browser: str = "default", linear_app: bool = True) -> str:
    """Open `url`. Returns a short note about what was used (shown on the panel when it is not the obvious thing)."""
    app_url = linear_app_url(url) if linear_app else None
    if app_url and linear_app_installed():
        try:
            launch_scheme(app_url)
            return "Opened in the Linear app"
        except OSError:
            pass  # the registered handler failed: the browser can still show the page
    if browser == "chrome":
        path = chrome_path()
        if path:
            subprocess.Popen([path, url], close_fds=True)  # a list, no shell: the URL is never parsed by a shell
            return ""
        webbrowser.open(url)
        return "Chrome not found: used the default browser"
    webbrowser.open(url)
    return ""
