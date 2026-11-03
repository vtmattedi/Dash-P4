#include <stdio.h>
#include <string>
#include <time.h>

#include "App/AcClient.h"
#include "App/Bindings.h"
#include "App/LightControl.h"
#include "App/Notifications.h"
#include "App/Registry.h"
#include "Pages.h"
#include "UI/Bind.h"
#include "UI/Modal.h"
#include "UI/Theme.h"

// Home: the three things the panel exists to drive -- the air conditioner, a light and a
// colour light -- plus the date and a weather slot.
//
// The AC and light cards are switches: tapping the card toggles the thing it represents and
// its colour is the state. Anything finer sits on a nested button, which LVGL does not bubble,
// so pressing one does not also toggle the card.
//
// An unbound or silent control stays on screen, dimmed and saying why, rather than being
// replaced by a notice: the page keeps its shape. Tapping an unbound card starts the flow
// that binds it. Dimming is done with explicit colours, not opacity, so no card is forced
// through an intermediate layer on every redraw. LVGL still delivers clicks to widgets in
// the disabled state, so every handler re-checks state itself.

namespace
{
constexpr int32_t kColW = 440;

// AC
lv_obj_t *g_acCard, *g_acChip, *g_acPower, *g_acNote;
lv_obj_t *g_acUnitTemp, *g_acUnitMinus, *g_acUnitPlus;
lv_obj_t *g_acRoom, *g_acTarget, *g_acTargetMinus, *g_acTargetPlus, *g_acAuto;
lv_obj_t *g_acDoor, *g_acSleep, *g_acSleepBtn, *g_acSyncBtn;
// Light
lv_obj_t *g_lightCard, *g_lightChip, *g_lightSwitch, *g_lightSub;
// RGB
lv_obj_t *g_rgbCard, *g_rgbChip, *g_rgbSwitch, *g_rgbSlider, *g_rgbHex, *g_rgbNote;
// Date / weather
lv_obj_t *g_dateLabel, *g_weatherLabel;

uint32_t g_bindingsShown = UINT32_MAX;

void sendFailed(const char *what)
{
    Notify(NotifySeverity::Warn, "home", std::string("Could not send: ") + what,
           "No transport is connected, or nothing is bound.");
}

// --- AC ---------------------------------------------------------------------------------

const char *acStateText(AcState s, bool *on)
{
    *on = false;
    switch (s)
    {
    case AC_ON:
        *on = true;
        return "On";
    case AC_ON_TARGET:
        *on = true;
        return "On - holding the target";
    case AC_OFF:
        return "Off";
    case AC_OFF_TARGET:
        return "Off - room at target";
    case AC_OFF_DOOR_OPEN:
        return "Paused - door open";
    case AC_OFF_TARGET_DOOR_OPEN:
        return "Off - at target, door open";
    default:
        return "Unknown";
    }
}

void onAcCard(lv_event_t *e)
{
    lv_obj_t *target = lv_event_get_target_obj(e);
    if (target != g_acCard)
    {
        return; // a tap that landed on a child other than the card itself
    }
    if (!AcClient_bound())
    {
        Bind_chooseFor(Role::Ac);
    }
    else if (!AcClient_togglePower())
    {
        sendFailed("air conditioner power");
    }
}

void onAcPower(lv_event_t *)
{
    if (!AcClient_bound())
    {
        Bind_chooseFor(Role::Ac);
    }
    else if (!AcClient_togglePower())
    {
        sendFailed("air conditioner power");
    }
}

void onAcUnit(lv_event_t *e)
{
    if (AcClient_bound() && !AcClient_stepUnitTemperature(static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)))))
    {
        sendFailed("setpoint");
    }
}

void onAcTarget(lv_event_t *e)
{
    const float step = static_cast<float>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e))) * 0.5f;
    if (AcClient_bound() && !AcClient_stepTarget(step))
    {
        sendFailed("room target");
    }
}

void onAcAuto(lv_event_t *)
{
    if (AcClient_bound() && !AcClient_toggleTarget())
    {
        sendFailed("thermostat");
    }
}

void onSleep(lv_event_t *)
{
    if (!AcClient_bound())
    {
        return;
    }
    FormField choice;
    choice.kind = FormField::Kind::Choice;
    choice.key = "when";
    choice.label = "Switch the unit off in...";
    choice.choices = {"Cancel timer", "30 minutes", "1 hour", "2 hours", "4 hours"};
    choice.value = "1 hour";
    Modal_form("Sleep timer", {choice}, "Set", Tone::Accent, [](const FormValues &v) -> std::string {
        static const struct
        {
            const char *label;
            uint32_t minutes;
        } table[] = {{"Cancel timer", 0}, {"30 minutes", 30}, {"1 hour", 60}, {"2 hours", 120}, {"4 hours", 240}};
        for (const auto &t : table)
        {
            if (v.at("when") == t.label)
            {
                if (!AcClient_setSleepMinutes(t.minutes))
                {
                    return "Could not send. Is the panel connected?";
                }
                Notify(NotifySeverity::Info, "home", t.minutes ? "Sleep timer set" : "Sleep timer cancelled",
                       t.minutes ? v.at("when") : "");
                return "";
            }
        }
        return "";
    });
}

void onSync(lv_event_t *)
{
    if (!AcClient_bound())
    {
        return;
    }
    FormField note;
    note.kind = FormField::Kind::Note;
    note.label = "Use after changing the unit with its own remote. This tells the controller what the unit is already "
                 "doing; it does not change the room.";
    FormField temp;
    temp.kind = FormField::Kind::Number;
    temp.key = "temperature";
    temp.label = "Temperature shown on the unit";
    temp.value = std::to_string(AcClient_unitTemperature() ? AcClient_unitTemperature() : 24);
    temp.min = AC_UNIT_TEMP_MIN;
    temp.max = AC_UNIT_TEMP_MAX;
    temp.step = 1;
    FormField power;
    power.kind = FormField::Kind::Switch;
    power.key = "power";
    power.label = "Unit is on";
    power.value = AcClient_powerOn() ? "true" : "false";
    Modal_form("Sync with unit", {note, temp, power}, "Sync", Tone::Accent, [](const FormValues &v) -> std::string {
        if (!AcClient_manualSync(atoi(v.at("temperature").c_str()), v.at("power") == "true"))
        {
            return "Could not send. Is the panel connected?";
        }
        Notify(NotifySeverity::Info, "home", "Unit state synced");
        return "";
    });
}

void setCardColours(lv_obj_t *card, bool active, bool dim)
{
    lv_obj_set_style_bg_color(card, active ? lv_color_hex(0x13293B) : UI_COL_SURFACE, 0);
    lv_obj_set_style_border_color(card, active ? UI_COL_INFO : UI_COL_BORDER, 0);
    lv_obj_set_style_border_width(card, active ? 2 : 1, 0);
    (void)dim;
}

void buildAc(lv_obj_t *parent)
{
    g_acCard = Theme_card(parent, kColW, lv_pct(100));
    lv_obj_add_flag(g_acCard, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(g_acCard, onAcCard, LV_EVENT_CLICKED, nullptr);
    lv_obj_set_style_pad_gap(g_acCard, 12, 0);

    lv_obj_t *head = Theme_row(g_acCard, lv_pct(100), 60);
    lv_obj_t *titles = Theme_column(head, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(titles, 1);
    lv_obj_set_style_pad_gap(titles, 2, 0);
    Theme_label(titles, "Air conditioner", UI_FONT_L, UI_COL_TEXT);
    g_acChip = Theme_chip(titles);
    g_acPower = Theme_iconButton(head, LV_SYMBOL_POWER, 56, Tone::Neutral);
    lv_obj_add_event_cb(g_acPower, onAcPower, LV_EVENT_CLICKED, nullptr);

    g_acNote = Theme_label(g_acCard, "", UI_FONT_M, UI_COL_TEXT_DIM);
    lv_obj_set_width(g_acNote, lv_pct(100));
    lv_label_set_long_mode(g_acNote, LV_LABEL_LONG_WRAP);

    // The unit's own setpoint: the number the controller writes to the unit.
    lv_obj_t *unit = Theme_row(g_acCard, lv_pct(100), 110);
    lv_obj_set_flex_align(unit, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    g_acUnitMinus = Theme_button(unit, LV_SYMBOL_MINUS, 72, 72, Tone::Neutral);
    lv_obj_add_event_cb(g_acUnitMinus, onAcUnit, LV_EVENT_CLICKED, reinterpret_cast<void *>(static_cast<intptr_t>(-1)));
    lv_obj_t *mid = Theme_column(unit, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_align(mid, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(mid, 0, 0);
    g_acUnitTemp = Theme_label(mid, "--", UI_FONT_XXL, UI_COL_TEXT);
    Theme_label(mid, "unit setpoint", UI_FONT_S, UI_COL_TEXT_DIM);
    g_acUnitPlus = Theme_button(unit, LV_SYMBOL_PLUS, 72, 72, Tone::Neutral);
    lv_obj_add_event_cb(g_acUnitPlus, onAcUnit, LV_EVENT_CLICKED, reinterpret_cast<void *>(static_cast<intptr_t>(1)));

    // The room: what the thermostat works against.
    lv_obj_t *room = Theme_row(g_acCard, lv_pct(100), 64);
    lv_obj_set_flex_align(room, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *roomBox = Theme_column(room, 120, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_gap(roomBox, 0, 0);
    Theme_label(roomBox, "room", UI_FONT_S, UI_COL_TEXT_DIM);
    g_acRoom = Theme_label(roomBox, "--", UI_FONT_XL, UI_COL_TEXT);
    g_acTargetMinus = Theme_button(room, LV_SYMBOL_MINUS, 52, 52, Tone::Neutral);
    lv_obj_add_event_cb(g_acTargetMinus, onAcTarget, LV_EVENT_CLICKED, reinterpret_cast<void *>(static_cast<intptr_t>(-1)));
    lv_obj_t *targetBox = Theme_column(room, 110, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_gap(targetBox, 0, 0);
    lv_obj_set_flex_align(targetBox, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    Theme_label(targetBox, "target", UI_FONT_S, UI_COL_TEXT_DIM);
    g_acTarget = Theme_label(targetBox, "--", UI_FONT_XL, UI_COL_TEXT);
    g_acTargetPlus = Theme_button(room, LV_SYMBOL_PLUS, 52, 52, Tone::Neutral);
    lv_obj_add_event_cb(g_acTargetPlus, onAcTarget, LV_EVENT_CLICKED, reinterpret_cast<void *>(static_cast<intptr_t>(1)));
    g_acAuto = Theme_button(room, "Auto", 84, 52, Tone::Neutral);
    lv_obj_add_event_cb(g_acAuto, onAcAuto, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *info = Theme_row(g_acCard, lv_pct(100), 40);
    g_acDoor = Theme_chip(info);
    g_acSleep = Theme_label(info, "", UI_FONT_M, UI_COL_TEXT_DIM);
    lv_obj_set_flex_grow(g_acSleep, 1);
    lv_obj_set_style_text_align(g_acSleep, LV_TEXT_ALIGN_RIGHT, 0);

    lv_obj_t *actions = Theme_row(g_acCard, lv_pct(100), 56);
    g_acSleepBtn = Theme_button(actions, LV_SYMBOL_BELL "  Sleep timer", 200, 52, Tone::Neutral);
    lv_obj_add_event_cb(g_acSleepBtn, onSleep, LV_EVENT_CLICKED, nullptr);
    g_acSyncBtn = Theme_button(actions, LV_SYMBOL_REFRESH "  Sync with unit", 200, 52, Tone::Neutral);
    lv_obj_add_event_cb(g_acSyncBtn, onSync, LV_EVENT_CLICKED, nullptr);
}

void refreshAc()
{
    const bool bound = AcClient_bound();
    const bool stale = AcClient_isStale();
    bool on = false;
    const AcState state = AcClient_state();
    const char *stateText = acStateText(state, &on);
    char buf[96];

    if (!bound)
    {
        Theme_chipSet(g_acChip, "not bound", UI_COL_TEXT_FAINT);
        Theme_setText(g_acNote, "Tap to choose which device is the air conditioner.");
    }
    else if (stale)
    {
        snprintf(buf, sizeof(buf), "%s is not answering", Bindings_get(Role::Ac).device.c_str());
        Theme_chipSet(g_acChip, "no data", UI_COL_WARN);
        Theme_setText(g_acNote, buf);
    }
    else
    {
        Theme_chipSet(g_acChip, stateText, on ? UI_COL_INFO : UI_COL_TEXT_DIM);
        Theme_setText(g_acNote, "");
    }
    const bool live = bound && !stale;
    setCardColours(g_acCard, live && on, !live);

    lv_obj_set_style_bg_color(g_acPower, live && on ? UI_COL_INFO : UI_COL_SURFACE_HI, 0);
    lv_obj_set_style_text_color(g_acPower, live && on ? lv_color_hex(0x04202F) : UI_COL_TEXT_DIM, 0);

    if (live && AcClient_unitTemperature() > 0)
    {
        snprintf(buf, sizeof(buf), "%d\xC2\xB0", AcClient_unitTemperature());
        Theme_setText(g_acUnitTemp, buf);
    }
    else
    {
        Theme_setText(g_acUnitTemp, "--");
    }
    lv_obj_set_style_text_color(g_acUnitTemp, live ? UI_COL_TEXT : UI_COL_TEXT_FAINT, 0);

    float room;
    if (live && AcClient_roomTemperature(room))
    {
        snprintf(buf, sizeof(buf), "%.1f\xC2\xB0", static_cast<double>(room));
        Theme_setText(g_acRoom, buf);
    }
    else
    {
        Theme_setText(g_acRoom, "--");
    }
    if (live && AcClient_target() > 0)
    {
        snprintf(buf, sizeof(buf), "%.1f\xC2\xB0", static_cast<double>(AcClient_target()));
        Theme_setText(g_acTarget, buf);
    }
    else
    {
        Theme_setText(g_acTarget, "--");
    }
    const bool auto_ = live && AcClient_targetEnabled();
    lv_obj_set_style_bg_color(g_acAuto, auto_ ? UI_COL_ACCENT : UI_COL_SURFACE_HI, 0);
    lv_obj_set_style_text_color(g_acAuto, auto_ ? lv_color_hex(0x1A1208) : UI_COL_TEXT, 0);

    // Door.
    const int door = Bindings_doorOpen();
    if (door < 0)
    {
        Theme_chipSet(g_acDoor, Bindings_configured(Role::Door) ? "door: no reading" : "door: not set", UI_COL_TEXT_FAINT);
    }
    else
    {
        Theme_chipSet(g_acDoor, door ? "door open" : "door closed", door ? UI_COL_WARN : UI_COL_OK);
    }

    // Sleep timer.
    const uint32_t deadline = AcClient_sleepDeadline();
    const time_t now = time(nullptr);
    if (live && deadline > 0 && now > 1700000000 && deadline > static_cast<uint32_t>(now))
    {
        const uint32_t left = deadline - static_cast<uint32_t>(now);
        snprintf(buf, sizeof(buf), "sleeps in %uh %02um", (unsigned)(left / 3600), (unsigned)(left / 60 % 60));
        Theme_setText(g_acSleep, buf);
    }
    else
    {
        Theme_setText(g_acSleep, "");
    }
    const lv_color_t btnText = live ? UI_COL_TEXT : UI_COL_TEXT_FAINT;
    lv_obj_set_style_text_color(g_acSleepBtn, btnText, 0);
    lv_obj_set_style_text_color(g_acSyncBtn, btnText, 0);
}

// --- Light --------------------------------------------------------------------------------

void onLightCard(lv_event_t *)
{
    if (!Bindings_configured(Role::Light))
    {
        Bind_chooseFor(Role::Light);
    }
    else if (!LightControl_toggle())
    {
        sendFailed("light");
    }
}

void buildLight(lv_obj_t *parent)
{
    g_lightCard = Theme_card(parent, kColW, 120);
    lv_obj_add_flag(g_lightCard, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(g_lightCard, onLightCard, LV_EVENT_CLICKED, nullptr);
    lv_obj_set_flex_flow(g_lightCard, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(g_lightCard, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *titles = Theme_column(g_lightCard, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(titles, 1);
    lv_obj_set_style_pad_gap(titles, 4, 0);
    Theme_label(titles, "Light", UI_FONT_L, UI_COL_TEXT);
    g_lightChip = Theme_chip(titles);
    g_lightSub = Theme_label(titles, "", UI_FONT_S, UI_COL_TEXT_FAINT);
    g_lightSwitch = lv_switch_create(g_lightCard);
    lv_obj_set_size(g_lightSwitch, 90, 50);
    lv_obj_remove_flag(g_lightSwitch, LV_OBJ_FLAG_CLICKABLE); // the card is the control
}

void refreshLight()
{
    const LightState s = LightControl_state();
    switch (s)
    {
    case LIGHT_UNBOUND:
        Theme_chipSet(g_lightChip, "not bound", UI_COL_TEXT_FAINT);
        Theme_setText(g_lightSub, "Tap to choose a resource");
        break;
    case LIGHT_UNKNOWN:
        Theme_chipSet(g_lightChip, "no data", UI_COL_WARN);
        Theme_setText(g_lightSub, (Bindings_get(Role::Light).device + " is not answering").c_str());
        break;
    case LIGHT_ON:
        Theme_chipSet(g_lightChip, "on", UI_COL_WARN);
        Theme_setText(g_lightSub, Bindings_get(Role::Light).device.c_str());
        break;
    default:
        Theme_chipSet(g_lightChip, "off", UI_COL_TEXT_DIM);
        Theme_setText(g_lightSub, Bindings_get(Role::Light).device.c_str());
        break;
    }
    if (s == LIGHT_ON)
    {
        lv_obj_add_state(g_lightSwitch, LV_STATE_CHECKED);
    }
    else
    {
        lv_obj_remove_state(g_lightSwitch, LV_STATE_CHECKED);
    }
    lv_obj_set_style_bg_color(g_lightCard, s == LIGHT_ON ? lv_color_hex(0x3A3012) : UI_COL_SURFACE, 0);
    lv_obj_set_style_border_color(g_lightCard, s == LIGHT_ON ? UI_COL_WARN : UI_COL_BORDER, 0);
}

// --- RGB ----------------------------------------------------------------------------------
//
// LVGL 9.2 has no colour wheel, so the colour is chosen with preset swatches plus hue and
// saturation sliders; brightness is its own slider, matching how the colour is stored (hue at
// full level, level separately -- see LightControl.h).

lv_obj_t *g_rgbHue, *g_rgbSat;
lv_obj_t *g_rgbSwatch[10];

const uint32_t kPresets[10] = {0xFFFFFF, 0xFFD2A0, 0xFF3030, 0xFF8C1A, 0xFFE030,
                               0x30E060, 0x20E0E0, 0x3070FF, 0xA050FF, 0xFF50B0};

uint32_t pickedColour()
{
    const lv_color_t c = lv_color_hsv_to_rgb(static_cast<uint16_t>(lv_slider_get_value(g_rgbHue)),
                                             static_cast<uint8_t>(lv_slider_get_value(g_rgbSat)), 100);
    return lv_color_to_u32(c) & 0x00FFFFFF;
}

void updateSaturationTrack()
{
    // The saturation track runs from white to the current hue.
    static lv_grad_dsc_t grad;
    const lv_color_t full = lv_color_hsv_to_rgb(static_cast<uint16_t>(lv_slider_get_value(g_rgbHue)), 100, 100);
    grad.dir = LV_GRAD_DIR_HOR;
    grad.stops_count = 2;
    grad.stops[0] = {lv_color_white(), LV_OPA_COVER, 0};
    grad.stops[1] = {full, LV_OPA_COVER, 255};
    lv_obj_set_style_bg_grad(g_rgbSat, &grad, LV_PART_MAIN);
}

void showPicked()
{
    updateSaturationTrack();
    char buf[16];
    snprintf(buf, sizeof(buf), "#%06X", (unsigned)pickedColour());
    Theme_setText(g_rgbHex, buf);
}

void onRgbCard(lv_event_t *e)
{
    if (lv_event_get_target_obj(e) != g_rgbCard)
    {
        return;
    }
    if (!Rgb_bound())
    {
        Bind_chooseFor(Role::Rgb);
    }
}

void onRgbSwitch(lv_event_t *)
{
    if (!Rgb_bound())
    {
        Bind_chooseFor(Role::Rgb);
        return;
    }
    if (!Rgb_setOn(!Rgb_on()))
    {
        sendFailed("colour light");
    }
}

void onHueSat(lv_event_t *e)
{
    if (!Rgb_bound())
    {
        return;
    }
    showPicked(); // dragging only previews ...
    if (lv_event_get_code(e) == LV_EVENT_RELEASED && !Rgb_setColour(pickedColour()))
    {
        sendFailed("colour"); // ... the colour is sent when the finger lifts, not 60 times a second
    }
}

void onPreset(lv_event_t *e)
{
    if (!Rgb_bound())
    {
        Bind_chooseFor(Role::Rgb);
        return;
    }
    const uint32_t hex = kPresets[reinterpret_cast<intptr_t>(lv_event_get_user_data(e))];
    if (!Rgb_setColour(hex))
    {
        sendFailed("colour");
    }
}

void onRgbBrightness(lv_event_t *e)
{
    if (!Rgb_bound() || lv_event_get_code(e) != LV_EVENT_RELEASED)
    {
        return;
    }
    if (!Rgb_setBrightness(static_cast<uint8_t>(lv_slider_get_value(g_rgbSlider))))
    {
        sendFailed("brightness");
    }
}

lv_obj_t *trackSlider(lv_obj_t *parent, int min, int max, lv_event_cb_t cb)
{
    lv_obj_t *s = lv_slider_create(parent);
    lv_slider_set_range(s, min, max);
    lv_obj_set_width(s, lv_pct(100));
    lv_obj_set_height(s, 20);
    lv_obj_set_ext_click_area(s, 18);
    lv_obj_set_style_bg_opa(s, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s, lv_color_white(), LV_PART_KNOB);
    lv_obj_set_style_pad_all(s, 4, LV_PART_KNOB);
    lv_obj_add_event_cb(s, cb, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(s, cb, LV_EVENT_RELEASED, nullptr);
    return s;
}

void buildRgb(lv_obj_t *parent)
{
    g_rgbCard = Theme_card(parent, kColW, LV_SIZE_CONTENT);
    lv_obj_add_flag(g_rgbCard, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(g_rgbCard, onRgbCard, LV_EVENT_CLICKED, nullptr);
    lv_obj_set_style_pad_gap(g_rgbCard, 12, 0);

    lv_obj_t *head = Theme_row(g_rgbCard, lv_pct(100), 54);
    lv_obj_t *titles = Theme_column(head, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(titles, 1);
    lv_obj_set_style_pad_gap(titles, 2, 0);
    Theme_label(titles, "Colour light", UI_FONT_L, UI_COL_TEXT);
    g_rgbChip = Theme_chip(titles);
    g_rgbSwitch = lv_switch_create(head);
    lv_obj_set_size(g_rgbSwitch, 90, 50);
    lv_obj_add_event_cb(g_rgbSwitch, onRgbSwitch, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *swatches = Theme_row(g_rgbCard, lv_pct(100), 40);
    lv_obj_set_style_pad_gap(swatches, 4, 0);
    for (int i = 0; i < 10; ++i)
    {
        lv_obj_t *b = lv_button_create(swatches);
        lv_obj_remove_style_all(b);
        lv_obj_set_size(b, 38, 38);
        lv_obj_set_style_radius(b, 10, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(kPresets[i]), 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(b, 2, 0);
        lv_obj_set_style_border_color(b, UI_COL_BORDER, 0);
        lv_obj_set_style_border_color(b, UI_COL_TEXT, LV_STATE_PRESSED);
        lv_obj_add_event_cb(b, onPreset, LV_EVENT_CLICKED, reinterpret_cast<void *>(static_cast<intptr_t>(i)));
        g_rgbSwatch[i] = b;
    }

    // Hue: a rainbow track.
    g_rgbHue = trackSlider(g_rgbCard, 0, 359, onHueSat);
    static lv_grad_dsc_t rainbow;
    rainbow.dir = LV_GRAD_DIR_HOR;
    rainbow.stops_count = 7;
    static const uint16_t hues[7] = {0, 60, 120, 180, 240, 300, 359};
    for (int i = 0; i < 7; ++i)
    {
        rainbow.stops[i] = {lv_color_hsv_to_rgb(hues[i], 100, 100), LV_OPA_COVER, static_cast<uint8_t>(i * 255 / 6)};
    }
    lv_obj_set_style_bg_grad(g_rgbHue, &rainbow, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_rgbHue, LV_OPA_COVER, LV_PART_MAIN);

    g_rgbSat = trackSlider(g_rgbCard, 0, 100, onHueSat);
    lv_slider_set_value(g_rgbSat, 100, LV_ANIM_OFF);
    lv_obj_set_style_bg_opa(g_rgbSat, LV_OPA_COVER, LV_PART_MAIN);

    lv_obj_t *level = Theme_row(g_rgbCard, lv_pct(100), 30);
    Theme_label(level, "Brightness", UI_FONT_S, UI_COL_TEXT_DIM);
    g_rgbHex = Theme_label(level, "#FFFFFF", UI_FONT_M, UI_COL_TEXT);
    lv_obj_set_flex_grow(g_rgbHex, 1);
    lv_obj_set_style_text_align(g_rgbHex, LV_TEXT_ALIGN_RIGHT, 0);
    g_rgbSlider = trackSlider(g_rgbCard, 1, 255, onRgbBrightness);
    lv_slider_set_value(g_rgbSlider, 255, LV_ANIM_OFF);
    lv_obj_set_style_bg_opa(g_rgbSlider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(g_rgbSlider, UI_COL_SURFACE_HI, LV_PART_MAIN);

    g_rgbNote = Theme_label(g_rgbCard, "", UI_FONT_S, UI_COL_TEXT_FAINT);
    lv_obj_set_width(g_rgbNote, lv_pct(100));
    lv_label_set_long_mode(g_rgbNote, LV_LABEL_LONG_DOT);
    updateSaturationTrack();
}

void refreshRgb()
{
    const bool bound = Rgb_bound();
    const bool live = Rgb_hasState();
    if (!bound)
    {
        Theme_chipSet(g_rgbChip, "not bound", UI_COL_TEXT_FAINT);
        Theme_setText(g_rgbNote, "Tap to choose a resource");
    }
    else if (!live)
    {
        Theme_chipSet(g_rgbChip, "no data", UI_COL_WARN);
        Theme_setText(g_rgbNote, (Bindings_get(Role::Rgb).device + " is not answering").c_str());
    }
    else
    {
        Theme_chipSet(g_rgbChip, Rgb_on() ? "on" : "off", Rgb_on() ? UI_COL_OK : UI_COL_TEXT_DIM);
        Theme_setText(g_rgbNote, (Bindings_get(Role::Rgb).device + " / " + Bindings_get(Role::Rgb).resource).c_str());
    }
    if (Rgb_on())
    {
        lv_obj_add_state(g_rgbSwitch, LV_STATE_CHECKED);
    }
    else
    {
        lv_obj_remove_state(g_rgbSwitch, LV_STATE_CHECKED);
    }
    // Do not fight the finger: follow the published value only while nothing is being dragged.
    const bool dragging = lv_obj_has_state(g_rgbHue, LV_STATE_PRESSED) || lv_obj_has_state(g_rgbSat, LV_STATE_PRESSED) ||
                          lv_obj_has_state(g_rgbSlider, LV_STATE_PRESSED);
    if (live && !dragging)
    {
        const uint32_t c = Rgb_colour();
        const lv_color_hsv_t hsv = lv_color_rgb_to_hsv((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF);
        lv_slider_set_value(g_rgbHue, hsv.h, LV_ANIM_OFF);
        lv_slider_set_value(g_rgbSat, hsv.s, LV_ANIM_OFF);
        lv_slider_set_value(g_rgbSlider, Rgb_brightness() ? Rgb_brightness() : 255, LV_ANIM_OFF);
        showPicked();
    }
    lv_obj_set_style_border_color(g_rgbCard, Rgb_on() ? lv_color_hex(Rgb_colour()) : UI_COL_BORDER, 0);
    lv_obj_set_style_border_width(g_rgbCard, Rgb_on() ? 2 : 1, 0);
}

// --- Date / weather -------------------------------------------------------------------------

void buildInfo(lv_obj_t *parent)
{
    lv_obj_t *card = Theme_card(parent, kColW, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_gap(card, 4, 0);
    g_dateLabel = Theme_label(card, "", UI_FONT_L, UI_COL_TEXT);
    g_weatherLabel = Theme_label(card, "Weather: waiting for a forecast", UI_FONT_M, UI_COL_TEXT_DIM);
}

void refreshInfo()
{
    const time_t now = time(nullptr);
    if (now > 1700000000)
    {
        struct tm local;
        localtime_r(&now, &local);
        char buf[48];
        strftime(buf, sizeof(buf), "%A %d %B", &local);
        Theme_setText(g_dateLabel, buf);
    }
    else
    {
        Theme_setText(g_dateLabel, "Waiting for the clock");
    }
}
} // namespace

lv_obj_t *PageHome_create(lv_obj_t *parent)
{
    lv_obj_t *page = Theme_row(parent, lv_pct(100), lv_pct(100));
    lv_obj_set_style_pad_all(page, UI_GUTTER, 0);
    lv_obj_set_style_pad_gap(page, UI_GUTTER, 0);
    lv_obj_set_flex_align(page, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    buildAc(page);

    lv_obj_t *right = Theme_column(page, kColW, lv_pct(100));
    lv_obj_add_flag(right, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(right, LV_DIR_VER);
    lv_obj_set_style_pad_gap(right, UI_GUTTER, 0);
    buildLight(right);
    buildRgb(right);
    buildInfo(right);

    PageHome_refresh(true);
    return page;
}

void PageHome_refresh(bool force)
{
    (void)force;
    refreshAc();
    refreshLight();
    refreshRgb();
    refreshInfo();
}
