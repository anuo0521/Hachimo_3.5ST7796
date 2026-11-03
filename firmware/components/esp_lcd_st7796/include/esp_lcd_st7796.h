/*
 * [Input] esp_lcd panel IO handle (4-wire SPI) and panel device configuration.
 * [Output] esp_lcd_panel_t implementation driving an ST7796S 320x480 TFT.
 * [Pos] Local LCD panel driver for the CL35BC1017-40A module.
 * [Sync] If this file changes, update `firmware/.folder.md`.
 */

#pragma once

#include "esp_err.h"
#include "esp_lcd_panel_dev.h"
#include "esp_lcd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Create an ST7796S panel handle on top of an SPI panel IO.
 *
 * The driver performs the vendor init sequence of the CL35BC1017-40A module
 * (320x480, RGB565, 4-wire SPI). Color data is forwarded to the panel exactly as
 * supplied: the caller must hand over big-endian RGB565 bytes, which is what the
 * controller expects on the SPI data phase.
 *
 * @param[in]  io                Panel IO handle created with esp_lcd_new_panel_io_spi().
 * @param[in]  panel_dev_config  Reset GPIO, RGB element order, bits per pixel.
 * @param[out] ret_panel         Created panel handle.
 */
esp_err_t esp_lcd_new_panel_st7796(
  const esp_lcd_panel_io_handle_t io,
  const esp_lcd_panel_dev_config_t *panel_dev_config,
  esp_lcd_panel_handle_t *ret_panel
);

#ifdef __cplusplus
}
#endif
