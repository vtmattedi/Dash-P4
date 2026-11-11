#include "StackportSheet.h"

#include <algorithm>
#include <stdio.h>
#include <string>
#include <vector>

#include "App/Calendar.h"
#include "App/Feed.h"
#include "App/FeedSync.h"
#include "UI/Modal.h"
#include "UI/Theme.h"

namespace
{
int rank(const FeedItem &item)
{
    return item.status == "down" ? 0 : item.status == "up" ? 2 : 1; // down, then paused/unknown, then up
}

lv_color_t statusColour(const std::string &status)
{
    return status == "down" ? UI_COL_DANGER : status == "up" ? UI_COL_OK : UI_COL_WARN;
}
} // namespace

void StackportSheet_open()
{
    ModalOptions options;
    options.title = "Stackport";
    lv_obj_t *body = nullptr;
    lv_obj_t *footer = nullptr;
    Modal_open(options, &body, &footer);
    if (!body)
    {
        return;
    }

    std::vector<const FeedItem *> services;
    for (const auto &pair : Feed_all())
    {
        if (pair.second.kind == "service")
        {
            services.push_back(&pair.second);
        }
    }
    std::stable_sort(services.begin(), services.end(),
                     [](const FeedItem *a, const FeedItem *b) { return rank(*a) < rank(*b); });

    const ServiceSummary summary = Cal_services();
    char head[64];
    if (summary.total == 0)
    {
        snprintf(head, sizeof(head), "No services yet. Sync to fetch them.");
    }
    else if (summary.down == 0)
    {
        snprintf(head, sizeof(head), "All %u services up", static_cast<unsigned>(summary.total));
    }
    else
    {
        snprintf(head, sizeof(head), "%u of %u down", static_cast<unsigned>(summary.down),
                 static_cast<unsigned>(summary.total));
    }
    lv_obj_t *headline = Theme_label(body, head, UI_FONT_L, summary.down ? UI_COL_DANGER : UI_COL_TEXT);
    lv_obj_set_width(headline, lv_pct(100));

    for (const FeedItem *item : services)
    {
        lv_obj_t *row = Theme_row(body, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_style_pad_gap(row, 12, 0);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        Theme_dot(row, 14, statusColour(item->status));
        lv_obj_t *text = Theme_column(row, 0, LV_SIZE_CONTENT);
        lv_obj_set_flex_grow(text, 1);
        lv_obj_set_style_pad_gap(text, 2, 0);
        lv_obj_t *title = Theme_label(text, item->title.empty() ? item->id.c_str() : item->title.c_str(), UI_FONT_M,
                                      UI_COL_TEXT);
        lv_obj_set_width(title, lv_pct(100));
        if (!item->description.empty())
        {
            lv_obj_t *d = Theme_label(text, item->description.c_str(), UI_FONT_S, UI_COL_TEXT_FAINT);
            lv_obj_set_width(d, lv_pct(100));
        }
        Theme_label(row, item->status.empty() ? "unknown" : item->status.c_str(), UI_FONT_M, statusColour(item->status));
    }

    if (footer)
    {
        Theme_actionButton(footer, "Sync now", 150, UI_TOUCH, Tone::Accent, []() { FeedSync_pollNow(); });
    }
}
