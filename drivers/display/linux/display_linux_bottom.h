/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
void display_linux_bottom_update(const uint8_t *fb, int width, int height);
void display_linux_bottom_set_clock_time(int64_t local_epoch);
// Idle clock: 0 off, 1 analog, 2 digital.
void display_linux_bottom_set_ambient_clock(int mode, bool is_24h);
bool display_linux_request_redraw(void);
// Re-check shortly that Samsung UI (charging screen, popups) has not covered Pebble.
void display_linux_recheck_foreground(void);
