#include "Battery.h"

#ifdef ESP_PLATFORM
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "esp_timer.h"
#endif

namespace
{
constexpr int kPinFullMv = 2450;  // vendor: calibrated pin reading with a full battery
constexpr int kPinEmptyMv = 2200; // vendor: ... with an empty one
constexpr int kCellFullMv = 4200;
constexpr int kCellEmptyMv = 3700;
constexpr int kPresentMinMv = 1000; // below this the divider is floating: no battery
constexpr int kSamplePeriodMs = 5000;
constexpr int kHistory = 12;        // 12 samples of 5 s: the trend window is one minute
constexpr int kTrendMv = 6;         // pin mV change over the window that counts as rising / falling

BatteryState g_state;
int g_history[kHistory];
int g_historyCount = 0;
int g_historyNext = 0;

#ifdef ESP_PLATFORM
constexpr const char *TAG = "battery";
constexpr adc_unit_t kUnit = ADC_UNIT_2;
constexpr adc_channel_t kChannel = ADC_CHANNEL_4; // GPIO53
constexpr adc_atten_t kAtten = ADC_ATTEN_DB_12;
adc_oneshot_unit_handle_t g_adc = nullptr;
adc_cali_handle_t g_cali = nullptr;
int64_t g_lastSampleUs = 0;
#endif
} // namespace

int Battery_percentFromPinMv(int pinMv)
{
    const int span = kPinFullMv - kPinEmptyMv;
    int pct = (pinMv - kPinEmptyMv) * 100 / span;
    return pct < 0 ? 0 : pct > 100 ? 100 : pct;
}

int Battery_millivoltsFromPinMv(int pinMv)
{
    const int span = kPinFullMv - kPinEmptyMv;
    return kCellEmptyMv + (pinMv - kPinEmptyMv) * (kCellFullMv - kCellEmptyMv) / span;
}

#ifdef ESP_PLATFORM
void Battery_begin()
{
    adc_oneshot_unit_init_cfg_t unit = {};
    unit.unit_id = kUnit;
    unit.ulp_mode = ADC_ULP_MODE_DISABLE;
    if (adc_oneshot_new_unit(&unit, &g_adc) != ESP_OK)
    {
        ESP_LOGW(TAG, "ADC unit not available; no battery gauge");
        g_adc = nullptr;
        return;
    }
    adc_oneshot_chan_cfg_t channel = {};
    channel.atten = kAtten;
    channel.bitwidth = ADC_BITWIDTH_DEFAULT;
    if (adc_oneshot_config_channel(g_adc, kChannel, &channel) != ESP_OK)
    {
        ESP_LOGW(TAG, "ADC channel not available; no battery gauge");
        adc_oneshot_del_unit(g_adc);
        g_adc = nullptr;
        return;
    }
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cali = {};
    cali.unit_id = kUnit;
    cali.chan = kChannel;
    cali.atten = kAtten;
    cali.bitwidth = ADC_BITWIDTH_DEFAULT;
    if (adc_cali_create_scheme_curve_fitting(&cali, &g_cali) != ESP_OK)
    {
        g_cali = nullptr;
    }
#endif
    ESP_LOGI(TAG, "ADC2 ch4 ready%s", g_cali ? " (calibrated)" : " (uncalibrated)");
}

static int readPinMv()
{
    // Average a burst: the divider is high impedance and the raw reading jitters.
    int sum = 0, ok = 0;
    for (int i = 0; i < 32; ++i)
    {
        int raw = 0;
        if (adc_oneshot_read(g_adc, kChannel, &raw) == ESP_OK)
        {
            sum += raw;
            ++ok;
        }
    }
    if (ok == 0)
    {
        return -1;
    }
    const int raw = sum / ok;
    int mv = 0;
    if (g_cali && adc_cali_raw_to_voltage(g_cali, raw, &mv) == ESP_OK)
    {
        return mv;
    }
    return raw * 3300 / 4095; // roughly, when the chip carries no calibration
}

void Battery_sample()
{
    if (!g_adc)
    {
        return;
    }
    const int64_t now = esp_timer_get_time();
    if (g_lastSampleUs != 0 && now - g_lastSampleUs < static_cast<int64_t>(kSamplePeriodMs) * 1000)
    {
        return;
    }
    g_lastSampleUs = now;
    const int mv = readPinMv();
    if (mv < 0)
    {
        return;
    }

    g_state.pinMv = mv;
    g_state.present = mv >= kPresentMinMv;
    g_state.percent = Battery_percentFromPinMv(mv);
    g_state.millivolts = Battery_millivoltsFromPinMv(mv);

    g_history[g_historyNext] = mv;
    g_historyNext = (g_historyNext + 1) % kHistory;
    if (g_historyCount < kHistory)
    {
        ++g_historyCount;
    }
    if (g_historyCount == kHistory)
    {
        const int oldest = g_history[g_historyNext]; // the slot about to be overwritten is the oldest
        const int delta = mv - oldest;
        if (delta >= kTrendMv)
        {
            g_state.charging = true;
        }
        else if (delta <= -kTrendMv)
        {
            g_state.charging = false;
        }
    }
}
#else
void Battery_begin() {}
void Battery_sample() {}
#endif

BatteryState Battery_state() { return g_state; }
