/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

/* OS-side client of the device-adapter protocol (pbl/drivers/linux_adapter.h). Callable from any
 * thread. Every call is non-blocking and safe with no adapter present. */

#include <stdbool.h>
#include <stdint.h>

#include <pbl/drivers/linux_adapter.h>

//! True once the adapter has answered HELLO and has not gone silent.
bool linux_adapter_online(void);
//! True when the adapter is online and declared the capability (PBL_ADAPTER_CAP_*).
bool linux_adapter_has(unsigned cap);
//! What the panel is doing now (enum pbl_adapter_display_mode), or 0 when unknown.
unsigned linux_adapter_display_actual(void);
//! Ambient light. 0, -EAGAIN (no adapter), -ENODEV (no light sensor) or -ENODATA (not valid).
int linux_adapter_lux(unsigned *lux);
//! Last reported state of a component (enum pbl_adapter_fault_state).
unsigned linux_adapter_fault_state(unsigned component);

//! Commands. Return 0 or -errno; -ENOSYS when the adapter lacks the capability.
int linux_adapter_vibe(unsigned strength, unsigned duration_ms);
int linux_adapter_backlight(unsigned level);
int linux_adapter_display(unsigned mode, unsigned reason);
//! DISPLAY with the ambient clock mode applied: AMBIENT_CLOCK when the clock is on, else OFF.
int linux_adapter_display_idle(unsigned reason);
//! Reports input and makes sure the panel is requested on.
int linux_adapter_wake(unsigned reason, unsigned source);
int linux_adapter_trace(bool enable, unsigned seconds);

//! Settings. Cached, and sent whenever the adapter (re)appears.
void linux_adapter_set_ambient_clock(int mode);
void linux_adapter_set_accel(unsigned samples, unsigned interval_us);
void linux_adapter_set_hrm(bool on, bool low_latency);
//! Suspend contract. Only sent to an adapter that declared SUSPEND.
void linux_adapter_idle(bool busy, uint64_t next_deadline_us);

//! Directories from the device profile and environment.
const char *linux_adapter_runtime_dir(void);
const char *linux_adapter_data_dir(void);

//! Called on the main thread. Implemented by the drivers.
void linux_watch_sensor_received(const struct pbl_adapter_sensor *packet);
void linux_watch_gesture_received(unsigned kind);
//! Input woke the display; the firmware starts Pebble's backlight timeout.
void linux_watch_user_wake(void);

//! Host loop, run by the main thread hook (and directly by tests).
void linux_adapter_init(void);
void linux_adapter_poll(void);
void linux_adapter_exit(void);
int linux_adapter_poll_fd(void);
unsigned linux_adapter_timeout_ms(void);
//! Overrides the runtime directory; call before linux_adapter_init().
void linux_adapter_set_runtime_dir(const char *dir);
