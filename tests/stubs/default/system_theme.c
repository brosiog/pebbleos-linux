/* SPDX-FileCopyrightText: 2024 Google LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include <stdlib.h>

#include <pbl/kernel/compiler.h>

#include <shell/system_theme.h>

const char *PBL_WEAK system_theme_get_font_key(TextStyleFont font) {
  return nullptr;
}

const char *PBL_WEAK system_theme_get_font_key_for_size(PreferredContentSize size,
                                                        TextStyleFont font) {
  return nullptr;
}

GFont PBL_WEAK system_theme_get_font(TextStyleFont font) {
  return nullptr;
}

GFont PBL_WEAK system_theme_get_font_for_size(PreferredContentSize size, TextStyleFont font) {
  return nullptr;
}

GFont PBL_WEAK system_theme_get_font_for_default_size(TextStyleFont font) {
  return nullptr;
}

PreferredContentSize PBL_WEAK system_theme_get_default_content_size_for_runtime_platform(void) {
  return PreferredContentSizeDefault;
}

PreferredContentSize PBL_WEAK
system_theme_convert_host_content_size_to_runtime_platform(PreferredContentSize size) {
  return size;
}

bool PBL_WEAK system_theme_is_dark(void) {
  return false;
}

GColor PBL_WEAK system_theme_color(GColor light, GColor dark) {
  return light;
}

GColor PBL_WEAK system_theme_get_bg_color(void) {
  return GColorWhite;
}

GColor PBL_WEAK system_theme_get_fg_color(void) {
  return GColorBlack;
}

GColor PBL_WEAK system_theme_get_secondary_fg_color(void) {
  return PBL_IF_COLOR_ELSE(GColorDarkGray, GColorBlack);
}

GDrawCommandProcessor *PBL_WEAK system_theme_get_icon_processor(void) {
  return nullptr;
}

GColor PBL_WEAK system_theme_invert_neutral(GColor color) {
  return color;
}

void PBL_WEAK system_theme_dark_filter_bitmap(GBitmap *bitmap, const GRect *rect) {
}
