/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

// A display on MIPI-DSI on the ESP32-P4 (hal_dsi.h), with the esp_lcd
// component of ESP-IDF: a DSI bus, a DBI channel for the commands of the
// controller, a DPI panel for the video (one frame buffer in PSRAM, which
// esp_lcd allocates and the DMA reads again and again). The controller
// itself is set up by its driver (common/drivers), through command().
// Compiled only for the chips with DSI (main/CMakeLists.txt).
#include "hal_dsi.h"

#include <stdio.h>

#include "esp_cache.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_ldo_regulator.h"

namespace {

esp_ldo_channel_handle_t phyPower = nullptr;    // the regulator of the PHY, kept for good
esp_lcd_dsi_bus_handle_t bus = nullptr;         // the DSI link
esp_lcd_panel_io_handle_t commands = nullptr;   // its DBI channel
esp_lcd_panel_handle_t video = nullptr;         // its DPI channel, once started
hal::dsi::Settings opened = {};                 // the settings given to open()

} // namespace

namespace hal {
namespace dsi {

bool open(const Settings *settings) {
    opened = *settings;

    // the PHY powered by its regulator
    if (settings->phyLdoChannel >= 0) {
        esp_ldo_channel_config_t ldo = {};
        ldo.chan_id = settings->phyLdoChannel;
        ldo.voltage_mv = settings->phyLdoMv;
        if (esp_ldo_acquire_channel(&ldo, &phyPower) != ESP_OK) {
            printf("qjsx: dsi: the regulator of the PHY is refused\n");
            return false;
        }
    }

    // the bus: its lanes at their rate
    esp_lcd_dsi_bus_config_t busConfig = {};
    busConfig.bus_id = 0;
    busConfig.num_data_lanes = (uint8_t)settings->lanes;
    busConfig.phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT;
    busConfig.lane_bit_rate_mbps = (uint32_t)settings->laneMbps;
    if (esp_lcd_new_dsi_bus(&busConfig, &bus) != ESP_OK) {
        printf("qjsx: dsi: the bus is refused\n");
        return false;
    }

    // the command channel: commands and parameters of 8 bits
    esp_lcd_dbi_io_config_t ioConfig = {};
    ioConfig.virtual_channel = 0;
    ioConfig.lcd_cmd_bits = 8;
    ioConfig.lcd_param_bits = 8;
    if (esp_lcd_new_panel_io_dbi(bus, &ioConfig, &commands) != ESP_OK) {
        printf("qjsx: dsi: the command channel is refused\n");
        return false;
    }

    return true;
}

bool command(uint8_t command, const uint8_t *data, size_t size) {
    return esp_lcd_panel_io_tx_param(commands, command, data, size) == ESP_OK;
}

uint16_t *startVideo() {
    // the video: RGB565, one frame buffer, the panel's timings
    esp_lcd_dpi_panel_config_t config = {};
    config.virtual_channel = 0;
    config.dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT;
    config.dpi_clock_freq_mhz = (uint32_t)opened.pixelClockMhz;
    config.in_color_format = LCD_COLOR_FMT_RGB565;
    config.num_fbs = 1;
    config.video_timing.h_size = (uint32_t)opened.width;
    config.video_timing.v_size = (uint32_t)opened.height;
    config.video_timing.hsync_pulse_width = (uint32_t)opened.hsyncPulse;
    config.video_timing.hsync_back_porch = (uint32_t)opened.hsyncBackPorch;
    config.video_timing.hsync_front_porch = (uint32_t)opened.hsyncFrontPorch;
    config.video_timing.vsync_pulse_width = (uint32_t)opened.vsyncPulse;
    config.video_timing.vsync_back_porch = (uint32_t)opened.vsyncBackPorch;
    config.video_timing.vsync_front_porch = (uint32_t)opened.vsyncFrontPorch;
    if (esp_lcd_new_panel_dpi(bus, &config, &video) != ESP_OK) {
        printf("qjsx: dsi: the video channel is refused\n");
        return nullptr;
    }

    // started: the DMA sends the frame buffer from now on
    if (esp_lcd_panel_init(video) != ESP_OK) {
        printf("qjsx: dsi: the video does not start\n");
        return nullptr;
    }

    // its frame buffer
    void *frame = nullptr;
    if (esp_lcd_dpi_panel_get_frame_buffer(video, 1, &frame) != ESP_OK) {
        printf("qjsx: dsi: no frame buffer\n");
        return nullptr;
    }

    return (uint16_t *)frame;
}

void flush(const void *start, size_t size) {
    // from the cache to the memory the DMA reads
    esp_cache_msync((void *)start, size, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
}

} // namespace dsi
} // namespace hal
