/*
 * SPDX-FileCopyrightText: 2024-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Frame reception (vendor bulk endpoint), the compressed-frame pool, hardware JPEG decoding and drawing.
 * From esp-iot-solution examples/usb/device/usb_extend_screen/main/{app_vendor.c, usb_frame.c, app_lcd.c},
 * with the Board Manager replaced by the caller's draw functions, two decode buffers, only the newest frame
 * decoded (and none while the output is off), and redraw on request.
 */

#include <inttypes.h>
#include <string.h>
#include "usb_screen_priv.h"

#include "driver/jpeg_decode.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "tusb.h"

static const char *TAG = "usb_screen";

// Compressed frames in flight between the USB task and the decoder. Each is frame_limit bytes of PSRAM.
#define FRAME_POOL_COUNT 4
// Two decode outputs: one may be on its way to the panel (DMA) while the next frame decodes into the other.
#define DECODE_BUFFER_COUNT 2
// The hardware decoder pads its output to the JPEG MCU; 16x16 (YUV420, libjpeg's default) is the worst case.
#define JPEG_MAX_MCU 16U
#define ALIGN_UP(value, alignment) (((value) + (alignment) - 1) & ~((alignment) - 1))

// -- Display packets (xfz1986 udisp protocol v2): a header, then the payload, split over bulk transfers.
#define UDISP_TYPE_RGB565 0
#define UDISP_TYPE_RGB888 1
#define UDISP_TYPE_YUV420 2
#define UDISP_TYPE_JPG 3
#define UDISP_TYPE_END 0xff // zero-length end marker the driver sends after a frame that filled whole packets

typedef struct {
    uint16_t crc16;
    uint8_t type;
    uint8_t cmd;
    uint16_t x;
    uint16_t y;
    uint16_t width;
    uint16_t height;
    uint32_t frame_id : 10;
    uint32_t payload_total : 22;
} __attribute__((packed)) udisp_frame_header_t;

typedef struct {
    uint32_t received;
    uint32_t total;
} frame_info_t;

typedef struct {
    size_t capacity;
    size_t len;
    uint8_t *data;
    frame_info_t info;
} frame_t;

static QueueHandle_t s_empty;
static QueueHandle_t s_filled;

static jpeg_decoder_handle_t s_decoder;
static jpeg_decode_cfg_t s_decode_cfg = {
    .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
    // Little-endian RGB565, the panel's (and LVGL's) layout. The reference example picks BGR for its
    // RGB565_LE panels too.
    .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
};
static uint8_t *s_out[DECODE_BUFFER_COUNT];
static size_t s_out_size[DECODE_BUFFER_COUNT];
static int s_latest = -1; // index of the newest complete decode, -1 = none yet
static volatile bool s_redraw;
static TaskHandle_t s_task;

static volatile uint32_t s_received, s_decoded, s_drawn, s_dropped, s_errors;
static volatile int64_t s_last_frame_us;
static volatile float s_fps;

/* ------------------------------------------------------------------ frame pool */

static esp_err_t pool_create(size_t frame_size)
{
    s_empty = xQueueCreate(FRAME_POOL_COUNT, sizeof(frame_t *));
    s_filled = xQueueCreate(FRAME_POOL_COUNT, sizeof(frame_t *));
    ESP_RETURN_ON_FALSE(s_empty && s_filled, ESP_ERR_NO_MEM, TAG, "frame queues");
    for (int i = 0; i < FRAME_POOL_COUNT; i++) {
        frame_t *frame = calloc(1, sizeof(frame_t));
        ESP_RETURN_ON_FALSE(frame, ESP_ERR_NO_MEM, TAG, "frame");
        size_t got = 0;
        const jpeg_decode_memory_alloc_cfg_t in_cfg = {.buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER};
        frame->data = jpeg_alloc_decoder_mem(frame_size, &in_cfg, &got);
        if (!frame->data) {
            free(frame);
            ESP_LOGE(TAG, "no memory for a %u byte frame buffer", (unsigned)frame_size);
            return ESP_ERR_NO_MEM;
        }
        frame->capacity = frame_size;
        xQueueSend(s_empty, &frame, 0);
    }
    return ESP_OK;
}

static frame_t *pool_get_empty(void)
{
    frame_t *frame = NULL;
    return xQueueReceive(s_empty, &frame, 0) == pdPASS ? frame : NULL;
}

static void pool_return_empty(frame_t *frame)
{
    frame->len = 0;
    xQueueSend(s_empty, &frame, 0);
}

static void pool_send_filled(frame_t *frame)
{
    xQueueSend(s_filled, &frame, 0);
}

/* ------------------------------------------------------------------ reception (USB task) */

// TinyUSB collects the bulk OUT packets in a FIFO. Reads never cross a frame boundary: the header is read
// on its own, then exactly the payload it announces, so the next header always starts a read even when two
// frames sit in the FIFO together.

static frame_t *s_current;    // receiving into this frame
static uint32_t s_skip_left;  // or discarding this many payload bytes

/// Start a frame from its header: receive it, or skip it.
static void begin_frame(const udisp_frame_header_t *hdr)
{
    switch (hdr->type) {
    case UDISP_TYPE_JPG:
        if (hdr->x != 0 || hdr->y != 0 || hdr->width != g_usb_screen_cfg.width ||
            hdr->height != g_usb_screen_cfg.height) {
            // The driver sends what Windows renders; another size means the mode was changed there.
            ESP_LOGW(TAG, "drop frame with area %ux%u at %u,%u (panel is %ux%u)", hdr->width, hdr->height, hdr->x,
                     hdr->y, g_usb_screen_cfg.width, g_usb_screen_cfg.height);
            break;
        }
        if (hdr->payload_total == 0 || hdr->payload_total > g_usb_screen_cfg.frame_limit) {
            ESP_LOGW(TAG, "drop frame: %" PRIu32 " bytes, limit %" PRIu32, (uint32_t)hdr->payload_total,
                     g_usb_screen_cfg.frame_limit);
            break;
        }
        usb_screen_note_frame();
        s_current = pool_get_empty();
        if (s_current) {
            s_current->info.total = hdr->payload_total;
            s_current->info.received = 0;
            return;
        }
        // The decoder is behind: drop this one whole, the next frame carries the full picture anyway.
        break;
    case UDISP_TYPE_RGB565:
    case UDISP_TYPE_RGB888:
    case UDISP_TYPE_YUV420:
        ESP_LOGW(TAG, "drop unsupported frame type %u", hdr->type);
        break;
    case UDISP_TYPE_END:
        return; // carries no payload
    default:
        // Not a header we know: the stream is out of step. Skipping its claimed length is the best guess.
        ESP_LOGW(TAG, "unknown packet type %u", hdr->type);
        break;
    }
    s_skip_left = hdr->payload_total;
    if (s_skip_left) {
        s_dropped++;
    }
}

static void frame_received(frame_t *frame, uint32_t len)
{
    frame->info.received += len;
    if (frame->info.received >= frame->info.total) {
        s_received++;
        s_last_frame_us = esp_timer_get_time();
        pool_send_filled(frame);
        s_current = NULL;
    }
}

void usb_screen_video_reset_stream(void)
{
    if (s_current) {
        pool_return_empty(s_current);
        s_current = NULL;
    }
    s_skip_left = 0;
}

void tud_vendor_rx_cb(uint8_t itf, uint8_t const *buffer, uint16_t bufsize)
{
    (void)buffer; // FIFO mode: the data is read below
    (void)bufsize;
    static uint8_t rx_buf[VENDOR_BUF_SIZE];

    for (;;) {
        const uint32_t available = tud_vendor_n_available(itf);
        if (available == 0) {
            return;
        }
        if (!s_current && s_skip_left == 0) {
            udisp_frame_header_t hdr;
            if (available < sizeof(hdr)) {
                return; // the rest of the header comes with the next packet
            }
            tud_vendor_n_read(itf, &hdr, sizeof(hdr));
            begin_frame(&hdr);
            continue;
        }
        if (s_current) {
            frame_t *frame = s_current;
            uint32_t want = frame->info.total - frame->info.received;
            if (want > available) {
                want = available;
            }
            if (frame->len + want > frame->capacity) {
                want = frame->capacity - frame->len; // cannot happen with total <= frame_limit; never overrun
            }
            const uint32_t n = tud_vendor_n_read(itf, frame->data + frame->len, want);
            if (n == 0) {
                return;
            }
            frame->len += n;
            frame_received(frame, n);
            continue;
        }
        uint32_t want = s_skip_left < sizeof(rx_buf) ? s_skip_left : sizeof(rx_buf);
        if (want > available) {
            want = available;
        }
        const uint32_t n = tud_vendor_n_read(itf, rx_buf, want);
        if (n == 0) {
            return;
        }
        s_skip_left -= n;
    }
}

/* ------------------------------------------------------------------ overlay: hand-back tab and menu */

#define RGB565(r, g, b) ((uint16_t)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3)))
#define OV_BG RGB565(0x10, 0x14, 0x18)
#define OV_CELL RGB565(0x26, 0x2D, 0x36)
#define OV_ICON RGB565(0xE8, 0xEA, 0xED)
#define OV_ACCENT RGB565(0xFF, 0x98, 0x49)
#define TAB_ALPHA 200  // of 255: the desktop shows through the tab a little
#define PANEL_ALPHA 235
#define TAB_RADIUS 8
#define PANEL_RADIUS 16
#define CELL_RADIUS 14

// What was under the overlay in a decoded buffer, so the overlay can change (tab <-> menu <-> gone) without a new
// frame from the PC. Rows are packed.
typedef struct {
    bool painted;
    uint32_t version;
    usb_screen_rect_t rect;
    uint16_t *backup;
} overlay_slot_t;
static overlay_slot_t s_ov[DECODE_BUFFER_COUNT];
// The largest overlay: the menu panel with USB_SCREEN_MENU_MAX cells.
#define OV_MAX_W (2 * USB_SCREEN_MENU_PAD + USB_SCREEN_MENU_CELL)
#define OV_MAX_H (2 * USB_SCREEN_MENU_PAD + USB_SCREEN_MENU_MAX * USB_SCREEN_MENU_CELL + \
                  (USB_SCREEN_MENU_MAX - 1) * USB_SCREEN_MENU_GAP)

static uint16_t blend565(uint16_t under, uint16_t over, uint32_t alpha)
{
    const uint32_t keep = 255 - alpha;
    const uint32_t r = (((under >> 11) & 0x1f) * keep + ((over >> 11) & 0x1f) * alpha) / 255;
    const uint32_t g = (((under >> 5) & 0x3f) * keep + ((over >> 5) & 0x3f) * alpha) / 255;
    const uint32_t b = ((under & 0x1f) * keep + (over & 0x1f) * alpha) / 255;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

/// Outside a rounded corner of a w x h box at (x, y) in its own coordinates. `flush_left` keeps the left corners square
/// (the box touches the screen edge).
static bool corner_cut(int w, int h, int r, bool flush_left, int x, int y)
{
    int cx, cy;
    if (x < r && !flush_left) {
        cx = r - 1;
    } else if (x >= w - r) {
        cx = w - r;
    } else {
        return false;
    }
    if (y < r) {
        cy = r - 1;
    } else if (y >= h - r) {
        cy = h - r;
    } else {
        return false;
    }
    const int dx = x - cx;
    const int dy = y - cy;
    return dx * dx + dy * dy > r * r;
}

static int iabs(int v)
{
    return v < 0 ? -v : v;
}

/// Is (x, y), in an 80x80 cell, part of the icon's glyph?
static bool icon_pixel(usb_screen_icon_t icon, int x, int y)
{
    const int dx = x - 40;
    const int dy = y - 40;
    switch (icon) {
    case USB_SCREEN_ICON_REST: { // a crescent moon
        const int ax = x - 38, ay = y - 40, bx = x - 49, by = y - 34;
        return ax * ax + ay * ay <= 20 * 20 && !(bx * bx + by * by <= 16 * 16);
    }
    case USB_SCREEN_ICON_SETTINGS: { // three sliders
        static const int line_y[3] = {25, 40, 55};
        static const int knob_x[3] = {28, 51, 35};
        for (int i = 0; i < 3; i++) {
            const int ky = y - line_y[i];
            const int kx = x - knob_x[i];
            if ((iabs(ky) <= 2 && x >= 16 && x <= 64) || kx * kx + ky * ky <= 8 * 8) {
                return true;
            }
        }
        return false;
    }
    case USB_SCREEN_ICON_DISCONNECT: { // the power symbol
        const int d2 = dx * dx + dy * dy;
        const bool ring = d2 >= 14 * 14 && d2 <= 19 * 19 && !(dy < 0 && iabs(dx) <= 7);
        const bool bar = iabs(dx) <= 2 && dy >= -22 && dy <= -4;
        return ring || bar;
    }
    case USB_SCREEN_ICON_CANCEL: // a cross
        return (iabs(dx - dy) <= 4 || iabs(dx + dy) <= 4) && iabs(dx) <= 18 && iabs(dy) <= 18;
    default:
        return false;
    }
}

static void paint_tab(uint16_t *px, int stride, const usb_screen_rect_t *t)
{
    const int sq = t->w < 34 ? 7 : 9; // the mark's squares
    const int gap = 4;
    const int mark = 2 * sq + gap;
    const int mx = (t->w - mark) / 2;
    const int my = (t->h - mark) / 2;
    for (int y = 0; y < t->h; y++) {
        uint16_t *row = px + (t->y + y) * stride + t->x;
        for (int x = 0; x < t->w; x++) {
            if (corner_cut(t->w, t->h, TAB_RADIUS, t->x == 0, x, y)) {
                continue;
            }
            const int ix = x - mx;
            const int iy = y - my;
            const bool in_mark = ix >= 0 && iy >= 0 && ix < mark && iy < mark && (ix < sq || ix >= sq + gap) &&
                                 (iy < sq || iy >= sq + gap);
            row[x] = in_mark ? OV_ACCENT : blend565(row[x], OV_BG, TAB_ALPHA);
        }
    }
}

static void paint_menu(uint16_t *px, int stride, const usb_screen_rect_t *panel, const usb_screen_rect_t *cells,
                       const usb_screen_icon_t *icons, int count)
{
    for (int y = 0; y < panel->h; y++) {
        uint16_t *row = px + (panel->y + y) * stride + panel->x;
        for (int x = 0; x < panel->w; x++) {
            if (corner_cut(panel->w, panel->h, PANEL_RADIUS, panel->x == 0, x, y)) {
                continue;
            }
            uint16_t c = blend565(row[x], OV_BG, PANEL_ALPHA);
            const int gx = panel->x + x;
            const int gy = panel->y + y;
            for (int i = 0; i < count; i++) {
                const int cx = gx - cells[i].x;
                const int cy = gy - cells[i].y;
                if (cx < 0 || cy < 0 || cx >= cells[i].w || cy >= cells[i].h) {
                    continue;
                }
                if (!corner_cut(cells[i].w, cells[i].h, CELL_RADIUS, false, cx, cy)) {
                    c = icon_pixel(icons[i], cx, cy) ? (icons[i] == USB_SCREEN_ICON_REST ? OV_ACCENT : OV_ICON)
                                                     : OV_CELL;
                }
                break;
            }
            row[x] = c;
        }
    }
}

/// What to show now: the menu if it is open, else the tab if there is one. False: nothing.
static bool overlay_wanted(usb_screen_rect_t *rect)
{
    if (g_usb_screen_menu_open && g_usb_screen_menu_count > 0) {
        usb_screen_rect_t cells[USB_SCREEN_MENU_MAX];
        usb_screen_menu_geometry(rect, cells);
        return true;
    }
    if (g_usb_screen_tab.w) {
        *rect = g_usb_screen_tab;
        return true;
    }
    return false;
}

static void overlay_restore(overlay_slot_t *slot, uint8_t *frame)
{
    const usb_screen_rect_t *r = &slot->rect;
    uint16_t *px = (uint16_t *)frame;
    for (int y = 0; y < r->h; y++) {
        memcpy(px + (r->y + y) * g_usb_screen_cfg.width + r->x, slot->backup + y * r->w, r->w * 2);
    }
    slot->painted = false;
}

/// Bring the overlay in `frame` (decoded buffer `index`) up to date. Waits for any copy still reading the buffer.
static void overlay_update(int index, uint8_t *frame)
{
    overlay_slot_t *slot = &s_ov[index];
    if (!slot->backup) {
        return;
    }
    const uint32_t version = g_usb_screen_overlay_version;
    if (slot->painted && slot->version == version) {
        return;
    }
    usb_screen_rect_t rect;
    const bool wanted = overlay_wanted(&rect);
    if (!slot->painted && !wanted) {
        return;
    }
    g_usb_screen_cfg.draw_idle(); // an earlier draw of this same buffer may still be copying it
    if (slot->painted) {
        overlay_restore(slot, frame);
    }
    if (!wanted) {
        return;
    }
    uint16_t *px = (uint16_t *)frame;
    for (int y = 0; y < rect.h; y++) {
        memcpy(slot->backup + y * rect.w, px + (rect.y + y) * g_usb_screen_cfg.width + rect.x, rect.w * 2);
    }
    slot->rect = rect;
    if (g_usb_screen_menu_open && g_usb_screen_menu_count > 0) {
        usb_screen_rect_t panel, cells[USB_SCREEN_MENU_MAX];
        usb_screen_menu_geometry(&panel, cells);
        paint_menu(px, g_usb_screen_cfg.width, &panel, cells, g_usb_screen_menu, g_usb_screen_menu_count);
    } else {
        paint_tab(px, g_usb_screen_cfg.width, &rect);
    }
    slot->painted = true;
    slot->version = version;
}

/* ------------------------------------------------------------------ decode + draw (own task) */

static esp_err_t output_layout(const uint8_t *jpg, size_t len, uint32_t *out_w, uint32_t *out_h)
{
    jpeg_decode_picture_info_t info = {0};
    ESP_RETURN_ON_ERROR(jpeg_decoder_get_info(jpg, len, &info), TAG, "JPEG header");
    ESP_RETURN_ON_FALSE(info.width == g_usb_screen_cfg.width && info.height == g_usb_screen_cfg.height,
                        ESP_ERR_INVALID_SIZE, TAG, "JPEG is %ux%u", (unsigned)info.width, (unsigned)info.height);
    uint32_t ah, av;
    switch (info.sample_method) {
    case JPEG_DOWN_SAMPLING_YUV444:
        ah = 8;
        av = 8;
        break;
    case JPEG_DOWN_SAMPLING_YUV422:
        ah = 16;
        av = 8;
        break;
    case JPEG_DOWN_SAMPLING_YUV420:
        ah = 16;
        av = 16;
        break;
    default:
        return ESP_ERR_NOT_SUPPORTED;
    }
    *out_w = ALIGN_UP(info.width, ah);
    *out_h = ALIGN_UP(info.height, av);
    return ESP_OK;
}

/// Decode into the buffer that is not the latest one (the latest may still be on its way to the panel).
static bool decode(const frame_t *frame)
{
    const int target = s_latest == 0 ? 1 : 0;
    const uint16_t w = g_usb_screen_cfg.width;
    const uint16_t h = g_usb_screen_cfg.height;
    uint32_t out_w, out_h;
    if (output_layout(frame->data, frame->len, &out_w, &out_h) != ESP_OK) {
        s_errors++;
        return false;
    }
    const size_t expected = out_w * out_h * 2;
    if (expected > s_out_size[target]) {
        ESP_LOGW(TAG, "JPEG output too large: %u > %u", (unsigned)expected, (unsigned)s_out_size[target]);
        s_errors++;
        return false;
    }
    uint32_t written = 0;
    esp_err_t err = jpeg_decoder_process(s_decoder, &s_decode_cfg, frame->data, frame->len, s_out[target],
                                         s_out_size[target], &written);
    if (err != ESP_OK || written != expected) {
        ESP_LOGD(TAG, "JPEG decode failed: %s (%u bytes)", esp_err_to_name(err), (unsigned)written);
        s_errors++;
        return false;
    }
    // MCU padding on the right: pack the rows (1024 wide needs none; padding below the picture is never drawn).
    if (out_w != w) {
        uint8_t *out = s_out[target];
        const size_t row = (size_t)w * 2;
        const size_t padded = (size_t)out_w * 2;
        for (size_t y = 1; y < h; y++) {
            memmove(out + y * row, out + y * padded, row);
        }
    }
    s_latest = target;
    s_ov[target].painted = false; // a fresh decode has no overlay in it
    s_decoded++;
    return true;
}

static void count_drawn(void)
{
    static int64_t window_start;
    static uint32_t window_frames;
    const int64_t now = esp_timer_get_time();
    s_drawn++;
    if (window_start == 0) {
        window_start = now;
    }
    if (++window_frames >= 30 || now - window_start > 2000000) {
        s_fps = window_frames * 1000000.0f / (float)(now - window_start);
        window_start = now;
        window_frames = 0;
    }
}

static void video_task(void *arg)
{
    (void)arg;
    frame_t *held = NULL; // the newest compressed frame, not decoded yet
    for (;;) {
        frame_t *frame = NULL;
        if (xQueueReceive(s_filled, &frame, pdMS_TO_TICKS(50)) == pdPASS) {
            // Only the newest frame matters: each one is the whole desktop. Skip any backlog.
            do {
                if (held) {
                    pool_return_empty(held);
                }
                held = frame;
            } while (xQueueReceive(s_filled, &frame, 0) == pdPASS);
        }
        // With the output off nothing is decoded (it would only compete with the other owner of the panel for
        // PSRAM bandwidth); the held frame is decoded the moment the output comes back.
        if (!g_usb_screen_output) {
            continue;
        }
        bool fresh = false;
        if (held) {
            fresh = decode(held);
            pool_return_empty(held);
            held = NULL;
        }
        if (!fresh && !s_redraw) {
            continue;
        }
        xSemaphoreTake(g_usb_screen_own, portMAX_DELAY);
        if (g_usb_screen_output && s_latest >= 0) {
            s_redraw = false;
            overlay_update(s_latest, s_out[s_latest]);
            esp_err_t err = g_usb_screen_cfg.draw(s_out[s_latest], g_usb_screen_cfg.width, g_usb_screen_cfg.height);
            if (err == ESP_OK) {
                count_drawn();
            } else {
                ESP_LOGD(TAG, "draw failed: %s", esp_err_to_name(err));
                s_errors++;
            }
        }
        xSemaphoreGive(g_usb_screen_own);
        // When the PC sends faster than we decode, the queue never empties: let the idle task (and the
        // task watchdog) run.
        vTaskDelay(1);
    }
}

void usb_screen_video_request_redraw(void)
{
    s_redraw = true;
}

void usb_screen_video_stats(usb_screen_stats_t *out)
{
    out->received = s_received;
    out->decoded = s_decoded;
    out->drawn = s_drawn;
    out->dropped = s_dropped;
    out->errors = s_errors;
    out->last_frame_us = s_last_frame_us;
    // A stale rate is misleading once frames stop: report 0 after two quiet seconds.
    out->fps = (s_last_frame_us && esp_timer_get_time() - s_last_frame_us < 2000000) ? s_fps : 0.0f;
}

esp_err_t usb_screen_video_start(void)
{
    const jpeg_decode_engine_cfg_t engine_cfg = {
        .intr_priority = 1,
        .timeout_ms = 50,
    };
    ESP_RETURN_ON_ERROR(jpeg_new_decoder_engine(&engine_cfg, &s_decoder), TAG, "JPEG decoder");

    const size_t out_len = ALIGN_UP((size_t)g_usb_screen_cfg.width, JPEG_MAX_MCU) *
                           ALIGN_UP((size_t)g_usb_screen_cfg.height, JPEG_MAX_MCU) * 2;
    for (int i = 0; i < DECODE_BUFFER_COUNT; i++) {
        const jpeg_decode_memory_alloc_cfg_t out_cfg = {.buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER};
        s_out[i] = jpeg_alloc_decoder_mem(out_len, &out_cfg, &s_out_size[i]);
        ESP_RETURN_ON_FALSE(s_out[i], ESP_ERR_NO_MEM, TAG, "decode buffer %d (%u bytes)", i, (unsigned)out_len);
    }
    for (int i = 0; i < DECODE_BUFFER_COUNT; i++) {
        s_ov[i].backup = malloc(OV_MAX_W * OV_MAX_H * 2); // PSRAM; without it the overlay is simply not drawn
    }
    ESP_RETURN_ON_ERROR(pool_create(g_usb_screen_cfg.frame_limit), TAG, "frame pool");
    // Core 1, next to the UI it takes the panel from; the USB task is on core 0.
    BaseType_t ok = xTaskCreatePinnedToCore(video_task, "usb_screen_vid", 4096, NULL, 5, &s_task, 1);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "video task");
    return ESP_OK;
}
