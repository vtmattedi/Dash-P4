#pragma once

// Guition JC1060P470C (I_W_Y): ESP32-P4 + 7" 1024x600 MIPI-DSI panel + ESP32-C6.
//
// Constants only, and nothing but <stdint.h>: this header is meant to be read by
// anything -- the NightMare hardware profile included -- without dragging the
// display stack along. It is the single place a pin number is written down.
//
// Sources: Guition's official data (https://pan.jczn1688.com/directlink/1/HMI%20display/)
// and the pin map of the Espressif P4 Function-EV-Board BSP it is derived from.
// [V] = seen in the vendor's board-specific code or sdkconfig.
// [S] = read from the schematic (official zip, 5-Schematic/JC1060P470C_I_W_Y-V1.0.pdf).
// [E] = only in the EV-board BSP header the vendor reuses; not yet seen working on
//       this PCB -- check against the schematic (5-Schematic/...-V1.0.pdf) first.

#include <stdint.h>

#define BOARD_NAME "guition-jc1060p470c"
#define BOARD_REVISION "v1"

// --- Panel: JD9165, MIPI-DSI, 2 lanes ---------------------------------------
// Two panel generations were sold under this model number. The back-of-shell label says "V2" after the material
// number on the newer one, and that is the generation this board support is for: its init table is the vendor's
// "New_Panel" table (board_jd9165.c), with the 136 pixel horizontal back porch below. The older generation needs the
// vendor's "Old_Panel" table and a 160 pixel back porch; it is not supported here (wrong table = stripes).
#define BOARD_LCD_H_RES 1024 // [V]
#define BOARD_LCD_V_RES 600  // [V]
#define BOARD_LCD_BITS_PER_PIXEL 16 // RGB565

// The schematic wires the panel's reset (net LCD_RST, FPC pin 4) to P4 GPIO0, active low. The EV-board BSP the vendor
// reuses uses it too. Without a reset pulse the panel keeps its state across a warm reboot and the DSI link stalls.
#define BOARD_LCD_RESET_GPIO 0 // [S]
#define BOARD_LCD_DSI_LANES 2     // [V]
#define BOARD_LCD_DSI_LANE_MBPS 750 // [V]
// The DSI PHY is supplied by the P4's on-chip LDO channel 3 at 2.5V and has to be
// powered before the bus is created.
#define BOARD_LCD_DSI_PHY_LDO_CHAN 3     // [V]
#define BOARD_LCD_DSI_PHY_LDO_MV 2500    // [V]

// DPI timing, the vendor's New_Panel BSP: 52 MHz, total 1344 x 635 = 60.9 Hz.
#define BOARD_LCD_DPI_CLOCK_MHZ 52
#define BOARD_LCD_HSYNC_WIDTH 24
#define BOARD_LCD_HSYNC_BACK_PORCH 136
#define BOARD_LCD_HSYNC_FRONT_PORCH 160
#define BOARD_LCD_VSYNC_WIDTH 2
#define BOARD_LCD_VSYNC_BACK_PORCH 21
#define BOARD_LCD_VSYNC_FRONT_PORCH 12

// LVGL renders in strips of this many lines, in two buffers in PSRAM (80 KB each: small enough for the 256 KB L2 cache to
// hold, so drawing is close to internal-RAM speed, and internal RAM stays free for Wi-Fi/TLS), and each strip is copied
// into the panel's single frame buffer.
#define BOARD_LVGL_STRIP_LINES 40

// --- Backlight: LEDC PWM on one GPIO, active high ---------------------------
#define BOARD_BACKLIGHT_GPIO 23 // [V]
#define BOARD_BACKLIGHT_PWM_HZ 20000 // [V]
#define BOARD_BACKLIGHT_PWM_BITS 10  // [V]

// --- I2C: touch + audio codec (+ camera SCCB) share one bus -----------------
#define BOARD_I2C_PORT 1       // [V] touch driver asks the bus on port 1
#define BOARD_I2C_SDA_GPIO 7   // [V]
#define BOARD_I2C_SCL_GPIO 8   // [V]
#define BOARD_I2C_FREQ_HZ 400000 // [V] (the Arduino demo used 100000)

// --- Touch: GT911 -----------------------------------------------------------
// RST and INT are not routed, so the controller picks its address by itself.
#define BOARD_TOUCH_RESET_GPIO (-1) // [V]
#define BOARD_TOUCH_INT_GPIO (-1)   // [V]
#define BOARD_TOUCH_I2C_ADDR 0x5D   // [V] default; 0x14 is the alternative

// --- Audio: ES8311 codec + power amplifier ----------------------------------
#define BOARD_I2S_PORT 1        // [V] CONFIG_BSP_I2S_NUM
#define BOARD_I2S_MCLK_GPIO 13  // [E]
#define BOARD_I2S_SCLK_GPIO 12  // [E]
#define BOARD_I2S_LRCK_GPIO 10  // [E]
#define BOARD_I2S_DOUT_GPIO 9   // [E]
#define BOARD_I2S_DIN_GPIO 48   // [E]
#define BOARD_AUDIO_PA_GPIO 11  // [E] amplifier enable

// --- microSD: SDMMC slot 0, 4-bit -------------------------------------------
#define BOARD_SD_D0_GPIO 39  // [E]
#define BOARD_SD_D1_GPIO 40  // [E]
#define BOARD_SD_D2_GPIO 41  // [E]
#define BOARD_SD_D3_GPIO 42  // [E]
#define BOARD_SD_CLK_GPIO 43 // [E]
#define BOARD_SD_CMD_GPIO 44 // [E]

// --- ESP32-C6 Wi-Fi/BLE co-processor over ESP-Hosted (SDIO slot 1, 4-bit) ---
// The P4 has no radio. These are the pins the ESP-Hosted host driver uses; they
// are repeated in board/sdkconfig.board because Kconfig cannot read this file.
#define BOARD_C6_SDIO_SLOT 1       // [V]
#define BOARD_C6_SDIO_CLK_GPIO 18  // [V]
#define BOARD_C6_SDIO_CMD_GPIO 19  // [V]
#define BOARD_C6_SDIO_D0_GPIO 14   // [V]
#define BOARD_C6_SDIO_D1_GPIO 15   // [V]
#define BOARD_C6_SDIO_D2_GPIO 16   // [V]
#define BOARD_C6_SDIO_D3_GPIO 17   // [V]
#define BOARD_C6_RESET_GPIO 54     // [V] active high
#define BOARD_C6_SDIO_CLOCK_KHZ 40000 // [V]
