#pragma once
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/// Power the MIPI-DSI PHY from the P4's on-chip LDO. Must precede board_display_init().
esp_err_t board_power_dsi_phy(void);

#ifdef __cplusplus
}
#endif
