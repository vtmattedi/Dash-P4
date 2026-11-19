# DashLink protocol, version 0

A direct link between the PC app (`dashlink`) and the Dash-P4 panel on the local network. No broker.

- **The panel listens, the PC connects.** The PC only makes outbound connections, so Windows needs no firewall rule for
  it, and the panel's address is the only thing the PC has to know (shown in the panel's Settings; discovery comes later).
- **One TCP connection**, default port **7340**. One PC at a time in v0.
- **Panel memory:** every buffer the panel allocates for this must live in PSRAM; the link must not use internal RAM
  beyond the socket itself.

## Framing

```
+----------------+------+----------------------+
| length (u32 LE)| type | payload (UTF-8 JSON) |
+----------------+------+----------------------+
```

`length` counts the type byte and the payload (not itself). Maximum 16384. A frame outside 1..16384 closes the link.
`type` is one ASCII letter. The payload is one JSON object (may be absent for `P`/`O`, meaning `{}`).
Unknown types and unknown JSON keys are ignored, so later versions can add to them.

## Messages

| Type | From | Meaning | Payload |
|---|---|---|---|
| `C` challenge | panel | first thing sent after accept | `{"v":0,"nonce":"<32 hex>","device":"<panel name>","fw":"<version>"}` |
| `H` hello | PC | answers the challenge | `{"v":0,"name":"<pc name>","os":"windows","app":"dashlink 0.1","caps":["window","media","open_url"],"auth":"<hex>"}` |
| `W` welcome | panel | accepted | `{"heartbeat_s":5}` |
| `E` error | either | refused or failing; the sender then closes | `{"code":"auth\|version\|busy\|protocol","msg":"..."}` |
| `P` ping | either | liveness | `{"t":<ms>}` |
| `O` pong | either | answers a ping, echoing `t` | `{"t":<ms>}` |
| `S` state | PC | what the PC is doing; **only the keys that changed** | see below |
| `A` action | panel | please do something on the PC | `{"id":<n>,"name":"open_url","args":{...}}` |
| `R` result | PC | the outcome of an action | `{"id":<n>,"ok":true,"msg":""}` |
| `U` update | PC | a firmware image follows | `{"target":"p4\|c6","size":<bytes>,"sha256":"<hex of the whole image>"}` |
| `D` data | PC | the next piece of that image | **raw bytes** (1..16383), not JSON |
| `T` status | panel | how the update is going | `{"state":"ready\|running\|done\|failed","pct":<0-100>,"msg":"..."}` |

The challenge also carries `"elf"`: the sha256 (64 hex characters) of the ELF the running firmware was built from. It is
the build's identity; the same value is inside `firmware.bin` (the app descriptor), so the PC can tell which build the
panel runs without a version bump.

### Firmware push

`python -m dashlink push-fw` sends a build over the link the PC already has, so there is no web server and no
inbound connection to the PC (no firewall prompt).

1. PC sends `U` with the image size and its sha256. The panel answers `T{"state":"ready"}`, or `T{"state":"failed","msg":...}`
   (an update is already running, the image does not fit, bad request).
2. PC sends the image as `D` frames, in order, and the panel writes them to the idle slot as they arrive (`T running` about
   every 10 %). A `T failed` at any point means the panel stopped; the running firmware is untouched.
3. When `size` bytes are in, the panel checks the sha256, validates the image (for the P4 the bootloader's own checks, for
   the C6 the C6's) and switches the boot slot, then sends `T{"state":"done"}` and restarts about a second later.
4. The PC reconnects, reads the new `elf` in the challenge and compares it with the one in the file it sent. If they
   differ, the new image did not start and the bootloader rolled back (an image is marked good 20 s after a healthy start).

The panel needs no new key: only a PC that completed the pairing handshake can push. As everything in v0, the link is not
encrypted: anyone who can read and inject packets on the LAN can read the image or interfere. A signature check on the
image is the planned next step. Closing the connection mid-update aborts it.

### Authentication

A shared pairing token, set in the PC app and in the panel's settings. v0 stays unencrypted on purpose (see below). The proof is

```
auth = hex( HMAC-SHA256( key = token, message = nonce + "|" + name ) )
```

where `nonce` and `name` are the strings exactly as sent in `C` and `H`. The panel compares in constant time and answers
`W` or `E{"code":"auth"}`. **v0 is not encrypted**: it keeps strangers on the LAN from driving the link, it does not hide
the traffic. TLS (or a Noise handshake) is the planned upgrade; do not carry secrets over it.

### Liveness

If nothing was sent for `heartbeat_s`, a side sends `P`; the other answers `O`. A side that has heard nothing for
`3 x heartbeat_s` closes the connection. The PC reconnects with a growing delay (1, 2, 4 ... 30 s).

### State

```json
{"window": {"title": "Pull request #14", "app": "chrome.exe", "pid": 4120}}
```

`window` is sent whole when it changes, and `null` means "none" (nothing focused). Titles are cut to 200 characters. The PC
sends it right after `W`. Changes are rate-limited to about two per second. More keys (media, monitoring values) are added
later; the panel ignores keys it does not know.

```json
{"display": "off"}
```

`display` is the PC's monitor: `"on"`, `"off"` or `"dim"`. It is sent (with the `display` capability) once Windows has
reported it, and then whenever it changes. Only the monitor is reported: the PC going to sleep or hibernating is not, the
link simply ends. The panel treats `off` as "blank yourself and the LED strip" and a later `on` as "wake"; `dim` changes
nothing, and a link that drops leaves the panel as it was. The panel's own "Screen off" is separate and local: it
blanks the panel and sends nothing to the PC.

```json
{"call": {"active": true, "app": "Meet", "muted": false}}
```

`call` (capability `call`) is sent whole when it changes; `null` means no call. The PC polls about once a second. `active` is
true while some process (other than a short ignore list) has an active capture session on the default communications
microphone: Meet in a browser, Discord, Teams, Slack huddles. `app` is a short label (Discord, Teams, Slack, Zoom; a browser
is "Meet" when the focused window's title says so, else "Browser"). `muted` is the **microphone's** mute flag, the one signal
every app shares and the PC can read back; an app's own mute button is not read. The panel drops the call state when the link
drops.

```json
{"media": {"title": "Song", "artist": "Band", "app": "Spotify", "playing": true}}
```

`media` (capability `media_info`) is what Windows' media session says is playing (the same source as the volume flyout), sent whole
when it changes; `null` means nothing is loaded. It comes from a long-lived Windows PowerShell the companion starts (the API is
WinRT); without it the transport buttons still work through the media keys and nothing is reported.

```json
{"vitals": {"cpu": 13, "ram": 72, "ram_free": 8.7, "disk": 88, "gpu": 34, "gpu_temp": 48, "cpu_temp": 54, "temp": 28, "battery": 80, "charging": false}}
```

`vitals` (capability `vitals`) is sent whole about every two seconds, only when a value changed. `cpu`, `ram` (share used), `disk`
(C:, share used) and `gpu` are percent; `ram_free` is GB; `gpu` and `gpu_temp` need an NVIDIA GPU (nvidia-smi). `cpu_temp` is
the CPU's temperature and only exists while LibreHardwareMonitor or OpenHardwareMonitor runs on the PC (Windows has no built-in
reading). Without one, `temp` is the hottest ACPI thermal zone, which is a chassis sensor and barely follows the load: the panel
shows it as "Case", never as the CPU. `battery` (percent) and `charging` only exist on a PC with a battery.

```json
{"windows": [{"id": 263548, "app": "VS Code", "title": "x.py - Dash-P4 - Visual Studio Code", "ws": "Dash-P4"}],
 "badges": [{"app": "Slack", "count": 3}], "launchers": [{"name": "Stackport PRs", "icon": "github"}]}
```

`windows` (capability `windows`) lists the open top-level windows (at most 16, editors first, then the rest in z-order; titles cut
to 80 characters); `id` is the window handle and is what `focus` takes. `ws` tells windows of one app apart (a VS Code folder).
`badges` are unread counts that apps put in their window titles ("(3) Slack", "Inbox (12) - Gmail"): Windows offers no way to read
another app's taskbar badge. `launchers` (capability `launch`) are the entries of the PC's own launchers file: `name` (what `launch` takes) and `icon` (`vscode`, `github`, `trackfy` or empty: the panel draws the initial). A bare string is accepted as a name with no icon. `recent` (capability `recent`) is VS Code's recently opened folders, most recent first: `[{"name": "Trackfy", "where": "C:\\Users\\x"}]`; the `open_recent` action takes a `name` from it.

### Actions (panel -> PC)

| `name` | `args` | Effect on the PC |
|---|---|---|
| `open_url` | `{"url":"https://...","browser":"chrome"}` (`browser` is `chrome` or `default`, optional) | Opens the URL in Chrome, or the default browser (also when Chrome is not found; the result says so). A `linear.app` link opens in the Linear desktop app (`linear://`) when it is installed. Only `http` and `https`, at most 2048 characters, no whitespace or control characters, and the host must pass the PC's allowlist when one is set. At most 5 per minute. |
| `media` | `{"cmd":"play_pause\|next\|prev\|stop"}` | Presses the media key. What is playing is reported separately as state `media`. |
| `mic` | `{"mode":"mute\|unmute\|toggle"}` | Sets the default communications microphone's mute flag (cap `mic`). The PC then reports `call.muted`. |
| `focus` | `{"id": 263548}` | Brings that window to the front (cap `focus`). Only an `id` the PC listed in `windows` a moment ago works; anything else answers "that window is gone". |
| `launch` | `{"name": "Stackport PRs"}` | Starts the launcher of that name (cap `launch`). The PC's launchers file (`%APPDATA%\dashlink\launchers.json`, or `--launchers`) says what it does: run a program with fixed arguments, or open a link; the panel only ever sends the name. |
| `sleep_display` | none | Turns the monitors off (`SC_MONITORPOWER`; cap `sleep_display`). Any mouse or key input turns them back on. The panel sends it from the drawer's "PC off" tile ("Monitors off"); the PC then reports `display: "off"`. |
| `sleep_pc` | none | Puts the PC to sleep (suspend, not hibernate; cap `sleep_pc`), about a second after answering, so the result gets out before the link drops. The panel sends it from the drawer's "PC off" tile ("Sleep"). The panel cannot wake the PC again: with the PC asleep there is no link. |
| `wake_display` | none | Wakes a blanked monitor with a one-pixel mouse nudge (cap `wake_display`). It shows the lock screen if the PC is locked; it never unlocks it. The panel sends it when you touch the panel while the PC's monitor is off. |

The PC maps an action **name** to its own code. The panel never sends commands to run. An unknown name answers
`R{"ok":false,"msg":"unknown action"}`.

## v0 scope

v0 is a proof that two-way communication works: pairing, liveness, the focused window PC -> panel, and actions panel -> PC.
Deliberately **not** in v0: encryption, now-playing metadata, Linux/KDE adapters.

## Planned

`dock` (per-application button profiles pushed to the panel, presses sent back as actions), PC monitoring values, panel
discovery over UDP broadcast, TLS, and a separate bulk channel for camera/monitor pixels. Those add message types or
`caps`; they do not change the framing.
