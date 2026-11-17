#include "board_audio.h"

#include <stdbool.h>

#include "board_pins.h"
#include "board_touch.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"

static const char *TAG = "board_audio";

static i2s_chan_handle_t s_rx;
static esp_codec_dev_handle_t s_dev;
static const audio_codec_data_if_t *s_data_if;
static const audio_codec_ctrl_if_t *s_ctrl_if;
static const audio_codec_gpio_if_t *s_gpio_if;
static const audio_codec_if_t *s_codec_if;
static float s_gain_db = 30.0f;

bool board_audio_is_open(void)
{
    return s_dev != NULL;
}

void board_audio_set_gain(float db)
{
    s_gain_db = db < 0 ? 0 : db > 37.5f ? 37.5f : db;
    if (s_dev) {
        esp_codec_dev_set_in_gain(s_dev, s_gain_db);
    }
}

esp_err_t board_audio_open(void)
{
    if (s_dev) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(board_i2c_bus_init(), TAG, "i2c bus");

    // Receive only; the codec's data in line is the microphone.
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(BOARD_I2S_PORT, I2S_ROLE_MASTER);
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, NULL, &s_rx), TAG, "i2s channel");
    const i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(BOARD_AUDIO_RATE),
        .slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = BOARD_I2S_MCLK_GPIO,
            .bclk = BOARD_I2S_SCLK_GPIO,
            .ws = BOARD_I2S_LRCK_GPIO,
            .dout = I2S_GPIO_UNUSED,
            .din = BOARD_I2S_DIN_GPIO,
        },
    };
    esp_err_t err = i2s_channel_init_std_mode(s_rx, &std_cfg);
    if (err == ESP_OK) {
        err = i2s_channel_enable(s_rx);
    }
    if (err != ESP_OK) {
        i2s_del_channel(s_rx);
        s_rx = NULL;
        ESP_LOGE(TAG, "i2s: %s", esp_err_to_name(err));
        return err;
    }

    const audio_codec_i2s_cfg_t i2s_cfg = {.port = BOARD_I2S_PORT, .tx_handle = NULL, .rx_handle = s_rx};
    s_data_if = audio_codec_new_i2s_data(&i2s_cfg);
    s_gpio_if = audio_codec_new_gpio();
    const audio_codec_i2c_cfg_t i2c_cfg = {.port = BOARD_I2C_PORT, .addr = ES8311_CODEC_DEFAULT_ADDR, .bus_handle = board_i2c_bus()};
    s_ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    const es8311_codec_cfg_t es_cfg = {
        .ctrl_if = s_ctrl_if,
        .gpio_if = s_gpio_if,
        .codec_mode = ESP_CODEC_DEV_TYPE_IN,
        .pa_pin = -1, // no speaker: the amplifier stays off
        .pa_reverted = false,
        .master_mode = false,
        .use_mclk = true,
        .digital_mic = false,
        .invert_mclk = false,
        .invert_sclk = false,
        .hw_gain = {.pa_voltage = 5.0, .codec_dac_voltage = 3.3},
    };
    s_codec_if = es8311_codec_new(&es_cfg);
    if (!s_data_if || !s_ctrl_if || !s_codec_if) {
        ESP_LOGE(TAG, "codec interfaces failed (data %p ctrl %p codec %p): is the ES8311 answering on I2C?", s_data_if, s_ctrl_if, s_codec_if);
        board_audio_close();
        return ESP_FAIL;
    }
    const esp_codec_dev_cfg_t dev_cfg = {.dev_type = ESP_CODEC_DEV_TYPE_IN, .codec_if = s_codec_if, .data_if = s_data_if};
    s_dev = esp_codec_dev_new(&dev_cfg);
    const esp_codec_dev_sample_info_t fs = {.bits_per_sample = 16, .channel = 1, .channel_mask = 0, .sample_rate = BOARD_AUDIO_RATE, .mclk_multiple = 0};
    if (!s_dev || esp_codec_dev_open(s_dev, &fs) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "codec open failed");
        board_audio_close();
        return ESP_FAIL;
    }
    esp_codec_dev_set_in_gain(s_dev, s_gain_db);
    ESP_LOGI(TAG, "microphone open: %d Hz mono, gain %.1f dB", BOARD_AUDIO_RATE, s_gain_db);
    return ESP_OK;
}

void board_audio_close(void)
{
    if (s_dev) {
        esp_codec_dev_close(s_dev);
        esp_codec_dev_delete(s_dev);
        s_dev = NULL;
    }
    if (s_codec_if) {
        audio_codec_delete_codec_if(s_codec_if);
        s_codec_if = NULL;
    }
    if (s_ctrl_if) {
        audio_codec_delete_ctrl_if(s_ctrl_if);
        s_ctrl_if = NULL;
    }
    if (s_gpio_if) {
        audio_codec_delete_gpio_if(s_gpio_if);
        s_gpio_if = NULL;
    }
    if (s_data_if) {
        audio_codec_delete_data_if(s_data_if);
        s_data_if = NULL;
    }
    if (s_rx) {
        i2s_channel_disable(s_rx);
        i2s_del_channel(s_rx);
        s_rx = NULL;
    }
}

int board_audio_read(int16_t *dst, size_t samples)
{
    if (!s_dev) {
        return ESP_ERR_INVALID_STATE;
    }
    const int rc = esp_codec_dev_read(s_dev, dst, (int)(samples * sizeof(int16_t)));
    return rc == ESP_CODEC_DEV_OK ? (int)samples : ESP_FAIL;
}
