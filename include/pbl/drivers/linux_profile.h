/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

/* Device profile of a Linux watch port: the values that differ between devices and do not belong
 * in the adapter protocol. Built-in defaults describe the Galaxy Watch3. A profile file
 * (--profile FILE or PBL_DEVICE_PROFILE) overrides them, one `key = value` per line, `#` starts a
 * comment. See spec/device-profile.md. */

#include <stdbool.h>
#include <stddef.h>

#define LINUX_PROFILE_MAX_NAMES  8
#define LINUX_PROFILE_MAX_DELAYS 4

struct linux_profile {
  //! Used when neither --runtime-dir, PBL_RUNTIME_DIR nor XDG_RUNTIME_DIR gives one.
  char runtime_dir[128];
  //! Persistent OS data (RTC state). PBL_DATA_DIR overrides it.
  char data_dir[128];
  //! evdev device names. A leading `*` matches a substring, otherwise the name must be equal.
  char keys[LINUX_PROFILE_MAX_NAMES][48];
  char touch[LINUX_PROFILE_MAX_NAMES][48];
  char bezel[LINUX_PROFILE_MAX_NAMES][48];
  //! Milliseconds after waking at which to check that a vendor UI has not covered Pebble.
  unsigned reclaim_delays_ms[LINUX_PROFILE_MAX_DELAYS];
  unsigned reclaim_delays;
};

//! Names the profile file; call before the first linux_profile_get().
void linux_profile_set_file(const char *path);
//! Loads the built-in defaults, then the profile file if one is configured. Safe to call twice.
const struct linux_profile *linux_profile_get(void);
//! True when a device name matches one of the profile's names.
bool linux_profile_match(const char (*names)[48], const char *device);
