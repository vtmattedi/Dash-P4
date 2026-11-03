#include "Heap.h"

#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "heap";

size_t Heap_internalFree()
{
    return heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

size_t Heap_internalLargest()
{
    return heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
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
