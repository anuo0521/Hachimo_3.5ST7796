/*
 * [Input] esp_lcd SPI panel IO, reset GPIO, and RGB565 frames.
 * [Output] ST7796S panel vtable: init sequence, windowed GRAM writes, MADCTL control.
 * [Pos] Local LCD panel driver for the CL35BC1017-40A 3.5" module.
 * [Sync] If this file changes, update `firmware/.folder.md`.
 */

#include "esp_lcd_st7796.h"

#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_interface.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "lcd_st7796";

#define ST7796_CMD_SLPOUT 0x11
#define ST7796_CMD_DISPOFF 0x28
#define ST7796_CMD_DISPON 0x29
#define ST7796_CMD_CASET 0x2A
#define ST7796_CMD_RASET 0x2B
#define ST7796_CMD_RAMWR 0x2C
#define ST7796_CMD_MADCTL 0x36
#define ST7796_CMD_COLMOD 0x3A
#define ST7796_CMD_INVOFF 0x20
#define ST7796_CMD_INVON 0x21
#define ST7796_CMD_SLPIN 0x10

#define ST7796_MADCTL_MY 0x80
#define ST7796_MADCTL_MX 0x40
#define ST7796_MADCTL_MV 0x20
#define ST7796_MADCTL_BGR 0x08

#define ST7796_COLMOD_RGB565 0x55

typedef struct {
  esp_lcd_panel_t base;
  esp_lcd_panel_io_handle_t io;
  int reset_gpio_num;
  bool reset_level;
  int x_gap;
  int y_gap;
  uint8_t madctl_val;
  uint8_t colmod_val;
  uint8_t fb_bits_per_pixel;
} st7796_panel_t;

static esp_err_t st7796_tx_param(esp_lcd_panel_io_handle_t io, int cmd, const void *param, size_t len) {
  return esp_lcd_panel_io_tx_param(io, cmd, param, len);
}

/*
 * Vendor sequence exported by 鸿讯电子 for CL35BC1017-40A (ST7796S, 4-line SPI).
 * Command set unlock (0xF0 0xC3 / 0xF0 0x96) and re-lock (0xF0 0x3C / 0xF0 0x69)
 * bracket the panel-specific tuning; the gamma pairs were measured for this glass.
 */
static esp_err_t st7796_run_init_sequence(st7796_panel_t *panel) {
  esp_lcd_panel_io_handle_t io = panel->io;
  static const uint8_t gamma_pos[] = {
    0xF0, 0x09, 0x0B, 0x06, 0x04, 0x15, 0x2F, 0x54, 0x42, 0x3C, 0x17, 0x14, 0x18, 0x1B,
  };
  static const uint8_t gamma_neg[] = {
    0xF0, 0x09, 0x0B, 0x06, 0x04, 0x03, 0x2D, 0x43, 0x42, 0x3B, 0x16, 0x14, 0x17, 0x1B,
  };
  static const uint8_t power_seq[] = {0x40, 0x8A, 0x00, 0x00, 0x29, 0x19, 0xA5, 0x33};

  ESP_RETURN_ON_ERROR(st7796_tx_param(io, ST7796_CMD_SLPOUT, NULL, 0), TAG, "sleep out");
  vTaskDelay(pdMS_TO_TICKS(120));

  ESP_RETURN_ON_ERROR(st7796_tx_param(io, 0xF0, (uint8_t[]) {0xC3}, 1), TAG, "unlock cmdset");
  ESP_RETURN_ON_ERROR(st7796_tx_param(io, 0xF0, (uint8_t[]) {0x96}, 1), TAG, "unlock cmdset");

  ESP_RETURN_ON_ERROR(st7796_tx_param(io, ST7796_CMD_MADCTL, &panel->madctl_val, 1), TAG, "madctl");
  ESP_RETURN_ON_ERROR(st7796_tx_param(io, ST7796_CMD_COLMOD, &panel->colmod_val, 1), TAG, "colmod");
  ESP_RETURN_ON_ERROR(st7796_tx_param(io, 0xB4, (uint8_t[]) {0x01}, 1), TAG, "inversion control");
  ESP_RETURN_ON_ERROR(st7796_tx_param(io, 0xB7, (uint8_t[]) {0xC6}, 1), TAG, "entry mode");
  ESP_RETURN_ON_ERROR(st7796_tx_param(io, 0xE8, power_seq, sizeof(power_seq)), TAG, "display timing");
  ESP_RETURN_ON_ERROR(st7796_tx_param(io, 0xC1, (uint8_t[]) {0x06}, 1), TAG, "power control 2");
  ESP_RETURN_ON_ERROR(st7796_tx_param(io, 0xC2, (uint8_t[]) {0xA7}, 1), TAG, "power control 3");
  ESP_RETURN_ON_ERROR(st7796_tx_param(io, 0xC5, (uint8_t[]) {0x18}, 1), TAG, "vcom control");
  ESP_RETURN_ON_ERROR(st7796_tx_param(io, 0xE0, gamma_pos, sizeof(gamma_pos)), TAG, "gamma positive");
  ESP_RETURN_ON_ERROR(st7796_tx_param(io, 0xE1, gamma_neg, sizeof(gamma_neg)), TAG, "gamma negative");
  ESP_RETURN_ON_ERROR(st7796_tx_param(io, 0xF0, (uint8_t[]) {0x3C}, 1), TAG, "lock cmdset");
  ESP_RETURN_ON_ERROR(st7796_tx_param(io, 0xF0, (uint8_t[]) {0x69}, 1), TAG, "lock cmdset");

  vTaskDelay(pdMS_TO_TICKS(120));
  ESP_RETURN_ON_ERROR(st7796_tx_param(io, ST7796_CMD_DISPON, NULL, 0), TAG, "display on");
  return ESP_OK;
}

static esp_err_t panel_st7796_reset(esp_lcd_panel_t *panel) {
  st7796_panel_t *st7796 = __containerof(panel, st7796_panel_t, base);
  esp_lcd_panel_io_handle_t io = st7796->io;
  if (st7796->reset_gpio_num >= 0) {
    gpio_config_t io_conf = {
      .mode = GPIO_MODE_OUTPUT,
      .pin_bit_mask = 1ULL << st7796->reset_gpio_num,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io_conf), TAG, "configure reset gpio");
    gpio_set_level(st7796->reset_gpio_num, st7796->reset_level);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(st7796->reset_gpio_num, !st7796->reset_level);
    vTaskDelay(pdMS_TO_TICKS(120));
  } else {
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, ST7796_CMD_SLPOUT, NULL, 0), TAG, "software reset");
    vTaskDelay(pdMS_TO_TICKS(120));
  }
  return ESP_OK;
}

static esp_err_t panel_st7796_init(esp_lcd_panel_t *panel) {
  st7796_panel_t *st7796 = __containerof(panel, st7796_panel_t, base);
  ESP_RETURN_ON_ERROR(st7796_run_init_sequence(st7796), TAG, "init sequence");
  return ESP_OK;
}

static esp_err_t panel_st7796_del(esp_lcd_panel_t *panel) {
  st7796_panel_t *st7796 = __containerof(panel, st7796_panel_t, base);
  if (st7796->reset_gpio_num >= 0) {
    gpio_reset_pin(st7796->reset_gpio_num);
  }
  free(st7796);
  return ESP_OK;
}

static esp_err_t panel_st7796_draw_bitmap(
  esp_lcd_panel_t *panel,
  int x_start,
  int y_start,
  int x_end,
  int y_end,
  const void *color_data
) {
  st7796_panel_t *st7796 = __containerof(panel, st7796_panel_t, base);
  esp_lcd_panel_io_handle_t io = st7796->io;

  x_start += st7796->x_gap;
  x_end += st7796->x_gap;
  y_start += st7796->y_gap;
  y_end += st7796->y_gap;

  ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, ST7796_CMD_CASET, (uint8_t[]) {
    (uint8_t) ((x_start >> 8) & 0xFF),
    (uint8_t) (x_start & 0xFF),
    (uint8_t) (((x_end - 1) >> 8) & 0xFF),
    (uint8_t) ((x_end - 1) & 0xFF),
  }, 4), TAG, "set column address");

  ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, ST7796_CMD_RASET, (uint8_t[]) {
    (uint8_t) ((y_start >> 8) & 0xFF),
    (uint8_t) (y_start & 0xFF),
    (uint8_t) (((y_end - 1) >> 8) & 0xFF),
    (uint8_t) ((y_end - 1) & 0xFF),
  }, 4), TAG, "set row address");

  size_t len = (size_t) (x_end - x_start) * (size_t) (y_end - y_start)
    * st7796->fb_bits_per_pixel / 8U;
  ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_color(io, ST7796_CMD_RAMWR, color_data, len), TAG, "write gram");
  return ESP_OK;
}

static esp_err_t panel_st7796_invert_color(esp_lcd_panel_t *panel, bool invert_color_data) {
  st7796_panel_t *st7796 = __containerof(panel, st7796_panel_t, base);
  int command = invert_color_data ? ST7796_CMD_INVON : ST7796_CMD_INVOFF;
  ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(st7796->io, command, NULL, 0), TAG, "invert color");
  return ESP_OK;
}

static esp_err_t panel_st7796_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y) {
  st7796_panel_t *st7796 = __containerof(panel, st7796_panel_t, base);
  if (mirror_x) st7796->madctl_val |= ST7796_MADCTL_MX;
  else st7796->madctl_val &= (uint8_t) ~ST7796_MADCTL_MX;
  if (mirror_y) st7796->madctl_val |= ST7796_MADCTL_MY;
  else st7796->madctl_val &= (uint8_t) ~ST7796_MADCTL_MY;
  ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(st7796->io, ST7796_CMD_MADCTL, &st7796->madctl_val, 1), TAG, "madctl");
  return ESP_OK;
}

static esp_err_t panel_st7796_swap_xy(esp_lcd_panel_t *panel, bool swap_axes) {
  st7796_panel_t *st7796 = __containerof(panel, st7796_panel_t, base);
  if (swap_axes) st7796->madctl_val |= ST7796_MADCTL_MV;
  else st7796->madctl_val &= (uint8_t) ~ST7796_MADCTL_MV;
  ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(st7796->io, ST7796_CMD_MADCTL, &st7796->madctl_val, 1), TAG, "madctl");
  return ESP_OK;
}

static esp_err_t panel_st7796_set_gap(esp_lcd_panel_t *panel, int x_gap, int y_gap) {
  st7796_panel_t *st7796 = __containerof(panel, st7796_panel_t, base);
  st7796->x_gap = x_gap;
  st7796->y_gap = y_gap;
  return ESP_OK;
}

static esp_err_t panel_st7796_disp_on_off(esp_lcd_panel_t *panel, bool on_off) {
  st7796_panel_t *st7796 = __containerof(panel, st7796_panel_t, base);
  int command = on_off ? ST7796_CMD_DISPON : ST7796_CMD_DISPOFF;
  ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(st7796->io, command, NULL, 0), TAG, "display on/off");
  return ESP_OK;
}

static esp_err_t panel_st7796_disp_sleep(esp_lcd_panel_t *panel, bool sleep) {
  st7796_panel_t *st7796 = __containerof(panel, st7796_panel_t, base);
  int command = sleep ? ST7796_CMD_SLPIN : ST7796_CMD_SLPOUT;
  ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(st7796->io, command, NULL, 0), TAG, "display sleep");
  vTaskDelay(pdMS_TO_TICKS(120));
  return ESP_OK;
}

esp_err_t esp_lcd_new_panel_st7796(
  const esp_lcd_panel_io_handle_t io,
  const esp_lcd_panel_dev_config_t *panel_dev_config,
  esp_lcd_panel_handle_t *ret_panel
) {
  ESP_RETURN_ON_FALSE(io && panel_dev_config && ret_panel, ESP_ERR_INVALID_ARG, TAG, "invalid argument");

  st7796_panel_t *st7796 = calloc(1, sizeof(st7796_panel_t));
  ESP_RETURN_ON_FALSE(st7796, ESP_ERR_NO_MEM, TAG, "no memory for panel");
  st7796->io = io;
  st7796->reset_gpio_num = panel_dev_config->reset_gpio_num;
  st7796->reset_level = panel_dev_config->flags.reset_active_high;
  st7796->fb_bits_per_pixel = (uint8_t) panel_dev_config->bits_per_pixel;
  st7796->colmod_val = ST7796_COLMOD_RGB565;
  /* Landscape: MV swaps the address axes so GRAM windows are 480x320. The
   * module's panel glass is BGR ordered, matching the vendor init sequence. */
  st7796->madctl_val = ST7796_MADCTL_MV;
  if (panel_dev_config->rgb_ele_order == LCD_RGB_ELEMENT_ORDER_BGR) {
    st7796->madctl_val |= ST7796_MADCTL_BGR;
  }

  st7796->base.reset = panel_st7796_reset;
  st7796->base.init = panel_st7796_init;
  st7796->base.del = panel_st7796_del;
  st7796->base.draw_bitmap = panel_st7796_draw_bitmap;
  st7796->base.invert_color = panel_st7796_invert_color;
  st7796->base.mirror = panel_st7796_mirror;
  st7796->base.swap_xy = panel_st7796_swap_xy;
  st7796->base.set_gap = panel_st7796_set_gap;
  st7796->base.disp_on_off = panel_st7796_disp_on_off;
  st7796->base.disp_sleep = panel_st7796_disp_sleep;
  *ret_panel = &st7796->base;
  return ESP_OK;
}
