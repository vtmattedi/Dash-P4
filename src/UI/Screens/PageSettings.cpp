#include <stdio.h>
#include <string>

#include <NightMare.h>
#include <Version.h>

#include "App/Bindings.h"
#include "App/Heap.h"
#include "App/Net.h"
#include "App/Notifications.h"
#include "App/Ota.h"
#include "App/Prefs.h"
#include "BoardTest.h"
#include "Pages.h"
#include "UI/Bind.h"
#include "UI/Modal.h"
#include "UI/Rest.h"
#include "UI/Theme.h"
#include "UI/WifiSetup.h"
#include "board.h"
#include "esp_chip_info.h"
#include "esp_system.h"
#include "esp_timer.h"
#if DASH_PC_SCREEN
#include "App/PcScreen.h"
#include "PcLink/PcLink.h"
#endif

// Settings: display, network, what the panel is bound to, diagnostics, about.
// Notification preferences and the Explorer's defaults join in later phases.

namespace
{
lv_obj_t *g_backlightValue, *g_restBacklightValue, *g_restAfterValue;
lv_obj_t *g_wifi, *g_transport;
lv_obj_t *g_bindingText[static_cast<int>(Role::Count)];
lv_obj_t *g_about;
bool g_stickyOn = false;
uint32_t g_bindingsShown = UINT32_MAX;

lv_obj_t *sectionRow(lv_obj_t *card, const char *label, lv_obj_t **valueOut = nullptr)
{
    lv_obj_t *row = Theme_row(card, lv_pct(100), 56);
    lv_obj_t *l = Theme_label(row, label, UI_FONT_M, UI_COL_TEXT_DIM);
    lv_obj_set_width(l, 190);
    if (valueOut)
    {
        *valueOut = nullptr;
    }
    return row;
}

lv_obj_t *buttonIn(lv_obj_t *parent, const char *text, int32_t width, lv_event_cb_t cb, Tone tone = Tone::Neutral,
                   void *user = nullptr)
{
    lv_obj_t *b = Theme_button(parent, text, width, 52, tone);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user);
    return b;
}

// --- Display ----------------------------------------------------------------------------

void percent(char *buf, size_t n, int level) { snprintf(buf, n, "%d%%", level * 100 / 255); }

void onBacklight(lv_event_t *e)
{
    const int level = lv_slider_get_value(lv_event_get_target_obj(e));
    Prefs_setBacklightActive(level);
    if (!Rest_active())
    {
        board_set_backlight(static_cast<uint8_t>(level));
    }
    char buf[16];
    percent(buf, sizeof(buf), level);
    Theme_setText(g_backlightValue, buf);
}

void onRestBacklight(lv_event_t *e)
{
    const int level = lv_slider_get_value(lv_event_get_target_obj(e));
    Prefs_setBacklightRest(level);
    char buf[16];
    percent(buf, sizeof(buf), level);
    Theme_setText(g_restBacklightValue, buf);
}

const int kRestSteps[] = {0, 30, 60, 120, 300, 600, 1800};

void restAfterText(char *buf, size_t n, int seconds)
{
    if (seconds == 0)
    {
        snprintf(buf, n, "never");
    }
    else if (seconds < 60)
    {
        snprintf(buf, n, "%d s", seconds);
    }
    else
    {
        snprintf(buf, n, "%d min", seconds / 60);
    }
}

void onRestAfter(lv_event_t *e)
{
    const int index = lv_slider_get_value(lv_event_get_target_obj(e));
    Prefs_setRestAfter(kRestSteps[index]);
    char buf[16];
    restAfterText(buf, sizeof(buf), kRestSteps[index]);
    Theme_setText(g_restAfterValue, buf);
}

lv_obj_t *sliderRow(lv_obj_t *card, const char *label, int min, int max, int value, lv_event_cb_t cb, lv_obj_t **valueLabel)
{
    lv_obj_t *row = Theme_row(card, lv_pct(100), 56);
    lv_obj_t *l = Theme_label(row, label, UI_FONT_M, UI_COL_TEXT_DIM);
    lv_obj_set_width(l, 190);
    lv_obj_t *slider = lv_slider_create(row);
    lv_slider_set_range(slider, min, max);
    lv_slider_set_value(slider, value, LV_ANIM_OFF);
    lv_obj_set_flex_grow(slider, 1);
    lv_obj_set_height(slider, 18);
    lv_obj_set_ext_click_area(slider, 22);
    lv_obj_add_event_cb(slider, cb, LV_EVENT_VALUE_CHANGED, nullptr);
    *valueLabel = Theme_label(row, "", UI_FONT_M, UI_COL_TEXT);
    lv_obj_set_width(*valueLabel, 90);
    lv_obj_set_style_text_align(*valueLabel, LV_TEXT_ALIGN_RIGHT, 0);
    return slider;
}

// --- Network ----------------------------------------------------------------------------

#if DASH_PC_SCREEN
// --- PC screen ---------------------------------------------------------------------------

lv_obj_t *g_pcSummary;

void onPcShow(lv_event_t *) { PcLink_show(); }

void onPcAuto(lv_event_t *e)
{
    const bool on = lv_obj_has_state(static_cast<lv_obj_t *>(lv_event_get_target(e)), LV_STATE_CHECKED);
    Prefs_setPcAuto(on);
    PcLink_setAuto(on);
}

void onPcTab(lv_event_t *e)
{
    const bool on = lv_obj_has_state(static_cast<lv_obj_t *>(lv_event_get_target(e)), LV_STATE_CHECKED);
    Prefs_setPcTab(on);
    PcLink_setTab(on);
}

void buildPcScreen(lv_obj_t *page)
{
    lv_obj_t *card = Theme_card(page, lv_pct(100), LV_SIZE_CONTENT);
    Theme_cardHeader(card, "PC screen");
    g_pcSummary = Theme_label(card, "", UI_FONT_M, UI_COL_TEXT_DIM);
    lv_obj_t *row = Theme_row(card, lv_pct(100), 56);
    buttonIn(row, LV_SYMBOL_IMAGE "  Show the PC screen", 280, onPcShow, Tone::Accent);
    lv_obj_t *spacer = lv_obj_create(row);
    Theme_plain(spacer);
    lv_obj_set_height(spacer, 1);
    lv_obj_set_flex_grow(spacer, 1);
    Theme_label(row, "Switch to the PC when it streams", UI_FONT_M, UI_COL_TEXT_DIM);
    lv_obj_t *sw = lv_switch_create(row);
    if (Prefs_get().pcAuto)
    {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(sw, onPcAuto, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_t *tabRow = Theme_row(card, lv_pct(100), 56);
    lv_obj_t *tabSpacer = lv_obj_create(tabRow);
    Theme_plain(tabSpacer);
    lv_obj_set_height(tabSpacer, 1);
    lv_obj_set_flex_grow(tabSpacer, 1);
    Theme_label(tabRow, "Menu tab on the PC screen", UI_FONT_M, UI_COL_TEXT_DIM);
    lv_obj_t *tabSw = lv_switch_create(tabRow);
    if (Prefs_get().pcTab)
    {
        lv_obj_add_state(tabSw, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(tabSw, onPcTab, LV_EVENT_VALUE_CHANGED, nullptr);
    Theme_label(card,
                "Windows extended display over the high-speed USB-C port (Espressif USB Extended Display driver). "
                "The PC tab and the status bar's PC button show it. On the PC's picture the tab at the left edge opens "
                "Rest / Settings / Disconnect / Cancel; Settings, Disconnect and holding three fingers for two seconds close the connection.",
                UI_FONT_S, UI_COL_TEXT_FAINT);
}
#endif

void onWifi(lv_event_t *) { WifiSetup_open(); }

void refreshNetwork()
{
    const NightMare::WiFiInfo wifi = WiFi_info();
    char buf[200];
    if (wifi.state == NightMare::WiFiState::CONNECTED)
    {
        snprintf(buf, sizeof(buf), "%s   %s   %d dBm   channel %u", wifi.ssid.c_str(), wifi.ip.c_str(), (int)wifi.rssi,
                 (unsigned)wifi.channel);
    }
    else
    {
        snprintf(buf, sizeof(buf), "%s   (%s)", wifi.ssid.empty() ? "not configured" : wifi.ssid.c_str(),
                 WiFi_stateName(wifi.state));
    }
    Theme_setText(g_wifi, buf);

    const NetState net = Net_state();
    snprintf(buf, sizeof(buf), "active %s   preferred %s   MQTT %s (%s)   ESP-NOW %s",
             NightMare::ConnectionTypeName(net.active), NightMare::ConnectionTypeName(net.preferred),
             net.mqtt ? "up" : "down", NightMare::Mqtt_profile() == NightMare::MqttProfile::LOCAL ? "local" : "remote",
             net.espNow ? "up" : "n/a");
    Theme_setText(g_transport, buf);
}

// --- Bindings ---------------------------------------------------------------------------

void onChange(lv_event_t *e) { Bind_chooseFor(static_cast<Role>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)))); }
void onUnbind(lv_event_t *e) { Bind_unbind(static_cast<Role>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)))); }

void refreshBindings()
{
    if (g_bindingsShown == Bindings_revision())
    {
        return;
    }
    g_bindingsShown = Bindings_revision();
    for (int r = 0; r < static_cast<int>(Role::Count); ++r)
    {
        const Binding &b = Bindings_get(static_cast<Role>(r));
        std::string text = "not bound";
        if (!b.device.empty())
        {
            text = b.device;
            if (!b.resource.empty())
            {
                text += " / " + b.resource;
            }
            if (static_cast<Role>(r) == Role::Door)
            {
                text += b.invert ? "   (1 = closed)" : "   (1 = open)";
            }
        }
        Theme_setText(g_bindingText[r], text.c_str());
        lv_obj_set_style_text_color(g_bindingText[r], b.device.empty() ? UI_COL_TEXT_FAINT : UI_COL_TEXT, 0);
    }
}

// --- Diagnostics ------------------------------------------------------------------------

void notifyOf(NotifySeverity s, const char *title, const char *body)
{
    Notify(s, "diagnostics", title, body);
}

void onSticky(lv_event_t *)
{
    g_stickyOn = !g_stickyOn;
    if (g_stickyOn)
    {
        NotifySticky("diag.sticky", NotifySeverity::Warn, "diagnostics", "Test condition is live",
                     "Tap the banner to open the notification centre");
    }
    else
    {
        NotifyClear("diag.sticky", "Test condition cleared");
    }
}

void onConfirm(lv_event_t *)
{
    Modal_confirm("Reboot Adler?", "Adler will restart and be unreachable for about ten seconds.", "Reboot",
                  Tone::Danger, [] { notifyOf(NotifySeverity::Ok, "Confirmed", "The reboot would have been sent."); });
}

void onForm(lv_event_t *)
{
    std::vector<FormField> fields;
    FormField note;
    note.kind = FormField::Kind::Note;
    note.label = "Every field kind the Explorer's action forms use.";
    fields.push_back(note);

    FormField text;
    text.kind = FormField::Kind::Text;
    text.key = "name";
    text.label = "Name";
    text.hint = "device name";
    text.required = true;
    fields.push_back(text);

    FormField number;
    number.kind = FormField::Kind::Number;
    number.key = "minutes";
    number.label = "Minutes";
    number.value = "30";
    number.min = 0;
    number.max = 240;
    number.step = 5;
    fields.push_back(number);

    FormField sw;
    sw.kind = FormField::Kind::Switch;
    sw.key = "power";
    sw.label = "Power";
    sw.value = "true";
    fields.push_back(sw);

    FormField choice;
    choice.kind = FormField::Kind::Choice;
    choice.key = "mode";
    choice.label = "Mode";
    choice.choices = {"cool", "heat", "fan", "auto"};
    choice.value = "auto";
    fields.push_back(choice);

    Modal_form("Form test", fields, "Send", Tone::Accent, [](const FormValues &v) -> std::string {
        std::string summary;
        for (const auto &kv : v)
        {
            summary += kv.first + "=" + kv.second + "  ";
        }
        if (v.at("name") == "x")
        {
            return "\"x\" is not a name I like. Try another.";
        }
        notifyOf(NotifySeverity::Info, "Form submitted", summary.c_str());
        return "";
    });
}

void onBusy(lv_event_t *)
{
    const ModalId id = Modal_busy("Working", "Pretending to ask a device. Cancel works.", nullptr);
    lv_timer_t *t = lv_timer_create(
        [](lv_timer_t *timer) {
            const ModalId closing = static_cast<ModalId>(reinterpret_cast<uintptr_t>(lv_timer_get_user_data(timer)));
            Modal_close(closing);
            notifyOf(NotifySeverity::Ok, "Done", "The pretend request finished.");
        },
        3000, reinterpret_cast<void *>(static_cast<uintptr_t>(id)));
    lv_timer_set_repeat_count(t, 1);
}

void onOutput(lv_event_t *)
{
    Modal_text("INFO SYSTEM - Adler",
               "{\n  \"uptime_ms\": 123456,\n  \"cpu_mhz\": 160,\n  \"heap\": {\"free\": 191232, \"min\": 160020},\n"
               "  \"wifi\": {\"rssi\": -58, \"channel\": 6},\n  \"reset_reason\": \"power-on\"\n}");
}

void onBoardTest(lv_event_t *) { BoardTest_create(lv_layer_top()); }

void onRestNow(lv_event_t *) { Rest_now(); }

// --- Firmware update ---------------------------------------------------------------------

void watchUpdate(ModalId progress)
{
    lv_timer_t *timer = lv_timer_create(
        [](lv_timer_t *t) {
            const ModalId id = static_cast<ModalId>(reinterpret_cast<uintptr_t>(lv_timer_get_user_data(t)));
            const OtaState state = Ota_state();
            if (state == OtaState::Running)
            {
                const int pct = Ota_percent();
                Modal_setBusyText(id, pct >= 0 ? "Downloading... " + std::to_string(pct) + "%" : Ota_message());
                return;
            }
            lv_timer_delete(t);
            Modal_close(id);
            if (state == OtaState::Done && Ota_isCoprocessor())
            {
                Modal_confirm("C6 update installed",
                              "The new co-processor firmware is written. Restart now to run it? Wi-Fi drops for a few seconds.",
                              "Restart", Tone::Accent, [] { Ota_restartAfterCoprocessor(); }, "Later");
            }
            else if (state == OtaState::Done)
            {
                Modal_confirm("Update installed", "The new firmware is written. Restart now to run it?", "Restart", Tone::Accent,
                              [] { esp_restart(); }, "Later");
            }
            else
            {
                Modal_message("Update failed", Ota_message(), Tone::Danger);
            }
        },
        400, reinterpret_cast<void *>(static_cast<uintptr_t>(progress)));
    (void)timer;
}

void onRestart(lv_event_t *)
{
    Modal_confirm("Restart this display?", "It will restart now and be back in about ten seconds.", "Restart", Tone::Danger,
                  [] {
                      // Let the modal close and the last frame reach the panel before the reset.
                      lv_timer_t *t = lv_timer_create([](lv_timer_t *) { esp_restart(); }, 300, nullptr);
                      lv_timer_set_repeat_count(t, 1);
                  });
}

void onUpdate(lv_event_t *)
{
    FormField note;
    note.kind = FormField::Kind::Note;
    note.label = "Downloads firmware.bin into the idle slot and keeps the current firmware until the new one proves itself. "
                 "On your PC, serve .pio/build/jc1060p470c with: python -m http.server 8000";
    FormField url;
    url.kind = FormField::Kind::Text;
    url.key = "url";
    url.label = "Firmware URL";
    url.hint = "http://192.168.0.10:8000/firmware.bin";
    url.value = PersistentSettings.get("ota_url", "");
    url.required = true;
    Modal_form("Update firmware", {note, url}, "Download", Tone::Accent, [](const FormValues &v) -> std::string {
        const std::string &address = v.at("url");
        if (address.rfind("http://", 0) != 0 && address.rfind("https://", 0) != 0)
        {
            return "The address must start with http:// or https://";
        }
        PersistentSettings.set("ota_url", address);
        if (!Ota_start(address))
        {
            return "An update is already running.";
        }
        watchUpdate(Modal_busy("Updating firmware", "Starting...", nullptr));
        return "";
    });
}

void onUpdateCoprocessor(lv_event_t *)
{
    FormField note;
    note.kind = FormField::Kind::Note;
    note.label = "Writes an ESP32-C6 firmware (ESP-Hosted slave) into the Wi-Fi co-processor's idle slot over the "
                 "board's internal link. For ESP-NOW use the dash-p4-c6 build: serve its .pio/build/esp32c6 folder "
                 "with python -m http.server 8000. Images for other chips are refused.";
    FormField url;
    url.kind = FormField::Kind::Text;
    url.key = "url";
    url.label = "C6 firmware URL";
    url.hint = "http://192.168.0.10:8000/firmware.bin";
    url.value = PersistentSettings.get("c6_ota_url", "");
    url.required = true;
    Modal_form("Update the C6", {note, url}, "Download", Tone::Accent, [](const FormValues &v) -> std::string {
        const std::string &address = v.at("url");
        if (address.rfind("http://", 0) != 0 && address.rfind("https://", 0) != 0)
        {
            return "The address must start with http:// or https://";
        }
        PersistentSettings.set("c6_ota_url", address);
        if (!Ota_startCoprocessor(address))
        {
            return "An update is already running.";
        }
        watchUpdate(Modal_busy("Updating the C6", "Starting...", nullptr));
        return "";
    });
}
} // namespace

lv_obj_t *PageSettings_create(lv_obj_t *parent)
{
    lv_obj_t *page = Theme_column(parent, lv_pct(100), lv_pct(100));
    lv_obj_add_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(page, LV_DIR_VER);
    lv_obj_set_style_pad_all(page, UI_GUTTER, 0);
    lv_obj_set_style_pad_gap(page, UI_GUTTER, 0);

    char buf[24];

    // Display.
    lv_obj_t *display = Theme_card(page, lv_pct(100), LV_SIZE_CONTENT);
    Theme_cardHeader(display, "Display");
    sliderRow(display, "Brightness", 8, 255, Prefs_get().blActive, onBacklight, &g_backlightValue);
    percent(buf, sizeof(buf), Prefs_get().blActive);
    lv_label_set_text(g_backlightValue, buf);
    sliderRow(display, "Resting brightness", 0, 255, Prefs_get().blRest, onRestBacklight, &g_restBacklightValue);
    percent(buf, sizeof(buf), Prefs_get().blRest);
    lv_label_set_text(g_restBacklightValue, buf);
    int restIndex = 0;
    for (int i = 0; i < 7; ++i)
    {
        if (kRestSteps[i] == Prefs_get().restAfterSec)
        {
            restIndex = i;
        }
    }
    sliderRow(display, "Rest after", 0, 6, restIndex, onRestAfter, &g_restAfterValue);
    restAfterText(buf, sizeof(buf), kRestSteps[restIndex]);
    lv_label_set_text(g_restAfterValue, buf);
    lv_obj_t *restRow = Theme_row(display, lv_pct(100), 56);
    buttonIn(restRow, "Rest now", 180, onRestNow);

#if DASH_PC_SCREEN
    buildPcScreen(page);
#endif

    // Network.
    lv_obj_t *net = Theme_card(page, lv_pct(100), LV_SIZE_CONTENT);
    Theme_cardHeader(net, "Network");
    g_wifi = Theme_label(net, "", UI_FONT_M, UI_COL_TEXT_DIM);
    g_transport = Theme_label(net, "", UI_FONT_M, UI_COL_TEXT_DIM);
    lv_obj_t *netRow = Theme_row(net, lv_pct(100), 56);
    buttonIn(netRow, LV_SYMBOL_WIFI "  Wi-Fi network...", 230, onWifi);
#if NM_NETWORK_ESPNOW
    Theme_label(netRow, "ESP-NOW through the C6 first (needs the dash-p4-c6 firmware), remote MQTT as the fallback.", UI_FONT_S, UI_COL_TEXT_FAINT);
#else
    Theme_label(netRow, "Remote MQTT only. ESP-NOW and the local broker are switched off in this build.", UI_FONT_S, UI_COL_TEXT_FAINT);
#endif

    // Bindings.
    lv_obj_t *bind = Theme_card(page, lv_pct(100), LV_SIZE_CONTENT);
    Theme_cardHeader(bind, "What the panel controls");
    for (int r = 0; r < static_cast<int>(Role::Count); ++r)
    {
        lv_obj_t *row = Theme_row(bind, lv_pct(100), 56);
        lv_obj_t *l = Theme_label(row, Bindings_label(static_cast<Role>(r)), UI_FONT_M, UI_COL_TEXT_DIM);
        lv_obj_set_width(l, 190);
        g_bindingText[r] = Theme_label(row, "", UI_FONT_M, UI_COL_TEXT);
        lv_obj_set_flex_grow(g_bindingText[r], 1);
        lv_label_set_long_mode(g_bindingText[r], LV_LABEL_LONG_DOT);
        buttonIn(row, "Change", 120, onChange, Tone::Neutral, reinterpret_cast<void *>(static_cast<intptr_t>(r)));
        buttonIn(row, "Unbind", 120, onUnbind, Tone::Neutral, reinterpret_cast<void *>(static_cast<intptr_t>(r)));
    }

    // Firmware.
    lv_obj_t *firmware = Theme_card(page, lv_pct(100), LV_SIZE_CONTENT);
    Theme_cardHeader(firmware, "Firmware");
    lv_obj_t *fwRow = Theme_row(firmware, lv_pct(100), 56);
    Theme_label(fwRow, ("Running " VERSION ", built " BUILD_TIMESTAMP), UI_FONT_M, UI_COL_TEXT_DIM);
    lv_obj_t *spacer = lv_obj_create(fwRow);
    Theme_plain(spacer);
    lv_obj_set_height(spacer, 1);
    lv_obj_set_flex_grow(spacer, 1);
    buttonIn(fwRow, "Restart", 140, onRestart, Tone::Danger);
    buttonIn(fwRow, "Update...", 160, onUpdate, Tone::Accent);
    lv_obj_t *c6Row = Theme_row(firmware, lv_pct(100), 56);
    Theme_label(c6Row, "Wi-Fi co-processor (ESP32-C6)", UI_FONT_M, UI_COL_TEXT_DIM);
    lv_obj_t *c6Spacer = lv_obj_create(c6Row);
    Theme_plain(c6Spacer);
    lv_obj_set_height(c6Spacer, 1);
    lv_obj_set_flex_grow(c6Spacer, 1);
    buttonIn(c6Row, "Update C6...", 180, onUpdateCoprocessor);

    // Diagnostics.
    lv_obj_t *diag = Theme_card(page, lv_pct(100), LV_SIZE_CONTENT);
    Theme_cardHeader(diag, "Diagnostics");
    lv_obj_t *grid = Theme_row(diag, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(grid, 12, 0);
    buttonIn(grid, "Toast: info", 200, [](lv_event_t *) { notifyOf(NotifySeverity::Info, "Information", "Something worth knowing."); }, Tone::Neutral);
    buttonIn(grid, "Toast: ok", 200, [](lv_event_t *) { notifyOf(NotifySeverity::Ok, "All good", "That worked."); }, Tone::Ok);
    buttonIn(grid, "Toast: warning", 200, [](lv_event_t *) { notifyOf(NotifySeverity::Warn, "Careful", "Adler has gone quiet."); }, Tone::Warn);
    buttonIn(grid, "Toast: critical", 200, [](lv_event_t *) { notifyOf(NotifySeverity::Crit, "Door open", "Front door open for 10 minutes. Tap to dismiss."); }, Tone::Danger);
    buttonIn(grid, "Toggle live banner", 200, onSticky);
    buttonIn(grid, "Modal: confirm", 200, onConfirm);
    buttonIn(grid, "Modal: form", 200, onForm);
    buttonIn(grid, "Modal: busy", 200, onBusy);
    buttonIn(grid, "Modal: output", 200, onOutput);
    buttonIn(grid, "Board self-test", 200, onBoardTest, Tone::Accent);
    // The buttons above were sized 200; the ones with long labels need a little more room.
    for (uint32_t i = 0; i < lv_obj_get_child_count(grid); ++i)
    {
        lv_obj_set_width(lv_obj_get_child(grid, i), 200);
    }

    // About.
    lv_obj_t *about = Theme_card(page, lv_pct(100), LV_SIZE_CONTENT);
    Theme_cardHeader(about, "About");
    g_about = Theme_label(about, "", UI_FONT_M, UI_COL_TEXT_DIM);

    PageSettings_refresh(true);
    return page;
}

void PageSettings_refresh(bool force)
{
    static uint32_t lastMs = 0;
    const uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    if (force)
    {
        g_bindingsShown = UINT32_MAX;
    }
    refreshBindings();
    if (!force && now - lastMs < 1000)
    {
        return;
    }
    lastMs = now;
    refreshNetwork();
#if DASH_PC_SCREEN
    Theme_setText(g_pcSummary, PcScreen_summary());
#endif

    esp_chip_info_t chip;
    esp_chip_info(&chip);
    char buf[420];
    const uint32_t up = now / 1000;
    snprintf(buf, sizeof(buf),
             "firmware   %s   built %s\nboard      %s %s, ESP32-P4 rev v%d.%d\nname       %s\nuptime     %ud %02uh %02um %02us\n"
             "internal   %u KB free (largest %u KB, lowest %u KB)\nPSRAM      %u of %u MB free",
             VERSION, BUILD_TIMESTAMP, BOARD_NAME, BOARD_REVISION, chip.revision / 100, chip.revision % 100,
             gDeviceIdentity.getDeviceName().c_str(), (unsigned)(up / 86400), (unsigned)(up / 3600 % 24),
             (unsigned)(up / 60 % 60), (unsigned)(up % 60), (unsigned)(Heap_internalFree() / 1024),
             (unsigned)(Heap_internalLargest() / 1024), (unsigned)(Heap_internalMinFree() / 1024),
             (unsigned)(Heap_psramFree() / (1024 * 1024)), (unsigned)(Heap_psramTotal() / (1024 * 1024)));
    Theme_setText(g_about, buf);
}
