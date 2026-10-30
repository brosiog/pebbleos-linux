/* SPDX-FileCopyrightText: 2025 Elad Dvash */
/* SPDX-License-Identifier: Apache-2.0 */

#include "themes.h"

#include "menu.h"
#include "option_menu.h"
#include "window.h"

#include <pbl/kernel/assert.h>
#include <pbl/kernel/fatal.h>
#include <pbl/logging/logging.h>
#include <pbl/services/i18n/i18n.h>
#include <pbl/util/math.h>
#include <pbl/util/size.h>

#include <applib/graphics/gtypes.h>
#include <applib/ui/app_window_stack.h>
#include <applib/ui/dialogs/expandable_dialog.h>
#include <applib/ui/menu_layer.h>
#include <kernel/pbl_malloc.h>
#include <shell/prefs.h>

#ifdef CONFIG_THEMING

#define DEFAULT_THEME_HIGHLIGHT_COLOR GColorVividCerulean

typedef struct ColorDefinition {
  const char *name;
  const GColor color;
} ColorDefinition;

static const ColorDefinition s_color_definitions[11] = {
  {"Default", GColorClear},
  {"Red", GColorSunsetOrange},
  {"Orange", GColorChromeYellow},
  {"Yellow", GColorYellow},
  {"Green", GColorGreen},
  {"Cyan", GColorCyan},
  {"Light Blue", GColorVividCerulean},
  {"Royal Blue", GColorVeryLightBlue},
  {"Purple", GColorLavenderIndigo},
  {"Magenta", GColorMagenta},
  {"Pink", GColorBrilliantRose},
};
static const char *color_names[ARRAY_LENGTH(s_color_definitions)];
static bool color_names_initialized = false;

static const char **prv_get_color_names(bool short_list) {
  if (!color_names_initialized) {
    for (size_t i = 0; i < ARRAY_LENGTH(s_color_definitions); i++) {
      color_names[i] = (char *)s_color_definitions[i].name;
    }
    color_names_initialized = true;
  }
  return color_names;
}

static int prv_color_to_index(GColor color, GColor default_color) {
  if (color.argb == GColorClear.argb || color.argb == default_color.argb) {
    return 0;
  }
  for (size_t i = 0; i < ARRAY_LENGTH(s_color_definitions); i++) {
    GColor selected_color = s_color_definitions[i].color;
    if ((uint8_t)(color.argb) == (uint8_t)(selected_color.argb)) {
      return i;
    }
  }
  return -1;
}

/////////////////////////////
// Unified Accent Color Settings
/////////////////////////////

static void prv_color_menu_select(OptionMenu *option_menu, int selection, void *context) {
  GColor color;
  if (selection == 0) {
    /* Default option selected -> restore default color. */
    color = DEFAULT_THEME_HIGHLIGHT_COLOR;
  } else {
    color = s_color_definitions[selection].color;
  }

  /* Set the theme highlight color */
  shell_prefs_set_theme_highlight_color(color);

  app_window_stack_remove(&option_menu->window, true /* animated */);
}

static void prv_option_menu_selection_will_change(OptionMenu *option_menu, uint16_t new_row,
                                                  uint16_t old_row, void *context) {
  if (new_row == old_row) {
    return;
  }
  GColor color = s_color_definitions[new_row].color;
  if (color.argb != GColorClear.argb) {
    option_menu_set_highlight_colors(option_menu, color, gcolor_legible_over(color));
  } else {
    option_menu_set_highlight_colors(option_menu, DEFAULT_THEME_HIGHLIGHT_COLOR,
                                     gcolor_legible_over(DEFAULT_THEME_HIGHLIGHT_COLOR));
  }
}

static OptionMenu *prv_push_color_menu(void) {
  const char *title = i18n_noop("Accent Color");
  int selected =
      prv_color_to_index(shell_prefs_get_theme_highlight_color(), DEFAULT_THEME_HIGHLIGHT_COLOR);
  const char **color_names = prv_get_color_names(false);
  const OptionMenuCallbacks callbacks = {
    .select = prv_color_menu_select,
    .selection_will_change = prv_option_menu_selection_will_change,
  };
  if (selected < 0) {
    // Invalid color stored - fall back to default instead of crashing
    // This can happen if an invalid color was synced from the phone
    PBL_LOG_WRN("Invalid menu color, using default");
    selected = 0;
  }
  OptionMenu *const option_menu = settings_option_menu_create(
      title, OptionMenuContentType_SingleLine, selected, &callbacks,
      ARRAY_LENGTH(s_color_definitions), true /* icons_enabled */, color_names, nullptr);

  if (option_menu) {
    if (selected == 0) {
      option_menu_set_highlight_colors(option_menu, DEFAULT_THEME_HIGHLIGHT_COLOR,
                                       gcolor_legible_over(DEFAULT_THEME_HIGHLIGHT_COLOR));
    } else {
      option_menu_set_highlight_colors(option_menu, s_color_definitions[selected].color,
                                       gcolor_legible_over(s_color_definitions[selected].color));
    }
  }

  return option_menu;
}

/////////////////////////////
// Themes Menu
/////////////////////////////

typedef enum ThemesMenuIndex {
  ThemesMenuIndex_DarkMode,
  ThemesMenuIndex_AccentColor,

  ThemesMenuIndexCount,
} ThemesMenuIndex;

typedef struct SettingsThemesData {
  SettingsCallbacks callbacks;
} SettingsThemesData;

static void prv_deinit_cb(SettingsCallbacks *context) {
  i18n_free_all(context);
  app_free(context);
}

static uint16_t prv_num_rows_cb(SettingsCallbacks *context) {
  return ThemesMenuIndexCount;
}

static void prv_draw_row_cb(SettingsCallbacks *context, GContext *ctx, const Layer *cell_layer,
                            uint16_t row, bool selected) {
  const char *title = nullptr;
  const char *subtitle = nullptr;
  switch ((ThemesMenuIndex)row) {
    case ThemesMenuIndex_DarkMode:
      /// Title of the Dark Mode toggle in Themes Settings.
      title = i18n_noop("Dark Mode");
      subtitle = shell_prefs_get_theme_dark_mode() ? i18n_noop("On") : i18n_noop("Off");
      break;
    case ThemesMenuIndex_AccentColor: {
      title = i18n_noop("Accent Color");
      const int index = prv_color_to_index(shell_prefs_get_theme_highlight_color(),
                                           DEFAULT_THEME_HIGHLIGHT_COLOR);
      subtitle = s_color_definitions[MAX(index, 0)].name;
      break;
    }
    case ThemesMenuIndexCount:
      break;
  }
  PBL_ASSERTN(title);
  menu_cell_basic_draw(ctx, cell_layer, i18n_get(title, context), i18n_get(subtitle, context),
                       nullptr);
}

static void prv_select_click_cb(SettingsCallbacks *context, uint16_t row) {
  switch ((ThemesMenuIndex)row) {
    case ThemesMenuIndex_DarkMode:
      shell_prefs_set_theme_dark_mode(!shell_prefs_get_theme_dark_mode());
      break;
    case ThemesMenuIndex_AccentColor: {
      OptionMenu *option_menu = prv_push_color_menu();
      if (option_menu) {
        app_window_stack_push(&option_menu->window, true /* animated */);
      }
      break;
    }
    case ThemesMenuIndexCount:
      break;
  }
  settings_menu_reload_data(SettingsMenuItemThemes);
}

static Window *prv_init(void) {
  SettingsThemesData *data = app_malloc_check(sizeof(*data));
  *data = (SettingsThemesData){
    .callbacks = {
      .deinit = prv_deinit_cb,
      .draw_row = prv_draw_row_cb,
      .select_click = prv_select_click_cb,
      .num_rows = prv_num_rows_cb,
    },
  };
  return settings_window_create(SettingsMenuItemThemes, &data->callbacks);
}
#else
static Window *prv_init(void) {
  pbl_panic();
  return nullptr;
}
#endif // CONFIG_THEMING

const SettingsModuleMetadata *settings_themes_get_info(void) {
  static const SettingsModuleMetadata s_module_info = {
    /// Title of the Themes Settings submenu in Settings
    .name = i18n_noop("Themes"),
    .init = prv_init,
  };

  return &s_module_info;
}
