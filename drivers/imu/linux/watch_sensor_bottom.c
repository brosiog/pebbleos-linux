/* SPDX-License-Identifier: Apache-2.0 */
#include "watch_sensor_bottom.h"

uint64_t linux_watch_sensor_boottime_us(void) {
  return pbl_adapter_boottime_us();
}

void linux_watch_sensor_config(unsigned interval_us, unsigned samples) {
  linux_adapter_set_accel(samples, interval_us);
}

void linux_watch_hrm_config(bool enabled, bool low_latency) {
  linux_adapter_set_hrm(enabled, low_latency);
}
