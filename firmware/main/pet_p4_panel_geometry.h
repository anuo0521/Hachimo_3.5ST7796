/*
 * [Input] PET_P4_PANEL_ST7796 from pet_p4_panel.h (host tests build these units
 *         without sdkconfig, where it resolves to 0).
 * [Output] The single definition of the UI-to-panel letterbox fit.
 * [Pos] Shared geometry between pet_p4_renderer.c and pet_p4_touch_core.c.
 * [Sync] If the fit changes, both the drawing and the touch mapping follow;
 *        never re-declare these numbers at a call site.
 */

#pragma once

#include "pet_p4_panel.h"

#if PET_P4_PANEL_ST7796
/* The 640x480 UI is uniformly scaled into 424x320 and centred on the 480x320
 * landscape panel, leaving a 28 px black pillar on either side. 424 is a
 * multiple of 8 so PPA and the band writer keep 4-pixel alignment. */
#define PET_P4_PANEL_FIT_WIDTH 424
#define PET_P4_PANEL_FIT_OFFSET_X 28
#endif
