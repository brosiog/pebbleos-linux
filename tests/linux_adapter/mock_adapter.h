/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

/* A stand-in device adapter for tests and CI. It speaks the v2 protocol on adapter.sock, answers
 * HELLO with configurable capabilities, and records everything the OS sends. */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include <pbl/drivers/linux_adapter.h>

struct mock_options {
  const char *dir;
  uint32_t caps;
  //! Delay between receiving DISPLAY and reporting it in STATE.
  unsigned display_latency_ms;
  //! Never answer HELLO (an adapter that is bound but dead).
  bool silent;
  //! Light the panel at once on USER_ACTIVITY and WRIST_UP, like a real adapter's fast path.
  bool fast_path;
  //! STATE period; 0 selects 10 s.
  unsigned heartbeat_ms;
  //! Zero selects the normal mock limits.
  unsigned accel_min_interval_us, accel_max_interval_us, accel_max_samples;
  FILE *log;
};

struct mock;

int mock_start(const struct mock_options *options, struct mock **out);
void mock_stop(struct mock *mock);
//! Re-announces itself, as an adapter that restarted would.
void mock_restart(struct mock *mock);
void mock_send_gesture(struct mock *mock, unsigned kind);
void mock_send_fault(struct mock *mock, unsigned component, unsigned state);
void mock_send_sensor(struct mock *mock, unsigned type, unsigned count, unsigned interval_us,
                      int16_t first, int16_t second);
void mock_set_lux(struct mock *mock, unsigned lux, bool valid);
//! Sends raw bytes to the OS socket (for malformed-input tests).
int mock_send_raw(struct mock *mock, const void *data, size_t size);

//! Messages of a type received since start.
unsigned mock_count(struct mock *mock, unsigned type);
//! Copies the latest message of a type; false when none arrived.
bool mock_last(struct mock *mock, unsigned type, void *out, size_t size);
unsigned mock_display_actual(struct mock *mock);
//! Waits up to `timeout_ms` for `type` to have been received `count` times.
bool mock_wait_count(struct mock *mock, unsigned type, unsigned count, unsigned timeout_ms);
