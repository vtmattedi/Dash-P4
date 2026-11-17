#pragma once
#include <string>

/// Record `seconds` (1..10) from the board microphone and return a JSON line with the levels (rms, peak, dc offset, and the rms
/// per half second). `ok` is false when the microphone did not open or read. The recording is discarded. Blocks for the
/// recording time; not for the LVGL task.
std::string MicTest_run(int seconds, bool &ok);
