/*
 * [Input] The PET_P4_PANEL_* choice from sdkconfig.
 * [Output] One boolean that answers "is the replacement ST7796 fitted", which is
 *          what the panel geometry, touch mapper and renderer actually care
 *          about; the transport underneath (SPI or 8080 parallel) is decided
 *          inside pet_p4_lcd.c.
 * [Pos] Shared panel identity, included everywhere the ST7796 needs special
 *       casing.
 * [Sync] If a third ST7796 transport appears, extend it here rather than
 *        teaching every call site about the new symbol.
 */

#pragma once

#if defined(__has_include)
#if __has_include("sdkconfig.h")
#include "sdkconfig.h"
#endif
#endif

/* Both transports drive the same glass, so everything above the bus sees one
 * panel. Checking !WAVESHARE would be shorter but would silently start treating
 * an unconfigured tree as ST7796 hardware, which is how footprints get wrong. */
#if CONFIG_PET_P4_PANEL_ST7796_SPI || CONFIG_PET_P4_PANEL_ST7796_I80
#define PET_P4_PANEL_ST7796 1
#else
#define PET_P4_PANEL_ST7796 0
#endif
