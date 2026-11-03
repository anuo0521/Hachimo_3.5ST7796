# [Input] ST7796 local panel driver sources.
# [Output] esp_lcd_st7796 component.
# [Pos] Firmware component for the CL35BC1017-40A 3.5" SPI panel.
# [Sync] If this file changes, update `firmware/.folder.md`.

idf_component_register(
  SRCS "esp_lcd_st7796.c"
  INCLUDE_DIRS "include"
  REQUIRES esp_lcd
  PRIV_REQUIRES esp_driver_gpio
)
