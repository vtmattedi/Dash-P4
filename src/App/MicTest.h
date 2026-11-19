#pragma once
#include <string>
#include <vector>

struct MicResult
{
    bool ok = false;
    std::string error;
    int rms = 0;  // of the whole take, without the codec's start-up pop
    int peak = 0;
    int dc = 0;
    std::vector<int> halves; // rms per half second (the first one without the pop)
};

/// Record `seconds` (1..10) from the board microphone and measure it. The first 0.3 s are the codec settling (a loud pop)
/// and are left out of the numbers. The recording is discarded. Blocks for the recording time; not for the LVGL task.
MicResult MicTest_measure(int seconds);

/// The same as one JSON line (the MIC console command, the boot self-test). `ok` as in MicResult.
std::string MicTest_run(int seconds, bool &ok);
