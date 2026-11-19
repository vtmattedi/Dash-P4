"""What is playing on Windows, and the transport buttons.

Windows exposes it through the System Media Transport Controls (GlobalSystemMediaTransportControlsSessionManager): the same
source as the volume flyout's media card, so it covers Spotify, YouTube Music in a browser, VLC and so on. It is a WinRT API
and Python's standard library cannot reach it, so one long-lived Windows PowerShell does and prints a JSON line when
something changes. If PowerShell or the API is missing, control still works through the media keys and nothing is reported.
"""

from __future__ import annotations

import asyncio
import base64
import json
import logging
import os
import sys

from . import windows

log = logging.getLogger("dashlink")

_SCRIPT = r"""
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
Add-Type -AssemblyName System.Runtime.WindowsRuntime
$asTask = ([System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
  $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1' })[0]
function Await($op, $type) { $t = $asTask.MakeGenericMethod($type).Invoke($null, @($op)); $t.Wait(-1) | Out-Null; $t.Result }
$mgrType = [Windows.Media.Control.GlobalSystemMediaTransportControlsSessionManager, Windows.Media.Control, ContentType = WindowsRuntime]
$propType = [Windows.Media.Control.GlobalSystemMediaTransportControlsSessionMediaProperties, Windows.Media.Control, ContentType = WindowsRuntime]
$mgr = Await ($mgrType::RequestAsync()) $mgrType
$last = ''
$parentId = __PARENT__
while ($true) {
  if (-not (Get-Process -Id $parentId -ErrorAction SilentlyContinue)) { exit }  # the companion is gone: do not outlive it
  $out = $null
  try {
    $s = $mgr.GetCurrentSession()
    if ($s) {
      $p = Await ($s.TryGetMediaPropertiesAsync()) $propType
      $i = $s.GetPlaybackInfo()
      $out = [ordered]@{ title = [string]$p.Title; artist = [string]$p.Artist; app = [string]$s.SourceAppUserModelId; playing = ([int]$i.PlaybackStatus -eq 4) }
    }
  } catch { $out = $null }
  $line = if ($out) { $out | ConvertTo-Json -Compress } else { 'null' }
  if ($line -ne $last) { $last = $line; [Console]::Out.WriteLine($line); [Console]::Out.Flush() }
  Start-Sleep -Milliseconds 700
}
"""


def _friendly(app_id: str) -> str:
    """'Spotify.exe' / 'chrome.exe' / 'SpotifyAB.SpotifyMusic_zpdnekdrzrea0!Spotify' -> 'Spotify' / 'Chrome'."""
    name = app_id.split("!")[-1].split("\\")[-1]
    if name.lower().endswith(".exe"):
        name = name[:-4]
    return (name[:1].upper() + name[1:])[:20] if name else ""


class NowPlaying:
    """media.get() -> {"title","artist","app","playing"} or None (what the last line from PowerShell said);
    media.control(cmd) -> True when the media key was pressed."""

    def __init__(self) -> None:
        self._value: dict | None = None
        self._proc: asyncio.subprocess.Process | None = None
        self._task: asyncio.Task | None = None
        self._failed = False

    async def start(self) -> None:
        if self._task is None and not self._failed:
            self._task = asyncio.create_task(self._run())

    async def _run(self) -> None:
        encoded = base64.b64encode(_SCRIPT.replace("__PARENT__", str(os.getpid())).encode("utf-16-le")).decode()
        try:
            self._proc = await asyncio.create_subprocess_exec(
                "powershell.exe", "-NoLogo", "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-EncodedCommand", encoded,
                stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.DEVNULL, stdin=asyncio.subprocess.DEVNULL,
                creationflags=0x08000000,  # CREATE_NO_WINDOW
            )
            assert self._proc.stdout is not None
            while True:
                line = await self._proc.stdout.readline()
                if not line:
                    break
                text = line.decode("utf-8", "replace").strip().lstrip("﻿")
                try:
                    data = json.loads(text)
                except ValueError:
                    continue
                if isinstance(data, dict):
                    data["app"] = _friendly(str(data.get("app", "")))
                    self._value = {k: data.get(k) for k in ("title", "artist", "app", "playing")}
                else:
                    self._value = None
        except (OSError, AssertionError) as error:
            log.warning("no now-playing information: %s", error)
        finally:
            self._failed = True  # PowerShell ended: do not respawn in a loop
            self._value = None
            if self._proc and self._proc.returncode is None:
                self._proc.kill()

    async def get(self) -> dict | None:
        await self.start()
        return self._value

    async def control(self, command: str) -> bool:
        return windows.send_media_key(command)


def create_media_provider():
    if sys.platform != "win32":
        return None
    return NowPlaying()
