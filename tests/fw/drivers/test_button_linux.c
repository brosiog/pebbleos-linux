/* SPDX-License-Identifier: Apache-2.0 */

#include "../../../drivers/button/linux/linux_watch_input.h"

#include <pbl/drivers/button.h>
#include <pbl/input/input.h>

#include <board/board.h>
#include <clar.h>
#include <kernel/events.h>
#include <pbl_arch_posix.h>

const BoardConfigButton BOARD_CONFIG_BUTTON = {
  .buttons = {
    [BUTTON_ID_BACK] = {.code = PBL_INPUT_KEY_BACK},
    [BUTTON_ID_UP] = {.code = PBL_INPUT_KEY_UP},
    [BUTTON_ID_SELECT] = {.code = PBL_INPUT_KEY_SELECT},
    [BUTTON_ID_DOWN] = {.code = PBL_INPUT_KEY_DOWN},
  },
};

static uint32_t s_queue_free;
static unsigned s_reports;
static uint16_t s_code;
static int32_t s_value;

uint32_t event_kernel_queue_num_free(void) {
  return s_queue_free;
}

void pbl_posix_irq_run(void (*isr)(void *), void *arg) {
  isr(arg);
}

void pbl_input_report(enum pbl_input_type type, uint16_t code, int32_t value, bool sync) {
  cl_assert_equal_i(type, PBL_INPUT_EV_KEY);
  cl_assert(sync);
  s_reports++;
  s_code = code;
  s_value = value;
}

void test_button_linux__initialize(void) {
  s_queue_free = 32;
  button_init();
  button_set_rotated(false);
  s_reports = 0;
}

void test_button_linux__cleanup(void) {
  for (int i = LINUX_BUTTON_BACK; i <= LINUX_BUTTON_DOWN; i++)
    linux_watch_button_changed(i, false);
  cl_assert_equal_i(button_get_state_bits(), 0);
}

static void prv_click(int physical, ButtonId logical, uint16_t code) {
  unsigned before = s_reports;
  linux_watch_button_changed(physical, true);
  cl_assert_equal_i(s_reports, before + 1);
  cl_assert_equal_i(s_code, code);
  cl_assert_equal_i(s_value, 1);
  cl_assert(button_is_pressed(logical));
  linux_watch_button_changed(physical, false);
  cl_assert_equal_i(s_reports, before + 2);
  cl_assert_equal_i(s_code, code);
  cl_assert_equal_i(s_value, 0);
  cl_assert_equal_i(button_get_state_bits(), 0);
}

void test_button_linux__normal_and_rotated_controls(void) {
  prv_click(LINUX_BUTTON_BACK, BUTTON_ID_BACK, PBL_INPUT_KEY_BACK);
  prv_click(LINUX_BUTTON_SELECT, BUTTON_ID_SELECT, PBL_INPUT_KEY_SELECT);
  prv_click(LINUX_BUTTON_UP, BUTTON_ID_UP, PBL_INPUT_KEY_UP);
  prv_click(LINUX_BUTTON_DOWN, BUTTON_ID_DOWN, PBL_INPUT_KEY_DOWN);
  button_set_rotated(true);
  prv_click(LINUX_BUTTON_BACK, BUTTON_ID_SELECT, PBL_INPUT_KEY_SELECT);
  prv_click(LINUX_BUTTON_SELECT, BUTTON_ID_BACK, PBL_INPUT_KEY_BACK);
  prv_click(LINUX_BUTTON_UP, BUTTON_ID_DOWN, PBL_INPUT_KEY_DOWN);
  prv_click(LINUX_BUTTON_DOWN, BUTTON_ID_UP, PBL_INPUT_KEY_UP);
  button_set_rotated(false);
  prv_click(LINUX_BUTTON_SELECT, BUTTON_ID_SELECT, PBL_INPUT_KEY_SELECT);
}

void test_button_linux__rotation_while_held_preserves_release(void) {
  linux_watch_button_changed(LINUX_BUTTON_SELECT, true);
  button_set_rotated(true);
  linux_watch_button_changed(LINUX_BUTTON_SELECT, true);
  cl_assert_equal_i(s_reports, 1);
  linux_watch_button_changed(LINUX_BUTTON_SELECT, false);
  cl_assert_equal_i(s_reports, 2);
  cl_assert_equal_i(s_code, PBL_INPUT_KEY_SELECT);
  cl_assert_equal_i(s_value, 0);
  cl_assert_equal_i(button_get_state_bits(), 0);
  prv_click(LINUX_BUTTON_SELECT, BUTTON_ID_BACK, PBL_INPUT_KEY_BACK);
}

void test_button_linux__held_collision_does_not_release_another_button(void) {
  linux_watch_button_changed(LINUX_BUTTON_SELECT, true);
  button_set_rotated(true);
  linux_watch_button_changed(LINUX_BUTTON_BACK, true);
  linux_watch_button_changed(LINUX_BUTTON_BACK, false);
  cl_assert_equal_i(s_reports, 1);
  cl_assert(button_is_pressed(BUTTON_ID_SELECT));
  linux_watch_button_changed(LINUX_BUTTON_SELECT, false);
  cl_assert_equal_i(s_reports, 2);
  cl_assert_equal_i(button_get_state_bits(), 0);
}

void test_button_linux__queue_pressure_drops_matching_release(void) {
  button_set_rotated(true);
  s_queue_free = 0;
  linux_watch_button_changed(LINUX_BUTTON_DOWN, true);
  button_set_rotated(false);
  s_queue_free = 32;
  linux_watch_button_changed(LINUX_BUTTON_DOWN, false);
  cl_assert_equal_i(s_reports, 0);
  cl_assert_equal_i(button_get_state_bits(), 0);
  prv_click(LINUX_BUTTON_DOWN, BUTTON_ID_DOWN, PBL_INPUT_KEY_DOWN);
}
