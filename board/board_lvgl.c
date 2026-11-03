#include "board_lvgl.h"
#include "board_pins.h"
#include "board_display.h"
#include "board_touch.h"

#include "esp_check.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "board_lvgl";

static lv_display_t *s_display;
static lv_indev_t *s_touch;
static volatile uint32_t s_frames;
static bool s_paused;

// The panel's copy-done interrupt, chained through board_display (which also wakes other frame producers):
// the same thing esp_lvgl_port's own callback did before board_display took it over.
static void onCopyDone(void *ctx)
{
    lv_display_flush_ready((lv_display_t *)ctx);
}

// Counts completed LVGL flushes: if this stays at 0 while the UI is built, the flush is stuck (or nothing was rendered).
static void onRefreshFinished(lv_event_t *e)
{
    (void)e;
    s_frames++;
}

// Counts consecutive failed touch reads; the bus is reset after a handful.
static void touchRead(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    static uint8_t failures;
    esp_lcd_touch_point_data_t point[1] = {0};
    uint8_t count = 0;
    esp_lcd_touch_handle_t tp = board_touch();
    if (esp_lcd_touch_read_data(tp) == ESP_OK && esp_lcd_touch_get_data(tp, point, &count, 1) == ESP_OK) {
        failures = 0;
    } else {
        count = 0;
        if (++failures == 5) {
            ESP_LOGW(TAG, "touch reads keep failing: resetting the I2C bus");
            i2c_master_bus_reset(board_i2c_bus());
        }
        if (failures > 200) {
            failures = 100; // keep trying every few seconds, without spamming
        }
    }
    if (count > 0) {
        data->point.x = point[0].x;
        data->point.y = point[0].y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

uint32_t board_lvgl_frames(void)
{
    return s_frames;
}

lv_display_t *board_lvgl_display(void)
{
    return s_display;
}

lv_indev_t *board_lvgl_touch(void)
{
    return s_touch;
}

bool board_lvgl_paused(void)
{
    return s_paused;
}

esp_err_t board_lvgl_pause(bool pause)
{
    ESP_RETURN_ON_FALSE(s_display, ESP_ERR_INVALID_STATE, TAG, "lvgl not started");
    ESP_RETURN_ON_FALSE(lvgl_port_lock(0), ESP_FAIL, TAG, "lvgl lock");
    const bool changed = pause != s_paused;
    if (changed) {
        s_paused = pause;
        if (pause) {
            // Draw what is pending now; afterwards every invalidation is ignored, so nothing is rendered.
            lv_refr_now(s_display);
            lv_display_enable_invalidation(s_display, false); // a counter in LVGL 9: kept balanced by s_paused
            if (s_touch) {
                lv_indev_enable(s_touch, false); // stops the read timer from touching the GT911
            }
        } else {
            lv_display_enable_invalidation(s_display, true);
            lv_obj_invalidate(lv_display_get_screen_active(s_display));
            lv_obj_invalidate(lv_display_get_layer_top(s_display));
            lv_obj_invalidate(lv_display_get_layer_sys(s_display));
            if (s_touch) {
                lv_indev_reset(s_touch, NULL); // forget a press that was in progress when we paused
                lv_indev_enable(s_touch, true);
            }
            lv_display_trigger_activity(s_display);
        }
    }
    lvgl_port_unlock();
    if (changed && pause) {
        // LVGL returns while its last strip (80 KB) is still being copied by DMA; that takes well under a
        // millisecond, and the next producer's first draw must not collide with it.
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    ESP_LOGI(TAG, "%s", pause ? "paused: panel handed over" : "resumed");
    return ESP_OK;
}

bool board_lvgl_lock(uint32_t timeout_ms)
{
    return lvgl_port_lock(timeout_ms);
}

void board_lvgl_unlock(void)
{
    lvgl_port_unlock();
}

esp_err_t board_lvgl_start(void)
{
    if (s_display) {
        return ESP_OK;
    }

    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_stack = 16 * 1024; // the UI builds big widget trees from callbacks
    port_cfg.task_priority = 4;
    port_cfg.task_affinity = 1;      // core 0 carries Wi-Fi/hosted; keep drawing off it
    port_cfg.task_max_sleep_ms = 20;
    port_cfg.timer_period_ms = 2;
    ESP_RETURN_ON_ERROR(lvgl_port_init(&port_cfg), TAG, "lvgl port");

    // Partial rendering: two strip buffers in PSRAM (cache-resident, see BOARD_LVGL_STRIP_LINES), flushed into the panel's one frame buffer.
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = board_display_io(),
        .panel_handle = board_display_panel(),
        .control_handle = NULL,
        .buffer_size = BOARD_LCD_H_RES * BOARD_LVGL_STRIP_LINES,
        .double_buffer = true,
        .hres = BOARD_LCD_H_RES,
        .vres = BOARD_LCD_V_RES,
        .monochrome = false,
        .rotation = {.swap_xy = false, .mirror_x = false, .mirror_y = false},
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = {
            .buff_dma = false,
            .buff_spiram = true,
            .sw_rotate = false,
            .swap_bytes = false,
            .full_refresh = false,
            .direct_mode = false,
        },
    };
    const lvgl_port_display_dsi_cfg_t dsi_cfg = {
        .flags = {.avoid_tearing = false},
    };
    ESP_LOGI(TAG, "LVGL: %d-line strips x2 in PSRAM", BOARD_LVGL_STRIP_LINES);
    s_display = lvgl_port_add_disp_dsi(&disp_cfg, &dsi_cfg);
    ESP_RETURN_ON_FALSE(s_display, ESP_FAIL, TAG, "add display");
    ESP_RETURN_ON_ERROR(board_display_claim_done_callback(onCopyDone, s_display), TAG, "copy-done callback");
    lv_display_add_event_cb(s_display, onRefreshFinished, LV_EVENT_FLUSH_FINISH, NULL);
    ESP_LOGI(TAG, "display added (%dx%d)", BOARD_LCD_H_RES, BOARD_LCD_V_RES);

    // Our own pointer device instead of lvgl_port_add_touch(): the port wraps every touch read in ESP_ERROR_CHECK, so one
    // transient I2C error (it happened after minutes of idle) aborted the whole firmware. Here a failed read is a
    // released finger, and a run of failures resets the I2C bus.
    ESP_RETURN_ON_FALSE(board_touch(), ESP_ERR_INVALID_STATE, TAG, "touch not initialised");
    ESP_RETURN_ON_FALSE(lvgl_port_lock(0), ESP_FAIL, TAG, "lvgl lock");
    s_touch = lv_indev_create();
    if (s_touch) {
        lv_indev_set_type(s_touch, LV_INDEV_TYPE_POINTER);
        lv_indev_set_display(s_touch, s_display);
        lv_indev_set_read_cb(s_touch, touchRead);
        lv_timer_set_period(lv_indev_get_read_timer(s_touch), 10); // the default 33 ms makes sliders trail the finger
    }
    lvgl_port_unlock();
    ESP_RETURN_ON_FALSE(s_touch, ESP_FAIL, TAG, "add touch");
    ESP_LOGI(TAG, "touch added");
    return ESP_OK;
}
