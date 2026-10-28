/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
bool wayland_linux_init(const char *path);
void wayland_linux_poll(void);
int wayland_linux_fd(void);
void wayland_linux_activate(void);
bool wayland_linux_obscured(void);
bool wayland_linux_take_resume_redraw(void);
bool wayland_linux_update(const uint8_t *pixels, int width, int height);
void wayland_linux_exit(void);
