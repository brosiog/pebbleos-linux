/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include <string.h>

#include <applib/graphics/gdraw_command_private.h>
#include <clar.h>
#include <shell/system_theme.h>

static bool s_dark;

bool shell_prefs_get_theme_dark_mode(void) {
  return s_dark;
}

static bool prv_dark_supported(void) {
#if PBL_COLOR && defined(CONFIG_THEMING) && !defined(CONFIG_RECOVERY_FW)
  return true;
#else
  return false;
#endif
}

void test_system_theme_color__initialize(void) {
  s_dark = false;
}

void test_system_theme_color__palette_and_toggle(void) {
  for (unsigned int i = 0; i < 4; i++) {
    s_dark = i % 2;
    const bool dark = s_dark && prv_dark_supported();
    cl_assert_equal_b(dark, system_theme_is_dark());
    cl_assert_equal_i(dark ? GColorBlackARGB8 : GColorWhiteARGB8, system_theme_get_bg_color().argb);
    cl_assert_equal_i(dark ? GColorWhiteARGB8 : GColorBlackARGB8, system_theme_get_fg_color().argb);
    cl_assert_equal_i(
        dark ? GColorLightGrayARGB8 : PBL_IF_COLOR_ELSE(GColorDarkGrayARGB8, GColorBlackARGB8),
        system_theme_get_secondary_fg_color().argb);
    cl_assert_equal_i(dark ? GColorBlueARGB8 : GColorRedARGB8,
                      system_theme_color(GColorRed, GColorBlue).argb);
  }
}

void test_system_theme_color__neutral_icons(void) {
  s_dark = true;
  const bool dark = prv_dark_supported();
  cl_assert_equal_i(dark ? GColorWhiteARGB8 : GColorBlackARGB8,
                    system_theme_invert_neutral(GColorBlack).argb);
  cl_assert_equal_i(dark ? GColorDarkGrayARGB8 : GColorLightGrayARGB8,
                    system_theme_invert_neutral(GColorLightGray).argb);
  cl_assert_equal_i(GColorRedARGB8, system_theme_invert_neutral(GColorRed).argb);
  cl_assert_equal_i(GColorClearARGB8, system_theme_invert_neutral(GColorClear).argb);

  GDrawCommandProcessor *processor = system_theme_get_icon_processor();
  if (!dark) {
    cl_assert_equal_p(nullptr, processor);
    return;
  }
  cl_assert(processor);
  GDrawCommand source = {
    .stroke_color = GColorBlack,
    .fill_color = GColorLightGray,
  };
  GDrawCommand result = source;
  processor->command(processor, &result, sizeof(result), nullptr, &source);
  cl_assert_equal_i(GColorWhiteARGB8, result.stroke_color.argb);
  cl_assert_equal_i(GColorDarkGrayARGB8, result.fill_color.argb);
  cl_assert_equal_i(GColorBlackARGB8, source.stroke_color.argb);
  s_dark = false;
  cl_assert_equal_p(nullptr, system_theme_get_icon_processor());
}

void test_system_theme_color__filter_palette_and_alpha(void) {
  uint8_t pixels[256];
  for (unsigned int i = 0; i < sizeof(pixels); i++) {
    pixels[i] = i;
  }
  GBitmap bitmap = {
    .addr = pixels,
    .row_size_bytes = 64,
    .bounds = GRect(0, 0, 64, 4),
    .info.format = GBitmapFormat8Bit,
    .info.version = GBITMAP_VERSION_CURRENT,
  };
  system_theme_dark_filter_bitmap(&bitmap, &bitmap.bounds);
  for (unsigned int i = 0; i < sizeof(pixels); i++) {
    cl_assert_equal_i(i, pixels[i]);
  }
  s_dark = true;
  system_theme_dark_filter_bitmap(&bitmap, &bitmap.bounds);
  for (unsigned int i = 0; i < sizeof(pixels); i++) {
    cl_assert_equal_i(i & 0xc0, pixels[i] & 0xc0);
  }
  if (prv_dark_supported()) {
    cl_assert_equal_i(GColorBlackARGB8, pixels[GColorWhiteARGB8]);
    cl_assert_equal_i(GColorWhiteARGB8, pixels[GColorBlackARGB8]);
    cl_assert_equal_i(GColorRedARGB8, pixels[GColorRedARGB8]);
    cl_assert_equal_i(GColorBlueARGB8, pixels[GColorBlueARGB8]);
    cl_assert_equal_i(GColorDarkGrayARGB8, pixels[GColorLightGrayARGB8]);
    cl_assert_equal_i(GColorLightGrayARGB8, pixels[GColorDarkGrayARGB8]);
  }
  // A second pass restores every color, including translucent pixels.
  system_theme_dark_filter_bitmap(&bitmap, &bitmap.bounds);
  for (unsigned int i = 0; i < sizeof(pixels); i++) {
    cl_assert_equal_i(i, pixels[i]);
  }
}

void test_system_theme_color__filter_clips_rectangles_and_round_rows(void) {
  uint8_t pixels[24];
  memset(pixels, GColorWhiteARGB8, sizeof(pixels));
  GBitmap bitmap = {
    .addr = pixels,
    .row_size_bytes = 6,
    .bounds = GRect(1, 1, 4, 2),
    .info.format = GBitmapFormat8Bit,
    .info.version = GBITMAP_VERSION_CURRENT,
  };
  const GRect rect = GRect(-2, -3, 5, 8);
  s_dark = true;
  system_theme_dark_filter_bitmap(&bitmap, &rect);
  for (unsigned int y = 0; y < 4; y++) {
    for (unsigned int x = 0; x < 6; x++) {
      const bool touched = prv_dark_supported() && y >= 1 && y <= 2 && x >= 1 && x <= 2;
      cl_assert_equal_i(touched ? GColorBlackARGB8 : GColorWhiteARGB8, pixels[y * 6 + x]);
    }
  }

  memset(pixels, GColorWhiteARGB8, sizeof(pixels));
  const GBitmapDataRowInfoInternal rows[] = {
    {.offset = 0, .min_x = 2, .max_x = 3},
    {.offset = 6, .min_x = 1, .max_x = 4},
    {.offset = 12, .min_x = 2, .max_x = 3},
  };
  bitmap.info.format = GBitmapFormat8BitCircular;
  bitmap.bounds = GRect(0, 0, 6, 3);
  bitmap.data_row_infos = rows;
  system_theme_dark_filter_bitmap(&bitmap, &bitmap.bounds);
  for (unsigned int i = 0; i < sizeof(pixels); i++) {
    const unsigned int y = i / 6;
    const unsigned int x = i % 6;
    const bool touched = prv_dark_supported() && y < 3 && x >= rows[y].min_x && x <= rows[y].max_x;
    cl_assert_equal_i(touched ? GColorBlackARGB8 : GColorWhiteARGB8, pixels[i]);
  }
}

void test_system_theme_color__filter_ignores_unsupported_formats(void) {
  uint8_t pixels[8];
  memset(pixels, 0xa5, sizeof(pixels));
  GBitmap bitmap = {
    .addr = pixels,
    .row_size_bytes = 4,
    .bounds = GRect(0, 0, 4, 2),
    .info.format = GBitmapFormat1Bit,
    .info.version = GBITMAP_VERSION_CURRENT,
  };
  s_dark = true;
  system_theme_dark_filter_bitmap(&bitmap, &bitmap.bounds);
  system_theme_dark_filter_bitmap(nullptr, &bitmap.bounds);
  for (unsigned int i = 0; i < sizeof(pixels); i++) {
    cl_assert_equal_i(0xa5, pixels[i]);
  }
}
