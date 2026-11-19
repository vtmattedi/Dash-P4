"""Launchers: names the panel can start, defined on THIS PC in a JSON file. The panel only ever sends the name; what runs is
whatever the file says, so nobody on the network can start anything that is not listed here.

The file (default %APPDATA%\\dashlink\\launchers.json, or --launchers PATH) is a list:

    [
      {"name": "Dash-P4",   "run": ["code", "D:\\\\MattediWorks\\\\nightmare\\\\devices\\\\Dash-P4"]},
      {"name": "Terminal",  "run": ["wt.exe"]},
      {"name": "Linear",    "url": "https://linear.app"}
    ]

A tile can instead hold a list of choices -- {"name": "GitHub", "icon": "github", "menu": [{"name": "Stackport", "url": ...}, ...]} --
the panel then shows them in a popover and asks for one by name. A link may carry "profile": "Profile 1" to open in that
Chrome profile.

`run` is the program and its arguments (no shell is involved); `url` opens in the default browser, or in Chrome with
"browser": "chrome". A program launcher that opens a folder in VS Code (`code <folder>`) first looks for a VS Code window
of that folder and just switches to it; "focus": "text" does the same for any window whose title has that text. An optional "icon" picks the picture on the panel's deck: vscode, github, trackfy or stackport (a program
named code gets the VS Code one and a github.com link the GitHub one when none is given; anything else shows its initial). It is
read again when it changes, so editing it needs no restart."""

from __future__ import annotations

import json
import logging
import os
import shutil
import subprocess
import sys
from pathlib import Path

log = logging.getLogger("dashlink")

MAX_LAUNCHERS = 12
MAX_NAME = 24
MAX_MENU = 8
ICONS = ("vscode", "github", "trackfy", "stackport")


def find_window(entry_run: list[str] | None = None, text: str | None = None) -> int | None:
    """The handle of an already open window this launcher would only duplicate: a VS Code window of the folder it opens, or
    one whose title has `text`. None when there is none, or when this is not Windows."""
    try:
        from .providers import windows_pc
    except Exception:  # not Windows: nothing to look for
        return None
    folder = ""
    if entry_run and Path(entry_run[0]).stem.lower() == "code" and len(entry_run) > 1 and os.path.isdir(entry_run[-1]):
        folder = Path(entry_run[-1]).name.lower()
    if not folder and not text:
        return None
    try:
        for window in windows_pc.list_windows():
            title = window["title"].lower()
            if text and text.lower() in title:
                return window["id"]
            if folder and window["app"] == "VS Code" and windows_pc.workspace(window["app"], window["title"]).lower() == folder:
                return window["id"]
    except Exception:
        log.exception("window search failed")
    return None


def default_path() -> Path:
    base = os.environ.get("APPDATA") or str(Path.home())
    return Path(base) / "dashlink" / "launchers.json"


class Launchers:
    def __init__(self, path: Path | None = None) -> None:
        self.path = path or default_path()
        self._mtime: float | None = None
        self._entries: dict[str, dict] = {}

    def _reload(self) -> None:
        try:
            mtime = self.path.stat().st_mtime
        except OSError:
            if self._entries:
                self._entries, self._mtime = {}, None
            return
        if mtime == self._mtime:
            return
        self._mtime = mtime
        entries: dict[str, dict] = {}
        try:
            data = json.loads(self.path.read_text(encoding="utf-8-sig"))
            for item in data if isinstance(data, list) else []:
                name = str(item.get("name", "")).strip()[:MAX_NAME]
                if not name or name in entries:
                    continue
                if isinstance(item.get("menu"), list):
                    # A tile that opens a list of choices instead of starting one thing.
                    choices: dict[str, dict] = {}
                    for sub in item["menu"][:MAX_MENU]:
                        sub_name = str(sub.get("name", "")).strip()[:MAX_NAME] if isinstance(sub, dict) else ""
                        parsed = self._parse(sub) if sub_name else None
                        if parsed is not None and sub_name not in choices:
                            choices[sub_name] = parsed
                    if choices:
                        icon = item.get("icon")
                        entries[name] = {"menu": choices, "icon": icon if icon in ICONS else ""}
                else:
                    parsed = self._parse(item)
                    if parsed is not None:
                        entries[name] = parsed
                if len(entries) >= MAX_LAUNCHERS:
                    break
        except (OSError, ValueError, AttributeError) as error:
            log.warning("cannot read %s: %s", self.path, error)
        self._entries = entries
        log.info("%d launchers from %s", len(entries), self.path)

    @staticmethod
    def _parse(item: dict) -> dict | None:
        """One thing to start, from its JSON object; None when it is not a valid program or link."""
        run, url, icon = item.get("run"), item.get("url"), item.get("icon")
        if isinstance(run, list) and run and all(isinstance(a, str) for a in run):
            if icon not in ICONS and Path(run[0]).stem.lower() == "code":
                icon = "vscode"
            entry: dict = {"run": run}
            if isinstance(item.get("focus"), str):
                entry["focus"] = item["focus"][:60]
        elif isinstance(url, str) and url.startswith(("http://", "https://")):
            browser = item.get("browser", "default")
            entry = {"url": url, "browser": browser if browser in ("default", "chrome") else "default"}
            if isinstance(item.get("profile"), str) and item["profile"]:
                entry["profile"] = item["profile"][:40]  # a Chrome profile directory: "Default", "Profile 1"
            if icon not in ICONS and "github.com" in url:
                icon = "github"
        else:
            return None
        entry["icon"] = icon if icon in ICONS else ""
        return entry

    def names(self) -> list[str]:
        self._reload()
        return list(self._entries)

    def deck(self) -> list[dict]:
        """What the panel shows: the name and the icon of each launcher, in file order."""
        self._reload()
        deck = []
        for name, entry in self._entries.items():
            tile = {"name": name, "icon": entry["icon"]}
            if "menu" in entry:
                tile["menu"] = [{"name": n, "icon": c["icon"]} for n, c in entry["menu"].items()]
            deck.append(tile)
        return deck

    def launch(self, name: object, item: object = None) -> tuple[bool, str]:
        self._reload()
        entry = self._entries.get(name) if isinstance(name, str) else None
        if entry is not None and "menu" in entry:  # a tile with choices: the panel names which one
            entry = entry["menu"].get(item) if isinstance(item, str) else None
        if entry is None:
            return False, "no such launcher"
        try:
            if "url" in entry:
                from .browsers import open_in_browser

                open_in_browser(entry["url"], entry.get("browser", "default"), linear_app=False, profile=entry.get("profile"))
            else:
                flags = 0x00000008 | 0x00000200 if sys.platform == "win32" else 0  # DETACHED_PROCESS | NEW_PROCESS_GROUP
                handle = find_window(entry["run"], entry.get("focus"))
                if handle is not None:
                    from .providers import windows_pc

                    if windows_pc.focus_window(handle):
                        return True, ""
                command = list(entry["run"])
                command[0] = shutil.which(command[0]) or command[0]  # "code" is code.cmd, which CreateProcess will not find by name
                subprocess.Popen(command, shell=False, close_fds=True, creationflags=flags,
                                 stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        except (OSError, ValueError) as error:
            return False, f"could not start it: {error}"[:80]
        return True, ""
