#pragma once

/* Monospace clock digits from JetBrains Mono (OFL). Regenerate:
 * npx lv_font_conv --font JetBrainsMono-Regular.ttf --size 58 --bpp 4 \
 *   --format lvgl --no-compress -o lv_font_clock_mono_58.c \
 *   --symbols "0123456789:"
 * npx lv_font_conv --font JetBrainsMono-Regular.ttf --size 28 --bpp 4 \
 *   --format lvgl --no-compress -o lv_font_clock_mono_28.c \
 *   --symbols "0123456789: AMP+-" */

#include "lvgl.h"

LV_FONT_DECLARE(lv_font_clock_mono_58);
LV_FONT_DECLARE(lv_font_clock_mono_28);
LV_FONT_DECLARE(lv_font_clock_mono_20);
