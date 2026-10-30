/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include "system_theme.h"

#include <pbl/util/math.h>
#include <pbl/util/size.h>

#include <applib/graphics/gdraw_command.h>
#include <shell/prefs.h>

bool system_theme_is_dark(void) {
#if PBL_COLOR && defined(CONFIG_THEMING) && !defined(CONFIG_RECOVERY_FW)
  return shell_prefs_get_theme_dark_mode();
#else
  return false;
#endif
}

GColor system_theme_color(GColor light, GColor dark) {
  return system_theme_is_dark() ? dark : light;
}

GColor system_theme_get_bg_color(void) {
  return system_theme_color(GColorWhite, GColorBlack);
}

GColor system_theme_get_fg_color(void) {
  return system_theme_color(GColorBlack, GColorWhite);
}

GColor system_theme_get_secondary_fg_color(void) {
  return system_theme_color(PBL_IF_COLOR_ELSE(GColorDarkGray, GColorBlack), GColorLightGray);
}

GColor system_theme_invert_neutral(GColor color) {
  if (!system_theme_is_dark()) {
    return color;
  }
  switch (color.argb) {
    case GColorBlackARGB8:
      return GColorWhite;
    case GColorWhiteARGB8:
      return GColorBlack;
    case GColorDarkGrayARGB8:
      return GColorLightGray;
    case GColorLightGrayARGB8:
      return GColorDarkGray;
    default:
      return color;
  }
}

static void prv_invert_neutral_command(GDrawCommandProcessor *processor,
                                       GDrawCommand *processed_command,
                                       size_t processed_command_max_size,
                                       const GDrawCommandList *list, const GDrawCommand *command) {
  GDrawCommand *const source = (GDrawCommand *)command;
  gdraw_command_set_stroke_color(
      processed_command, system_theme_invert_neutral(gdraw_command_get_stroke_color(source)));
  gdraw_command_set_fill_color(processed_command,
                               system_theme_invert_neutral(gdraw_command_get_fill_color(source)));
}

static GDrawCommandProcessor s_invert_neutral_processor = {
  .command = prv_invert_neutral_command,
};

GDrawCommandProcessor *system_theme_get_icon_processor(void) {
  return system_theme_is_dark() ? &s_invert_neutral_processor : nullptr;
}

// Maps a 6-bit RGB color to the one with the inverted HSL lightness and the same hue and
// saturation: adding (3 - max - min) to each 2-bit channel mirrors max + min around 3.
static uint8_t prv_invert_lightness_rgb(uint8_t rgb) {
  const uint8_t r = (rgb >> 4) & 0x3;
  const uint8_t g = (rgb >> 2) & 0x3;
  const uint8_t b = rgb & 0x3;
  const int shift = 3 - MAX(r, MAX(g, b)) - MIN(r, MIN(g, b));
  return (uint8_t)(((r + shift) << 4) | ((g + shift) << 2) | (b + shift));
}

void system_theme_dark_filter_bitmap(GBitmap *bitmap, const GRect *rect) {
  if (!system_theme_is_dark() || !bitmap ||
      (gbitmap_get_format(bitmap) != GBitmapFormat8Bit &&
       gbitmap_get_format(bitmap) != GBitmapFormat8BitCircular)) {
    return;
  }
  static uint8_t s_lut[64];
  static bool s_lut_ready;
  if (!s_lut_ready) {
    for (unsigned int i = 0; i < ARRAY_LENGTH(s_lut); i++) {
      s_lut[i] = prv_invert_lightness_rgb(i);
    }
    s_lut_ready = true;
  }

  GRect area = *rect;
  grect_clip(&area, &bitmap->bounds);
  for (int16_t y = area.origin.y; y < grect_get_max_y(&area); y++) {
    const GBitmapDataRowInfo row = gbitmap_get_data_row_info(bitmap, y);
    const int16_t min_x = MAX(row.min_x, area.origin.x);
    const int16_t max_x = MIN(row.max_x, grect_get_max_x(&area) - 1);
    for (int16_t x = min_x; x <= max_x; x++) {
      const uint8_t pixel = row.data[x];
      row.data[x] = (pixel & 0xC0) | s_lut[pixel & 0x3F];
    }
  }
}
