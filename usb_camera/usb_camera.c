// The panel as a USB webcam: UVC over TinyUSB (descriptors in tusb/), frames JPEG-encoded by the P4's hardware encoder.
// The USB device itself (PHY, stack, bus) is usb_device/; this is its camera personality.

#include "usb_camera.h"

#include <string.h>
#include "driver/jpeg_encode.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "tusb.h"
#include "usb_camera_descriptors.h"
#include "usb_device.h"

static const char *TAG = "usb_camera";

// A JPEG of one 1288x728 frame at quality 80 is about 100-250 KB; the buffers leave room for a busy scene at high
// quality. Two of them, so one frame can be encoded while the previous one is still on its way.
#define JPEG_BUFFER_BYTES (512 * 1024)
#define ENCODE_AVG_FRAMES 30

static jpeg_encoder_handle_t s_encoder;
static uint8_t *s_jpeg[2];
static size_t s_jpeg_cap;
static uint32_t s_quality = 80;

static SemaphoreHandle_t s_lock; // guards s_inflight / s_ready / s_size
static int s_inflight = -1;      // buffer the stack is sending, -1 none
static int s_ready = -1;         // buffer encoded and waiting for the stack, -1 none
static uint32_t s_size[2];

// Defined with the hooks at the end of the file (a tentative definition: C allows it for a static const).
static const usb_device_personality_t s_personality;
static volatile bool s_mounted;
static bool s_started;
static usb_camera_stats_t s_stats;
static uint64_t s_encode_us_sum;
static uint32_t s_encode_frames;

/// Hand a buffer to the stack. Call with s_lock held. The stack reads it until the completion callback.
static bool start_transfer(int index)
{
    if (!tud_video_n_frame_xfer(0, 0, s_jpeg[index], s_size[index])) {
        return false;
    }
    s_inflight = index;
    s_stats.sent++;
    return true;
}

esp_err_t usb_camera_submit(const void *rgb565, size_t len, uint16_t width, uint16_t height)
{
    if (!s_started || !rgb565) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!tud_video_n_streaming(0, 0)) {
        s_stats.dropped_idle++;
        return ESP_ERR_INVALID_STATE;
    }
    if (width != USB_CAMERA_WIDTH || height != USB_CAMERA_HEIGHT || len < (size_t)width * height * 2) {
        s_stats.errors++;
        return ESP_ERR_INVALID_SIZE;
    }

    // Pick a buffer the stack is not using and nothing is waiting in; if there is none, the host is behind.
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int target = -1;
    if (s_ready == -1) {
        target = s_inflight == 0 ? 1 : 0;
    }
    xSemaphoreGive(s_lock);
    if (target < 0) {
        s_stats.dropped_busy++;
        return ESP_ERR_NOT_FINISHED;
    }

    const jpeg_encode_cfg_t cfg = {
        .height = height,
        .width = width,
        .src_type = JPEG_ENCODE_IN_FORMAT_RGB565,
        .sub_sample = JPEG_DOWN_SAMPLING_YUV420,
        .image_quality = s_quality,
    };
    uint32_t jpeg_size = 0;
    int64_t t0 = esp_timer_get_time();
    esp_err_t err = jpeg_encoder_process(s_encoder, &cfg, rgb565, (uint32_t)len, s_jpeg[target], (uint32_t)s_jpeg_cap,
                                         &jpeg_size);
    if (err != ESP_OK) {
        s_stats.errors++;
        ESP_LOGE(TAG, "JPEG encode failed: %s", esp_err_to_name(err));
        return err;
    }
    s_encode_us_sum += (uint64_t)(esp_timer_get_time() - t0);
    if (++s_encode_frames == ENCODE_AVG_FRAMES) {
        s_stats.encode_us_avg = (uint32_t)(s_encode_us_sum / ENCODE_AVG_FRAMES);
        s_encode_us_sum = 0;
        s_encode_frames = 0;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_size[target] = jpeg_size;
    s_stats.last_jpeg = jpeg_size;
    esp_err_t result = ESP_OK;
    if (s_inflight == -1) {
        if (!start_transfer(target)) {
            // The host closed the stream between the check and now.
            s_stats.dropped_idle++;
            result = ESP_ERR_INVALID_STATE;
        }
    } else {
        s_ready = target;
    }
    xSemaphoreGive(s_lock);
    return result;
}

void usb_camera_get_stats(usb_camera_stats_t *out)
{
    if (out) {
        *out = s_stats;
    }
}

bool usb_camera_streaming(void)
{
    return s_started && tud_video_n_streaming(0, 0);
}

bool usb_camera_mounted(void)
{
    return s_mounted;
}

esp_err_t usb_camera_start(const usb_camera_config_t *cfg)
{
    ESP_RETURN_ON_FALSE(!s_started, ESP_ERR_INVALID_STATE, TAG, "already started");
    if (cfg && cfg->jpeg_quality) {
        ESP_RETURN_ON_FALSE(cfg->jpeg_quality <= 100, ESP_ERR_INVALID_ARG, TAG, "jpeg_quality is 1..100");
        s_quality = cfg->jpeg_quality;
    }

    s_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_NO_MEM, TAG, "mutex");

    // The encoder and its output buffers before the device appears on the bus: the host opens the stream soon after.
    // Priority 1, the same as usb_screen's decoder: the encoder and the decoder share the chip's JPEG interrupt group, and
    // the second one created is refused if it asks for a different priority than the first one set.
    const jpeg_encode_engine_cfg_t engine = {.intr_priority = 1, .timeout_ms = 100};
    ESP_RETURN_ON_ERROR(jpeg_new_encoder_engine(&engine, &s_encoder), TAG, "JPEG encoder");
    const jpeg_encode_memory_alloc_cfg_t mem = {.buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER};
    for (int i = 0; i < 2; i++) {
        size_t actual = 0;
        s_jpeg[i] = jpeg_alloc_encoder_mem(JPEG_BUFFER_BYTES, &mem, &actual);
        ESP_RETURN_ON_FALSE(s_jpeg[i], ESP_ERR_NO_MEM, TAG, "JPEG output buffer %d", i);
        s_jpeg_cap = actual;
    }

    ESP_RETURN_ON_ERROR(usb_device_register(USB_DEVICE_CAMERA, &s_personality), TAG, "personality");
    // Normally the camera is what the device presents; a firmware with other personalities starts the device itself
    // first (usb_device_start(), which then wins) and selects the camera when it is wanted.
    ESP_RETURN_ON_ERROR(usb_device_start(USB_DEVICE_CAMERA), TAG, "USB device");
    s_started = true;
    ESP_LOGI(TAG, "USB camera %dx%d MJPEG q%u, JPEG buffers 2 x %u KB", USB_CAMERA_WIDTH, USB_CAMERA_HEIGHT,
             (unsigned)s_quality, (unsigned)(s_jpeg_cap / 1024));
    return ESP_OK;
}

/************************************ Personality hooks, and the video class callbacks ***************************/

static void on_mount(void)
{
    s_mounted = true;
    ESP_LOGI(TAG, "mounted (enumerated by the host)");
}

static void on_unmount(void)
{
    s_mounted = false;
    ESP_LOGI(TAG, "unmounted");
}

static void on_suspend(bool remote_wakeup_en)
{
    (void)remote_wakeup_en;
    ESP_LOGI(TAG, "bus suspended");
}

static void on_resume(void)
{
    ESP_LOGI(TAG, "bus resumed");
}

static const usb_device_personality_t s_personality = {
    .name = "camera",
    .device_descriptor = usb_camera_desc_device,
    .configuration_descriptor = usb_camera_desc_configuration,
    .string_descriptor = usb_camera_desc_string,
    .on_mount = on_mount,
    .on_unmount = on_unmount,
    .on_suspend = on_suspend,
    .on_resume = on_resume,
};

// The host chose a format and frame interval (COMMIT): log what it asked for. Only one is offered.
int tud_video_commit_cb(uint_fast8_t ctl_idx, uint_fast8_t stm_idx, video_probe_and_commit_control_t const *parameters)
{
    (void)ctl_idx;
    (void)stm_idx;
    ESP_LOGI(TAG, "host commits format %u frame %u interval %u x100ns", parameters->bFormatIndex,
             parameters->bFrameIndex, (unsigned)parameters->dwFrameInterval);
    return VIDEO_ERROR_NONE;
}

// A frame left the stack: start the one that was waiting, if any.
void tud_video_frame_xfer_complete_cb(uint_fast8_t ctl_idx, uint_fast8_t stm_idx)
{
    (void)ctl_idx;
    (void)stm_idx;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_inflight = -1;
    if (s_ready != -1) {
        int next = s_ready;
        s_ready = -1;
        if (!start_transfer(next)) {
            s_stats.errors++;
        }
    }
    xSemaphoreGive(s_lock);
}
