#include "board_display.h"
#include "board_pins.h"
#include "board_power.h"

#include "driver/ledc.h"
#include "esp_check.h"
#include "esp_lcd_jd9165.h"
#include "board_jd9165.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_cache.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "board_display";

#define BACKLIGHT_TIMER LEDC_TIMER_1
#define BACKLIGHT_CHANNEL LEDC_CHANNEL_1
#define BACKLIGHT_DUTY_MAX ((1 << BOARD_BACKLIGHT_PWM_BITS) - 1)

static esp_lcd_panel_handle_t s_panel;
static esp_lcd_panel_io_handle_t s_io;
static uint8_t s_backlight;
static bool s_backlight_ready;

// Given by every finished copy into the frame buffer; taken by board_display_draw_frame() before it starts one.
static SemaphoreHandle_t s_copy_idle;
static board_display_done_hook_t s_done_hook;
static void *s_done_ctx;
// A copy of 1024x600x2 bytes PSRAM to PSRAM takes a few ms; this only catches a lost interrupt.
#define COPY_TIMEOUT_MS 200

static bool onCopyDone(esp_lcd_panel_handle_t panel, esp_lcd_dpi_panel_event_data_t *edata, void *ctx)
{
    (void)panel;
    (void)edata;
    (void)ctx;
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_copy_idle, &woken);
    if (s_done_hook) {
        s_done_hook(s_done_ctx);
    }
    return woken == pdTRUE;
}

esp_err_t board_display_claim_done_callback(board_display_done_hook_t hook, void *ctx)
{
    ESP_RETURN_ON_FALSE(s_panel, ESP_ERR_INVALID_STATE, TAG, "display not initialised");
    s_done_hook = hook;
    s_done_ctx = ctx;
    const esp_lcd_dpi_panel_event_callbacks_t cbs = {.on_color_trans_done = onCopyDone};
    return esp_lcd_dpi_panel_register_event_callbacks(s_panel, &cbs, NULL);
}

esp_err_t board_display_draw_frame(const void *rgb565, uint16_t width, uint16_t height)
{
    ESP_RETURN_ON_FALSE(s_panel && s_copy_idle, ESP_ERR_INVALID_STATE, TAG, "display not initialised");
    ESP_RETURN_ON_FALSE(width == BOARD_LCD_H_RES && height == BOARD_LCD_V_RES, ESP_ERR_INVALID_SIZE, TAG,
                        "frame is %ux%u", width, height);
    if (xSemaphoreTake(s_copy_idle, pdMS_TO_TICKS(COPY_TIMEOUT_MS)) != pdTRUE) {
        // The panel's own guard still refuses an overlapping copy; carry on and let it decide.
        ESP_LOGW(TAG, "previous frame copy did not report completion");
    }
    esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel, 0, 0, width, height, rgb565);
    if (err != ESP_OK) {
        xSemaphoreGive(s_copy_idle); // nothing started, so nothing will give it back
    }
    return err;
}

esp_err_t board_display_wait_idle(void)
{
    ESP_RETURN_ON_FALSE(s_copy_idle, ESP_ERR_INVALID_STATE, TAG, "display not initialised");
    if (xSemaphoreTake(s_copy_idle, pdMS_TO_TICKS(COPY_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    xSemaphoreGive(s_copy_idle);
    return ESP_OK;
}

static esp_err_t backlight_init(void)
{
    if (s_backlight_ready) {
        return ESP_OK;
    }
    const ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = BOARD_BACKLIGHT_PWM_BITS,
        .timer_num = BACKLIGHT_TIMER,
        .freq_hz = BOARD_BACKLIGHT_PWM_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "backlight timer");
    const ledc_channel_config_t channel = {
        .gpio_num = BOARD_BACKLIGHT_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = BACKLIGHT_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = BACKLIGHT_TIMER,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&channel), TAG, "backlight channel");
    s_backlight_ready = true;
    return ESP_OK;
}

void board_set_backlight(uint8_t level)
{
    if (backlight_init() != ESP_OK) {
        return;
    }
    s_backlight = level;
    // 8-bit level onto the PWM resolution. Perceived brightness is far from
    // linear at the bottom end, which is why callers keep a floor of their own.
    const uint32_t duty = ((uint32_t)level * BACKLIGHT_DUTY_MAX) / 255;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, BACKLIGHT_CHANNEL, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, BACKLIGHT_CHANNEL);
}

uint8_t board_get_backlight(void)
{
    return s_backlight;
}

esp_lcd_panel_handle_t board_display_panel(void)
{
    return s_panel;
}

esp_lcd_panel_io_handle_t board_display_io(void)
{
    return s_io;
}

esp_err_t board_display_init(void)
{
    if (s_panel) {
        return ESP_OK;
    }

    // Backlight first and dark: the panel shows garbage until it is initialised.
    ESP_RETURN_ON_ERROR(backlight_init(), TAG, "backlight");
    board_set_backlight(0);

    ESP_RETURN_ON_ERROR(board_power_dsi_phy(), TAG, "phy power");

    esp_lcd_dsi_bus_handle_t bus = NULL;
    const esp_lcd_dsi_bus_config_t bus_cfg = {
        .bus_id = 0,
        .num_data_lanes = BOARD_LCD_DSI_LANES,
        .phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT,
        .lane_bit_rate_mbps = BOARD_LCD_DSI_LANE_MBPS,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_dsi_bus(&bus_cfg, &bus), TAG, "dsi bus");

    const esp_lcd_dbi_io_config_t dbi_cfg = {
        .virtual_channel = 0,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_dbi(bus, &dbi_cfg, &s_io), TAG, "dbi io");

    // One frame buffer in PSRAM; esp_lvgl_port copies each rendered strip into it.
    esp_lcd_dpi_panel_config_t dpi_cfg = {
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = BOARD_LCD_DPI_CLOCK_MHZ,
        .virtual_channel = 0,
        .pixel_format = LCD_COLOR_PIXEL_FORMAT_RGB565,
        .num_fbs = 1,
        .video_timing = {
            .h_size = BOARD_LCD_H_RES,
            .v_size = BOARD_LCD_V_RES,
            .hsync_pulse_width = BOARD_LCD_HSYNC_WIDTH,
            .hsync_back_porch = BOARD_LCD_HSYNC_BACK_PORCH,
            .hsync_front_porch = BOARD_LCD_HSYNC_FRONT_PORCH,
            .vsync_pulse_width = BOARD_LCD_VSYNC_WIDTH,
            .vsync_back_porch = BOARD_LCD_VSYNC_BACK_PORCH,
            .vsync_front_porch = BOARD_LCD_VSYNC_FRONT_PORCH,
        },
        .flags.use_dma2d = true,
    };
    jd9165_vendor_config_t vendor = {
        .mipi_config = {
            .dsi_bus = bus,
            .dpi_config = &dpi_cfg,
        },
    };
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = BOARD_LCD_RESET_GPIO,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = BOARD_LCD_BITS_PER_PIXEL,
        .vendor_config = &vendor,
    };
    ESP_RETURN_ON_ERROR(board_lcd_new_panel_jd9165(s_io, &panel_cfg, &s_panel), TAG, "jd9165");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "panel reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "panel init");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG, "panel on");
    s_copy_idle = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(s_copy_idle, ESP_ERR_NO_MEM, TAG, "copy semaphore");
    xSemaphoreGive(s_copy_idle);
    ESP_RETURN_ON_ERROR(board_display_claim_done_callback(NULL, NULL), TAG, "copy callback");
    // The buffer starts undefined: black, so nothing but the UI is ever scanned out.
    void *fb = NULL;
    ESP_RETURN_ON_ERROR(esp_lcd_dpi_panel_get_frame_buffer(s_panel, 1, &fb), TAG, "frame buffer");
    memset(fb, 0, BOARD_LCD_H_RES * BOARD_LCD_V_RES * 2);
    esp_cache_msync(fb, BOARD_LCD_H_RES * BOARD_LCD_V_RES * 2, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    ESP_LOGI(TAG, "JD9165 %dx%d, %d lanes @ %d Mbps, dpi %d MHz",
             BOARD_LCD_H_RES, BOARD_LCD_V_RES, BOARD_LCD_DSI_LANES, BOARD_LCD_DSI_LANE_MBPS,
             BOARD_LCD_DPI_CLOCK_MHZ);
    return ESP_OK;
}
