/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
struct linux_battery_sample {
  int millivolts;
  int percent;
  bool charging;
  bool full;
  bool powered;
};
struct linux_battery_sample battery_linux_sample(void);
