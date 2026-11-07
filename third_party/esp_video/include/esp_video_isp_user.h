/*
 * Dash-P4 addition to esp_video (see third_party/NOTES.md); not part of Espressif's release.
 *
 * The ISP pipeline controller runs Espressif's tuning loop (esp_ipa) on every frame, and the loop rewrites
 * saturation, contrast and the white-balance gains each cycle, so a value written straight to the ISP is gone a
 * frame later. This is the user's say on top of the loop: the pipeline controller applies it to what the loop
 * computed, every cycle. With the defaults below nothing changes.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float exposure_ev;  /*!< Exposure compensation in EV (stops), -3..+3. 0 = the tuning loop's own target. Applied by
                             scaling the brightness measurements the auto-exposure loop sees, so it settles at a
                             correspondingly brighter (+) or darker (-) real exposure, within its exposure and gain
                             limits. */
    float saturation;   /*!< Multiplies the tuned saturation. 1.0 = as tuned, 0 = grey, up to 2.0 */
    float contrast;     /*!< Multiplies the tuned contrast. 1.0 = as tuned */
    int32_t brightness; /*!< Added to the tuned ISP brightness (-128..127). 0 = as tuned. The ISP applies it after
                             the exposure and white-balance statistics are taken, so the auto-exposure loop does not
                             work against it. */
    bool wb_manual;     /*!< false: the tuning loop's automatic white balance. true: hold red_gain / blue_gain */
    float red_gain;     /*!< Used with wb_manual. 0 or less: hold the automatic gain in effect right now */
    float blue_gain;    /*!< Used with wb_manual. 0 or less: hold the automatic gain in effect right now */
} esp_video_isp_user_t;

#define ESP_VIDEO_ISP_USER_DEFAULT() \
    { .exposure_ev = 0.0f, .saturation = 1.0f, .contrast = 1.0f, .brightness = 0, .wb_manual = false, .red_gain = 0.0f, .blue_gain = 0.0f }

/**
 * @brief Set the user's adjustments. Takes effect with the next ISP pipeline cycle (a frame). Safe from any task,
 *        before or after the pipeline starts.
 */
esp_err_t esp_video_isp_pipeline_set_user(const esp_video_isp_user_t *user);

/**
 * @brief Read them back. red_gain / blue_gain are the gains in effect (a held manual gain, or the automatic one
 *        the tuning loop computed last), so a UI can show where the white balance is.
 */
void esp_video_isp_pipeline_get_user(esp_video_isp_user_t *user);

#ifdef __cplusplus
}
#endif
