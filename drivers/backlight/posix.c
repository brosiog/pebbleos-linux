/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include <pbl/drivers/backlight.h>
#include <pbl/drivers/backlight/posix.h>
#ifdef CONFIG_LINUX_WATCH
#include <pbl/drivers/linux_adapter_client.h>
#endif

static int prv_init(const struct pbl_backlight *bl) {
  return 0;
}

static int prv_set_brightness(const struct pbl_backlight *bl, uint8_t brightness) {
#ifdef CONFIG_LINUX_WATCH
  // Pebble's light service owns the timeout: level 0 means it expired.
  if (!brightness)
    return linux_adapter_display_idle(PBL_ADAPTER_REASON_TIMEOUT);
  linux_adapter_backlight(brightness > 100 ? 100 : brightness);
  return linux_adapter_display(PBL_ADAPTER_DISPLAY_ON, PBL_ADAPTER_REASON_USER);
#else
  return 0;
#endif
}

static const struct pbl_backlight_ops s_ops = {
  .init = prv_init,
  .set_brightness = prv_set_brightness,
};

PBL_BACKLIGHT_STATE_DEFINE(pbl_backlight_posix);
const struct pbl_backlight pbl_backlight_posix =
    PBL_BACKLIGHT_INIT(pbl_backlight_posix, "backlight", nullptr, &s_ops);
PBL_DEVICE_REGISTER(pbl_backlight_posix, &pbl_backlight_posix.dev);
