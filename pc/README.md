# dashlink: the PC side of the Dash-P4 link

Links a PC to the panel over the local network, directly (no broker). **Version 0 is a proof that two-way communication
works**: pairing, liveness, the focused window (PC to panel), and two actions the panel can trigger (open a link, press a
media key, wake the monitor, turn the monitors off, put the PC to sleep). The PC also reports when its monitor goes off or on, so the panel can blank with it. Not in v0, on purpose: encryption, now-playing metadata, Linux. The wire format is in [PROTOCOL.md](PROTOCOL.md).

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

## Updating the panel's firmware

```
pio run -e jc1060p470c
python -m dashlink push-fw --host <the panel's IP> --token <pairing token>
```

It sends `.pio/build/jc1060p470c/firmware.bin` (or `--file`) over the link, the panel writes it to the idle slot, checks
it, restarts, and the command confirms the new build is the one running (or says the panel rolled back). If `dashlink`
is already running, `push-fw` uses its link through a localhost-only control port (`--control-port`, default 7341).
`--target c6` pushes a C6 co-processor image, `--force` pushes a build the panel already runs. The first time, a panel
without this feature has to be flashed over USB. See "Firmware push" in PROTOCOL.md; `fakepanel.py` does the same
steps for the tests.

`--target dashboard` pushes the Dashboard's build (`devices/Dashboard`) the same way, but straight to that panel: it
never goes through a running `dashlink` or Dash-P4. Open update mode on the Dashboard first (Settings > Update from PC),
and use its `--host` and its own `DASHLINK_TOKEN`. The Dashboard answers only the challenge, pings and the push.

## Tests

```
python -m unittest discover -s tests -v
```

Standard library only. The tests run the real client against `fakepanel.py` on localhost.
