// The one USB device of the P4's high-speed port: PHY, TinyUSB stack, task, connect/disconnect, and the dispatch of
// TinyUSB's descriptor and bus callbacks to whichever personality is presented. See usb_device.h.
// The PHY and task set-up follow esp-iot-solution examples/usb/device/usb_extend_screen/main/app_usb.c.

#include "usb_device.h"

#include "esp_check.h"
#include "esp_log.h"
#include "esp_private/usb_phy.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "tusb.h"

static const char *TAG = "usb_device";

// How long the bus stays down when switching personality. Hosts debounce a disconnect (Windows ~100 ms, others
// less); a shorter gap can look like a glitch and the host keeps the old device.
#define DISCONNECT_GAP_MS 400

static const usb_device_personality_t *s_personality[USB_DEVICE_KIND_COUNT];
static volatile usb_device_kind_t s_current = USB_DEVICE_NONE;
static bool s_started;
static usb_device_kind_t s_preferred = USB_DEVICE_KIND_COUNT; // none
static bool s_connected;
static SemaphoreHandle_t s_lock;
static usb_phy_handle_t s_phy;

const char *usb_device_kind_name(usb_device_kind_t kind)
{
    switch (kind) {
    case USB_DEVICE_NONE:
        return "none";
    case USB_DEVICE_SCREEN:
        return "screen";
    case USB_DEVICE_CAMERA:
        return "camera";
    default:
        return "?";
    }
}

esp_err_t usb_device_register(usb_device_kind_t kind, const usb_device_personality_t *personality)
{
    ESP_RETURN_ON_FALSE(kind > USB_DEVICE_NONE && kind < USB_DEVICE_KIND_COUNT && personality &&
                            personality->device_descriptor && personality->configuration_descriptor &&
                            personality->string_descriptor,
                        ESP_ERR_INVALID_ARG, TAG, "bad personality");
    s_personality[kind] = personality;
    if (s_started && s_preferred == kind && s_current != kind) {
        // The personality the firmware asked for arrived after the bus started: present it now.
        return usb_device_select(kind);
    }
    return ESP_OK;
}

usb_device_kind_t usb_device_current(void)
{
    return s_current;
}

bool usb_device_started(void)
{
    return s_started;
}

static void device_task(void *arg)
{
    (void)arg;
    for (;;) {
        tud_task();
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

void usb_device_prefer(usb_device_kind_t kind)
{
    s_preferred = kind < USB_DEVICE_KIND_COUNT ? kind : USB_DEVICE_KIND_COUNT;
}

esp_err_t usb_device_start(usb_device_kind_t initial)
{
    if (s_started) {
        return ESP_OK;
    }
    if (s_preferred < USB_DEVICE_KIND_COUNT) {
        // The firmware's preference wins; until that personality registers the bus stays off.
        initial = s_personality[s_preferred] ? s_preferred : USB_DEVICE_NONE;
    }
    ESP_RETURN_ON_FALSE(initial < USB_DEVICE_KIND_COUNT, ESP_ERR_INVALID_ARG, TAG, "bad initial personality");
    ESP_RETURN_ON_FALSE(initial == USB_DEVICE_NONE || s_personality[initial], ESP_ERR_INVALID_STATE, TAG,
                        "personality '%s' is not registered", usb_device_kind_name(initial));
    s_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_NO_MEM, TAG, "mutex");

    s_current = initial;
    ESP_RETURN_ON_ERROR(phy_init(), TAG, "USB PHY");
    ESP_RETURN_ON_FALSE(tusb_init(), ESP_FAIL, TAG, "TinyUSB device stack");
    if (initial == USB_DEVICE_NONE) {
        tud_disconnect();
        s_connected = false;
    } else {
        s_connected = true;
    }
    // Core 0: core 1 carries the decode/draw task and the UI.
    BaseType_t ok = xTaskCreatePinnedToCore(device_task, "usb_device", 4096, NULL, 6, NULL, 0);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "usb task");
    s_started = true;
    ESP_LOGI(TAG, "USB device started, presenting '%s'", usb_device_kind_name(initial));
    return ESP_OK;
}

esp_err_t usb_device_select(usb_device_kind_t kind)
{
    ESP_RETURN_ON_FALSE(s_started, ESP_ERR_INVALID_STATE, TAG, "not started");
    ESP_RETURN_ON_FALSE(kind < USB_DEVICE_KIND_COUNT, ESP_ERR_INVALID_ARG, TAG, "bad personality");
    ESP_RETURN_ON_FALSE(kind == USB_DEVICE_NONE || s_personality[kind], ESP_ERR_INVALID_STATE, TAG,
                        "personality '%s' is not registered", usb_device_kind_name(kind));

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (kind == s_current && (s_connected || kind == USB_DEVICE_NONE)) {
        xSemaphoreGive(s_lock);
        return ESP_OK;
    }
    const usb_device_kind_t old = s_current;
    const bool was_connected = s_connected;
    if (was_connected) {
        tud_disconnect();
        s_connected = false;
    }
    // The stack gets no bus event from a soft disconnect: tell the old personality it is gone.
    if (old != USB_DEVICE_NONE && s_personality[old] && s_personality[old]->on_unmount) {
        s_personality[old]->on_unmount();
    }
    s_current = kind;
    if (kind != USB_DEVICE_NONE) {
        // Let the host see the bus drop before the next device appears on it.
        vTaskDelay(pdMS_TO_TICKS(was_connected ? DISCONNECT_GAP_MS : 50));
        tud_connect();
        s_connected = true;
    }
    ESP_LOGI(TAG, "presenting '%s' (was '%s')", usb_device_kind_name(kind), usb_device_kind_name(old));
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

/************************************************** TinyUSB callbacks ***********************************************/

static const usb_device_personality_t *active(void)
{
    const usb_device_kind_t kind = s_current;
    return kind > USB_DEVICE_NONE && kind < USB_DEVICE_KIND_COUNT ? s_personality[kind] : NULL;
}

uint8_t const *tud_descriptor_device_cb(void)
{
    const usb_device_personality_t *p = active();
    return p ? p->device_descriptor() : NULL;
}

uint8_t const *tud_descriptor_configuration_cb(uint8_t index)
{
    const usb_device_personality_t *p = active();
    return p ? p->configuration_descriptor(index) : NULL;
}

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    const usb_device_personality_t *p = active();
    return p ? p->string_descriptor(index, langid) : NULL;
}

void tud_mount_cb(void)
{
    const usb_device_personality_t *p = active();
    if (p && p->on_mount) {
        p->on_mount();
    }
}

void tud_umount_cb(void)
{
    const usb_device_personality_t *p = active();
    if (p && p->on_unmount) {
        p->on_unmount();
    }
}

void tud_suspend_cb(bool remote_wakeup_en)
{
    const usb_device_personality_t *p = active();
    if (p && p->on_suspend) {
        p->on_suspend(remote_wakeup_en);
    }
}

void tud_resume_cb(void)
{
    const usb_device_personality_t *p = active();
    if (p && p->on_resume) {
        p->on_resume();
    }
}

/********************************* Class callbacks a firmware without the screen still needs ****************************/
// The HID class is always compiled in (tusb_config.h) and its two report callbacks are mandatory. usb_screen/ defines
// the real ones; these weak stubs only keep the link whole in a firmware that has no screen.

__attribute__((weak)) uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type,
                                                     uint8_t *buffer, uint16_t reqlen)
{
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)reqlen;
    return 0;
}

__attribute__((weak)) void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type,
                                                 uint8_t const *buffer, uint16_t bufsize)
{
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)bufsize;
}
