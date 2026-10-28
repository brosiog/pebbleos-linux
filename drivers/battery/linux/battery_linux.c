/* SPDX-License-Identifier: Apache-2.0 */
#include "battery_linux_bottom.h"

#include <errno.h>

#include <pbl/drivers/battery.h>
#include <pbl/drivers/battery/linux.h>

void battery_init(void) {
}

int battery_get_millivolts(void) {
  return battery_linux_sample().millivolts;
}

int battery_linux_get_percent(void) {
  return battery_linux_sample().percent;
}

int battery_get_constants(BatteryConstants *constants) {
  // Samsung current telemetry uses platform-specific units; do not invent it.
  return -ENOTSUP;
}

int battery_charge_status_get(BatteryChargeStatus *status) {
  *status = battery_linux_sample().full ? BatteryChargeStatusComplete : BatteryChargeStatusUnknown;
  return 0;
}

bool battery_charge_controller_thinks_we_are_charging_impl(void) {
  return battery_linux_sample().charging;
}

bool battery_is_usb_connected_impl(void) {
  // Pebble's external-power state also represents the watch's wireless dock.
  return battery_linux_sample().powered;
}

void battery_set_charge_enable(bool charging_enabled) {
  // Charging remains managed by the existing Samsung kernel driver.
}

void battery_set_fast_charge(bool fast_charge_enabled) {
}
