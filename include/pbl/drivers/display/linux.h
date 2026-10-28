/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

// The adapter owns physical idle/wake state independently of Pebble brightness.
bool display_linux_is_interactive(void);
uint64_t display_linux_frame_count(void);
