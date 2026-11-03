#include "ResourcePage.h"

#include <algorithm>
#include <stdio.h>
#include <stdlib.h>

#include <ArduinoJson.h>
#include <NightMare.h>

#include "App/Net.h"
#include "App/Notifications.h"
#include "App/Registry.h"
#include "Explorer/DeviceView.h"
#include "Explorer/Watch.h"
#include "UI/Bind.h"
#include "UI/Modal.h"
#include "UI/Theme.h"
#include "esp_timer.h"

namespace
{
uint32_t nowMs() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

struct Ctx
{
    std::string device;
    std::string resource;
    ResourceSpec spec;      // a copy: the cached manifest may be re-parsed under us
    bool inManifest = false;
    ValueType type = ValueType::Unknown;
    ModalId modal = 0;
    lv_timer_t *timer = nullptr;
    lv_obj_t *value = nullptr;
    lv_obj_t *age = nullptr;
    lv_obj_t *chart = nullptr;
    lv_obj_t *chartNote = nullptr;
    lv_obj_t *watchButton = nullptr;
    uint32_t chartRevision = UINT32_MAX;
};

bool numeric(const std::string &s, bool integer)
{
    if (s.empty())
    {
        return false;
    }
    char *end = nullptr;
    if (integer)
    {
        strtol(s.c_str(), &end, 10);
    }
    else
    {
        strtod(s.c_str(), &end);
    }
    return end && *end == '\0';
}

// --- live value -------------------------------------------------------------------------

void drawChart(Ctx *c)
{
    if (!c->chart)
    {
        return;
    }
    const std::vector<WatchSample> *samples = Watch_samples(c->device, c->resource);
    if (!samples)
    {
        lv_obj_add_flag(c->chart, LV_OBJ_FLAG_HIDDEN);
        Theme_setText(c->chartNote, "Not watched. Watch it to record a history.");
        return;
    }
    if (c->chartRevision == Watch_revision())
    {
        return;
    }
    c->chartRevision = Watch_revision();
    if (samples->size() < 2)
    {
        lv_obj_add_flag(c->chart, LV_OBJ_FLAG_HIDDEN);
        Theme_setText(c->chartNote, "Watching. Waiting for more samples...");
        return;
    }
    lv_obj_remove_flag(c->chart, LV_OBJ_FLAG_HIDDEN);
    float lo = (*samples)[0].value, hi = lo;
    for (const WatchSample &s : *samples)
    {
        lo = std::min(lo, s.value);
        hi = std::max(hi, s.value);
    }
    if (hi - lo < 0.001f)
    {
        hi = lo + 1;
    }
    const float pad = (hi - lo) * 0.1f;
    lv_chart_set_point_count(c->chart, static_cast<uint32_t>(samples->size()));
    lv_chart_series_t *series = lv_chart_get_series_next(c->chart, nullptr);
    lv_chart_set_range(c->chart, LV_CHART_AXIS_PRIMARY_Y, static_cast<int32_t>((lo - pad) * 100),
                       static_cast<int32_t>((hi + pad) * 100));
    for (size_t i = 0; i < samples->size(); ++i)
    {
        lv_chart_set_value_by_id(c->chart, series, static_cast<uint32_t>(i),
                                        static_cast<int32_t>((*samples)[i].value * 100));
    }
    lv_chart_refresh(c->chart);
    char buf[96];
    snprintf(buf, sizeof(buf), "%u samples   min %g   max %g", (unsigned)samples->size(), static_cast<double>(lo),
             static_cast<double>(hi));
    Theme_setText(c->chartNote, buf);
}

void refresh(Ctx *c)
{
    const Device *d = Registry_find(c->device);
    const ResourceState *s = d ? Registry_value(c->device, c->resource) : nullptr;
    if (c->spec.kind == ResKind::Value && c->value)
    {
        if (!s)
        {
            Theme_setText(c->value, "no value yet");
            lv_obj_set_style_text_color(c->value, UI_COL_TEXT_FAINT, 0);
            Theme_setText(c->age, "the device has not published this resource's state");
        }
        else if (s->withdrawn)
        {
            Theme_setText(c->value, "withdrawn");
            lv_obj_set_style_text_color(c->value, UI_COL_WARN, 0);
            Theme_setText(c->age, "the owner published an empty state");
        }
        else
        {
            Theme_setText(c->value, Explorer_display(c->type, s->value, 200).c_str());
            lv_obj_set_style_text_color(c->value, UI_COL_TEXT, 0);
            char age[24], line[96];
            Theme_formatAge(nowMs() - s->lastSeenMs, age, sizeof(age));
            snprintf(line, sizeof(line), "%s   %s%s", age, s->retained ? "retained" : "live",
                     s->updates > 1 ? ("   " + std::to_string(s->updates) + " updates").c_str() : "");
            Theme_setText(c->age, line);
        }
    }
    drawChart(c);
}

void onTimer(lv_timer_t *t) { refresh(static_cast<Ctx *>(lv_timer_get_user_data(t))); }

void onClosed(Ctx *c)
{
    if (c->timer)
    {
        lv_timer_delete(c->timer);
    }
    delete c;
}

// --- editing ----------------------------------------------------------------------------

void setBool(Ctx *c, bool on)
{
    if (!Resource_set(c->device, c->resource, on ? "true" : "false"))
    {
        Notify(NotifySeverity::Warn, "explorer", "Could not send", "No transport is connected.");
        return;
    }
    Notify(NotifySeverity::Info, "explorer", "Set sent", c->device + " / " + c->resource + " = " + (on ? "true" : "false"));
}

void editValue(Ctx *c)
{
    const ResourceState *s = Registry_value(c->device, c->resource);
    FormField f;
    f.kind = FormField::Kind::Text;
    f.key = "value";
    f.label = std::string("New value (") + ValueType_name(c->type == ValueType::Unknown ? ValueType::String : c->type) + ")";
    f.value = s && !s->withdrawn ? s->value : "";
    f.hint = c->type == ValueType::Colour ? "#RRGGBB or a number" : "value";
    const ValueType type = c->type;
    const std::string device = c->device, resource = c->resource;
    Modal_form("Set " + c->resource, {f}, "Send", Tone::Accent,
               [type, device, resource](const FormValues &v) -> std::string {
                   std::string payload = v.at("value");
                   if (type == ValueType::Integer && !numeric(payload, true))
                   {
                       return "That is not a whole number.";
                   }
                   if (type == ValueType::Float && !numeric(payload, false))
                   {
                       return "That is not a number.";
                   }
                   if (type == ValueType::Boolean && payload != "true" && payload != "false" && payload != "1" && payload != "0")
                   {
                       return "Use true or false.";
                   }
                   if (type == ValueType::Colour && !payload.empty() && payload[0] == '#')
                   {
                       char *end = nullptr;
                       const unsigned long rgb = strtoul(payload.c_str() + 1, &end, 16);
                       if (!end || *end != '\0' || payload.size() != 7)
                       {
                           return "Use #RRGGBB.";
                       }
                       payload = std::to_string(rgb);
                   }
                   if (!Resource_set(device, resource, payload))
                   {
                       return "Could not send. Is the panel connected?";
                   }
                   Notify(NotifySeverity::Info, "explorer", "Set sent", device + " / " + resource + " = " + payload);
                   return "";
               });
}

// --- actions ----------------------------------------------------------------------------

std::string buildPayload(const ResourceSpec &spec, const FormValues &v, std::string &problem)
{
    JsonDocument doc;
    for (const ArgSpec &arg : spec.args)
    {
        const std::string &text = v.at(arg.name);
        if (text.empty())
        {
            if (arg.required)
            {
                problem = arg.name + " is required";
                return "";
            }
            continue;
        }
        switch (arg.type)
        {
        case ValueType::Boolean:
            doc[arg.name] = (text == "true" || text == "1");
            break;
        case ValueType::Integer:
            if (!numeric(text, true))
            {
                problem = arg.name + " must be a whole number";
                return "";
            }
            doc[arg.name] = static_cast<long>(strtol(text.c_str(), nullptr, 10));
            break;
        case ValueType::Float:
            if (!numeric(text, false))
            {
                problem = arg.name + " must be a number";
                return "";
            }
            doc[arg.name] = strtod(text.c_str(), nullptr);
            break;
        default:
            doc[arg.name] = text;
            break;
        }
    }
    std::string out;
    serializeJson(doc, out);
    return out;
}

void invokeAction(Ctx *c)
{
    const std::string device = c->device, resource = c->resource;
    const ResourceSpec spec = c->spec;
    if (spec.args.empty())
    {
        Modal_confirm("Run " + resource + "?", device + " will be asked to run this action.", "Run", Tone::Accent,
                      [device, resource] {
                          if (!Resource_invoke(device, resource, ""))
                          {
                              Notify(NotifySeverity::Warn, "explorer", "Could not send", "No transport is connected.");
                              return;
                          }
                          Notify(NotifySeverity::Info, "explorer", resource + " invoked", device);
                      });
        return;
    }
    std::vector<FormField> fields;
    for (const ArgSpec &arg : spec.args)
    {
        FormField f;
        f.key = arg.name;
        f.label = arg.name + "  (" + ValueType_name(arg.type) + ")";
        f.required = arg.required;
        if (arg.type == ValueType::Boolean)
        {
            f.kind = FormField::Kind::Switch;
            f.value = "false";
        }
        else
        {
            f.kind = FormField::Kind::Text;
            f.hint = arg.required ? "required" : "optional";
        }
        fields.push_back(f);
    }
    Modal_form("Run " + resource, fields, "Run", Tone::Accent, [spec, device, resource](const FormValues &v) -> std::string {
        std::string problem;
        const std::string payload = buildPayload(spec, v, problem);
        if (!problem.empty())
        {
            return problem;
        }
        if (!Resource_invoke(device, resource, payload))
        {
            return "Could not send. Is the panel connected?";
        }
        Notify(NotifySeverity::Info, "explorer", resource + " invoked", device + "  " + payload);
        return "";
    });
}

// --- the page ---------------------------------------------------------------------------

lv_obj_t *button(lv_obj_t *parent, const char *text, int32_t w, Tone tone, std::function<void()> fn)
{
    lv_obj_t *b = Theme_button(parent, text, w, 52, tone);
    auto *heap = new std::function<void()>(std::move(fn));
    lv_obj_add_event_cb(b, [](lv_event_t *e) { (*static_cast<std::function<void()> *>(lv_event_get_user_data(e)))(); },
                        LV_EVENT_CLICKED, heap);
    lv_obj_add_event_cb(b, [](lv_event_t *e) { delete static_cast<std::function<void()> *>(lv_event_get_user_data(e)); },
                        LV_EVENT_DELETE, heap);
    return b;
}

lv_obj_t *wrapRow(lv_obj_t *parent)
{
    lv_obj_t *row = Theme_row(parent, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(row, 10, 0);
    return row;
}
} // namespace

bool Resource_set(const std::string &device, const std::string &resource, const std::string &payload)
{
    return Net_publish(device + "/resource/" + resource + "/set", payload, false);
}

bool Resource_invoke(const std::string &device, const std::string &resource, const std::string &payloadJson)
{
    return Net_publishBytes(device + "/resource/" + resource + "/invoke",
                            reinterpret_cast<const uint8_t *>(payloadJson.data()), payloadJson.size(), false);
}

void ResourcePage_open(const std::string &device, const std::string &resource)
{
    const Device *d = Registry_find(device);
    if (!d)
    {
        return;
    }
    Ctx *c = new Ctx();
    c->device = device;
    c->resource = resource;
    for (const ResourceRow &row : Explorer_rows(*d))
    {
        if (row.name == resource)
        {
            c->inManifest = row.inManifest;
            c->type = row.type;
            if (row.spec)
            {
                c->spec = *row.spec;
            }
            else
            {
                c->spec.name = row.name;
                c->spec.kind = row.kind;
                c->spec.type = row.type;
            }
        }
    }
    const bool isValue = c->spec.kind == ResKind::Value;

    lv_obj_t *body, *footer;
    ModalOptions options;
    options.title = device + "  /  " + resource;
    options.size = ModalSize::Wide;
    options.onClosed = [c] { onClosed(c); };
    c->modal = Modal_open(options, &body, &footer);

    // What it is.
    lv_obj_t *chips = Theme_row(body, lv_pct(100), LV_SIZE_CONTENT);
    ResourceRow shown;
    shown.kind = c->spec.kind;
    shown.writable = c->spec.writable;
    lv_obj_t *kindChip = Theme_chip(chips);
    Theme_chipSet(kindChip, Explorer_kindLabel(shown), UI_COL_INFO);
    if (c->type != ValueType::Unknown)
    {
        lv_obj_t *typeChip = Theme_chip(chips);
        Theme_chipSet(typeChip, ValueType_name(c->type), UI_COL_TEXT_DIM);
    }
    if (!c->inManifest)
    {
        lv_obj_t *chip = Theme_chip(chips);
        Theme_chipSet(chip, "not in manifest", UI_COL_WARN);
    }
    if (!c->spec.dependsOn.empty())
    {
        lv_obj_t *chip = Theme_chip(chips);
        Theme_chipSet(chip, ("mirrors " + c->spec.dependsOn).c_str(), UI_COL_TEXT_DIM);
    }
    const std::string roles = Bindings_rolesOf(device, resource);
    if (!roles.empty())
    {
        lv_obj_t *chip = Theme_chip(chips);
        Theme_chipSet(chip, ("used as " + roles).c_str(), UI_COL_ACCENT);
    }

    if (isValue)
    {
        lv_obj_t *card = Theme_card(body, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(card, UI_COL_BG, 0);
        c->value = Theme_label(card, "", UI_FONT_XL, UI_COL_TEXT);
        lv_obj_set_width(c->value, lv_pct(100));
        lv_label_set_long_mode(c->value, LV_LABEL_LONG_WRAP);
        c->age = Theme_label(card, "", UI_FONT_S, UI_COL_TEXT_DIM);

        // Edit.
        if (c->spec.writable || !c->inManifest)
        {
            lv_obj_t *row = wrapRow(body);
            if (c->type == ValueType::Boolean)
            {
                button(row, "Turn on", 160, Tone::Ok, [c] { setBool(c, true); });
                button(row, "Turn off", 160, Tone::Neutral, [c] { setBool(c, false); });
            }
            else
            {
                button(row, "Set value...", 190, Tone::Accent, [c] { editValue(c); });
            }
            if (!c->inManifest)
            {
                Theme_label(row, "Not declared in a manifest, so it may be read-only.", UI_FONT_S, UI_COL_TEXT_FAINT);
            }
        }

        // Watch.
        const bool plottable = c->type == ValueType::Integer || c->type == ValueType::Float || c->type == ValueType::Boolean;
        if (plottable)
        {
            lv_obj_t *row = wrapRow(body);
            const std::string dev = device, res = resource;
            c->watchButton = button(row, Watch_isWatched(dev, res) ? "Stop watching" : "Watch", 190, Tone::Neutral,
                                    [c, dev, res] {
                                        if (!Watch_toggle(dev, res))
                                        {
                                            Notify(NotifySeverity::Warn, "explorer", "Watch list is full",
                                                   "Stop watching something first.");
                                            return;
                                        }
                                        PersistentSettings.set("explorer_watch", Watch_serialize());
                                        lv_obj_t *label = lv_obj_get_child(c->watchButton, 0);
                                        Theme_setText(label, Watch_isWatched(dev, res) ? "Stop watching" : "Watch");
                                        c->chartRevision = UINT32_MAX;
                                        refresh(c);
                                    });

            c->chart = lv_chart_create(body);
            lv_obj_set_size(c->chart, lv_pct(100), 150);
            lv_chart_set_type(c->chart, LV_CHART_TYPE_LINE);
            lv_chart_set_div_line_count(c->chart, 3, 0);
            lv_obj_set_style_bg_color(c->chart, UI_COL_BG, 0);
            lv_obj_set_style_bg_opa(c->chart, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(c->chart, 0, 0);
            lv_obj_set_style_line_color(c->chart, UI_COL_BORDER, LV_PART_MAIN);
            lv_obj_set_style_size(c->chart, 0, 0, LV_PART_INDICATOR);
            lv_obj_set_style_line_width(c->chart, 3, LV_PART_ITEMS);
            lv_chart_add_series(c->chart, UI_COL_ACCENT, LV_CHART_AXIS_PRIMARY_Y);
            lv_obj_add_flag(c->chart, LV_OBJ_FLAG_HIDDEN);
            c->chartNote = Theme_label(body, "", UI_FONT_S, UI_COL_TEXT_DIM);
        }
    }
    else if (c->spec.kind == ResKind::Action)
    {
        lv_obj_t *card = Theme_card(body, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(card, UI_COL_BG, 0);
        if (c->spec.args.empty())
        {
            Theme_label(card, "Takes no arguments.", UI_FONT_M, UI_COL_TEXT_DIM);
        }
        for (const ArgSpec &arg : c->spec.args)
        {
            Theme_label(card, (arg.name + "   " + ValueType_name(arg.type) + (arg.required ? "   required" : "   optional")).c_str(),
                        UI_FONT_M, UI_COL_TEXT);
        }
        lv_obj_t *row = wrapRow(body);
        button(row, "Run...", 190, Tone::Accent, [c] { invokeAction(c); });
    }
    else
    {
        lv_obj_t *card = Theme_card(body, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(card, UI_COL_BG, 0);
        Theme_label(card, "An event is something that happened, published once and not retained, so there is no current value. "
                          "Open the device's Topics tab to see occurrences as they arrive.",
                    UI_FONT_M, UI_COL_TEXT_DIM);
        lv_obj_set_width(lv_obj_get_child(card, 0), lv_pct(100));
        lv_label_set_long_mode(lv_obj_get_child(card, 0), LV_LABEL_LONG_WRAP);
    }

    // Give it a job on this panel.
    if (isValue)
    {
        Theme_label(body, "Use on this panel", UI_FONT_S, UI_COL_TEXT_DIM);
        lv_obj_t *row = wrapRow(body);
        const std::string dev = device, res = resource;
        if (c->type == ValueType::Boolean)
        {
            button(row, "Light", 130, Tone::Neutral, [dev, res] { Bind_apply(Role::Light, dev, res); });
            button(row, "Door sensor", 170, Tone::Neutral, [dev, res] { Bind_apply(Role::Door, dev, res); });
        }
        if (c->type == ValueType::Integer || c->type == ValueType::Colour)
        {
            button(row, "Colour light", 170, Tone::Neutral, [dev, res] { Bind_apply(Role::Rgb, dev, res); });
        }
        button(row, "Resting corner", 190, Tone::Neutral, [dev, res] { Bind_apply(Role::Rest, dev, res); });
        button(row, "This device is the AC", 260, Tone::Neutral, [dev] { Bind_apply(Role::Ac, dev, ""); });
    }

    c->timer = lv_timer_create(onTimer, 500, c);
    refresh(c);
}
