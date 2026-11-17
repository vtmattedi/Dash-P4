#include "LedStrip.h"

#ifdef ESP_PLATFORM
#include "board_pins.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "led_strip.h"

namespace
{
constexpr const char *TAG = "ledstrip";

led_strip_handle_t g_strip = nullptr;
SemaphoreHandle_t g_lock = nullptr;
uint32_t g_rgb = 0;
uint8_t g_brightness = BOARD_LED_STRIP_MAX_BRIGHTNESS;
bool g_off = false;         // LedStrip_off() was the last thing asked for
bool g_sleeping = false;    // the PC's monitor is off: dark, but the colour is kept for the wake
int64_t g_lastRefreshUs = 0;
uint32_t g_frame[BOARD_LED_STRIP_COUNT] = {};
bool g_useFrame = false;    // per-pixel frame (animation) instead of one colour

// Push the stored colour, scaled by the brightness, to every pixel. Caller holds g_lock.
bool show()
{
    if (g_off || g_sleeping)
    {
        return led_strip_clear(g_strip) == ESP_OK;
    }
    for (int i = 0; i < BOARD_LED_STRIP_COUNT; ++i)
    {
        const uint32_t rgb = g_useFrame ? g_frame[i] : g_rgb;
        led_strip_set_pixel(g_strip, i, ((rgb >> 16) & 0xFF) * g_brightness / 255, ((rgb >> 8) & 0xFF) * g_brightness / 255,
                            (rgb & 0xFF) * g_brightness / 255);
    }
    return led_strip_refresh(g_strip) == ESP_OK;
}

template <typename F> bool locked(F body)
{
    if (!g_strip || !g_lock || xSemaphoreTake(g_lock, pdMS_TO_TICKS(50)) != pdTRUE)
    {
        return false;
    }
    const bool ok = body();
    xSemaphoreGive(g_lock);
    return ok;
}
} // namespace

void LedStrip_begin()
{
    if (g_strip)
    {
        return;
    }
    led_strip_config_t strip = {};
    strip.strip_gpio_num = BOARD_LED_STRIP_GPIO;
    strip.max_leds = BOARD_LED_STRIP_COUNT;
    strip.led_model = LED_MODEL_WS2812;
    strip.color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB;

    led_strip_rmt_config_t rmt = {};
    rmt.clk_src = RMT_CLK_SRC_DEFAULT;
    rmt.resolution_hz = 10 * 1000 * 1000;
    rmt.flags.with_dma = false; // DMA buffers live in internal RAM, which is the scarce pool here

    if (led_strip_new_rmt_device(&strip, &rmt, &g_strip) != ESP_OK)
    {
        ESP_LOGW(TAG, "RMT device for GPIO%d not available; no LED strip", BOARD_LED_STRIP_GPIO);
        g_strip = nullptr;
        return;
    }
    g_lock = xSemaphoreCreateMutex();
    g_rgb = BOARD_LED_STRIP_BOOT_RGB;
    xSemaphoreTake(g_lock, portMAX_DELAY);
    show();
    xSemaphoreGive(g_lock);
    ESP_LOGI(TAG, "WS2812B x%d on GPIO%d, dark until LedFeedback shows something, max %d/255", BOARD_LED_STRIP_COUNT,
             BOARD_LED_STRIP_GPIO, g_brightness);
}

bool LedStrip_ready() { return g_strip != nullptr; }

bool LedStrip_setColour(uint32_t rgb)
{
    return locked([&] {
        g_rgb = rgb & 0xFFFFFF;
        g_useFrame = false;
        g_off = false;
        return show();
    });
}

bool LedStrip_setFrame(const uint32_t *rgb, int count)
{
    return locked([&] {
        for (int i = 0; i < BOARD_LED_STRIP_COUNT; ++i)
        {
            g_frame[i] = i < count ? (rgb[i] & 0xFFFFFF) : 0;
        }
        g_useFrame = true;
        g_off = false;
        return show();
    });
}

int LedStrip_count() { return BOARD_LED_STRIP_COUNT; }

bool LedStrip_setBrightness(uint8_t level)
{
    return locked([&] {
        g_brightness = level > BOARD_LED_STRIP_MAX_BRIGHTNESS ? BOARD_LED_STRIP_MAX_BRIGHTNESS : level;
        g_off = false;
        return show();
    });
}

bool LedStrip_off()
{
    return locked([] {
        g_off = true;
        return show();
    });
}

void LedStrip_tick()
{
    const int64_t now = esp_timer_get_time();
    if (now - g_lastRefreshUs < 1000 * 1000)
    {
        return;
    }
    g_lastRefreshUs = now;
    locked([] { return show(); });
}

bool LedStrip_sleep(bool sleeping)
{
    return locked([&] {
        g_sleeping = sleeping;
        return show();
    });
}

bool LedStrip_isOff() { return g_off; }
uint32_t LedStrip_colour() { return g_rgb; }
uint8_t LedStrip_brightness() { return g_brightness; }
#else
void LedStrip_tick() {}
void LedStrip_begin() {}
bool LedStrip_ready() { return false; }
bool LedStrip_setColour(uint32_t) { return false; }
bool LedStrip_setBrightness(uint8_t) { return false; }
bool LedStrip_setFrame(const uint32_t *, int) { return false; }
int LedStrip_count() { return 10; }
bool LedStrip_off() { return false; }
bool LedStrip_sleep(bool) { return false; }
bool LedStrip_isOff() { return true; }
uint32_t LedStrip_colour() { return 0; }
uint8_t LedStrip_brightness() { return 255; }
#endif
