#include "CameraMode.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "board.h"
#include "driver/ppa.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_private/esp_cache_private.h" // esp_cache_get_alignment()
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <NightMare/Config.h>

#include "Prefs.h"
#include "UI/Ui.h"
#include "usb_camera.h"
#include "usb_device.h"

static const char *TAG = "cammode";

// The picture adjustments are NightMare Configs: persisted by the library (restored in startNightMareESP()), settable
// from the network and the console like any other Config, and the single source of truth. Units as camera_adjust_t.
// Remote writes are range-checked here; the camera picks changes up in managerTask().
namespace
{
template <int Lo, int Hi> bool inRange(Config<int> &, const int &requested) { return requested >= Lo && requested <= Hi; }

Config<int> g_cfgExposure("camera:exposure", 0);       // tenths of an EV, -30..30
Config<int> g_cfgBrightness("camera:brightness", 0);   // -50..50
Config<int> g_cfgSaturation("camera:saturation", 100); // percent, 0..200
Config<int> g_cfgContrast("camera:contrast", 100);     // percent, 50..150
Config<bool> g_cfgWbHold("camera:wb_hold", false);     // hold the white balance instead of following the scene
Config<int> g_cfgWarmth("camera:warmth", 0);           // -50..50, shifts the held white balance

int clampTo(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

camera_adjust_t readConfigs()
{
    camera_adjust_t a = CAMERA_ADJUST_DEFAULT();
    a.exposure = static_cast<int8_t>(clampTo(g_cfgExposure.value(), -30, 30));
    a.brightness = static_cast<int8_t>(clampTo(g_cfgBrightness.value(), -50, 50));
    a.saturation = static_cast<uint8_t>(clampTo(g_cfgSaturation.value(), 0, 200));
    a.contrast = static_cast<uint8_t>(clampTo(g_cfgContrast.value(), 50, 150));
    a.wb_hold = g_cfgWbHold.value();
    a.warmth = static_cast<int8_t>(clampTo(g_cfgWarmth.value(), -50, 50));
    return a;
}

bool sameAdjust(const camera_adjust_t &a, const camera_adjust_t &b)
{
    return a.exposure == b.exposure && a.brightness == b.brightness && a.saturation == b.saturation &&
           a.contrast == b.contrast && a.wb_hold == b.wb_hold && a.warmth == b.warmth;
}

void writeConfigs(const camera_adjust_t &a)
{
    g_cfgExposure.set(a.exposure);
    g_cfgBrightness.set(a.brightness);
    g_cfgSaturation.set(a.saturation);
    g_cfgContrast.set(a.contrast);
    g_cfgWbHold.set(a.wb_hold);
    g_cfgWarmth.set(a.warmth);
}
} // namespace

namespace
{
constexpr size_t kPreviewBytes = static_cast<size_t>(kCameraPreviewW) * kCameraPreviewH * 2;
// The camera stays up this long after the last user is gone, so flipping between pages does not restart it.
constexpr int64_t kLingerUs = 2 * 1000 * 1000;
constexpr int64_t kRetryUs = 3 * 1000 * 1000;
// While a PC is watching, the panel preview takes every Nth frame so it never slows the USB stream down.
constexpr unsigned kPreviewEveryWhenStreaming = 2;

ppa_client_handle_t g_ppa = nullptr;
size_t g_align = 0;
uint8_t *g_buf[2] = {nullptr, nullptr};
lv_image_dsc_t g_dsc[2];
unsigned g_next = 0;

std::atomic<lv_obj_t *> g_image{nullptr};
camera_adjust_t g_adjust = CAMERA_ADJUST_DEFAULT(); // what the camera has now (a slider being dragged is ahead of the Configs)
camera_adjust_t g_seen = CAMERA_ADJUST_DEFAULT();   // the Configs as managerTask() last looked at them

std::atomic<float> g_fps{0.0f};
int64_t g_fpsStart = 0;
unsigned g_fpsCount = 0;

char g_error[48] = "";
char g_status[112] = "";

size_t alignUp(size_t n) { return (n + g_align - 1) & ~(g_align - 1); }

// Scale a frame to half size into the next preview buffer and give it to the image. Runs on the camera task.
void preview(const camera_frame_t *frame, lv_obj_t *image)
{
    if (frame->width != 2 * kCameraPreviewW || frame->height != 2 * kCameraPreviewH)
    {
        return; // not the sensor mode this preview was sized for
    }
    uint8_t *out = g_buf[g_next];

    ppa_srm_oper_config_t op;
    memset(&op, 0, sizeof(op));
    op.in.buffer = frame->data;
    op.in.pic_w = frame->width;
    op.in.pic_h = frame->height;
    op.in.block_w = frame->width;
    op.in.block_h = frame->height;
    op.in.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
    op.out.buffer = out;
    op.out.buffer_size = alignUp(kPreviewBytes);
    op.out.pic_w = kCameraPreviewW;
    op.out.pic_h = kCameraPreviewH;
    op.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
    op.rotation_angle = PPA_SRM_ROTATION_ANGLE_0;
    op.scale_x = 0.5f;
    op.scale_y = 0.5f;
    op.mode = PPA_TRANS_MODE_BLOCKING;
    if (ppa_do_scale_rotate_mirror(g_ppa, &op) != ESP_OK)
    {
        return;
    }
    // The other buffer is the one LVGL may still be drawing from; the lock keeps the switch between its frames.
    if (board_lvgl_lock(20))
    {
        lv_image_set_src(image, &g_dsc[g_next]);
        board_lvgl_unlock();
        g_next ^= 1u;
    }
}

void onFrame(const camera_frame_t *frame, void *)
{
    // The PC first: it is what is waiting.
    const bool streaming = usb_camera_streaming();
    if (streaming)
    {
        usb_camera_submit(frame->data, frame->len, frame->width, frame->height);
    }

    lv_obj_t *image = g_image.load();
    static unsigned skipped;
    if (image && (!streaming || ++skipped >= kPreviewEveryWhenStreaming))
    {
        skipped = 0;
        preview(frame, image);
    }

    if (++g_fpsCount == 30)
    {
        const int64_t now = esp_timer_get_time();
        g_fps.store(30e6f / static_cast<float>(now - g_fpsStart));
        g_fpsStart = now;
        g_fpsCount = 0;
    }
}

// Starts and stops the camera as users come and go, and follows the USB personality when someone else changes it
// (the PC tab selects the screen).
void managerTask(void *)
{
    int64_t lastWanted = 0;
    int64_t retryAt = 0;
    for (;;)
    {
        vTaskDelay(pdMS_TO_TICKS(250));
        const int64_t now = esp_timer_get_time();
        // The Configs changed (restored at startup, or written from the network): take them over. A slider being
        // dragged moves g_adjust only, so it is not undone here.
        const camera_adjust_t fromConfigs = readConfigs();
        if (!sameAdjust(fromConfigs, g_seen))
        {
            g_seen = fromConfigs;
            g_adjust = fromConfigs;
            camera_set_adjust(&g_adjust);
        }

        const bool wanted = g_image.load() != nullptr || usb_camera_streaming();
        if (wanted)
        {
            lastWanted = now;
        }

        if (wanted && !camera_running() && now >= retryAt)
        {
            const size_t internalBefore = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
            camera_config_t cfg = CAMERA_CONFIG_DEFAULT();
            cfg.i2c_bus = board_i2c_bus();
            const esp_err_t err = camera_start(&cfg, onFrame, nullptr);
            if (err == ESP_OK)
            {
                g_error[0] = 0;
                g_fpsStart = esp_timer_get_time();
                g_fpsCount = 0;
                ESP_LOGI(TAG, "camera started: internal RAM %uK -> %uK free (largest block %uK)",
                         static_cast<unsigned>(internalBefore / 1024),
                         static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                         static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024));
            }
            else
            {
                snprintf(g_error, sizeof(g_error), "Camera: %s", esp_err_to_name(err));
                ESP_LOGE(TAG, "camera_start: %s", esp_err_to_name(err));
                retryAt = now + kRetryUs;
            }
        }
        else if (!wanted && camera_running() && now - lastWanted > kLingerUs)
        {
            camera_stop();
            g_fps.store(0.0f);
            ESP_LOGI(TAG, "camera stopped");
        }

        // The saved choice follows what the port really presents.
        const bool sharing = usb_device_current() == USB_DEVICE_CAMERA;
        if (sharing != Prefs_get().camShare && board_lvgl_lock(200))
        {
            Prefs_setCamShare(sharing);
            board_lvgl_unlock();
        }
    }
}

void switchTask(void *arg)
{
    const bool toCamera = arg != nullptr;
    usb_device_select(toCamera ? USB_DEVICE_CAMERA : USB_DEVICE_SCREEN);
    vTaskDelete(nullptr);
}
} // namespace

void CameraMode_begin()
{
    const Prefs &prefs = Prefs_get();

    // The Configs hold their defaults until startNightMareESP() restores the saved values; managerTask() picks those
    // up as soon as they differ, long before the camera runs.
    g_cfgExposure.onWrite = inRange<-30, 30>;
    g_cfgBrightness.onWrite = inRange<-50, 50>;
    g_cfgSaturation.onWrite = inRange<0, 200>;
    g_cfgContrast.onWrite = inRange<50, 150>;
    g_cfgWarmth.onWrite = inRange<-50, 50>;
    g_adjust = readConfigs();
    g_seen = g_adjust;
    camera_set_adjust(&g_adjust); // kept, and applied when the camera starts

    ppa_client_config_t ppaConfig = {};
    ppaConfig.oper_type = PPA_OPERATION_SRM;
    if (ppa_register_client(&ppaConfig, &g_ppa) != ESP_OK || esp_cache_get_alignment(MALLOC_CAP_SPIRAM, &g_align) != ESP_OK)
    {
        ESP_LOGE(TAG, "no PPA client: the camera preview is unavailable");
        snprintf(g_error, sizeof(g_error), "Camera: scaler unavailable");
    }
    else
    {
        for (int i = 0; i < 2; ++i)
        {
            g_buf[i] = static_cast<uint8_t *>(heap_caps_aligned_calloc(g_align, 1, alignUp(kPreviewBytes), MALLOC_CAP_SPIRAM));
            memset(&g_dsc[i], 0, sizeof(g_dsc[i]));
            g_dsc[i].header.magic = LV_IMAGE_HEADER_MAGIC;
            g_dsc[i].header.cf = LV_COLOR_FORMAT_RGB565;
            g_dsc[i].header.w = kCameraPreviewW;
            g_dsc[i].header.h = kCameraPreviewH;
            g_dsc[i].header.stride = kCameraPreviewW * 2;
            g_dsc[i].data_size = kPreviewBytes;
            g_dsc[i].data = g_buf[i];
        }
        if (!g_buf[0] || !g_buf[1])
        {
            ESP_LOGE(TAG, "no memory for the preview");
            snprintf(g_error, sizeof(g_error), "Camera: out of memory");
            g_ppa = nullptr;
        }
    }

    // The USB port starts with the personality the user left it in; whichever of the screen and the camera registers
    // first, usb_device keeps the bus off until the preferred one is there.
    usb_device_prefer(prefs.camShare ? USB_DEVICE_CAMERA : USB_DEVICE_SCREEN);
    usb_camera_config_t usbConfig = {};
    if (usb_camera_start(&usbConfig) != ESP_OK)
    {
        ESP_LOGE(TAG, "usb_camera_start failed: the USB webcam is unavailable");
    }

    xTaskCreate(managerTask, "cammode", 4096, nullptr, 3, nullptr);
    ESP_LOGI(TAG, "camera mode ready; USB port starts as the %s", prefs.camShare ? "webcam" : "PC screen");
}

void CameraMode_setPreview(lv_obj_t *image)
{
    g_image.store(g_ppa ? image : nullptr);
}

camera_adjust_t CameraMode_adjust() { return g_adjust; }

void CameraMode_setAdjust(const camera_adjust_t &adjust, bool persist)
{
    g_adjust = adjust;
    camera_set_adjust(&g_adjust);
    camera_get_adjust(&g_adjust); // the clamped values
    if (persist)
    {
        writeConfigs(g_adjust);
        g_seen = readConfigs(); // ours: not a change from outside
    }
}

bool CameraMode_sharing() { return usb_device_current() == USB_DEVICE_CAMERA; }

void CameraMode_setSharing(bool on)
{
    if (on == CameraMode_sharing())
    {
        return;
    }
    Prefs_setCamShare(on);
    // usb_device_select() waits for the PC to notice the old device going: not on the LVGL task.
    xTaskCreate(switchTask, "usbswitch", 3072, on ? reinterpret_cast<void *>(1) : nullptr, 3, nullptr);
}

bool CameraMode_running() { return camera_running(); }

float CameraMode_fps() { return g_fps.load(); }

const char *CameraMode_status()
{
    const char *usb;
    switch (usb_device_current())
    {
    case USB_DEVICE_CAMERA:
        usb = usb_camera_streaming() ? "A PC is using the camera"
              : usb_camera_mounted() ? "Webcam connected to a PC, not in use"
                                     : "Webcam on: waiting for a PC on the high-speed USB-C port";
        break;
    case USB_DEVICE_SCREEN:
        usb = "The USB port is the PC screen. Turn the webcam on to share the camera instead";
        break;
    default:
        usb = "USB port off";
        break;
    }
    if (g_error[0])
    {
        snprintf(g_status, sizeof(g_status), "%s. %s", g_error, usb);
    }
    else
    {
        snprintf(g_status, sizeof(g_status), "%s", usb);
    }
    return g_status;
}

namespace
{
NightMareResults answer(bool ok, const std::string &text)
{
    NightMareResults res;
    res.result = ok;
    res.response = text;
    return res;
}

std::string upper(std::string text)
{
    for (char &c : text)
    {
        c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
    }
    return text;
}

std::string statusJson()
{
    const camera_adjust_t a = CameraMode_adjust();
    float red = 0, blue = 0;
    camera_get_wb_gains(&red, &blue);
    char line[420];
    snprintf(line, sizeof(line),
             "{\"running\":%s,\"fps\":%.1f,\"sharing\":%s,\"usb\":\"%s\",\"ev\":%.1f,\"br\":%d,\"sat\":%u,\"con\":%u,"
             "\"wb\":\"%s\",\"warm\":%d,\"red\":%.2f,\"blue\":%.2f}",
             CameraMode_running() ? "true" : "false", static_cast<double>(CameraMode_fps()),
             CameraMode_sharing() ? "true" : "false", usb_device_kind_name(usb_device_current()), a.exposure / 10.0,
             a.brightness, a.saturation, a.contrast, a.wb_hold ? "hold" : "auto", a.warmth, static_cast<double>(red),
             static_cast<double>(blue));
    return line;
}
} // namespace

NightMareResults CameraMode_command(const NightMareMessage &message)
{
    if (!board_lvgl_lock(500))
    {
        return answer(false, "UI busy, try again");
    }
    bool ok = true;
    std::string out;
    // args[0] is the subcommand itself (the parser fills both), so the first value is args[1].
    auto arg = [&](int i) { return message.argc > i ? message.args[i] : std::string(); };

    if (message.subcommand.empty())
    {
        out = statusJson();
    }
    else if (message.subcommand == "SHOW")
    {
        Ui_show(UI_PAGE_CAMERA);
        out = "showing the Camera tab";
    }
    else if (message.subcommand == "HOME")
    {
        Ui_show(UI_PAGE_HOME);
        out = "showing the Home tab";
    }
    else if (message.subcommand == "SHARE")
    {
        const std::string value = upper(arg(1));
        if (value == "ON" || value == "OFF")
        {
            CameraMode_setSharing(value == "ON");
            out = value == "ON" ? "USB port: webcam (switching, about half a second)" : "USB port: PC screen (switching)";
        }
        else
        {
            ok = false;
            out = "usage: CAMERA SHARE ON|OFF";
        }
    }
    else if (message.subcommand == "SET")
    {
        camera_adjust_t a = CameraMode_adjust();
        const std::string name = upper(arg(1));
        const std::string text = arg(2);
        const int value = atoi(text.c_str());
        if (name == "EV") a.exposure = static_cast<int8_t>(value);
        else if (name == "BR") a.brightness = static_cast<int8_t>(value);
        else if (name == "SAT") a.saturation = static_cast<uint8_t>(value);
        else if (name == "CON") a.contrast = static_cast<uint8_t>(value);
        else if (name == "WARM") a.warmth = static_cast<int8_t>(value);
        else if (name == "WB") a.wb_hold = upper(text) == "HOLD";
        else ok = false;
        if (ok)
        {
            CameraMode_setAdjust(a, true);
            out = statusJson();
        }
        else
        {
            out = "usage: CAMERA SET <EV|BR|SAT|CON|WARM> <value> | CAMERA SET WB AUTO|HOLD";
        }
    }
    else if (message.subcommand == "RESET")
    {
        CameraMode_setAdjust(CAMERA_ADJUST_DEFAULT(), true);
        out = statusJson();
    }
    else
    {
        ok = false;
        out = "usage: CAMERA [SHOW|HOME|SHARE ON|OFF|SET <name> <value>|RESET]";
    }
    board_lvgl_unlock();
    return answer(ok, out);
}
