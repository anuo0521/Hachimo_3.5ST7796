/*
 * [Input] CONFIG_PET_P4_PANEL_ST7796_SPI from sdkconfig (when one is on the
 *         include path; host tests build these units without it).
 * [Output] The single definition of the UI-to-panel letterbox fit.
 * [Pos] Shared geometry between pet_p4_renderer.c and pet_p4_touch_core.c.
 * [Sync] If the fit changes, both the drawing and the touch mapping follow;
 *        never re-declare these numbers at a call site.
 */

#pragma once

#if defined(__has_include)
#if __has_include("sdkconfig.h")
#include "sdkconfig.h"
#endif
#endif

#if CONFIG_PET_P4_PANEL_ST7796_SPI
/* The 640x480 UI is uniformly scaled into 424x320 and centred on the 480x320
 * landscape panel, leaving a 28 px black pillar on either side. 424 is a
 * multiple of 8 so PPA and the band writer keep 4-pixel alignment. */
#define PET_P4_PANEL_FIT_WIDTH 424
#define PET_P4_PANEL_FIT_OFFSET_X 28
#endif
