#include "DeviceSheet.h"

#include <algorithm>
#include <stdio.h>
#include <string>
#include <vector>

#include <ArduinoJson.h>
#include <NightMare.h>

#include "App/Bindings.h"
#include "App/Mqttp.h"
#include "App/Net.h"
#include "App/Notifications.h"
#include "App/Registry.h"
#include "Explorer/DeviceView.h"
#include "Explorer/Facts.h"
#include "Explorer/Ping.h"
#include "Explorer/TopicTap.h"
#include "Explorer/Watch.h"
#include "ResourcePage.h"
#include "UI/Bind.h"
#include "UI/Modal.h"
#include "UI/Theme.h"
#include "esp_timer.h"

namespace
{
enum Tab
{
    TabOverview = 0,
    TabResources,
    TabConsole,
    TabPing,
    TabTopics,
    TabCount
};

const char *kTabNames[TabCount] = {"Overview", "Resources", "Console", "Ping", "Topics"};

uint32_t nowMs() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

struct Sheet
{
    bool open = false;
    uint32_t token = 0; // bumped per open: a late callback from an earlier sheet is ignored
    std::string device;
    ModalId modal = 0;
    int tab = -1;
    lv_obj_t *tabButtons[TabCount] = {};
    lv_obj_t *content = nullptr;
    lv_timer_t *timer = nullptr;

    // overview
    uint32_t overviewRevision = UINT32_MAX;
    // resources
    std::vector<std::string> rowNames;
    std::vector<lv_obj_t *> valueLabels;
    std::vector<lv_obj_t *> dots;
    lv_obj_t *resourceHeader = nullptr;
    uint32_t manifestSignature = 0;
    // console
    lv_obj_t *consoleOut = nullptr;
    lv_obj_t *consoleInput = nullptr;
    lv_obj_t *consoleBusy = nullptr;
    std::string consoleText;
    bool consoleWaiting = false;
    // ping
    lv_obj_t *pingStats = nullptr;
    lv_obj_t *pingChart = nullptr;
    PingStats pingSnapshot;
    bool pingDirty = false;
    // topics
    lv_obj_t *topicsList = nullptr;
    lv_obj_t *topicsCount = nullptr;
    lv_obj_t *pauseLabel = nullptr;
    uint32_t topicsRevision = UINT32_MAX;
    bool topicsSubscribed = false;
};

Sheet g;

const Device *device() { return Registry_find(g.device); }

// --- small builders ------------------------------------------------------------------------

lv_obj_t *kv(lv_obj_t *parent, const char *key, const std::string &value, lv_color_t colour = UI_COL_TEXT)
{
    lv_obj_t *row = Theme_row(parent, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(row, 30, 0);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_t *k = Theme_label(row, key, UI_FONT_M, UI_COL_TEXT_DIM);
    lv_obj_set_width(k, 190);
    lv_obj_t *v = Theme_label(row, value.c_str(), UI_FONT_M, colour);
    lv_obj_set_flex_grow(v, 1);
    lv_label_set_long_mode(v, LV_LABEL_LONG_WRAP);
    return v;
}

lv_obj_t *section(lv_obj_t *parent, const char *title)
{
    lv_obj_t *card = Theme_card(parent, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_gap(card, 4, 0);
    Theme_label(card, title, UI_FONT_L, UI_COL_TEXT);
    return card;
}

lv_obj_t *wrapRow(lv_obj_t *parent)
{
    lv_obj_t *row = Theme_row(parent, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(row, 10, 0);
    return row;
}

std::string onlineSince(const Device &d)
{
    char age[24];
    Theme_formatAge(nowMs() - d.lastSeenMs, age, sizeof(age));
    return age;
}

// --- tab: overview ------------------------------------------------------------------------

void refreshInfo()
{
    const Device *d = device();
    if (!d)
    {
        return;
    }
    // Ask the device to re-publish what it retains; the answers arrive as ordinary messages.
    const std::string console = g.device + "/console/in";
    bool ok = Net_publish(console, "INFO PUBLISH");
    ok = Net_publish(console, "INFO PUBLISH SYSTEM") && ok;
    ok = Net_publish(console, "INFO PUBLISH NETWORK") && ok;
    ok = Net_publish(console, ">manifest msgpack") && ok;
    if (ok)
    {
        Notify(NotifySeverity::Info, "explorer", "Refresh requested", g.device);
    }
    else
    {
        Notify(NotifySeverity::Warn, "explorer", "Could not send", "No transport is connected.");
    }
}

void rebootDevice()
{
    const std::string name = g.device;
    Modal_confirm("Reboot " + name + "?", name + " will restart and be unreachable for a few seconds.", "Reboot",
                  Tone::Danger, [name] {
                      if (Net_publish(name + "/console/in", "REBOOT"))
                      {
                          Notify(NotifySeverity::Info, "explorer", "Reboot sent", name);
                      }
                      else
                      {
                          Notify(NotifySeverity::Warn, "explorer", "Could not send", "No transport is connected.");
                      }
                  });
}

// Withdraw everything the device left retained, and forget it locally. A device that is still
// running simply reappears with its next message; this clears what a device that is gone left.
void deleteRetained()
{
    const std::string name = g.device;
    Modal_confirm("Delete " + name + "'s retained state?",
                  "This clears its status, info, telemetry, manifest and resource states from the broker so it "
                  "stops appearing. If it is still running it will come back by itself.",
                  "Delete", Tone::Danger, [name] {
                      const Device *d = Registry_find(name);
                      std::vector<std::string> topics = {name + "/status", name + "/info", name + "/telemetry/system",
                                                         name + "/telemetry/network", name + "/manifest/msgpack",
                                                         name + "/manifest/json", name + "/hardware/msgpack",
                                                         name + "/hardware/json", name + "/manifest/consume/msgpack"};
                      if (d)
                      {
                          for (const auto &entry : d->resources)
                          {
                              topics.push_back(name + "/resource/" + entry.first + "/state");
                          }
                      }
                      int sent = 0;
                      for (const std::string &t : topics)
                      {
                          sent += Net_publishBytes(t, nullptr, 0, true) ? 1 : 0;
                      }
                      Explorer_forgetManifest(name);
                      Registry_forget(name);
                      DeviceSheet_close();
                      Notify(sent ? NotifySeverity::Ok : NotifySeverity::Warn, "explorer",
                             sent ? name + " removed" : "Removed locally only",
                             sent ? std::to_string(sent) + " retained topics cleared" : "No transport is connected.");
                  });
}

void buildOverview(lv_obj_t *root)
{
    const Device *d = device();
    if (!d)
    {
        return;
    }
    const DeviceFacts f = Facts_parse(*d);

    lv_obj_t *chips = Theme_row(root, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_t *c1 = Theme_chip(chips);
    Theme_chipSet(c1, d->online ? "online" : "offline", d->online ? UI_COL_OK : UI_COL_TEXT_DIM);
    if (!d->kind.empty())
    {
        lv_obj_t *c = Theme_chip(chips);
        Theme_chipSet(c, d->kind.c_str(), UI_COL_INFO);
    }
    if (f.hasNetwork && !f.transportActive.empty())
    {
        lv_obj_t *c = Theme_chip(chips);
        Theme_chipSet(c, ("via " + f.transportActive).c_str(), UI_COL_TEXT_DIM);
    }
    if (f.hasBuild && !f.firmware.empty())
    {
        lv_obj_t *c = Theme_chip(chips);
        Theme_chipSet(c, ("firmware " + f.firmware).c_str(), UI_COL_TEXT_DIM);
    }
    const std::string roles = Bindings_rolesOf(d->name, "");
    lv_obj_t *seen = Theme_label(chips, ("last heard " + onlineSince(*d)).c_str(), UI_FONT_S, UI_COL_TEXT_FAINT);
    (void)seen;

    lv_obj_t *id = section(root, "Identity");
    kv(id, "Name", d->name);
    if (!d->hardware.empty())
    {
        kv(id, "Hardware signature", d->hardware);
    }
    if (!f.id.empty())
    {
        kv(id, "Device id", f.id);
    }
    if (!f.timezone.empty())
    {
        kv(id, "Time zone", f.timezone);
    }
    kv(id, "Resources", std::to_string(d->resources.size()) + " published" +
                            (d->manifest.empty() ? "   (no manifest)" : ""));

    if (f.hasHardware)
    {
        lv_obj_t *hw = section(root, "Hardware");
        if (!f.board.empty())
        {
            kv(hw, "Board", f.board);
        }
        kv(hw, "Chip", f.chip + "   " + std::to_string(f.cores) + " core" + (f.cores == 1 ? "" : "s") + "   rev " +
                           std::to_string(f.revision));
        kv(hw, "Flash", Facts_formatBytes(f.flashBytes));
        kv(hw, "Heap", Facts_formatBytes(f.heapBytes));
        if (f.psramBytes)
        {
            kv(hw, "PSRAM", Facts_formatBytes(f.psramBytes));
        }
    }
    if (f.hasBuild || f.hasBoot)
    {
        lv_obj_t *fw = section(root, "Firmware");
        if (f.hasBuild)
        {
            kv(fw, "Version", f.firmware);
            kv(fw, "Built", f.buildDate + " " + f.buildTime);
            if (!f.idfVersion.empty())
            {
                kv(fw, "ESP-IDF", f.idfVersion);
            }
        }
        if (f.hasBoot)
        {
            kv(fw, "Last reset", Facts_resetReason(f.resetReason),
               (f.resetReason == 4 || f.resetReason == 5 || f.resetReason == 6 || f.resetReason == 7 || f.resetReason == 9)
                   ? UI_COL_WARN
                   : UI_COL_TEXT);
        }
    }
    if (f.hasSystem)
    {
        lv_obj_t *sys = section(root, "Health");
        kv(sys, "Uptime", Facts_formatDuration(f.uptimeMs) + "   (at last report)");
        kv(sys, "CPU", std::to_string(f.cpuMhz) + " MHz");
        kv(sys, "Free heap", Facts_formatBytes(f.freeHeap) + "   lowest " + Facts_formatBytes(f.minFreeHeap));
        if (f.freePsram)
        {
            kv(sys, "Free PSRAM", Facts_formatBytes(f.freePsram));
        }
    }
    if (f.hasNetwork)
    {
        lv_obj_t *net = section(root, "Network");
        kv(net, "Transport", f.transportActive + " active, " + f.transportPreferred + " preferred   (" + f.transportState + ")");
        if (!f.ssid.empty())
        {
            kv(net, "Wi-Fi", f.ssid + "   " + f.ip + "   " + std::to_string(f.rssi) + " dBm   ch " + std::to_string(f.channel));
        }
        if (!f.mqttState.empty())
        {
            kv(net, "MQTT", f.mqttState + (f.mqttProfile.empty() ? "" : "   " + f.mqttProfile));
        }
        if (!f.espNowState.empty())
        {
            kv(net, "ESP-NOW", f.espNowState + (f.gatewayConnected ? "   gateway connected" : "") +
                                   (f.rttMs >= 0 ? "   rtt " + std::to_string(f.rttMs) + " ms" : ""));
        }
    }
    if (d->kind == "gateway")
    {
        lv_obj_t *gw = section(root, "Gateway");
        JsonDocument net;
        if (!d->gatewayNetwork.empty() && !deserializeJson(net, d->gatewayNetwork))
        {
            kv(gw, "Radio", std::string(net["radio"]["ssid"] | "") + "   channel " + std::to_string(net["radio"]["channel"] | 0));
            kv(gw, "ESP-NOW", (net["capabilities"]["esp_now"] | false) ? "supported" : "not supported");
            kv(gw, "Remote broker", (net["uplinks"]["remote_mqtt"]["ready"] | false) ? "ready" : "not ready");
            kv(gw, "Local broker", (net["uplinks"]["local_mqtt"]["ready"] | false) ? "ready" : "not ready");
        }
        JsonDocument clients;
        if (!d->gatewayClients.empty() && !deserializeJson(clients, d->gatewayClients))
        {
            JsonArrayConst list = clients["clients"];
            kv(gw, "Clients", std::to_string(list.size()) + " ESP-NOW device(s)");
            for (JsonObjectConst c : list)
            {
                kv(gw, "", std::string(c["name"] | "?") + (c["connected"] | false ? "   connected" : "   disconnected"),
                   UI_COL_TEXT_DIM);
            }
        }
        if (d->gatewayNetwork.empty() && d->gatewayClients.empty())
        {
            Theme_label(gw, "No gateway announcements heard.", UI_FONT_M, UI_COL_TEXT_FAINT);
        }
    }

    lv_obj_t *actions = wrapRow(root);
    Theme_actionButton(actions, LV_SYMBOL_REFRESH "  Refresh", 170, 52, Tone::Neutral, [] { refreshInfo(); });
    if (d->online)
    {
        Theme_actionButton(actions, LV_SYMBOL_POWER "  Reboot", 160, 52, Tone::Danger, [] { rebootDevice(); });
    }
    else
    {
        Theme_actionButton(actions, LV_SYMBOL_TRASH "  Delete retained", 230, 52, Tone::Danger, [] { deleteRetained(); });
    }
    const std::string name = d->name;
    Theme_actionButton(actions, "Use as the AC", 190, 52, Tone::Neutral, [name] { Bind_apply(Role::Ac, name, ""); });
    g.overviewRevision = d->revision;
}

// --- tab: resources -----------------------------------------------------------------------

std::vector<std::string> namesOf(const std::vector<ResourceRow> &rows)
{
    std::vector<std::string> names;
    for (const ResourceRow &r : rows)
    {
        names.push_back(r.name);
    }
    return names;
}

void setResourceHeader(const Device &d)
{
    const Manifest *m = Explorer_manifest(d);
    std::string text = std::to_string(d.resources.size()) + " publishing";
    lv_color_t colour = UI_COL_TEXT_DIM;
    if (!m)
    {
        text += "   -   no manifest";
    }
    else if (!m->ok)
    {
        text += "   -   manifest unreadable: " + m->error;
        colour = UI_COL_WARN;
    }
    else
    {
        text += "   -   " + std::to_string(m->resources.size()) + " declared (manifest v" + std::to_string(m->manifestVersion) + ")";
    }
    Theme_setText(g.resourceHeader, text.c_str());
    lv_obj_set_style_text_color(g.resourceHeader, colour, 0);
}

void onResourceRow(lv_event_t *e)
{
    const std::string *name = static_cast<std::string *>(lv_event_get_user_data(e));
    ResourcePage_open(g.device, *name);
}

void rebuildResources(const std::vector<ResourceRow> &rows)
{
    // Everything below the header is the list.
    while (lv_obj_get_child_count(g.content) > 1)
    {
        lv_obj_delete(lv_obj_get_child(g.content, lv_obj_get_child_count(g.content) - 1));
    }
    g.valueLabels.clear();
    g.dots.clear();
    for (const ResourceRow &r : rows)
    {
        lv_obj_t *row = lv_obj_create(g.content);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, lv_pct(100), 58);
        lv_obj_set_style_bg_color(row, UI_COL_SURFACE, 0);
        lv_obj_set_style_bg_color(row, UI_COL_SURFACE_ALT, LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(row, 12, 0);
        lv_obj_set_style_pad_hor(row, 14, 0);
        lv_obj_set_style_pad_gap(row, 12, 0);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_SCROLL_CHAIN_VER);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        auto *name = new std::string(r.name);
        lv_obj_add_event_cb(row, onResourceRow, LV_EVENT_CLICKED, name);
        lv_obj_add_event_cb(row, [](lv_event_t *e) { delete static_cast<std::string *>(lv_event_get_user_data(e)); },
                            LV_EVENT_DELETE, name);

        g.dots.push_back(Theme_dot(row, 12, UI_COL_TEXT_FAINT));
        lv_obj_t *title = Theme_label(row, r.name.c_str(), UI_FONT_M, UI_COL_TEXT);
        lv_obj_set_width(title, 250);
        lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
        lv_obj_t *kind = Theme_label(row, Explorer_kindLabel(r), UI_FONT_S, r.writable ? UI_COL_ACCENT : UI_COL_TEXT_FAINT);
        lv_obj_set_width(kind, 80);
        lv_obj_t *value = Theme_label(row, "", UI_FONT_M, UI_COL_TEXT_DIM);
        lv_obj_set_flex_grow(value, 1);
        lv_label_set_long_mode(value, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_RIGHT, 0);
        g.valueLabels.push_back(value);
    }
    if (rows.empty())
    {
        Theme_label(g.content, "This device has published no resources yet.", UI_FONT_M, UI_COL_TEXT_FAINT);
    }
}

void refreshResources()
{
    const Device *d = device();
    if (!d)
    {
        return;
    }
    const std::vector<ResourceRow> rows = Explorer_rows(*d);
    setResourceHeader(*d);
    const std::vector<std::string> names = namesOf(rows);
    if (names != g.rowNames)
    {
        g.rowNames = names;
        rebuildResources(rows);
    }
    for (size_t i = 0; i < rows.size() && i < g.valueLabels.size(); ++i)
    {
        const ResourceRow &r = rows[i];
        std::string text;
        lv_color_t colour = UI_COL_TEXT;
        lv_color_t dot = UI_COL_TEXT_FAINT;
        if (r.kind == ResKind::Action)
        {
            text = "tap to run";
            colour = UI_COL_TEXT_FAINT;
        }
        else if (r.kind == ResKind::Event)
        {
            text = "event";
            colour = UI_COL_TEXT_FAINT;
        }
        else if (r.withdrawn)
        {
            text = "withdrawn";
            colour = UI_COL_WARN;
            dot = UI_COL_WARN;
        }
        else if (!r.hasValue)
        {
            text = "no value";
            colour = UI_COL_TEXT_FAINT;
        }
        else
        {
            text = Explorer_display(r.type, r.value);
            dot = UI_COL_OK;
        }
        Theme_setText(g.valueLabels[i], text.c_str());
        lv_obj_set_style_text_color(g.valueLabels[i], colour, 0);
        lv_obj_set_style_bg_color(g.dots[i], dot, 0);
    }
}

void requestManifest()
{
    if (Net_publish(g.device + "/console/in", ">manifest msgpack"))
    {
        Notify(NotifySeverity::Info, "explorer", "Manifest requested", g.device);
    }
    else
    {
        Notify(NotifySeverity::Warn, "explorer", "Could not send", "No transport is connected.");
    }
}

void buildResources(lv_obj_t *root)
{
    lv_obj_t *head = Theme_row(root, lv_pct(100), 52);
    g.resourceHeader = Theme_label(head, "", UI_FONT_M, UI_COL_TEXT_DIM);
    lv_obj_set_flex_grow(g.resourceHeader, 1);
    lv_label_set_long_mode(g.resourceHeader, LV_LABEL_LONG_DOT);
    Theme_actionButton(head, "Request manifest", 210, 48, Tone::Neutral, [] { requestManifest(); });
    g.rowNames.clear();
    g.rowNames.push_back("\x01"); // force the first refresh to build the list
    refreshResources();
}

// --- tab: console -------------------------------------------------------------------------

void appendConsole(const std::string &text)
{
    g.consoleText += text;
    if (g.consoleText.size() > 6000)
    {
        g.consoleText.erase(0, g.consoleText.size() - 6000);
    }
    if (g.consoleOut)
    {
        Theme_setText(g.consoleOut, g.consoleText.c_str());
        lv_obj_scroll_to_y(g.content, LV_COORD_MAX, LV_ANIM_OFF);
    }
}

void runCommand(const std::string &command)
{
    if (command.empty())
    {
        return;
    }
    if (g.consoleWaiting)
    {
        Notify(NotifySeverity::Info, "explorer", "Still waiting for the last command");
        return;
    }
    appendConsole("> " + command + "\n");
    g.consoleWaiting = true;
    if (g.consoleBusy)
    {
        lv_obj_remove_flag(g.consoleBusy, LV_OBJ_FLAG_HIDDEN);
    }
    const uint32_t token = g.token;
    const uint32_t id = Mqttp_request(g.device, command, [token](const MqttpResult &r) {
        if (!g.open || token != g.token)
        {
            return;
        }
        g.consoleWaiting = false;
        if (g.consoleBusy)
        {
            lv_obj_add_flag(g.consoleBusy, LV_OBJ_FLAG_HIDDEN);
        }
        std::string text;
        switch (r.status)
        {
        case MqttpStatus::Ok:
            text = r.response + (r.truncated ? "\n[truncated]" : "") + "\n   (" + std::to_string(r.rttMs) + " ms)\n\n";
            break;
        case MqttpStatus::Timeout:
            text = "[no answer: the device did not reply in time. An empty reply is indistinguishable from silence.]\n\n";
            break;
        case MqttpStatus::Failed:
            text = "[failed: " + r.response + "]\n\n";
            break;
        default:
            text = "[cancelled]\n\n";
            break;
        }
        appendConsole(text);
    });
    if (id == 0)
    {
        g.consoleWaiting = false;
        if (g.consoleBusy)
        {
            lv_obj_add_flag(g.consoleBusy, LV_OBJ_FLAG_HIDDEN);
        }
        appendConsole("[too many requests in flight]\n\n");
    }
}

void buildConsole(lv_obj_t *root)
{
    static const char *presets[] = {"INFO", "INFO SYSTEM", "INFO NETWORK", "STATUS", "TIME", "PING", ">list", "NETWORK GET"};
    lv_obj_t *row = wrapRow(root);
    for (const char *p : presets)
    {
        const std::string command = p;
        Theme_actionButton(row, p, 10 + static_cast<int32_t>(command.size()) * 11 + 30, 48, Tone::Neutral,
                           [command] { runCommand(command); });
    }

    lv_obj_t *inputRow = Theme_row(root, lv_pct(100), 56);
    g.consoleInput = lv_textarea_create(inputRow);
    lv_textarea_set_one_line(g.consoleInput, true);
    lv_textarea_set_placeholder_text(g.consoleInput, "any NightMare command, e.g. CONFIG LIST");
    lv_obj_set_flex_grow(g.consoleInput, 1);
    lv_obj_set_height(g.consoleInput, 52);
    lv_obj_set_style_text_font(g.consoleInput, UI_FONT_M, 0);
    Keyboard_attach(g.consoleInput);
    Theme_actionButton(inputRow, "Send", 110, 52, Tone::Accent, [] {
        const std::string text = lv_textarea_get_text(g.consoleInput);
        runCommand(text);
        lv_textarea_set_text(g.consoleInput, "");
    });
    Theme_actionButton(inputRow, "Clear", 100, 52, Tone::Neutral, [] {
        g.consoleText.clear();
        Theme_setText(g.consoleOut, "");
    });

    lv_obj_t *card = Theme_card(root, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, UI_COL_BG, 0);
    lv_obj_set_style_min_height(card, 160, 0);
    g.consoleBusy = Theme_label(card, "waiting for the device...", UI_FONT_S, UI_COL_ACCENT);
    lv_obj_add_flag(g.consoleBusy, LV_OBJ_FLAG_HIDDEN);
    g.consoleOut = Theme_label(card, g.consoleText.c_str(), UI_FONT_S, UI_COL_TEXT);
    lv_obj_set_width(g.consoleOut, lv_pct(100));
    lv_label_set_long_mode(g.consoleOut, LV_LABEL_LONG_WRAP);
}

// --- tab: ping ----------------------------------------------------------------------------

void showPing()
{
    if (!g.pingStats)
    {
        return;
    }
    const PingStats &s = g.pingSnapshot;
    char buf[200];
    if (s.total == 0)
    {
        Theme_setText(g.pingStats, "Not run yet.");
    }
    else
    {
        snprintf(buf, sizeof(buf), "%s   sent %d / %d   received %d   lost %d (%d%%)\nmin %u ms    avg %.0f ms    max %u ms",
                 s.running ? "running" : "done", s.sent, s.total, s.received, s.lost,
                 s.sent ? s.lost * 100 / s.sent : 0, (unsigned)s.minMs, static_cast<double>(s.avgMs), (unsigned)s.maxMs);
        Theme_setText(g.pingStats, buf);
    }
    if (g.pingChart && s.total > 0)
    {
        lv_chart_set_point_count(g.pingChart, static_cast<uint32_t>(std::max(s.total, 2)));
        lv_chart_series_t *series = lv_chart_get_series_next(g.pingChart, nullptr);
        uint32_t top = 50;
        for (int32_t v : s.rtts)
        {
            top = std::max(top, static_cast<uint32_t>(std::max<int32_t>(v, 0)));
        }
        lv_chart_set_range(g.pingChart, LV_CHART_AXIS_PRIMARY_Y, 0, static_cast<int32_t>(top + top / 5));
        for (int i = 0; i < s.total; ++i)
        {
            lv_chart_set_value_by_id(g.pingChart, series, static_cast<uint32_t>(i),
                                            i < static_cast<int>(s.rtts.size()) && s.rtts[i] >= 0 ? s.rtts[i] : LV_CHART_POINT_NONE);
        }
        lv_chart_refresh(g.pingChart);
    }
}

void startPing(int count)
{
    const uint32_t token = g.token;
    g.pingSnapshot = PingStats();
    Ping_start(g.device, count, [token](const PingStats &s) {
        if (!g.open || token != g.token)
        {
            return;
        }
        g.pingSnapshot = s;
        g.pingDirty = true;
    });
    g.pingSnapshot.total = count;
    g.pingDirty = true;
}

void buildPing(lv_obj_t *root)
{
    lv_obj_t *row = wrapRow(root);
    Theme_actionButton(row, "Ping x5", 150, 52, Tone::Accent, [] { startPing(5); });
    Theme_actionButton(row, "Ping x20", 150, 52, Tone::Neutral, [] { startPing(20); });
    Theme_actionButton(row, "Stop", 120, 52, Tone::Neutral, [] { Ping_stop(); });
    lv_obj_t *note = Theme_label(root,
                                 "Round trip through the active transport and the device's command handler. "
                                 "It answers \"is it responding, and how fast\".",
                                 UI_FONT_S, UI_COL_TEXT_FAINT);
    lv_obj_set_width(note, lv_pct(100));
    lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
    g.pingStats = Theme_label(root, "", UI_FONT_M, UI_COL_TEXT);
    lv_obj_set_width(g.pingStats, lv_pct(100));
    g.pingChart = lv_chart_create(root);
    lv_obj_set_size(g.pingChart, lv_pct(100), 220);
    lv_chart_set_type(g.pingChart, LV_CHART_TYPE_BAR);
    lv_chart_set_div_line_count(g.pingChart, 4, 0);
    lv_obj_set_style_bg_color(g.pingChart, UI_COL_BG, 0);
    lv_obj_set_style_bg_opa(g.pingChart, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g.pingChart, 0, 0);
    lv_obj_set_style_line_color(g.pingChart, UI_COL_BORDER, LV_PART_MAIN);
    lv_obj_set_style_pad_column(g.pingChart, 4, LV_PART_ITEMS);
    lv_chart_add_series(g.pingChart, UI_COL_ACCENT, LV_CHART_AXIS_PRIMARY_Y);
    g.pingSnapshot = Ping_stats().device == g.device ? Ping_stats() : PingStats();
    showPing();
}

// --- tab: topics --------------------------------------------------------------------------

void rebuildTopics()
{
    if (!g.topicsList)
    {
        return;
    }
    lv_obj_clean(g.topicsList);
    size_t shown = 0, matching = 0;
    const uint32_t now = nowMs();
    for (size_t i = 0; i < Tap_count() && shown < 60; ++i)
    {
        const TapEntry &e = Tap_at(i);
        if (!Tap_topicIsFor(e.topic, g.device))
        {
            continue;
        }
        ++matching;
        lv_obj_t *row = Theme_column(g.topicsList, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_style_pad_gap(row, 0, 0);
        lv_obj_set_style_pad_ver(row, 4, 0);
        char age[24];
        Theme_formatAge(now - e.atMs, age, sizeof(age));
        const std::string head = std::string(e.retained ? "R  " : "   ") + e.topic.substr(g.device.size() + 1) + "    " + age;
        Theme_label(row, head.c_str(), UI_FONT_S, UI_COL_ACCENT);
        lv_obj_t *text = Theme_label(row, e.text.c_str(), UI_FONT_S, e.binary ? UI_COL_TEXT_FAINT : UI_COL_TEXT_DIM);
        lv_obj_set_width(text, lv_pct(100));
        lv_label_set_long_mode(text, LV_LABEL_LONG_DOT);
        ++shown;
    }
    if (shown == 0)
    {
        Theme_label(g.topicsList, "Nothing yet. Messages for this device appear here as they arrive.", UI_FONT_M, UI_COL_TEXT_FAINT);
    }
    char buf[48];
    snprintf(buf, sizeof(buf), "%u shown", (unsigned)matching);
    Theme_setText(g.topicsCount, buf);
    g.topicsRevision = Tap_revision();
}

void buildTopics(lv_obj_t *root)
{
    if (!g.topicsSubscribed)
    {
        // Everything the device publishes, not only the families the panel always follows.
        Net_subscribe(g.device + "/#");
        g.topicsSubscribed = true;
        Tap_acquire();
    }
    lv_obj_t *row = Theme_row(root, lv_pct(100), 52);
    lv_obj_t *pause = Theme_actionButton(row, Tap_paused() ? "Resume" : "Pause", 130, 48, Tone::Neutral, [] {
        Tap_setPaused(!Tap_paused());
        Theme_setText(g.pauseLabel, Tap_paused() ? "Resume" : "Pause");
    });
    g.pauseLabel = lv_obj_get_child(pause, 0);
    Theme_actionButton(row, "Clear", 110, 48, Tone::Neutral, [] { Tap_clear(); });
    g.topicsCount = Theme_label(row, "", UI_FONT_M, UI_COL_TEXT_DIM);
    g.topicsList = Theme_column(root, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_gap(g.topicsList, 2, 0);
    rebuildTopics();
}

// --- the sheet ----------------------------------------------------------------------------

void leaveTab(int tab)
{
    if (tab == TabTopics && g.topicsSubscribed)
    {
        Net_unsubscribe(g.device + "/#");
        g.topicsSubscribed = false;
        Tap_release();
    }
}

void styleTabs()
{
    for (int i = 0; i < TabCount; ++i)
    {
        const bool active = (i == g.tab);
        lv_obj_set_style_bg_color(g.tabButtons[i], active ? UI_COL_ACCENT : UI_COL_SURFACE_HI, 0);
        lv_obj_set_style_text_color(g.tabButtons[i], active ? lv_color_hex(0x1A1208) : UI_COL_TEXT, 0);
    }
}

void showTab(int tab)
{
    if (g.tab == tab)
    {
        return;
    }
    leaveTab(g.tab);
    g.tab = tab;
    lv_obj_clean(g.content);
    g.consoleOut = g.consoleInput = g.consoleBusy = nullptr;
    g.pingStats = g.pingChart = nullptr;
    g.topicsList = g.topicsCount = g.pauseLabel = nullptr;
    g.resourceHeader = nullptr;
    g.valueLabels.clear();
    g.dots.clear();
    g.rowNames.clear();
    lv_obj_scroll_to_y(g.content, 0, LV_ANIM_OFF);
    styleTabs();
    switch (tab)
    {
    case TabOverview:
        buildOverview(g.content);
        break;
    case TabResources:
        buildResources(g.content);
        break;
    case TabConsole:
        buildConsole(g.content);
        break;
    case TabPing:
        buildPing(g.content);
        break;
    default:
        buildTopics(g.content);
        break;
    }
}

void onTick(lv_timer_t *)
{
    if (!g.open)
    {
        return;
    }
    const Device *d = device();
    if (!d)
    {
        DeviceSheet_close(); // forgotten under us
        return;
    }
    switch (g.tab)
    {
    case TabOverview:
        if (d->revision != g.overviewRevision)
        {
            lv_obj_clean(g.content);
            buildOverview(g.content);
        }
        break;
    case TabResources:
        refreshResources();
        break;
    case TabPing:
        if (g.pingDirty)
        {
            g.pingDirty = false;
            showPing();
        }
        break;
    case TabTopics:
        if (g.topicsRevision != Tap_revision())
        {
            rebuildTopics();
        }
        break;
    default:
        break;
    }
}

void onClosed()
{
    if (g.timer)
    {
        lv_timer_delete(g.timer);
        g.timer = nullptr;
    }
    leaveTab(g.tab);
    Ping_stop();
    Mqttp_unlisten(g.device);
    g.open = false;
    g.modal = 0;
    g.content = nullptr;
    g.tab = -1;
}
} // namespace

void DeviceSheet_open(const std::string &name)
{
    if (g.open)
    {
        Modal_close(g.modal);
    }
    g = Sheet();
    g.open = true;
    g.token = static_cast<uint32_t>(esp_timer_get_time() / 1000) | 1;
    g.device = name;

    lv_obj_t *body, *footer;
    ModalOptions options;
    options.title = name;
    options.size = ModalSize::Sheet;
    options.onClosed = onClosed;
    g.modal = Modal_open(options, &body, &footer);
    lv_obj_add_flag(footer, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(body, 14, 0);

    // Replies to this device's console are listened for from the moment the sheet opens, so
    // the first command is not racing a subscription.
    Mqttp_listen(name);

    lv_obj_t *tabs = Theme_row(body, lv_pct(100), 52);
    lv_obj_set_style_pad_gap(tabs, 8, 0);
    for (int i = 0; i < TabCount; ++i)
    {
        g.tabButtons[i] = Theme_actionButton(tabs, kTabNames[i], 140, 48, Tone::Neutral, [i] { showTab(i); });
    }

    g.content = Theme_column(body, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(g.content, 1);
    lv_obj_add_flag(g.content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(g.content, LV_DIR_VER);
    lv_obj_set_style_pad_gap(g.content, 10, 0);
    lv_obj_set_style_pad_top(g.content, 6, 0);

    g.timer = lv_timer_create(onTick, 400, nullptr);
    showTab(TabOverview);
}

void DeviceSheet_close()
{
    if (g.open)
    {
        Modal_close(g.modal);
    }
}

bool DeviceSheet_isOpen() { return g.open; }
