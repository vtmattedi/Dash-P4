#include "MicTest.h"

#include <algorithm>
#include <math.h>
#include <stdio.h>

#include "board.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

MicResult MicTest_measure(int seconds)
{
    MicResult r;
    seconds = seconds < 1 ? 1 : seconds > 10 ? 10 : seconds;
    const size_t samples = static_cast<size_t>(seconds) * BOARD_AUDIO_RATE;
    int16_t *buffer = static_cast<int16_t *>(heap_caps_malloc(samples * sizeof(int16_t), MALLOC_CAP_SPIRAM));
    if (!buffer)
    {
        r.error = "no memory for the recording";
        return r;
    }
    const esp_err_t opened = board_audio_open();
    if (opened != ESP_OK)
    {
        heap_caps_free(buffer);
        r.error = std::string("the microphone did not open: ") + esp_err_to_name(opened);
        return r;
    }
    const int got = board_audio_read(buffer, samples);
    board_audio_close();
    if (got < 0)
    {
        heap_caps_free(buffer);
        r.error = "reading the microphone failed";
        return r;
    }
    const size_t skip = BOARD_AUDIO_RATE * 3 / 10; // the start-up pop
    const size_t chunk = BOARD_AUDIO_RATE / 2;
    int64_t sum = 0, sumSq = 0;
    size_t counted = 0;
    for (size_t start = 0; start < samples; start += chunk)
    {
        const size_t from = std::max(start, skip);
        const size_t end = std::min(start + chunk, samples);
        int64_t sq = 0;
        for (size_t i = from; i < end; ++i)
        {
            const int v = buffer[i];
            sum += v;
            sumSq += static_cast<int64_t>(v) * v;
            sq += static_cast<int64_t>(v) * v;
            r.peak = std::max(r.peak, v < 0 ? -v : v);
        }
        const size_t n = end > from ? end - from : 1;
        counted += end > from ? end - from : 0;
        r.halves.push_back(static_cast<int>(sqrt(static_cast<double>(sq) / static_cast<double>(n))));
    }
    heap_caps_free(buffer);
    if (counted == 0)
        counted = 1;
    r.rms = static_cast<int>(sqrt(static_cast<double>(sumSq) / static_cast<double>(counted)));
    r.dc = static_cast<int>(sum / static_cast<int64_t>(counted));
    r.ok = true;
    return r;
}

std::string MicTest_run(int seconds, bool &ok)
{
    const MicResult r = MicTest_measure(seconds);
    ok = r.ok;
    if (!r.ok)
        return r.error;
    std::string halves;
    for (int h : r.halves)
        halves += (halves.empty() ? "" : ",") + std::to_string(h);
    char line[400];
    snprintf(line, sizeof(line), "{\"seconds\":%d,\"rms\":%d,\"peak\":%d,\"dc\":%d,\"rmsPerHalfSecond\":[%s]}", seconds, r.rms, r.peak, r.dc,
             halves.c_str());
    return line;
}
