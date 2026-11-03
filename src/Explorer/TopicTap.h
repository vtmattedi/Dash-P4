#pragma once
#include <stdint.h>
#include <string>

#include "App/NetTypes.h"

// A bounded tail of the raw messages the panel hears, for the Monitor page and the device
// sheet's Topics tab. Newest first. Payloads are clipped and made printable: this is for
// looking at, not for parsing.

struct TapEntry
{
    uint32_t atMs = 0;
    bool retained = false;
    std::string topic;
    std::string text;   // printable, clipped to TAP_TEXT_MAX
    size_t length = 0;  // real payload length
    bool binary = false;
};

#define TAP_CAPACITY 400
#define TAP_TEXT_MAX 240

/// Whoever shows the tail (the Monitor page, a device sheet's Topics tab) holds a user for as long as it is on screen.
/// Nothing is recorded while there are no users, and the tail is freed when the last one leaves: it costs about 100 KB
/// of small allocations at capacity, which the panel cannot spare for something nobody is looking at.
void Tap_acquire();
void Tap_release();
bool Tap_active();

/// Record a message (ignored while paused or when nobody is looking).
void Tap_record(const NetMessage &message);

size_t Tap_count();
const TapEntry &Tap_at(size_t indexNewestFirst);

void Tap_setPaused(bool paused);
bool Tap_paused();
void Tap_clear();

/// Bumped whenever the tail changes.
uint32_t Tap_revision();

/// Messages recorded since the panel started, paused ones excluded.
uint32_t Tap_total();

/// True when `topic` belongs to `device` (first segment equal).
bool Tap_topicIsFor(const std::string &topic, const std::string &device);
