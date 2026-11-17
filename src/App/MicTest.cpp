#include "MicTest.h"

#include <algorithm>
#include <math.h>
#include <stdio.h>

#include "board.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

std::string MicTest_run(int seconds, bool &ok)
{
    ok = false;
    seconds = seconds < 1 ? 1 : seconds > 10 ? 10 : seconds;
    const size_t samples = static_cast<size_t>(seconds) * BOARD_AUDIO_RATE;
    int16_t *buffer = static_cast<int16_t *>(heap_caps_malloc(samples * sizeof(int16_t), MALLOC_CAP_SPIRAM));
    if (!buffer)
    {
        return "no memory for the recording";
    }
    const esp_err_t opened = board_audio_open();
    if (opened != ESP_OK)
    {
        heap_caps_free(buffer);
        return std::string("microphone did not open: ") + esp_err_to_name(opened);
    }
    const int64_t t0 = esp_timer_get_time();
    const int got = board_audio_read(buffer, samples);
    const int64_t elapsedMs = (esp_timer_get_time() - t0) / 1000;
    board_audio_close();
    if (got < 0)
    {
        heap_caps_free(buffer);
        return "reading the microphone failed";
    }
    // The whole take, and one number per half second so speech or a tap shows up as a bump.
    int64_t sum = 0, sumSq = 0;
    int peak = 0;
    std::string perHalf;
    const size_t chunk = BOARD_AUDIO_RATE / 2;
    for (size_t start = 0; start < samples; start += chunk)
    {
        int64_t sq = 0;
        const size_t end = std::min(start + chunk, samples);
        for (size_t i = start; i < end; ++i)
        {
            const int v = buffer[i];
            sum += v;
            sumSq += static_cast<int64_t>(v) * v;
            sq += static_cast<int64_t>(v) * v;
            peak = std::max(peak, v < 0 ? -v : v);
        }
        perHalf += (perHalf.empty() ? "" : ",") + std::to_string(static_cast<int>(sqrt(static_cast<double>(sq) / static_cast<double>(end - start))));
    }
    heap_caps_free(buffer);
    char line[400];
    snprintf(line, sizeof(line), "{\"seconds\":%d,\"tookMs\":%lld,\"rms\":%d,\"peak\":%d,\"dc\":%d,\"rmsPerHalfSecond\":[%s]}", seconds,
             static_cast<long long>(elapsedMs), static_cast<int>(sqrt(static_cast<double>(sumSq) / static_cast<double>(samples))), peak,
             static_cast<int>(sum / static_cast<int64_t>(samples)), perHalf.c_str());
    ok = true;
    return line;
}
