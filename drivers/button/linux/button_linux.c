/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include "linux_watch_input.h"

#include <pbl/drivers/button.h>
#include <pbl/drivers/debounced_button.h>
#include <pbl/input/input.h>

#include <board/board.h>
#include <kernel/events.h>
#include <pbl_arch_posix.h>

static const ButtonId s_buttons[] = {
  [LINUX_BUTTON_BACK] = BUTTON_ID_BACK,
  [LINUX_BUTTON_UP] = BUTTON_ID_UP,
  [LINUX_BUTTON_SELECT] = BUTTON_ID_SELECT,
  [LINUX_BUTTON_DOWN] = BUTTON_ID_DOWN,
};

static bool s_initialized;
static uint8_t s_state;
// Presses dropped because the UI was not keeping up; their releases are dropped too.
static uint8_t s_dropped;
// Kernel queue slots kept free for releases of delivered presses and other system events.
#define BUTTON_QUEUE_RESERVE 12

struct prv_button_event {
  ButtonId id;
  bool pressed;
};

static void prv_button_isr(void *arg) {
  const struct prv_button_event *e = arg;
  const uint8_t bit = 1u << e->id;
  if (!s_initialized) {
    s_state = e->pressed ? (s_state | bit) : (s_state & ~bit);
    return;
  }
  // A full kernel queue is a fatal reset. When the UI falls behind (bezel spun while an app is
  // busy), skip the click rather than reboot.
  if (e->pressed && event_kernel_queue_num_free() <= BUTTON_QUEUE_RESERVE) {
    s_dropped |= bit;
    return;
  }
  if (!e->pressed && (s_dropped & bit)) {
    s_dropped &= ~bit;
    return;
  }
  if (e->pressed) {
    s_state |= bit;
  } else {
    s_state &= ~bit;
  }
  pbl_input_report_key(BOARD_CONFIG_BUTTON.buttons[e->id].code, e->pressed, true);
}

void linux_watch_button_changed(int button, bool pressed) {
  struct prv_button_event e = {.id = s_buttons[button], .pressed = pressed};
  pbl_posix_irq_run(prv_button_isr, &e);
}

void button_init(void) {
  s_initialized = true;
}

bool button_is_pressed(ButtonId id) {
  return id < NUM_BUTTONS && (s_state & (1u << id)) != 0;
}

uint8_t button_get_state_bits(void) {
  return s_state;
}

void button_set_rotated(bool rotated) {
}

void debounced_button_init(void) {
  button_init();
}
