#include "MicTestUi.h"

#include <algorithm>
#include <string>

#include "App/MicTest.h"
#include "Modal.h"
#include "board.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace
{
constexpr int kSeconds = 5;
bool g_running = false;
ModalId g_busy = 0;

std::string bar(int rms, int top)
{
    const int cells = top > 0 ? std::min(24, rms * 24 / top) : 0;
    return std::string(static_cast<size_t>(cells), '#');
}

std::string report(const MicResult &r)
{
    if (!r.ok)
    {
        return r.error;
    }
    std::vector<int> sorted = r.halves;
    std::sort(sorted.begin(), sorted.end());
    const int floorRms = sorted.empty() ? 0 : sorted[sorted.size() / 2];
    const int loudest = sorted.empty() ? 0 : sorted.back();
    const bool heard = loudest > floorRms * 2 + 150;
    std::string text = heard ? "Heard sound: the microphone works.\n" : "No sound above the noise floor. Try again closer, or the microphone is not reaching the codec.\n";
    text += "Noise floor " + std::to_string(floorRms) + ", loudest " + std::to_string(loudest) + ", peak " + std::to_string(r.peak) + ", offset " +
            std::to_string(r.dc) + "\n\n";
    for (size_t i = 0; i < r.halves.size(); ++i)
    {
        text += std::to_string(static_cast<double>(i) / 2.0).substr(0, 3) + " s  " + bar(r.halves[i], std::max(loudest, 1)) + "  " +
                std::to_string(r.halves[i]) + "\n";
    }
    return text;
}

void worker(void *)
{
    const MicResult result = MicTest_measure(kSeconds);
    if (board_lvgl_lock(2000))
    {
        Modal_close(g_busy);
        g_busy = 0;
        Modal_message("Microphone test", report(result), result.ok ? Tone::Info : Tone::Danger);
        g_running = false;
        board_lvgl_unlock();
    }
    vTaskDelete(nullptr);
}
} // namespace

void MicTestUi_open()
{
    if (g_running)
    {
        return;
    }
    g_running = true;
    g_busy = Modal_busy("Recording", "Talk or tap next to the board for 5 seconds...");
    if (xTaskCreate(worker, "mictest", 6144, nullptr, 2, nullptr) != pdPASS)
    {
        Modal_close(g_busy);
        g_busy = 0;
        g_running = false;
        Modal_message("Microphone test", "Could not start the test (out of memory).", Tone::Danger);
    }
}
