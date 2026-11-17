#pragma once

// The call dock: a pill floating near the bottom of the screen, over every page (also the rest screen), while the linked PC
// reports a call (Meet in a browser, Discord, Teams, Slack huddle: an app recording from the microphone). It names the app,
// shows whether the microphone is muted, and has one big button to mute or unmute it. Gone when the call ends or the link drops.
// The LED strip says the same thing (App/LedFeedback: red muted, green live).

/// Build it (hidden). Once, with the LVGL lock held.
void CallDock_create();
