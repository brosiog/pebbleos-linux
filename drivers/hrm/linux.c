/* SPDX-License-Identifier: Apache-2.0 */
#include "../imu/linux/watch_sensor_bottom.h"

#include <pbl/drivers/hrm/linux.h>
static bool s_enabled;
void hrm_init(HRMDevice *dev) {
  (void)dev;
}
bool hrm_enable(HRMDevice *dev, HRMFeature features, bool low_latency) {
  (void)dev;
  if (features != HRMFeature_BPM)
    return false;
  s_enabled = true;
  linux_watch_hrm_config(true, low_latency);
  return true;
}
void hrm_disable(HRMDevice *dev) {
  (void)dev;
  s_enabled = false;
  linux_watch_hrm_config(false, false);
}
bool hrm_is_enabled(HRMDevice *dev) {
  (void)dev;
  return s_enabled;
}
void hrm_set_activity_scene(HRMDevice *dev, HRMActivityScene scene) {
  (void)dev;
  (void)scene;
}
