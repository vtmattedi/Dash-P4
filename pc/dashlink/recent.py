"""VS Code's recently used folders, for the panel's Recent tile.

VS Code (this build) no longer keeps a usable "recent" list in its settings database, so this reads what it does keep: one folder
per workspace under %APPDATA%/Code/User/workspaceStorage/<id>/workspace.json, whose state.vscdb is touched whenever that
workspace is used. Newest first, so it is the same order as the jump list. Only local folders and .code-workspace files are
offered (a remote one cannot be opened from here with a plain path). The panel only ever sends a name from this list; the path
stays on the PC."""

from __future__ import annotations

import json
import logging
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path
from urllib.parse import unquote, urlparse

from .launchers import find_window

log = logging.getLogger("dashlink")

MAX_RECENT = 10
MAX_NAME = 32
MAX_SCAN = 60  # newest workspace folders looked at; some are gone or remote
REREAD_SECONDS = 20.0


def _storage_dir() -> Path:
    return Path(os.environ.get("APPDATA") or Path.home()) / "Code" / "User" / "workspaceStorage"


def _where(path: str, limit: int = 34) -> str:
    """The folder a project is in, cut at a folder boundary from the left when long: .../nightmare/devices."""
    parts = [p.strip("\\/") for p in Path(path).parent.parts]
    parts = [p for p in parts if p]
    text = "/".join(parts)
    if len(text) <= limit:
        return text
    kept: list[str] = []
    for part in reversed(parts):
        if sum(len(k) + 1 for k in kept) + len(part) + 4 > limit:
            break
        kept.insert(0, part)
    return ".../" + "/".join(kept) if kept else ".../" + parts[-1][-(limit - 4):]


def _local_path(uri: object) -> str | None:
    if not isinstance(uri, str):
        return None
    parsed = urlparse(uri)
    if parsed.scheme != "file":
        return None
    path = unquote(parsed.path)
    if sys.platform == "win32" and len(path) > 2 and path[0] == "/" and path[2] == ":":
        path = path[1:]  # /d:/x -> d:/x
    return os.path.normpath(path)


def _used(directory: Path) -> float:
    for name in ("state.vscdb", "workspace.json"):
        try:
            return (directory / name).stat().st_mtime
        except OSError:
            continue
    return 0.0


def _open_workspaces() -> set[str]:
    """Names (lower case) of the folders and workspaces VS Code has a window for right now. Empty when this is not Windows or
    the windows cannot be listed: then nothing is tagged open, which is the safe reading."""
    try:
        from .providers import windows_pc

        names: set[str] = set()
        for window in windows_pc.list_windows():
            if window["app"] != "VS Code":
                continue
            name = windows_pc.workspace(window["app"], window["title"]).lower()
            names.add(name[: -len(" (workspace)")] if name.endswith(" (workspace)") else name)
        return names
    except Exception:
        return set()


class RecentFolders:
    def __init__(self, storage: Path | None = None, clock=time.monotonic, open_windows=_open_workspaces) -> None:
        self.storage = storage or _storage_dir()
        self.clock = clock
        self.open_windows = open_windows
        self._read_at: float | None = None
        self._items: list[dict] = []

    def _reload(self) -> None:
        now = self.clock()
        if self._read_at is not None and now - self._read_at < REREAD_SECONDS:
            return
        self._read_at = now
        items: list[dict] = []
        try:
            directories = [Path(e.path) for e in os.scandir(self.storage) if e.is_dir()]
            directories.sort(key=_used, reverse=True)
            seen: set[str] = set()
            for directory in directories[:MAX_SCAN]:
                try:
                    data = json.loads((directory / "workspace.json").read_text(encoding="utf-8-sig"))
                except (OSError, ValueError):
                    continue
                path = _local_path(data.get("folder")) or _local_path(data.get("workspace"))
                if not path or not os.path.exists(path):
                    continue
                name = Path(path).stem if path.endswith(".code-workspace") else Path(path).name
                name = name[:MAX_NAME]
                if not name or name in seen:  # one entry per name: the panel names what to open
                    continue
                seen.add(name)
                items.append({"name": name, "path": path, "where": _where(path)})
                if len(items) >= MAX_RECENT:
                    break
        except OSError as error:
            log.warning("cannot read VS Code's workspaces: %s", error)
        self._items = items

    def entries(self) -> list[dict]:
        """What the panel is shown: name, where it lives and whether VS Code has a window of it open, most recent first. The
        list is re-read rarely, but `open` is looked at on every call: windows come and go faster than folders."""
        self._reload()
        opened = self.open_windows()
        return [{"name": i["name"], "where": i["where"], "open": i["name"].lower() in opened} for i in self._items]

    def open(self, name: object) -> tuple[bool, str]:
        self._reload()
        item = next((i for i in self._items if i["name"] == name), None) if isinstance(name, str) else None
        if item is None:
            return False, "not in VS Code's recent list"
        run = ["code", item["path"]]
        handle = find_window(run) if os.path.isdir(item["path"]) else None
        if handle is not None:
            from .providers import windows_pc

            if windows_pc.focus_window(handle):
                return True, ""
        code = shutil.which("code")
        if code is None:
            return False, "VS Code's command line (code) is not on the PATH"
        try:
            flags = 0x00000008 | 0x00000200 if sys.platform == "win32" else 0
            subprocess.Popen([code, item["path"]], shell=False, close_fds=True, creationflags=flags, stdin=subprocess.DEVNULL,
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        except OSError as error:
            return False, f"could not start it: {error}"[:80]
        return True, ""
