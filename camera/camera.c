// MIPI-CSI camera capture. Derived from the vendor's video_lcd_display demo (IDF 5.5.4, app_video.c; CC0): the
// same V4L2 sequence with user-pointer buffers in PSRAM, the control bus handed in instead of created, and one
// state struct instead of globals spread over a file.

#include "camera.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_private/esp_cache_private.h" // esp_cache_get_alignment()
#include "esp_video_device.h"
#include "esp_video_init.h"
#include "esp_video_isp_user.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "linux/videodev2.h"

static const char *TAG = "camera";

#define CAMERA_MAX_BUFFERS 3
#define CAMERA_TASK_STACK 4096
#define CAMERA_TASK_PRIORITY 4

typedef struct {
    bool initialised; ///< esp_video_init() done (it is not undone: see camera_stop)
    bool running;
    volatile bool stop;
    int fd;
    uint8_t buffer_count;
    uint8_t *buffers[CAMERA_MAX_BUFFERS];
    size_t buffer_size;
    uint16_t width;
    uint16_t height;
    uint32_t frames;
    camera_frame_cb_t cb;
    void *ctx;
    TaskHandle_t task;
    SemaphoreHandle_t stopped;
} camera_t;

static camera_t s_cam = {.fd = -1};

/* ---- picture adjustments (the esp_video patch in third_party/esp_video) ---- */
static camera_adjust_t s_adjust = CAMERA_ADJUST_DEFAULT();
static float s_held_red = 1.0f; /* the automatic gains in effect when the white balance was last held */
static float s_held_blue = 1.0f;

static int clamp_int(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static esp_err_t apply_adjust(void)
{
    esp_video_isp_user_t user = ESP_VIDEO_ISP_USER_DEFAULT();
    user.exposure_ev = s_adjust.exposure / 10.0f;
    user.brightness = (s_adjust.brightness * 128) / 100; /* -50..50 -> -64..64 of the ISP's -128..127 */
    user.saturation = s_adjust.saturation / 100.0f;
    user.contrast = s_adjust.contrast / 100.0f;
    if (s_adjust.wb_hold) {
        /* Warmth moves the held gains against each other: +50 is 25 % more red and 25 % less blue. */
        const float shift = s_adjust.warmth / 200.0f;
        user.wb_manual = true;
        user.red_gain = s_held_red * (1.0f + shift);
        user.blue_gain = s_held_blue * (1.0f - shift);
    }
    return esp_video_isp_pipeline_set_user(&user);
}

esp_err_t camera_set_adjust(const camera_adjust_t *adjust)
{
    ESP_RETURN_ON_FALSE(adjust, ESP_ERR_INVALID_ARG, TAG, "null");
    camera_adjust_t next = *adjust;
    next.exposure = (int8_t)clamp_int(next.exposure, -30, 30);
    next.brightness = (int8_t)clamp_int(next.brightness, -50, 50);
    next.saturation = (uint8_t)clamp_int(next.saturation, 0, 200);
    next.contrast = (uint8_t)clamp_int(next.contrast, 50, 150);
    next.warmth = (int8_t)clamp_int(next.warmth, -50, 50);
    if (next.wb_hold && !s_adjust.wb_hold) {
        /* Holding starts from the automatic gains in effect right now. */
        esp_video_isp_user_t now;
        esp_video_isp_pipeline_get_user(&now);
        s_held_red = now.red_gain > 0.0f ? now.red_gain : 1.0f;
        s_held_blue = now.blue_gain > 0.0f ? now.blue_gain : 1.0f;
    }
    s_adjust = next;
    return apply_adjust();
}

void camera_get_adjust(camera_adjust_t *out)
{
    if (out) {
        *out = s_adjust;
    }
}

void camera_get_wb_gains(float *red, float *blue)
{
    esp_video_isp_user_t now;
    esp_video_isp_pipeline_get_user(&now);
    if (red) {
        *red = now.red_gain;
    }
    if (blue) {
        *blue = now.blue_gain;
    }
}

static void free_buffers(void)
{
    for (int i = 0; i < CAMERA_MAX_BUFFERS; i++) {
        heap_caps_free(s_cam.buffers[i]);
        s_cam.buffers[i] = NULL;
    }
}

static esp_err_t video_init(const camera_config_t *cfg)
{
    if (s_cam.initialised) {
        return ESP_OK;
    }
    esp_video_init_csi_config_t csi[] = {{
        .sccb_config =
            {
                .init_sccb = false, // the bus is the board's, already created
                .i2c_handle = cfg->i2c_bus,
                .freq = 400000,
            },
        .reset_pin = cfg->reset_gpio,
        .pwdn_pin = cfg->pwdn_gpio,
    }};
    esp_video_init_config_t init = {.csi = csi};
    ESP_RETURN_ON_ERROR(esp_video_init(&init), TAG, "esp_video_init (no sensor answering on the I2C bus?)");
    s_cam.initialised = true;
    return ESP_OK;
}

static esp_err_t open_device(void)
{
    s_cam.fd = open(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, O_RDONLY);
    ESP_RETURN_ON_FALSE(s_cam.fd >= 0, ESP_FAIL, TAG, "open %s", ESP_VIDEO_MIPI_CSI_DEVICE_NAME);

    struct v4l2_capability cap;
    ESP_RETURN_ON_FALSE(ioctl(s_cam.fd, VIDIOC_QUERYCAP, &cap) == 0, ESP_FAIL, TAG, "QUERYCAP");
    ESP_LOGI(TAG, "driver %s, card %s, bus %s", cap.driver, cap.card, cap.bus_info);

    // What the ISP can hand out, for the log (a refused format is otherwise silent).
    for (uint32_t i = 0;; i++) {
        struct v4l2_fmtdesc desc = {.index = i, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE};
        if (ioctl(s_cam.fd, VIDIOC_ENUM_FMT, &desc) != 0) {
            break;
        }
        ESP_LOGI(TAG, "offers %c%c%c%c (%s)", (char)(desc.pixelformat & 0xff), (char)((desc.pixelformat >> 8) & 0xff),
                 (char)((desc.pixelformat >> 16) & 0xff), (char)((desc.pixelformat >> 24) & 0xff), desc.description);
    }

    struct v4l2_format fmt = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE};
    ESP_RETURN_ON_FALSE(ioctl(s_cam.fd, VIDIOC_G_FMT, &fmt) == 0, ESP_FAIL, TAG, "G_FMT");
    // The driver's default is already RGB565 at the sensor's native size; the vendor's demo only calls S_FMT when
    // the default differs, and the driver refuses a redundant one here.
    if (fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_RGB565) {
        struct v4l2_format want = {
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
            .fmt.pix = {.width = fmt.fmt.pix.width, .height = fmt.fmt.pix.height, .pixelformat = V4L2_PIX_FMT_RGB565},
        };
        ESP_RETURN_ON_FALSE(ioctl(s_cam.fd, VIDIOC_S_FMT, &want) == 0, ESP_FAIL, TAG, "S_FMT RGB565 (errno %d)", errno);
        ESP_RETURN_ON_FALSE(ioctl(s_cam.fd, VIDIOC_G_FMT, &fmt) == 0, ESP_FAIL, TAG, "G_FMT");
    }
    s_cam.width = (uint16_t)fmt.fmt.pix.width;
    s_cam.height = (uint16_t)fmt.fmt.pix.height;
    ESP_LOGI(TAG, "capture %ux%u RGB565", s_cam.width, s_cam.height);
    return ESP_OK;
}

static esp_err_t queue_buffers(void)
{
    size_t align = 0;
    ESP_RETURN_ON_ERROR(esp_cache_get_alignment(MALLOC_CAP_SPIRAM, &align), TAG, "cache alignment");
    s_cam.buffer_size = (size_t)s_cam.width * s_cam.height * 2;

    struct v4l2_requestbuffers req = {
        .count = s_cam.buffer_count,
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .memory = V4L2_MEMORY_USERPTR,
    };
    ESP_RETURN_ON_FALSE(ioctl(s_cam.fd, VIDIOC_REQBUFS, &req) == 0, ESP_FAIL, TAG, "REQBUFS");

    for (int i = 0; i < s_cam.buffer_count; i++) {
        s_cam.buffers[i] = heap_caps_aligned_calloc(align, 1, s_cam.buffer_size, MALLOC_CAP_SPIRAM);
        ESP_RETURN_ON_FALSE(s_cam.buffers[i], ESP_ERR_NO_MEM, TAG, "buffer %d (%u bytes)", i, (unsigned)s_cam.buffer_size);

        struct v4l2_buffer buf = {
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
            .memory = V4L2_MEMORY_USERPTR,
            .index = (uint32_t)i,
        };
        ESP_RETURN_ON_FALSE(ioctl(s_cam.fd, VIDIOC_QUERYBUF, &buf) == 0, ESP_FAIL, TAG, "QUERYBUF %d", i);
        buf.m.userptr = (unsigned long)s_cam.buffers[i];
        buf.length = s_cam.buffer_size;
        ESP_RETURN_ON_FALSE(ioctl(s_cam.fd, VIDIOC_QBUF, &buf) == 0, ESP_FAIL, TAG, "QBUF %d", i);
    }
    return ESP_OK;
}

static void capture_task(void *arg)
{
    (void)arg;
    const int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    while (!s_cam.stop) {
        struct v4l2_buffer buf = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_USERPTR};
        if (ioctl(s_cam.fd, VIDIOC_DQBUF, &buf) != 0) {
            ESP_LOGE(TAG, "DQBUF failed");
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        camera_frame_t frame = {
            .data = s_cam.buffers[buf.index],
            .len = s_cam.buffer_size,
            .width = s_cam.width,
            .height = s_cam.height,
            .index = (uint8_t)buf.index,
        };
        s_cam.frames++;
        if (s_cam.cb) {
            s_cam.cb(&frame, s_cam.ctx);
        }
        buf.m.userptr = (unsigned long)s_cam.buffers[buf.index];
        buf.length = s_cam.buffer_size;
        if (ioctl(s_cam.fd, VIDIOC_QBUF, &buf) != 0) {
            ESP_LOGE(TAG, "QBUF failed");
        }
    }
    ioctl(s_cam.fd, VIDIOC_STREAMOFF, &type);
    xSemaphoreGive(s_cam.stopped);
    vTaskDelete(NULL);
}

esp_err_t camera_start(const camera_config_t *cfg, camera_frame_cb_t cb, void *ctx)
{
    ESP_RETURN_ON_FALSE(cfg && cfg->i2c_bus && cb, ESP_ERR_INVALID_ARG, TAG, "i2c bus and callback are required");
    ESP_RETURN_ON_FALSE(!s_cam.running, ESP_ERR_INVALID_STATE, TAG, "already running");

    s_cam.buffer_count = cfg->buffer_count ? cfg->buffer_count : 2;
    ESP_RETURN_ON_FALSE(s_cam.buffer_count >= 2 && s_cam.buffer_count <= CAMERA_MAX_BUFFERS, ESP_ERR_INVALID_ARG,
                        TAG, "buffer_count must be 2 or 3");
    if (!s_cam.stopped) {
        s_cam.stopped = xSemaphoreCreateBinary();
        ESP_RETURN_ON_FALSE(s_cam.stopped, ESP_ERR_NO_MEM, TAG, "semaphore");
    }

    esp_err_t err = video_init(cfg);
    if (err == ESP_OK) {
        err = open_device();
    }
    if (err == ESP_OK) {
        err = queue_buffers();
    }
    if (err == ESP_OK) {
        const int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        err = ioctl(s_cam.fd, VIDIOC_STREAMON, &type) == 0 ? ESP_OK : ESP_FAIL;
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "STREAMON failed");
        }
    }
    if (err == ESP_OK) {
        s_cam.cb = cb;
        s_cam.ctx = ctx;
        s_cam.stop = false;
        s_cam.frames = 0;
        BaseType_t ok = cfg->task_core >= 0
                            ? xTaskCreatePinnedToCore(capture_task, "camera", CAMERA_TASK_STACK, NULL,
                                                      CAMERA_TASK_PRIORITY, &s_cam.task, cfg->task_core)
                            : xTaskCreate(capture_task, "camera", CAMERA_TASK_STACK, NULL, CAMERA_TASK_PRIORITY,
                                          &s_cam.task);
        if (ok != pdPASS) {
            const int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            ioctl(s_cam.fd, VIDIOC_STREAMOFF, &type);
            err = ESP_ERR_NO_MEM;
        }
    }
    if (err != ESP_OK) {
        if (s_cam.fd >= 0) {
            close(s_cam.fd);
            s_cam.fd = -1;
        }
        free_buffers();
        return err;
    }
    s_cam.running = true;
    return ESP_OK;
}

esp_err_t camera_stop(void)
{
    if (!s_cam.running) {
        return ESP_OK;
    }
    s_cam.stop = true;
    // The task is blocked in DQBUF for at most one frame time (33 ms at 30 fps).
    if (xSemaphoreTake(s_cam.stopped, pdMS_TO_TICKS(2000)) != pdTRUE) {
        ESP_LOGE(TAG, "capture task did not stop");
        return ESP_ERR_TIMEOUT;
    }
    close(s_cam.fd);
    s_cam.fd = -1;
    free_buffers();
    s_cam.running = false;
    return ESP_OK;
}

bool camera_running(void) { return s_cam.running; }
uint16_t camera_width(void) { return s_cam.width; }
uint16_t camera_height(void) { return s_cam.height; }
uint32_t camera_frames(void) { return s_cam.frames; }
