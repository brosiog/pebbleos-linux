/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include <pbl/drivers/als.h>
#include <pbl/drivers/ambient/posix.h>
#ifdef CONFIG_LINUX_WATCH
#include <pbl/drivers/linux_adapter_client.h>
#endif

static int prv_init(const struct pbl_als *als) {
  return 0;
}

static int prv_read(const struct pbl_als *als, uint32_t *level) {
#ifdef CONFIG_LINUX_WATCH
  unsigned lux;
  int result = linux_adapter_lux(&lux);
  if (result)
    return result;
  *level = lux > AMBIENT_LIGHT_LEVEL_MAX ? AMBIENT_LIGHT_LEVEL_MAX : lux;
  return 0;
#else
  *level = 0U;
  return 0;
#endif
}

static const struct pbl_als_ops s_ops = {
  .init = prv_init,
  .read = prv_read,
};

#ifdef CONFIG_LINUX_WATCH
static const struct pbl_als_scale s_scale = {.num = 1, .den = 1};
#define ALS_SCALE (&s_scale)
#else
#define ALS_SCALE nullptr
#endif
PBL_ALS_STATE_DEFINE(pbl_als_posix);
const struct pbl_als pbl_als_posix = PBL_ALS_INIT(pbl_als_posix, "als", nullptr, &s_ops, ALS_SCALE);
PBL_DEVICE_REGISTER(pbl_als_posix, &pbl_als_posix.dev);
