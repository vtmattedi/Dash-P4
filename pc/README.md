# dashlink: the PC side of the Dash-P4 link

Links a PC to the panel over the local network, directly (no broker). **Version 0 is a proof that two-way communication
works**: pairing, liveness, the focused window (PC to panel), and two actions the panel can trigger (open a link, press a
media key). Not in v0, on purpose: encryption, now-playing metadata, Linux. The wire format is in [PROTOCOL.md](PROTOCOL.md).

## Try it without the board

```
python -m dashlink.fakepanel --token demo            # stands in for the panel
python -m dashlink --host 127.0.0.1 --token demo     # in another terminal
```

`--send-url https://example.com` on the fake panel asks the PC to open that link once connected;
`--send-media play_pause` sends a media command.

## With the panel

```
python -m dashlink --host <the panel's IP> --token <pairing token>
```

The token can also come from the `DASHLINK_TOKEN` environment variable. `--allow-host github.com` (repeatable) restricts
which sites the panel may open on the PC. Window titles are sent to the panel as they are, so anything in a title is
visible on it.

The panel side (a TCP server in the firmware, a pairing token in Settings) is not written yet: today the only panel is
`fakepanel.py`, which doubles as the reference for what the firmware has to do.

## Tests

```
python -m unittest discover -s tests -v
```

Standard library only. The tests run the real client against `fakepanel.py` on localhost.
