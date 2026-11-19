#include "Heap.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "heap";

size_t Heap_internalFree()
{
    return heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

size_t Heap_internalLargest()
{
    // This one walks every block of the internal heap with the heap lock held. It is timed because a slow walk starves the
    // other tasks (the task watchdog fired with the LVGL task inside it); any call over 20 ms is logged.
    const int64_t start = esp_timer_get_time();
    const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const int64_t took = esp_timer_get_time() - start;
    if (took > 20000)
    {
        ESP_LOGW(TAG, "heap walk took %d ms", static_cast<int>(took / 1000));
    }
    return largest;
}

size_t Heap_internalMinFree()
{
    return heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

size_t Heap_psramFree()
{
    return heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
}

size_t Heap_psramTotal()
{
    return heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
}

void Heap_log(const char *tag)
{
    ESP_LOGI(TAG, "%s: internal=%uK (largest %uK, min %uK) psram=%uK", tag,
             (unsigned)(Heap_internalFree() / 1024), (unsigned)(Heap_internalLargest() / 1024),
             (unsigned)(Heap_internalMinFree() / 1024), (unsigned)(Heap_psramFree() / 1024));
}
