# SPDX-FileCopyrightText: 2026 Core Devices LLC
# SPDX-License-Identifier: Apache-2.0

pbl_clar_test(test_system_theme
  SOURCES
    fw/shell/system_theme.c
    fw/shell/system_theme_style.c
)

set(theme_color_sources
  fw/shell/system_theme_color.c
  fw/applib/graphics/gbitmap.c
  fw/applib/graphics/gtypes.c
  fw/applib/graphics/gdraw_command.c
  tests/fakes/fake_gbitmap_png.c
  fw/board/displays/display_gabbro.c
)

pbl_clar_test(test_system_theme_color
  SOURCES ${theme_color_sources}
  PLATFORMS emery gabbro flint
  DEFINES CONFIG_THEMING=1
)

pbl_clar_test(test_system_theme_color_disabled
  SOURCE test_system_theme_color.c
  SOURCES ${theme_color_sources}
  PLATFORMS emery
)

pbl_clar_test(test_system_theme_color_recovery
  SOURCE test_system_theme_color.c
  SOURCES ${theme_color_sources}
  PLATFORMS emery
  DEFINES CONFIG_THEMING=1 CONFIG_RECOVERY_FW=1
)
