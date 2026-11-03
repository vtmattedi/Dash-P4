/*
 * SPDX-FileCopyrightText: 2024-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * USB stack, link state and output ownership. The PHY and task set-up follow
 * esp-iot-solution examples/usb/device/usb_extend_screen/main/app_usb.c.
 */

#include <stdio.h>
#include <string.h>
#include "usb_screen_priv.h"
#include "usb_descriptors.h"

#include "esp_check.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_private/usb_phy.h"
#include "freertos/task.h"
#include "tusb.h"

static const char *TAG = "usb_screen";

usb_screen_config_t g_usb_screen_cfg;
SemaphoreHandle_t g_usb_screen_own;
volatile bool g_usb_screen_output;
usb_screen_rect_t g_usb_screen_tab;
usb_screen_icon_t g_usb_screen_menu[USB_SCREEN_MENU_MAX];
int g_usb_screen_menu_count;
bool g_usb_screen_menu_open;
uint32_t g_usb_screen_overlay_version = 1;
static usb_screen_icon_t s_pick;

static usb_phy_handle_t s_phy;
static volatile usb_screen_link_t s_link = USB_SCREEN_DETACHED;
static volatile bool s_connected = true;
static char s_serial[13];

const char *usb_screen_link_name(usb_screen_link_t link)
{
    switch (link) {
    case USB_SCREEN_DETACHED:
        return "detached";
    case USB_SCREEN_MOUNTED:
        return "connected";
    case USB_SCREEN_STREAMING:
        return "streaming";
    }
    return "?";
}

usb_screen_link_t usb_screen_link(void)
{
    return s_link;
}

void usb_screen_emit(usb_screen_event_t event)
{
    if (g_usb_screen_cfg.on_event) {
        g_usb_screen_cfg.on_event(event, g_usb_screen_cfg.event_ctx);
    }
}

static void set_link(usb_screen_link_t link)
{
    if (s_link == link) {
        return;
    }
    ESP_LOGI(TAG, "link: %s -> %s", usb_screen_link_name(s_link), usb_screen_link_name(link));
    s_link = link;
    usb_screen_emit(USB_SCREEN_EVENT_LINK);
}

void usb_screen_connect(bool on)
{
    if (!g_usb_screen_own || on == s_connected) {
        return;
    }
    s_connected = on;
    if (on) {
        tud_connect();
    } else {
        tud_disconnect();
        // The stack gets no bus event from a soft disconnect: say it ourselves.
        set_link(USB_SCREEN_DETACHED);
    }
    ESP_LOGI(TAG, "USB %s", on ? "connected to the bus" : "removed from the bus");
}

bool usb_screen_connected(void)
{
    return s_connected;
}

void usb_screen_note_frame(void)
{
    if (s_link == USB_SCREEN_MOUNTED) {
        set_link(USB_SCREEN_STREAMING);
    }
}

void usb_screen_set_output(bool on)
{
    if (!g_usb_screen_own) {
        return;
    }
    xSemaphoreTake(g_usb_screen_own, portMAX_DELAY);
    if (g_usb_screen_output != on) {
        g_usb_screen_output = on;
        if (!on && g_usb_screen_cfg.draw_idle) {
            // The last copy may still be reading a decode buffer and writing the panel.
            g_usb_screen_cfg.draw_idle();
        }
        ESP_LOGI(TAG, "output %s", on ? "on" : "off");
    }
    xSemaphoreGive(g_usb_screen_own);
}

bool usb_screen_output(void)
{
    return g_usb_screen_output;
}

void usb_screen_redraw(void)
{
    usb_screen_video_request_redraw();
}

void usb_screen_set_tab(const usb_screen_rect_t *tab)
{
    usb_screen_rect_t next = {0};
    if (tab && tab->w && tab->h && tab->x + tab->w <= g_usb_screen_cfg.width &&
        tab->y + tab->h <= g_usb_screen_cfg.height) {
        next = *tab;
    }
    if (g_usb_screen_own) {
        xSemaphoreTake(g_usb_screen_own, portMAX_DELAY);
    }
    g_usb_screen_tab = next;
    g_usb_screen_overlay_version++;
    if (g_usb_screen_own) {
        xSemaphoreGive(g_usb_screen_own);
    }
    usb_screen_video_request_redraw();
}

void usb_screen_set_menu(const usb_screen_icon_t *items, int count)
{
    if (count > USB_SCREEN_MENU_MAX) {
        count = USB_SCREEN_MENU_MAX;
    }
    xSemaphoreTake(g_usb_screen_own, portMAX_DELAY);
    g_usb_screen_menu_count = items && count > 0 ? count : 0;
    for (int i = 0; i < g_usb_screen_menu_count; i++) {
        g_usb_screen_menu[i] = items[i];
    }
    g_usb_screen_menu_open = false;
    g_usb_screen_overlay_version++;
    xSemaphoreGive(g_usb_screen_own);
    usb_screen_video_request_redraw();
}

usb_screen_icon_t usb_screen_menu_pick(void)
{
    return s_pick;
}

void usb_screen_menu_note_pick(usb_screen_icon_t icon)
{
    s_pick = icon;
}

void usb_screen_menu_set_open(bool open)
{
    xSemaphoreTake(g_usb_screen_own, portMAX_DELAY);
    if (g_usb_screen_menu_open != (open && g_usb_screen_menu_count > 0)) {
        g_usb_screen_menu_open = open && g_usb_screen_menu_count > 0;
        g_usb_screen_overlay_version++;
    }
    xSemaphoreGive(g_usb_screen_own);
    usb_screen_video_request_redraw();
}

void usb_screen_menu_geometry(usb_screen_rect_t *panel, usb_screen_rect_t items[USB_SCREEN_MENU_MAX])
{
    const int n = g_usb_screen_menu_count;
    const int w = 2 * USB_SCREEN_MENU_PAD + USB_SCREEN_MENU_CELL;
    const int h = 2 * USB_SCREEN_MENU_PAD + n * USB_SCREEN_MENU_CELL + (n > 0 ? n - 1 : 0) * USB_SCREEN_MENU_GAP;
    const int y = ((int)g_usb_screen_cfg.height - h) / 2;
    *panel = (usb_screen_rect_t){0, (uint16_t)(y < 0 ? 0 : y), (uint16_t)w, (uint16_t)h};
    for (int i = 0; i < n; i++) {
        items[i] = (usb_screen_rect_t){USB_SCREEN_MENU_PAD,
                                       (uint16_t)(panel->y + USB_SCREEN_MENU_PAD +
                                                  i * (USB_SCREEN_MENU_CELL + USB_SCREEN_MENU_GAP)),
                                       USB_SCREEN_MENU_CELL, USB_SCREEN_MENU_CELL};
    }
}

void usb_screen_get_stats(usb_screen_stats_t *out)
{
    if (out) {
        usb_screen_video_stats(out);
    }
}

static esp_err_t phy_init(void)
{
    // The P4's high-speed OTG controller behind its UTMI PHY: the board's "high-speed" USB-C port.
    usb_phy_config_t phy_conf = {
        .controller = USB_PHY_CTRL_OTG,
        .target = USB_PHY_TARGET_UTMI,
        .otg_mode = USB_OTG_MODE_DEVICE,
        .otg_speed = USB_PHY_SPEED_HIGH,
    };
    return usb_new_phy(&phy_conf, &s_phy);
}

static void tud_device_task(void *arg)
{
    (void)arg;
    for (;;) {
        tud_task();
    }
}

esp_err_t usb_screen_start(const usb_screen_config_t *cfg)
{
    ESP_RETURN_ON_FALSE(!g_usb_screen_own, ESP_ERR_INVALID_STATE, TAG, "already started");
    ESP_RETURN_ON_FALSE(cfg && cfg->width && cfg->height && cfg->draw && cfg->draw_idle, ESP_ERR_INVALID_ARG, TAG,
                        "width, height, draw and draw_idle are required");
    ESP_RETURN_ON_FALSE(cfg->jpeg_quality >= 1 && cfg->jpeg_quality <= 10 && cfg->max_fps && cfg->frame_limit,
                        ESP_ERR_INVALID_ARG, TAG, "jpeg_quality 1..10, max_fps and frame_limit are required");
    g_usb_screen_cfg = *cfg;
    if (!g_usb_screen_cfg.serial) {
        uint8_t mac[6] = {0};
        esp_read_mac(mac, ESP_MAC_BASE);
        snprintf(s_serial, sizeof(s_serial), "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4],
                 mac[5]);
        g_usb_screen_cfg.serial = s_serial;
    }

    g_usb_screen_own = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(g_usb_screen_own, ESP_ERR_NO_MEM, TAG, "mutex");

    const usb_descriptors_config_t desc = {
        .width = cfg->width,
        .height = cfg->height,
        .touch = cfg->touch != NULL,
        .jpeg_quality = cfg->jpeg_quality,
        .max_fps = cfg->max_fps,
        .frame_limit = cfg->frame_limit,
        .serial = g_usb_screen_cfg.serial,
    };
    ESP_RETURN_ON_ERROR(usb_descriptors_init(&desc), TAG, "descriptors");
    // Buffers and the decoder before the device appears on the bus: the PC starts streaming right after enumeration.
    ESP_RETURN_ON_ERROR(usb_screen_video_start(), TAG, "video");
    if (cfg->touch) {
        ESP_RETURN_ON_ERROR(usb_screen_touch_start(), TAG, "touch");
    }
    ESP_RETURN_ON_ERROR(phy_init(), TAG, "USB PHY");
    ESP_RETURN_ON_FALSE(tusb_init(), ESP_FAIL, TAG, "TinyUSB device stack");
    // Core 0: core 1 carries the decode/draw task and the UI.
    BaseType_t ok = xTaskCreatePinnedToCore(tud_device_task, "usb_screen", 4096, NULL, 6, NULL, 0);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "usb task");
    ESP_LOGI(TAG, "USB screen %ux%u, touch %s, jpeg q%u, %u fps max, frames up to %u KB, serial %s", cfg->width,
             cfg->height, cfg->touch ? "on" : "off", cfg->jpeg_quality, cfg->max_fps,
             (unsigned)(cfg->frame_limit / 1024), g_usb_screen_cfg.serial);
    return ESP_OK;
}

/************************************************** TinyUSB callbacks ***********************************************/

void tud_mount_cb(void)
{
    usb_screen_video_reset_stream();
    set_link(s_connected ? USB_SCREEN_MOUNTED : USB_SCREEN_DETACHED);
}

void tud_umount_cb(void)
{
    usb_screen_video_reset_stream();
    set_link(USB_SCREEN_DETACHED);
}

// The host stops the bus when it sleeps, when it powers the display down, and (without VBUS sensing) when the
// cable is pulled: all three mean "nobody is looking at this screen any more".
void tud_suspend_cb(bool remote_wakeup_en)
{
    (void)remote_wakeup_en;
    set_link(USB_SCREEN_DETACHED);
}

// Back from suspend the driver has to send a frame before the screen counts as streaming again.
void tud_resume_cb(void)
{
    set_link(tud_mounted() ? USB_SCREEN_MOUNTED : USB_SCREEN_DETACHED);
}
