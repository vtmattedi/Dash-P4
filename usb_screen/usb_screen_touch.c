/*
 * SPDX-FileCopyrightText: 2024-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Touch controller -> HID multi-touch reports. From esp-iot-solution
 * examples/usb/device/usb_extend_screen/main/{app_touch.c, app_hid.c}: reads only while the output is on (the
 * controller may have another reader otherwise), endpoint recovery on the device's own root port, and the
 * hand-back gesture.
 */

#include <string.h>
#include <sys/param.h>
#include "usb_screen_priv.h"
#include "usb_descriptors.h"

#include "device/usbd_pvt.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "tusb.h"

static const char *TAG = "usb_screen";

#define TOUCH_POINTS 5
// The GT911 reports false points when read faster than this.
#define TOUCH_PERIOD_MS 20
// Hand-back gesture: this many fingers, held this long.
#define GESTURE_FINGERS 3
#define GESTURE_HOLD_US (2 * 1000 * 1000)

typedef struct {
    uint8_t press_down;
    uint8_t index;
    uint16_t x;
    uint16_t y;
    uint16_t width;
    uint16_t height;
} __attribute__((packed)) touch_point_report_t;

typedef struct {
    touch_point_report_t data[TOUCH_POINTS];
    uint8_t cnt;
} __attribute__((packed)) touch_report_t;

static QueueHandle_t s_hid_queue;
static TaskHandle_t s_hid_task;

/* ------------------------------------------------------------------ HID (sender task) */

static void hid_reset_ep(void *arg)
{
    (void)arg;
    const uint8_t ep = 0x80 | EPNUM_HID_DATA;
    usbd_edpt_stall(TUD_OPT_RHPORT, ep);
    usbd_edpt_clear_stall(TUD_OPT_RHPORT, ep);
    usbd_edpt_release(TUD_OPT_RHPORT, ep);
}

static void hid_task(void *arg)
{
    (void)arg;
    touch_report_t report;
    uint32_t timeouts = 0;
    for (;;) {
        if (xQueueReceive(s_hid_queue, &report, portMAX_DELAY) != pdPASS) {
            continue;
        }
        if (!tud_hid_ready()) {
            xQueueSend(s_hid_queue, &report, 0);
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        ulTaskNotifyTake(pdTRUE, 0);
        if (!tud_hid_n_report(0, REPORT_ID_TOUCH, &report, sizeof(report))) {
            xQueueSend(s_hid_queue, &report, 0);
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (!ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100))) {
            // The host stopped polling the endpoint: reset it and resend.
            usbd_defer_func(hid_reset_ep, NULL, false);
            xQueueSend(s_hid_queue, &report, 0);
            vTaskDelay(pdMS_TO_TICKS(10));
            timeouts++;
            if (timeouts == 1 || timeouts % 10 == 0) {
                ESP_LOGW(TAG, "HID report timeout, endpoint reset (%lu)", (unsigned long)timeouts);
            }
        } else {
            if (timeouts) {
                xQueueSend(s_hid_queue, &report, 0);
            }
            timeouts = 0;
        }
    }
}

static void hid_send(const touch_report_t *report)
{
    if (tud_suspended()) {
        tud_remote_wakeup();
    }
    xQueueOverwrite(s_hid_queue, report);
}

void tud_hid_report_complete_cb(uint8_t itf, uint8_t const *report, uint16_t len)
{
    (void)itf;
    (void)report;
    (void)len;
    if (s_hid_task) {
        xTaskNotifyGive(s_hid_task);
    }
}

uint16_t tud_hid_get_report_cb(uint8_t itf, uint8_t report_id, hid_report_type_t report_type, uint8_t *buffer,
                               uint16_t reqlen)
{
    (void)itf;
    (void)report_type;
    (void)reqlen;
    if (report_id == REPORT_ID_MAX_COUNT) {
        buffer[0] = TOUCH_POINTS;
        return 1;
    }
    return 0;
}

void tud_hid_set_report_cb(uint8_t itf, uint8_t report_id, hid_report_type_t report_type, uint8_t const *buffer,
                           uint16_t bufsize)
{
    (void)itf;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)bufsize;
}

/* ------------------------------------------------------------------ touch (reader task) */

static uint16_t scale(uint16_t v, uint16_t from_max, uint16_t to_max)
{
    if (from_max == 0 || from_max == to_max) {
        return MIN(v, to_max);
    }
    return MIN((uint32_t)v * to_max / from_max, to_max);
}

/// One read of the controller into a report. Returns the number of fingers, or -1 on a failed read.
static int read_touch(touch_report_t *report)
{
    esp_lcd_touch_handle_t tp = g_usb_screen_cfg.touch;
    esp_lcd_touch_point_data_t points[TOUCH_POINTS] = {0};
    uint8_t count = 0;
    if (esp_lcd_touch_read_data(tp) != ESP_OK ||
        esp_lcd_touch_get_data(tp, points, &count, TOUCH_POINTS) != ESP_OK) {
        return -1;
    }
    memset(report, 0, sizeof(*report));
    for (int i = 0; i < count; i++) {
        report->data[i].index = points[i].track_id;
        report->data[i].press_down = 1;
        report->data[i].x = scale(points[i].x, tp->config.x_max, g_usb_screen_cfg.width);
        report->data[i].y = scale(points[i].y, tp->config.y_max, g_usb_screen_cfg.height);
        report->data[i].width = points[i].strength;
        report->data[i].height = points[i].strength;
    }
    report->cnt = count;
    return count;
}

static bool in_tab(const usb_screen_rect_t *t, int x, int y)
{
    const int m = USB_SCREEN_TAB_TOUCH_MARGIN;
    return t->w && x >= (int)t->x - m && x < (int)(t->x + t->w) + m && y >= (int)t->y - m && y < (int)(t->y + t->h) + m;
}

static void touch_task(void *arg)
{
    (void)arg;
    bool pressed = false;          // the host was last told a finger is down
    int64_t gesture_since = 0;     // when GESTURE_FINGERS went down, 0 = not held
    bool gesture_fired = false;    // fired for this hold; wait for all fingers up
    bool tab_press = false;        // this touch started on the hand-back tab: it is ours, not the host's
    bool tab_still_on = false;     // ...and is (still) one finger on the tab
    bool menu_touching = false;    // a finger is down while the menu is open
    int menu_x = 0, menu_y = 0;    // where it was last seen
    int64_t menu_idle_since = 0;   // when the menu was opened or last touched
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(TOUCH_PERIOD_MS));
        touch_report_t report;
        int fingers = 0;
        bool active;
        usb_screen_rect_t tab;
        bool menu_open;
        int menu_count;
        usb_screen_icon_t menu_icons[USB_SCREEN_MENU_MAX];
        usb_screen_rect_t panel = {0}, cells[USB_SCREEN_MENU_MAX] = {{0}};

        xSemaphoreTake(g_usb_screen_own, portMAX_DELAY);
        active = g_usb_screen_output && usb_screen_link() != USB_SCREEN_DETACHED;
        tab = g_usb_screen_tab;
        menu_open = g_usb_screen_menu_open;
        menu_count = g_usb_screen_menu_count;
        memcpy(menu_icons, g_usb_screen_menu, sizeof(menu_icons));
        if (menu_open) {
            usb_screen_menu_geometry(&panel, cells);
        }
        if (active) {
            fingers = read_touch(&report);
        }
        xSemaphoreGive(g_usb_screen_own);

        if (!active) {
            // Output handed away: make sure the host does not keep a finger down.
            if (pressed) {
                memset(&report, 0, sizeof(report));
                hid_send(&report);
                pressed = false;
            }
            gesture_since = 0;
            gesture_fired = false;
            tab_press = false;
            menu_touching = false;
            if (menu_open) {
                usb_screen_menu_set_open(false);
            }
            continue;
        }
        if (fingers < 0) {
            continue; // a failed read: keep what the host knows, try again next period
        }

        // The menu is open: it owns every touch. Lifting the finger decides: on an icon = pick it, anywhere else =
        // close. Nothing reaches the host, and it was told "all fingers up" before the menu opened.
        if (menu_open) {
            const int64_t now = esp_timer_get_time();
            if (fingers > 0) {
                menu_touching = true;
                menu_x = report.data[0].x;
                menu_y = report.data[0].y;
                menu_idle_since = now;
            } else if (menu_touching) {
                menu_touching = false;
                int hit = -1;
                for (int i = 0; i < menu_count; i++) {
                    if (menu_x >= cells[i].x && menu_x < cells[i].x + cells[i].w && menu_y >= cells[i].y &&
                        menu_y < cells[i].y + cells[i].h) {
                        hit = i;
                    }
                }
                usb_screen_menu_set_open(false);
                if (hit >= 0 && menu_icons[hit] != USB_SCREEN_ICON_CANCEL) {
                    ESP_LOGI(TAG, "menu: icon %d", (int)menu_icons[hit]);
                    usb_screen_menu_note_pick(menu_icons[hit]);
                    usb_screen_emit(USB_SCREEN_EVENT_MENU);
                }
            } else if (now - menu_idle_since >= USB_SCREEN_MENU_IDLE_US) {
                usb_screen_menu_set_open(false);
            }
            continue;
        }

        // The tab: a tap that starts and ends on it, one finger, never reaches the host.
        if (tab_press) {
            if (fingers == 0) {
                tab_press = false;
                if (tab_still_on) {
                    ESP_LOGI(TAG, "hand-back tab");
                    if (menu_count > 0) {
                        menu_idle_since = esp_timer_get_time();
                        menu_touching = false;
                        usb_screen_menu_set_open(true);
                    } else {
                        usb_screen_emit(USB_SCREEN_EVENT_TAB);
                    }
                }
            } else {
                tab_still_on = fingers == 1 && in_tab(&tab, report.data[0].x, report.data[0].y);
            }
            continue;
        }
        if (fingers > 0 && !pressed && in_tab(&tab, report.data[0].x, report.data[0].y)) {
            tab_press = true;
            tab_still_on = fingers == 1;
            continue;
        }

        if (fingers >= GESTURE_FINGERS) {
            const int64_t now = esp_timer_get_time();
            if (gesture_since == 0) {
                gesture_since = now;
            } else if (!gesture_fired && now - gesture_since >= GESTURE_HOLD_US) {
                gesture_fired = true;
                // Lift the fingers on the host first: whoever takes the panel next starts from a clean state.
                memset(&report, 0, sizeof(report));
                hid_send(&report);
                pressed = false;
                ESP_LOGI(TAG, "hand-back gesture");
                usb_screen_emit(USB_SCREEN_EVENT_GESTURE);
                continue;
            }
        } else if (fingers == 0) {
            gesture_since = 0;
            gesture_fired = false;
        }
        if (gesture_fired) {
            continue; // swallow the rest of the hold
        }

        if (fingers > 0) {
            hid_send(&report);
            pressed = true;
        } else if (pressed) {
            hid_send(&report); // all fingers up
            pressed = false;
        }
    }
}

esp_err_t usb_screen_touch_start(void)
{
    s_hid_queue = xQueueCreate(1, sizeof(touch_report_t));
    ESP_RETURN_ON_FALSE(s_hid_queue, ESP_ERR_NO_MEM, TAG, "hid queue");
    BaseType_t ok = xTaskCreatePinnedToCore(hid_task, "usb_screen_hid", 3072, NULL, 5, &s_hid_task, 0);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "hid task");
    ok = xTaskCreatePinnedToCore(touch_task, "usb_screen_tp", 3072, NULL, 5, NULL, 0);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "touch task");
    return ESP_OK;
}
