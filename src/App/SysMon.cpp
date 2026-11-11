#include "SysMon.h"

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "esp_timer.h"
#endif

namespace
{
SysMonState g_state;
#ifdef ESP_PLATFORM
constexpr int64_t kPeriodUs = 2000 * 1000;
int64_t g_lastUs = 0;
#endif
} // namespace

int SysMon_usedPercent(unsigned long long freeSize, unsigned long long totalSize)
{
    if (totalSize == 0)
    {
        return -1;
    }
    if (freeSize >= totalSize)
    {
        return 0;
    }
    return static_cast<int>(100 - freeSize * 100 / totalSize);
}

#ifdef ESP_PLATFORM
void SysMon_sample()
{
    const int64_t now = esp_timer_get_time();
    if (g_lastUs != 0 && now - g_lastUs < kPeriodUs)
    {
        return;
    }
    g_lastUs = now;
    const size_t internalFree = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const size_t psramFree = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    g_state.internalFreeKb = static_cast<unsigned>(internalFree / 1024);
    g_state.psramFreeKb = static_cast<unsigned>(psramFree / 1024);
    g_state.internalUsedPct = SysMon_usedPercent(internalFree, heap_caps_get_total_size(MALLOC_CAP_INTERNAL));
    g_state.psramUsedPct = SysMon_usedPercent(psramFree, heap_caps_get_total_size(MALLOC_CAP_SPIRAM));
}
#else
void SysMon_sample() {}
#endif

SysMonState SysMon_state() { return g_state; }
