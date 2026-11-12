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

### Actions (panel -> PC)

| `name` | `args` | Effect on the PC |
|---|---|---|
| `open_url` | `{"url":"https://...","browser":"chrome"}` (`browser` is `chrome` or `default`, optional) | Opens the URL in Chrome, or the default browser (also when Chrome is not found; the result says so). A `linear.app` link opens in the Linear desktop app (`linear://`) when it is installed. Only `http` and `https`, at most 2048 characters, no whitespace or control characters, and the host must pass the PC's allowlist when one is set. At most 5 per minute. |
| `media` | `{"cmd":"play_pause\|next\|prev\|stop"}` | Presses the media key. (What is playing is not reported in v0.) |

The PC maps an action **name** to its own code. The panel never sends commands to run. An unknown name answers
`R{"ok":false,"msg":"unknown action"}`.

## v0 scope

v0 is a proof that two-way communication works: pairing, liveness, the focused window PC -> panel, and actions panel -> PC.
Deliberately **not** in v0: encryption, now-playing metadata, Linux/KDE adapters.

## Planned

`dock` (per-application button profiles pushed to the panel, presses sent back as actions), PC monitoring values, panel
discovery over UDP broadcast, TLS, and a separate bulk channel for camera/monitor pixels. Those add message types or
`caps`; they do not change the framing.
