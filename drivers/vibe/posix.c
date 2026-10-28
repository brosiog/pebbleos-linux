/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include <pbl/drivers/vibe.h>
#include <pbl/drivers/vibe/posix.h>
#ifdef CONFIG_LINUX_WATCH
#include <pbl/drivers/linux_adapter_client.h>
#endif

static int prv_init(const struct pbl_vibe *vibe) {
  return 0;
}

#ifdef CONFIG_LINUX_WATCH
int pbl_vibe_posix_start_timed(int8_t strength, uint16_t duration_ms) {
  int magnitude = strength < 0 ? -(int)strength : strength;
  return linux_adapter_vibe(magnitude > 100 ? 100 : magnitude, duration_ms);
}
#endif

static int prv_start(const struct pbl_vibe *vibe, int8_t strength) {
#ifdef CONFIG_LINUX_WATCH
  int magnitude = strength < 0 ? -(int)strength : strength;
  // The real duration is not known here. The adapter caps a pulse at its own maximum.
  return linux_adapter_vibe(magnitude > 100 ? 100 : magnitude, PBL_ADAPTER_VIBE_MAX_MS);
#else
  return 0;
#endif
}

static int prv_stop(const struct pbl_vibe *vibe) {
#ifdef CONFIG_LINUX_WATCH
  return linux_adapter_vibe(0, 0);
#else
  return 0;
#endif
}

static const struct pbl_vibe_ops s_ops = {
  .init = prv_init,
  .start = prv_start,
  .stop = prv_stop,
};

PBL_VIBE_STATE_DEFINE(pbl_vibe_posix);
const struct pbl_vibe pbl_vibe_posix =
    PBL_VIBE_INIT(pbl_vibe_posix, "vibe", nullptr, &s_ops, VIBE_STRENGTH_OFF);
PBL_DEVICE_REGISTER(pbl_vibe_posix, &pbl_vibe_posix.dev);
