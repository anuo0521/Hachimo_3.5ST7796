/*
 * [Input] Panel driver operations (ST7796 over 4-wire SPI or the Waveshare DSI
 *         BSP), boot lifecycle, and RGB565 pixels.
 * [Output] Backlight-gated LCD initialization, first-frame reveal, and draws.
 * [Pos] ESP32-P4 LCD hardware boundary.
 * [Sync] If this file changes, update `firmware/.folder.md`.
 */

#include "pet_p4_lcd.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

#include "esp_check.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "pet_p4_touch.h"
#include "bsp/display.h"

#if CONFIG_PET_P4_PANEL_ST7796_SPI
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_st7796.h"
#include "driver/spi_master.h"
#else
#include "esp_lcd_mipi_dsi.h"
#include "driver/i2c_master.h"
#include "bsp/esp32_p4_wifi6_touch_lcd_4_3.h"
#include "bsp/esp-bsp.h"
#endif

static const char *TAG = "pet-p4-lcd";
static const int LCD_BACKLIGHT_FORCE_LEVEL = 1;
static const TickType_t LCD_REFRESH_WAIT_TICKS = pdMS_TO_TICKS(250);

#if CONFIG_PET_P4_PANEL_ST7796_SPI
#define PET_P4_LCD_BACKLIGHT_GPIO (CONFIG_PET_P4_LCD_PIN_BACKLIGHT)
/* Rows per SPI transaction when staging byte-swapped pixels. 16 rows keep the
 * staging buffer inside internal DMA-capable RAM (15 KiB) while still moving a
 * whole frame in 20 transactions. */
#define PET_P4_LCD_BAND_ROWS 16
#else
#define PET_P4_LCD_BACKLIGHT_GPIO (BSP_LCD_BACKLIGHT)
#endif

static esp_lcd_panel_handle_t g_panel_handle;
static esp_lcd_panel_io_handle_t g_lcd_io;
static SemaphoreHandle_t g_refresh_done_sem;
static esp_err_t g_backlight_status = ESP_ERR_INVALID_STATE;
static bool g_backlight_revealed;
static bool g_forced_backlight_logged;
#if !CONFIG_PET_P4_PANEL_ST7796_SPI
static bool g_refresh_callback_logged;
#endif
static uint16_t *g_test_row_buffer;
static size_t g_test_row_pixels;
#if CONFIG_PET_P4_PANEL_ST7796_SPI
static uint16_t *g_swap_buffer;
static size_t g_swap_pixels;
#endif

static uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t) (((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

static esp_err_t force_lcd_backlight_gpio(int level) {
  if (PET_P4_LCD_BACKLIGHT_GPIO < 0) return ESP_OK;
  gpio_config_t io_conf = {
    .pin_bit_mask = 1ULL << PET_P4_LCD_BACKLIGHT_GPIO,
    .mode = GPIO_MODE_OUTPUT,
    .pull_up_en = GPIO_PULLUP_DISABLE,
    .pull_down_en = GPIO_PULLDOWN_DISABLE,
    .intr_type = GPIO_INTR_DISABLE,
  };
  ESP_RETURN_ON_ERROR(gpio_config(&io_conf), TAG, "configure LCD backlight GPIO");
  ESP_RETURN_ON_ERROR(gpio_set_level(PET_P4_LCD_BACKLIGHT_GPIO, level), TAG, "force LCD backlight GPIO");
  if (level == LCD_BACKLIGHT_FORCE_LEVEL && !g_forced_backlight_logged) {
    ESP_LOGI(TAG, "LCD backlight revealed gpio=%d", LCD_BACKLIGHT_FORCE_LEVEL);
    g_forced_backlight_logged = true;
  }
  return ESP_OK;
}

esp_err_t pet_p4_lcd_prepare_boot(void) {
  g_backlight_revealed = false;
  g_backlight_status = force_lcd_backlight_gpio(1 - LCD_BACKLIGHT_FORCE_LEVEL);
  return g_backlight_status;
}

#if CONFIG_PET_P4_PANEL_ST7796_SPI

static bool IRAM_ATTR lcd_color_trans_done(
  esp_lcd_panel_io_handle_t panel_io,
  esp_lcd_panel_io_event_data_t *edata,
  void *user_ctx
) {
  (void) panel_io;
  (void) edata;
  BaseType_t task_awake = pdFALSE;
  if (user_ctx) xSemaphoreGiveFromISR((SemaphoreHandle_t) user_ctx, &task_awake);
  return task_awake == pdTRUE;
}

/* The ST7796 expects the high byte of every RGB565 sample first on the SPI data
 * phase, while every pixel producer in this firmware stores native little-endian
 * halfwords. Staging in bands keeps the swap off the render path's heap. */
static esp_err_t st7796_draw_rgb565(int x, int y, int width, int height, const uint16_t *pixels) {
  if (!g_panel_handle) return ESP_ERR_INVALID_STATE;
  if (!pixels || width <= 0 || height <= 0) return ESP_ERR_INVALID_ARG;

  const size_t required = (size_t) width * PET_P4_LCD_BAND_ROWS;
  if (required > g_swap_pixels) {
    free(g_swap_buffer);
    g_swap_buffer = (uint16_t *) heap_caps_malloc(required * sizeof(uint16_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!g_swap_buffer) {
      g_swap_buffer = (uint16_t *) heap_caps_malloc(required * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (!g_swap_buffer) {
      g_swap_pixels = 0;
      return ESP_ERR_NO_MEM;
    }
    g_swap_pixels = required;
  }

  for (int row = 0; row < height; row += PET_P4_LCD_BAND_ROWS) {
    int rows = (row + PET_P4_LCD_BAND_ROWS > height) ? (height - row) : PET_P4_LCD_BAND_ROWS;
    const uint16_t *src = pixels + (size_t) row * (size_t) width;
    size_t count = (size_t) rows * (size_t) width;
    for (size_t i = 0; i < count; i += 1) {
      uint16_t value = src[i];
      g_swap_buffer[i] = (uint16_t) ((value >> 8) | (value << 8));
    }
    if (g_refresh_done_sem) {
      while (xSemaphoreTake(g_refresh_done_sem, 0) == pdTRUE) {
      }
    }
    esp_err_t err = esp_lcd_panel_draw_bitmap(
      g_panel_handle,
      x,
      y + row,
      x + width,
      y + row + rows,
      g_swap_buffer
    );
    if (err != ESP_OK) return err;
    if (g_refresh_done_sem && xSemaphoreTake(g_refresh_done_sem, LCD_REFRESH_WAIT_TICKS) != pdTRUE) {
      ESP_LOGW(TAG, "LCD refresh wait timed out");
      return ESP_ERR_TIMEOUT;
    }
  }
  return ESP_OK;
}

static esp_err_t st7796_panel_bring_up(void) {
  spi_bus_config_t bus_config = {
    .sclk_io_num = CONFIG_PET_P4_LCD_PIN_SCLK,
    .mosi_io_num = CONFIG_PET_P4_LCD_PIN_MOSI,
    .miso_io_num = -1,
    .quadwp_io_num = -1,
    .quadhd_io_num = -1,
    .max_transfer_sz = (int) ((size_t) BSP_LCD_H_RES * PET_P4_LCD_BAND_ROWS * sizeof(uint16_t)) + 64,
  };
  ESP_RETURN_ON_ERROR(
    spi_bus_initialize((spi_host_device_t) CONFIG_PET_P4_LCD_SPI_HOST, &bus_config, SPI_DMA_CH_AUTO),
    TAG,
    "initialize SPI bus"
  );

  esp_lcd_panel_io_spi_config_t io_config = {
    .cs_gpio_num = CONFIG_PET_P4_LCD_PIN_CS,
    .dc_gpio_num = CONFIG_PET_P4_LCD_PIN_DC,
    .spi_mode = 0,
    .pclk_hz = CONFIG_PET_P4_LCD_SPI_CLOCK_HZ,
    .trans_queue_depth = CONFIG_PET_P4_LCD_SPI_QUEUE_DEPTH,
    .on_color_trans_done = lcd_color_trans_done,
    .user_ctx = g_refresh_done_sem,
    .lcd_cmd_bits = 8,
    .lcd_param_bits = 8,
  };
  ESP_RETURN_ON_ERROR(
    esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t) CONFIG_PET_P4_LCD_SPI_HOST, &io_config, &g_lcd_io),
    TAG,
    "create SPI panel IO"
  );

  esp_lcd_panel_dev_config_t panel_config = {
    .reset_gpio_num = CONFIG_PET_P4_LCD_PIN_RST,
    .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
    .bits_per_pixel = 16,
  };
  ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7796(g_lcd_io, &panel_config, &g_panel_handle), TAG, "create ST7796 panel");
  ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(g_panel_handle), TAG, "reset ST7796");
  ESP_RETURN_ON_ERROR(esp_lcd_panel_init(g_panel_handle), TAG, "init ST7796");
#if CONFIG_PET_P4_LCD_MIRROR_X || CONFIG_PET_P4_LCD_MIRROR_Y
  ESP_RETURN_ON_ERROR(
    esp_lcd_panel_mirror(g_panel_handle, CONFIG_PET_P4_LCD_MIRROR_X, CONFIG_PET_P4_LCD_MIRROR_Y),
    TAG,
    "mirror ST7796"
  );
#endif
  ESP_LOGI(
    TAG,
    "ST7796 ready %dx%d SPI%d sclk=%d mosi=%d cs=%d dc=%d rst=%d pclk=%dHz",
    BSP_LCD_H_RES,
    BSP_LCD_V_RES,
    CONFIG_PET_P4_LCD_SPI_HOST,
    CONFIG_PET_P4_LCD_PIN_SCLK,
    CONFIG_PET_P4_LCD_PIN_MOSI,
    CONFIG_PET_P4_LCD_PIN_CS,
    CONFIG_PET_P4_LCD_PIN_DC,
    CONFIG_PET_P4_LCD_PIN_RST,
    CONFIG_PET_P4_LCD_SPI_CLOCK_HZ
  );
  return ESP_OK;
}

esp_err_t pet_p4_lcd_init(void) {
  if (g_panel_handle) return ESP_OK;
  if (!g_refresh_done_sem) {
    g_refresh_done_sem = xSemaphoreCreateBinary();
    if (!g_refresh_done_sem) return ESP_ERR_NO_MEM;
  }
  ESP_LOGI(TAG, "Initialize CL35BC1017-40A ST7796 SPI panel");
  ESP_RETURN_ON_ERROR(st7796_panel_bring_up(), TAG, "bring up ST7796");

  /* Clear GRAM so the controller's power-on pattern is never shown. The
   * backlight stays off until the renderer commits its first full frame. */
  const int band = 32;
  const size_t pixels = (size_t) BSP_LCD_H_RES * (size_t) band;
  uint16_t *zeros = (uint16_t *) heap_caps_calloc(pixels, sizeof(uint16_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  if (zeros) {
    for (int y = 0; y < BSP_LCD_V_RES; y += band) {
      int h = (y + band > BSP_LCD_V_RES) ? (BSP_LCD_V_RES - y) : band;
      if (st7796_draw_rgb565(0, y, BSP_LCD_H_RES, h, zeros) != ESP_OK) break;
    }
    free(zeros);
  }

  g_backlight_status = force_lcd_backlight_gpio(1 - LCD_BACKLIGHT_FORCE_LEVEL);
  if (g_backlight_status != ESP_OK) {
    ESP_LOGW(TAG, "display backlight hide failed: %s", esp_err_to_name(g_backlight_status));
  }
  return ESP_OK;
}

#else  // Waveshare ST7701 over MIPI DSI

static bool IRAM_ATTR lcd_refresh_done_callback(
  esp_lcd_panel_handle_t panel,
  esp_lcd_dpi_panel_event_data_t *edata,
  void *user_ctx
) {
  (void) panel;
  (void) edata;
  SemaphoreHandle_t sem = (SemaphoreHandle_t) user_ctx;
  BaseType_t task_awake = pdFALSE;
  if (sem) {
    xSemaphoreGiveFromISR(sem, &task_awake);
  }
  return task_awake == pdTRUE;
}

esp_err_t pet_p4_lcd_init(void) {
  if (g_panel_handle) return ESP_OK;

  ESP_LOGI(TAG, "Initialize Waveshare ESP32-P4 LCD BSP");
  ESP_RETURN_ON_ERROR(bsp_display_new(NULL, &g_panel_handle, &g_lcd_io), TAG, "initialize display");
  if (!g_refresh_done_sem) {
    g_refresh_done_sem = xSemaphoreCreateBinary();
    if (!g_refresh_done_sem) return ESP_ERR_NO_MEM;
  }
  esp_lcd_dpi_panel_event_callbacks_t callbacks = {
    .on_refresh_done = lcd_refresh_done_callback,
  };
  ESP_RETURN_ON_ERROR(
    esp_lcd_dpi_panel_register_event_callbacks(g_panel_handle, &callbacks, g_refresh_done_sem),
    TAG,
    "register LCD refresh callback"
  );
  if (!g_refresh_callback_logged) {
    ESP_LOGI(TAG, "LCD refresh_done callback registered");
    g_refresh_callback_logged = true;
  }
  ESP_LOGI(
    TAG,
    "LCD pixel format=%s bits=%d",
    BSP_LCD_COLOR_FORMAT == ESP_LCD_COLOR_FORMAT_RGB565 ? "RGB565" : "RGB888",
    BSP_LCD_BITS_PER_PIXEL
  );
  // Component/storage migration may run for several seconds after an OTA.
  // Keep the panel hidden until the renderer has committed the first complete
  // frame so the controller's blue reset surface is never shown to the user.
  g_backlight_status = bsp_display_backlight_off();
  esp_err_t force_err = force_lcd_backlight_gpio(1 - LCD_BACKLIGHT_FORCE_LEVEL);
  if (g_backlight_status != ESP_OK || force_err != ESP_OK) {
    ESP_LOGW(TAG, "display backlight hide failed: %s", esp_err_to_name(g_backlight_status));
  }
  if (g_backlight_status == ESP_OK && force_err != ESP_OK) {
    g_backlight_status = force_err;
  }
  return ESP_OK;
}

#endif  // CONFIG_PET_P4_PANEL_ST7796_SPI

esp_err_t pet_p4_lcd_reveal(void) {
  if (!g_panel_handle) return ESP_ERR_INVALID_STATE;
  if (g_backlight_revealed) return g_backlight_status;
#if CONFIG_PET_P4_PANEL_ST7796_SPI
  g_backlight_status = ESP_OK;
#else
  g_backlight_status = bsp_display_backlight_on();
#endif
  esp_err_t force_err = force_lcd_backlight_gpio(LCD_BACKLIGHT_FORCE_LEVEL);
  if (g_backlight_status == ESP_OK && force_err != ESP_OK) {
    g_backlight_status = force_err;
  }
  if (g_backlight_status == ESP_OK) {
    g_backlight_revealed = true;
  } else {
    ESP_LOGW(TAG, "display first-frame reveal failed: %s", esp_err_to_name(g_backlight_status));
  }
  return g_backlight_status;
}

esp_err_t pet_p4_lcd_backlight_status(void) {
  return g_backlight_status;
}

esp_err_t pet_p4_lcd_keep_awake(void) {
  if (!g_panel_handle) return ESP_ERR_INVALID_STATE;
  if (!g_backlight_revealed) return ESP_ERR_INVALID_STATE;
  return g_backlight_status;
}

esp_err_t pet_p4_lcd_show_color_bar(void) {
  if (!g_panel_handle) return ESP_ERR_INVALID_STATE;
#if CONFIG_PET_P4_PANEL_ST7796_SPI
  /* The ST7796 has no DPI test-pattern generator; the software pattern below
   * is the equivalent diagnostic. */
  ESP_LOGW(TAG, "ST7796 has no hardware color bar; use the software pattern");
  return ESP_ERR_NOT_SUPPORTED;
#else
  ESP_LOGI(TAG, "Show LCD hardware color bar");
  esp_err_t err = esp_lcd_dpi_panel_set_pattern(g_panel_handle, MIPI_DSI_PATTERN_BAR_VERTICAL);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "failed to show color bar: %s", esp_err_to_name(err));
  }
  return err;
#endif
}

esp_err_t pet_p4_lcd_show_software_test_pattern(uint32_t phase) {
  if (!g_panel_handle) return ESP_ERR_INVALID_STATE;

  const int row_height = 32;
  const size_t required_pixels = (size_t) BSP_LCD_H_RES * (size_t) row_height;
  if (required_pixels > g_test_row_pixels) {
    free(g_test_row_buffer);
    g_test_row_buffer = (uint16_t *) heap_caps_malloc(required_pixels * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!g_test_row_buffer) {
      g_test_row_buffer = (uint16_t *) heap_caps_malloc(required_pixels * sizeof(uint16_t), MALLOC_CAP_8BIT);
    }
    if (!g_test_row_buffer) {
      g_test_row_pixels = 0;
      return ESP_ERR_NO_MEM;
    }
    g_test_row_pixels = required_pixels;
  }

  const uint16_t colors[] = {
    rgb565(255, 0, 0),
    rgb565(0, 255, 0),
    rgb565(0, 0, 255),
    rgb565(255, 255, 255),
    rgb565(0, 0, 0),
    rgb565(255, 255, 0),
    rgb565(0, 255, 255),
    rgb565(255, 0, 255),
  };
  const int color_count = (int) (sizeof(colors) / sizeof(colors[0]));
  const int moving_x = (int) ((phase * 19U) % (uint32_t) BSP_LCD_H_RES);

  for (int y = 0; y < BSP_LCD_V_RES; y += row_height) {
    int h = (y + row_height > BSP_LCD_V_RES) ? (BSP_LCD_V_RES - y) : row_height;
    for (int yy = 0; yy < h; yy += 1) {
      int screen_y = y + yy;
      for (int x = 0; x < BSP_LCD_H_RES; x += 1) {
        int bar = (x * color_count) / BSP_LCD_H_RES;
        uint16_t color = colors[bar < color_count ? bar : color_count - 1];
        if (x < 8 || x >= BSP_LCD_H_RES - 8 || screen_y < 8 || screen_y >= BSP_LCD_V_RES - 8) {
          color = rgb565(255, 255, 255);
        } else if (x >= moving_x && x < moving_x + 16) {
          color = rgb565(255, 120, 0);
        } else if (((x / 40) + (screen_y / 40) + (int) phase) % 9 == 0) {
          color = rgb565(16, 16, 16);
        }
        g_test_row_buffer[yy * BSP_LCD_H_RES + x] = color;
      }
    }
    esp_err_t err = pet_p4_lcd_draw_rgb565(0, y, BSP_LCD_H_RES, h, g_test_row_buffer);
    if (err != ESP_OK) return err;
  }
  return ESP_OK;
}

esp_err_t pet_p4_lcd_draw_rgb565(int x, int y, int width, int height, const uint16_t *pixels) {
#if CONFIG_PET_P4_PANEL_ST7796_SPI
  return st7796_draw_rgb565(x, y, width, height, pixels);
#else
  if (!g_panel_handle) return ESP_ERR_INVALID_STATE;
  if (!pixels || width <= 0 || height <= 0) return ESP_ERR_INVALID_ARG;

  if (g_refresh_done_sem) {
    while (xSemaphoreTake(g_refresh_done_sem, 0) == pdTRUE) {
    }
  }
  esp_err_t draw_err = esp_lcd_panel_draw_bitmap(g_panel_handle, x, y, x + width, y + height, pixels);
  if (draw_err != ESP_OK) return draw_err;
  if (g_refresh_done_sem && xSemaphoreTake(g_refresh_done_sem, LCD_REFRESH_WAIT_TICKS) != pdTRUE) {
    ESP_LOGW(TAG, "LCD refresh wait timed out");
    return ESP_ERR_TIMEOUT;
  }
  return ESP_OK;
#endif
}

int pet_p4_lcd_scan_i2c(char *buffer, size_t buffer_size) {
#if CONFIG_PET_P4_PANEL_ST7796_SPI
  return pet_p4_touch_i2c_scan(buffer, buffer_size);
#else
  if (!buffer || buffer_size == 0) return 0;
  buffer[0] = '\0';

  esp_err_t init_err = bsp_i2c_init();
  if (init_err != ESP_OK) {
    snprintf(buffer, buffer_size, "init:%s", esp_err_to_name(init_err));
    return 0;
  }

  i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
  if (!bus) {
    snprintf(buffer, buffer_size, "no-bus");
    return 0;
  }

  int found = 0;
  size_t used = 0;
  for (uint8_t addr = 0x08; addr <= 0x77; addr += 1) {
    if (i2c_master_probe(bus, addr, 50) != ESP_OK) continue;
    int written = snprintf(
      buffer + used,
      buffer_size - used,
      "%s0x%02X",
      found == 0 ? "" : ",",
      addr
    );
    if (written < 0 || (size_t) written >= buffer_size - used) {
      buffer[buffer_size - 1] = '\0';
      break;
    }
    used += (size_t) written;
    found += 1;
  }

  ESP_LOGI(TAG, "I2C scan found %d device(s): %s", found, found > 0 ? buffer : "none");
  return found;
#endif
}
