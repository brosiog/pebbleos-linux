/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
enum {
  LINUX_BUTTON_BACK,
  LINUX_BUTTON_UP,
  LINUX_BUTTON_SELECT,
  LINUX_BUTTON_DOWN
};
void linux_watch_button_changed(int button, bool pressed);
void linux_watch_touch_changed(bool pressed, int x, int y);
void linux_watch_touch_init(int width, int height);
