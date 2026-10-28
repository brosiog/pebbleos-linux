/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>

#include <pbl/drivers/linux_adapter_client.h>
void linux_watch_sensor_config(unsigned interval_us, unsigned samples);
void linux_watch_hrm_config(bool enabled, bool low_latency);
uint64_t linux_watch_sensor_boottime_us(void);
