#pragma once
#include <stdint.h>
#include <string>

// The panel's end of the DashLink: a TCP server a PC connects to (pc/PROTOCOL.md). Phase 0 of the PC link: pairing with a
// token, liveness, and remembering what the PC reports. It stays up next to MQTT and the feed; only the heavy streams
// planned for later (monitor, camera) will pause the rest, and only on command.
//
// Threading: the server runs in its own task (stack in PSRAM, so it never writes flash itself). Everything below is safe to
// call from any task; LinkServer_setToken persists, so call it from the LVGL task or a console handler like the other
// settings.

struct LinkStatus
{
    bool listening = false;
    bool connected = false;
    uint16_t port = 0;
    std::string ip;           // the panel's address, for "python -m dashlink --host <ip>"
    std::string peer;         // the PC's name from its HELLO, and its address
    std::string peerAddress;
    std::string peerOs;
    std::string peerApp;
    std::string caps;         // "window,open_url,media"
    uint32_t heardAgoMs = 0;  // since the last frame from the PC
    uint32_t sessions = 0;    // accepted pairings since boot
    uint32_t rejected = 0;    // refused (bad token, bad hello) since boot
    int pcDisplay = -1;       // the PC's monitor: -1 not reported, 0 off, 1 on, 2 dimmed. Kept when the link drops.
    bool callActive = false;  // the PC reports a call (an app is capturing the microphone). Cleared when the link drops.
    bool callMuted = false;   // the PC's default microphone is muted
    std::string callApp;      // "Google Meet", "Discord", ... or the process name
    std::string window;       // the PC's focused window: "app: title", empty when none
    std::string message;      // the last thing worth telling: "waiting for a PC", "wrong token", "link lost: ..."
};

/// Load (or create) the pairing token and start the server task. Call once, on the LVGL task, after the settings store is
/// open.
void LinkServer_begin();

LinkStatus LinkServer_status();

/// Whether a PC is paired and linked right now, without copying the whole status.
bool LinkServer_connected();

/// The pairing token a PC must use.
std::string LinkServer_token();

/// Set the pairing token (4 to 32 printable characters, no spaces); an empty string makes a new random one. The PC that is
/// linked is dropped, since its proof no longer fits. False when the token is not acceptable.
bool LinkServer_setToken(const std::string &token);

/// Drop the linked PC, if any.
void LinkServer_disconnect();

/// What became of an action sent to the PC.
struct LinkResult
{
    bool known = false; // false: no such action (never sent, or too old)
    bool done = false;  // the PC answered, or the link went away first
    bool ok = false;
    std::string message;
};

/// Ask the linked PC to open a link (browser: "chrome" or "default"). Returns an id to follow with LinkServer_result(), or
/// 0 when no PC is linked. Safe from any task; the frame goes out within a fraction of a second.
uint32_t LinkServer_openUrl(const std::string &url, const std::string &browser = "chrome");

/// Ask the linked PC to turn its monitor back on. Same id / result as LinkServer_openUrl(); 0 when no PC is linked.
uint32_t LinkServer_wakePcDisplay();
/// Ask the linked PC to turn its monitors off. Same id / result as above; the PC then reports display "off".
uint32_t LinkServer_sleepPcDisplay();
/// Ask the linked PC to go to sleep (suspend). Same id / result as above. The link drops with the PC, and nothing sent
/// from here can wake it again.
uint32_t LinkServer_sleepPc();

/// The PC's call state (see LinkStatus::callActive), without copying the whole status.
struct PcCall
{
    bool active = false;
    bool muted = false;
    std::string app;
};
PcCall LinkServer_call();
/// Ask the PC to mute, unmute or toggle its default microphone ("mute", "unmute", "toggle"). Same id / result as above; the
/// PC then reports the new state. 0 when no PC is linked.
uint32_t LinkServer_micMute(const std::string &mode);

/// The PC's monitor state (see LinkStatus::pcDisplay), without copying the whole status.
int LinkServer_pcDisplay();

LinkResult LinkServer_result(uint32_t id);
