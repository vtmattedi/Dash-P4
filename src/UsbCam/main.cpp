// USB camera firmware (env jc1060p470c_usbcam): the board's own MIPI-CSI camera (OV02C10) is previewed on the panel
// and offered to the PC as a UVC webcam on the high-speed USB-C port (stage 1b). The panel's other owner (LVGL) is
// paused for the preview, as it is for the PC's screen.

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/ppa.h"
#include "driver/uart.h"
#include "driver/usb_serial_jtag.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_private/esp_cache_private.h" // esp_cache_get_alignment()
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "board.h"
#include "camera.h"
#include "usb_camera.h"

static const char *TAG = "usb-cam";

#define ALIGN_UP(n, a) (((n) + ((a) - 1)) & ~((size_t)(a) - 1))

static ppa_client_handle_t s_ppa;
static size_t s_cache_align;
// Two output pictures take turns: the panel copy of one may still be running while the PPA writes the other.
static uint8_t *s_out[2];
static unsigned s_next;

static int64_t s_fps_start;
static unsigned s_fps_count;
static int64_t s_ppa_us; // time spent scaling / copying to the panel over the last 150 camera frames
static int64_t s_draw_us;
static unsigned s_previews; // previews drawn in the same window

// While a PC watches, the panel preview takes only every Nth frame: the scaler needs ~43 ms a frame, and the camera
// callback is also what feeds USB.
#define PREVIEW_EVERY_WHEN_STREAMING 4

// Scale a camera frame into the panel, as large as the PPA's 1/16 steps allow, centred, black around it.
static void preview(const camera_frame_t *frame)
{
    float fit = fminf((float)BOARD_LCD_H_RES / frame->width, (float)BOARD_LCD_V_RES / frame->height);
    float scale = floorf(fit * 16.0f) / 16.0f;
    uint32_t out_w = (uint32_t)(frame->width * scale);
    uint32_t out_h = (uint32_t)(frame->height * scale);

    uint8_t *out = s_out[s_next];
    s_next ^= 1;

    ppa_srm_oper_config_t op;
    memset(&op, 0, sizeof(op));
    op.in.buffer = frame->data;
    op.in.pic_w = frame->width;
    op.in.pic_h = frame->height;
    op.in.block_w = frame->width;
    op.in.block_h = frame->height;
    op.in.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
    op.out.buffer = out;
    op.out.buffer_size = ALIGN_UP((size_t)BOARD_LCD_H_RES * BOARD_LCD_V_RES * 2, s_cache_align);
    op.out.pic_w = BOARD_LCD_H_RES;
    op.out.pic_h = BOARD_LCD_V_RES;
    op.out.block_offset_x = ((BOARD_LCD_H_RES - out_w) / 2) & ~1u;
    op.out.block_offset_y = ((BOARD_LCD_V_RES - out_h) / 2) & ~1u;
    op.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
    op.rotation_angle = PPA_SRM_ROTATION_ANGLE_0;
    op.scale_x = scale;
    op.scale_y = scale;
    op.mode = PPA_TRANS_MODE_BLOCKING;

    int64_t t0 = esp_timer_get_time();
    esp_err_t err = ppa_do_scale_rotate_mirror(s_ppa, &op);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PPA scale failed: %s", esp_err_to_name(err));
        return;
    }
    int64_t t1 = esp_timer_get_time();
    err = board_display_draw_frame(out, BOARD_LCD_H_RES, BOARD_LCD_V_RES);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "panel draw failed: %s", esp_err_to_name(err));
    }
    int64_t t2 = esp_timer_get_time();
    s_ppa_us += t1 - t0;
    s_draw_us += t2 - t1;
    s_previews++;
}

// A rough read of the picture, sampled (every 8th pixel of every 8th row), to check the camera adjustments by numbers.
struct PictureStats
{
    float r, g, b;  // mean channels, 0..255
    float lumaSd;   // standard deviation of luma: contrast
    float chroma;   // mean |R-G|+|G-B|: saturation
};

static PictureStats measure(const camera_frame_t *frame)
{
    const uint16_t *px = reinterpret_cast<const uint16_t *>(frame->data);
    double sr = 0, sg = 0, sb = 0, sy = 0, syy = 0, sc = 0;
    unsigned n = 0;
    for (unsigned y = 0; y < frame->height; y += 8)
    {
        for (unsigned x = 0; x < frame->width; x += 8)
        {
            const uint16_t v = px[y * frame->width + x];
            const float r = ((v >> 11) & 0x1f) * 255.0f / 31.0f;
            const float g = ((v >> 5) & 0x3f) * 255.0f / 63.0f;
            const float b = (v & 0x1f) * 255.0f / 31.0f;
            const float luma = 0.299f * r + 0.587f * g + 0.114f * b;
            sr += r;
            sg += g;
            sb += b;
            sy += luma;
            syy += static_cast<double>(luma) * luma;
            sc += fabsf(r - g) + fabsf(g - b);
            n++;
        }
    }
    PictureStats out = {};
    if (n)
    {
        const double my = sy / n;
        out.r = static_cast<float>(sr / n);
        out.g = static_cast<float>(sg / n);
        out.b = static_cast<float>(sb / n);
        out.lumaSd = static_cast<float>(sqrt(fmax(0.0, syy / n - my * my)));
        out.chroma = static_cast<float>(sc / n);
    }
    return out;
}

static PictureStats s_picture; // the latest measurement, for the console

static void print_adjust(void)
{
    camera_adjust_t a;
    camera_get_adjust(&a);
    float red = 0, blue = 0;
    camera_get_wb_gains(&red, &blue);
    ESP_LOGI(TAG, "adjust: ev %+.1f, brightness %+d, saturation %u%%, contrast %u%%, white balance %s%s (gains r %.2f b %.2f); "
                  "picture: RGB %.0f/%.0f/%.0f, luma spread %.1f, chroma %.1f",
             a.exposure / 10.0, a.brightness, a.saturation, a.contrast, a.wb_hold ? "held" : "auto",
             a.wb_hold && a.warmth ? (a.warmth > 0 ? " warm" : " cool") : "", red, blue, s_picture.r, s_picture.g,
             s_picture.b, s_picture.lumaSd, s_picture.chroma);
}

// Serial console for trying the camera adjustments before there is a UI:
//   ev <-30..30> | br <-50..50> | sat <0..200> | con <50..150> | wb auto|hold | warm <-50..50> | reset | show
static void handle_command(char *line)
{
    char *cmd = strtok(line, " \t\r\n");
    if (!cmd)
    {
        return;
    }
    char *arg = strtok(nullptr, " \t\r\n");
    camera_adjust_t a;
    camera_get_adjust(&a);
    const int value = arg ? atoi(arg) : 0;
    if (!strcmp(cmd, "ev")) a.exposure = static_cast<int8_t>(value);
    else if (!strcmp(cmd, "br")) a.brightness = static_cast<int8_t>(value);
    else if (!strcmp(cmd, "sat")) a.saturation = static_cast<uint8_t>(value);
    else if (!strcmp(cmd, "con")) a.contrast = static_cast<uint8_t>(value);
    else if (!strcmp(cmd, "warm")) a.warmth = static_cast<int8_t>(value);
    else if (!strcmp(cmd, "wb")) a.wb_hold = arg && !strcmp(arg, "hold");
    else if (!strcmp(cmd, "reset")) a = CAMERA_ADJUST_DEFAULT();
    else if (strcmp(cmd, "show"))
    {
        ESP_LOGW(TAG, "commands: ev <-30..30> | br <-50..50> | sat <0..200> | con <50..150> | wb auto|hold | warm <-50..50> | reset | show");
        return;
    }
    if (strcmp(cmd, "show"))
    {
        camera_set_adjust(&a);
        vTaskDelay(pdMS_TO_TICKS(1500)); // let the auto loops settle before reading the picture
    }
    print_adjust();
}

static void console_task(void *)
{
    char line[64];
    unsigned len = 0;
    for (;;)
    {
        // Two ways in: the chip's own USB-Serial/JTAG port (what the board's flash/log USB-C port is, and where the log
        // goes as the secondary console) and the console UART.
        uint8_t c;
        if (usb_serial_jtag_read_bytes(&c, 1, pdMS_TO_TICKS(50)) != 1 &&
            uart_read_bytes(static_cast<uart_port_t>(CONFIG_ESP_CONSOLE_UART_NUM), &c, 1, pdMS_TO_TICKS(50)) != 1)
        {
            continue;
        }
        if (c == '\n' || c == '\r')
        {
            if (len)
            {
                line[len] = 0;
                len = 0;
                handle_command(line);
            }
        }
        else if (len < sizeof(line) - 1)
        {
            line[len++] = static_cast<char>(c);
        }
    }
}

static void on_frame(const camera_frame_t *frame, void *)
{
    // USB first: it is what a PC is waiting for.
    esp_err_t sent = usb_camera_submit(frame->data, frame->len, frame->width, frame->height);
    bool streaming = sent != ESP_ERR_INVALID_STATE;
    static unsigned skipped;
    if (!streaming || ++skipped >= PREVIEW_EVERY_WHEN_STREAMING) {
        skipped = 0;
        preview(frame);
    }

    static unsigned measureCount;
    if (++measureCount % 15 == 0) {
        s_picture = measure(frame); // about once a second, for the console's readings
    }

    if (++s_fps_count == 150) {
        int64_t now = esp_timer_get_time();
        usb_camera_stats_t usb;
        usb_camera_get_stats(&usb);
        ESP_LOGI(TAG, "camera %.1f fps; preview x%u (scale %.1f ms/frame); usb %s: sent %u, dropped busy %u, errors %u, last jpeg %u KB, encode %.1f ms",
                 150e6 / (double)(now - s_fps_start), s_previews, s_previews ? s_ppa_us / 1e3 / s_previews : 0.0,
                 usb_camera_streaming() ? "streaming" : (usb_camera_mounted() ? "mounted" : "no host"), usb.sent,
                 usb.dropped_busy, usb.errors, usb.last_jpeg / 1024, usb.encode_us_avg / 1e3);
        s_fps_start = now;
        s_fps_count = 0;
        s_ppa_us = 0;
        s_draw_us = 0;
        s_previews = 0;
    }
}

static void show_error(const char *text)
{
    if (board_lvgl_lock(0)) {
        lv_obj_t *label = lv_label_create(lv_screen_active());
        lv_label_set_text(label, text);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_28, 0);
        lv_obj_center(label);
        board_lvgl_unlock();
    }
}

extern "C" void app_main(void)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    uint32_t flash = 0;
    esp_flash_get_size(NULL, &flash);
    ESP_LOGI(TAG, "%s %s: chip rev v%d.%d, flash %u MB, psram %u MB", BOARD_NAME, BOARD_REVISION,
             chip.revision / 100, chip.revision % 100, (unsigned)(flash >> 20),
             (unsigned)(heap_caps_get_total_size(MALLOC_CAP_SPIRAM) >> 20));

    ESP_ERROR_CHECK(board_init());
    board_set_backlight(200);

    ppa_client_config_t ppa_config = {};
    ppa_config.oper_type = PPA_OPERATION_SRM;
    ESP_ERROR_CHECK(ppa_register_client(&ppa_config, &s_ppa));
    ESP_ERROR_CHECK(esp_cache_get_alignment(MALLOC_CAP_SPIRAM, &s_cache_align));
    size_t out_size = ALIGN_UP((size_t)BOARD_LCD_H_RES * BOARD_LCD_V_RES * 2, s_cache_align);
    for (auto &buffer : s_out) {
        buffer = (uint8_t *)heap_caps_aligned_calloc(s_cache_align, 1, out_size, MALLOC_CAP_SPIRAM);
        ESP_ERROR_CHECK(buffer ? ESP_OK : ESP_ERR_NO_MEM);
    }

    // USB before the camera: the PC may enumerate us at once, and an unplugged cable must not stop the preview.
    usb_camera_config_t usb_config = {};
    // usb_camera_start() registers the camera personality and starts the USB device presenting it.
    esp_err_t usb_err = usb_camera_start(&usb_config);
    if (usb_err != ESP_OK) {
        ESP_LOGE(TAG, "usb_camera_start: %s (preview only)", esp_err_to_name(usb_err));
    }

    camera_config_t config = CAMERA_CONFIG_DEFAULT();
    config.i2c_bus = board_i2c_bus();
    s_fps_start = esp_timer_get_time();

    // LVGL gives the panel up before the first frame; on failure it gets it back to say why.
    ESP_ERROR_CHECK(board_lvgl_pause(true));
    esp_err_t err = camera_start(&config, on_frame, nullptr);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "camera_start: %s", esp_err_to_name(err));
        ESP_ERROR_CHECK(board_lvgl_pause(false));
        char text[96];
        snprintf(text, sizeof(text), "Camera failed:\n%s", esp_err_to_name(err));
        show_error(text);
        return;
    }
    if (camera_width() != USB_CAMERA_WIDTH || camera_height() != USB_CAMERA_HEIGHT) {
        ESP_LOGE(TAG, "camera frames are %ux%u but the USB camera offers %dx%d: nothing will be sent to the PC",
                 camera_width(), camera_height(), USB_CAMERA_WIDTH, USB_CAMERA_HEIGHT);
    }
    // The console inputs need drivers so a task can wait for characters (log output keeps working).
    usb_serial_jtag_driver_config_t jtag_config = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    const esp_err_t jtag_err = usb_serial_jtag_driver_install(&jtag_config);
    const esp_err_t uart_err = uart_driver_install(static_cast<uart_port_t>(CONFIG_ESP_CONSOLE_UART_NUM), 512, 0, 0, nullptr, 0);
    if (jtag_err == ESP_OK || uart_err == ESP_OK)
    {
        xTaskCreate(console_task, "console", 4096, nullptr, 2, nullptr);
        ESP_LOGI(TAG, "serial console (USB-Serial/JTAG %s, UART%d %s): type 'help'", jtag_err == ESP_OK ? "ok" : "off",
                 CONFIG_ESP_CONSOLE_UART_NUM, uart_err == ESP_OK ? "ok" : "off");
    }
    else
    {
        ESP_LOGW(TAG, "no serial console (jtag %s, uart %s)", esp_err_to_name(jtag_err), esp_err_to_name(uart_err));
    }
    ESP_LOGI(TAG, "camera running %ux%u; internal heap %u KB free, PSRAM %u KB free", camera_width(), camera_height(),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) >> 10),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) >> 10));
}
