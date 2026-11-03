#pragma once

#include <NightMare/HardwareProfile.h>

// The pin numbers come from board/board_pins.h, which is plain constants, so unlike
// the older panel's copy this one cannot drift from the code that drives the pins.
#include "board_pins.h"

namespace NMHardware
{

#define NMHW_STR_(x) #x
#define NMHW_GPIO_(n) "GPIO" NMHW_STR_(n)
#define NMHW_GPIO(n) NMHW_GPIO_(n)
#define NMHW_COUNT(a) (sizeof(a) / sizeof((a)[0]))
#define NMHW_LINK(pin, device, terminal)                                      \
    {                                                                         \
        {"", EndpointKind::DeviceTerminal, "mcu", NMHW_GPIO(pin)},            \
        {"", EndpointKind::DeviceTerminal, device, terminal},                 \
    }

/*
 * Dash-P4 ("Lestrade")
 *
 * Physical hardware: Guition JC1060P470C -- one market-board assembly. Every
 * connection below is internal to that PCB.
 *
 * Terminals marked [E] in board_pins.h (audio, microSD) come from the Espressif
 * EV-board pin map the vendor reuses and are not yet confirmed on this PCB; they
 * are listed because the board has the parts, and are the first thing to check
 * against the schematic.
 */
inline Profile projectProfile()
{
    static const Terminal mcuTerminals[] = {
        // Backlight, touch bus
        {NMHW_GPIO(BOARD_BACKLIGHT_GPIO)},
        {NMHW_GPIO(BOARD_I2C_SDA_GPIO)},
        {NMHW_GPIO(BOARD_I2C_SCL_GPIO)},
        // Audio
        {NMHW_GPIO(BOARD_I2S_MCLK_GPIO)},
        {NMHW_GPIO(BOARD_I2S_SCLK_GPIO)},
        {NMHW_GPIO(BOARD_I2S_LRCK_GPIO)},
        {NMHW_GPIO(BOARD_I2S_DOUT_GPIO)},
        {NMHW_GPIO(BOARD_I2S_DIN_GPIO)},
        {NMHW_GPIO(BOARD_AUDIO_PA_GPIO)},
        // microSD
        {NMHW_GPIO(BOARD_SD_D0_GPIO)},
        {NMHW_GPIO(BOARD_SD_D1_GPIO)},
        {NMHW_GPIO(BOARD_SD_D2_GPIO)},
        {NMHW_GPIO(BOARD_SD_D3_GPIO)},
        {NMHW_GPIO(BOARD_SD_CLK_GPIO)},
        {NMHW_GPIO(BOARD_SD_CMD_GPIO)},
        // ESP32-C6 link
        {NMHW_GPIO(BOARD_C6_SDIO_CLK_GPIO)},
        {NMHW_GPIO(BOARD_C6_SDIO_CMD_GPIO)},
        {NMHW_GPIO(BOARD_C6_SDIO_D0_GPIO)},
        {NMHW_GPIO(BOARD_C6_SDIO_D1_GPIO)},
        {NMHW_GPIO(BOARD_C6_SDIO_D2_GPIO)},
        {NMHW_GPIO(BOARD_C6_SDIO_D3_GPIO)},
        {NMHW_GPIO(BOARD_C6_RESET_GPIO)},
    };

    // MIPI-DSI uses the P4's dedicated pads, not GPIOs.
    static const Terminal displayTerminals[] = {{"DSI"}, {"BL"}};
    static const Terminal touchTerminals[] = {{"SDA"}, {"SCL"}};
    static const Terminal codecTerminals[] = {{"MCLK"}, {"SCLK"}, {"LRCK"}, {"DOUT"}, {"DIN"}, {"PA"}};
    static const Terminal sdTerminals[] = {{"D0"}, {"D1"}, {"D2"}, {"D3"}, {"CLK"}, {"CMD"}};
    static const Terminal c6Terminals[] = {{"CLK"}, {"CMD"}, {"D0"}, {"D1"}, {"D2"}, {"D3"}, {"RESET"}};

    static const Device devices[] = {
        {"mcu", mcuTerminals, NMHW_COUNT(mcuTerminals), "ESP32-P4", "mcu", "ESP32-P4"},
        {"display", displayTerminals, NMHW_COUNT(displayTerminals), "TFT display", "display", "JD9165"},
        {"touch", touchTerminals, NMHW_COUNT(touchTerminals), "Capacitive touch controller", "touch_controller", "GT911"},
        {"codec", codecTerminals, NMHW_COUNT(codecTerminals), "Audio codec and amplifier", "audio", "ES8311"},
        {"sd", sdTerminals, NMHW_COUNT(sdTerminals), "microSD", "storage", "microSD"},
        {"radio", c6Terminals, NMHW_COUNT(c6Terminals), "Wi-Fi/BLE co-processor", "radio", "ESP32-C6"},
    };

    static const Connection internalConnections[] = {
        NMHW_LINK(BOARD_BACKLIGHT_GPIO, "display", "BL"),
        NMHW_LINK(BOARD_I2C_SDA_GPIO, "touch", "SDA"),
        NMHW_LINK(BOARD_I2C_SCL_GPIO, "touch", "SCL"),

        NMHW_LINK(BOARD_I2S_MCLK_GPIO, "codec", "MCLK"),
        NMHW_LINK(BOARD_I2S_SCLK_GPIO, "codec", "SCLK"),
        NMHW_LINK(BOARD_I2S_LRCK_GPIO, "codec", "LRCK"),
        NMHW_LINK(BOARD_I2S_DOUT_GPIO, "codec", "DOUT"),
        NMHW_LINK(BOARD_I2S_DIN_GPIO, "codec", "DIN"),
        NMHW_LINK(BOARD_AUDIO_PA_GPIO, "codec", "PA"),

        NMHW_LINK(BOARD_SD_D0_GPIO, "sd", "D0"),
        NMHW_LINK(BOARD_SD_D1_GPIO, "sd", "D1"),
        NMHW_LINK(BOARD_SD_D2_GPIO, "sd", "D2"),
        NMHW_LINK(BOARD_SD_D3_GPIO, "sd", "D3"),
        NMHW_LINK(BOARD_SD_CLK_GPIO, "sd", "CLK"),
        NMHW_LINK(BOARD_SD_CMD_GPIO, "sd", "CMD"),

        NMHW_LINK(BOARD_C6_SDIO_CLK_GPIO, "radio", "CLK"),
        NMHW_LINK(BOARD_C6_SDIO_CMD_GPIO, "radio", "CMD"),
        NMHW_LINK(BOARD_C6_SDIO_D0_GPIO, "radio", "D0"),
        NMHW_LINK(BOARD_C6_SDIO_D1_GPIO, "radio", "D1"),
        NMHW_LINK(BOARD_C6_SDIO_D2_GPIO, "radio", "D2"),
        NMHW_LINK(BOARD_C6_SDIO_D3_GPIO, "radio", "D3"),
        NMHW_LINK(BOARD_C6_RESET_GPIO, "radio", "RESET"),
    };

    static const HardwareDefinition definitions[] = {
        {
            "jc1060p470c",
            AssemblyKind::MarketBoard,
            "JC1060P470C",
            "Guition JC1060P470C",
            nullptr,

            {
                nullptr,
                0,

                devices,
                NMHW_COUNT(devices),

                nullptr,
                0,

                internalConnections,
                NMHW_COUNT(internalConnections),
            },
        },
    };

    // One physical assembly, and it is the one running the firmware.
    static const Assembly roots[] = {
        {
            "main",
            "jc1060p470c",
            "Lestrade",
        },
    };

    return {
        "main",

        definitions,
        NMHW_COUNT(definitions),

        roots,
        NMHW_COUNT(roots),

        nullptr,
        0,
    };
}

#undef NMHW_LINK
#undef NMHW_COUNT
#undef NMHW_GPIO
#undef NMHW_GPIO_
#undef NMHW_STR_

} // namespace NMHardware
