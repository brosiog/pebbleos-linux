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
static bool s_rotated;
static uint8_t s_state;
static uint8_t s_physical_state;
// Match each release to its press even if the orientation changes while held.
static ButtonId s_pressed_ids[NUM_BUTTONS];
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
  const uint8_t physical_bit = 1u << e->id;
  if (((s_physical_state & physical_bit) != 0) == e->pressed)
    return;
  ButtonId id;
  if (e->pressed) {
    s_physical_state |= physical_bit;
    static const ButtonId rotated_ids[NUM_BUTTONS] = {
      [BUTTON_ID_BACK] = BUTTON_ID_SELECT,
      [BUTTON_ID_UP] = BUTTON_ID_DOWN,
      [BUTTON_ID_SELECT] = BUTTON_ID_BACK,
      [BUTTON_ID_DOWN] = BUTTON_ID_UP,
    };
    id = s_pressed_ids[e->id] = s_rotated ? rotated_ids[e->id] : e->id;
  } else {
    s_physical_state &= ~physical_bit;
    id = s_pressed_ids[e->id];
  }
  const uint8_t bit = 1u << id;
  // A full kernel queue is a fatal reset. When the UI falls behind (bezel spun while an app is
  // busy), skip the click rather than reboot.
  if (e->pressed && ((s_state & bit) ||
                     (s_initialized && event_kernel_queue_num_free() <= BUTTON_QUEUE_RESERVE))) {
    s_dropped |= physical_bit;
    return;
  }
  if (!e->pressed && (s_dropped & physical_bit)) {
    s_dropped &= ~physical_bit;
    return;
  }
  if (e->pressed) {
    s_state |= bit;
  } else {
    s_state &= ~bit;
  }
  if (s_initialized)
    pbl_input_report_key(BOARD_CONFIG_BUTTON.buttons[id].code, e->pressed, true);
}

void linux_watch_button_changed(int button, bool pressed) {
  if (button < 0 || button >= (int)(sizeof(s_buttons) / sizeof(s_buttons[0])))
    return;
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
  s_rotated = rotated;
}

void debounced_button_init(void) {
  button_init();
}
