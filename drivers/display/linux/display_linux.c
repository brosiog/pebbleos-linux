/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include "display_linux_bottom.h"

#include <string.h>

#include <pbl/drivers/display/display.h>
#include <pbl/drivers/linux_adapter_client.h>
#include <pbl/drivers/rtc.h>
#include <pbl/kernel/assert.h>
#include <pbl/services/clock.h>
#include <pbl/services/light.h>
#include <pbl/services/time.h>
#include <pbl/services/system_task.h>

#include <board/board.h>
#include <pbl_arch_posix.h>
#include <process_management/process_manager.h>
#include <shell/prefs.h>

static void prv_redraw(void *unused) {
  PebbleEvent event = {.type = PEBBLE_RENDER_REQUEST_EVENT};
  process_manager_send_event_to_process(PebbleTask_App, &event);
}

static void prv_redraw_isr(void *result) {
  *(bool *)result = system_task_add_callback_from_isr_droppable(prv_redraw, nullptr);
}

bool display_linux_request_redraw(void) {
  bool queued = false;
  pbl_posix_irq_run(prv_redraw_isr, &queued);
  return queued;
}

// The OS decides what the display does; the adapter only reports gestures and executes the result.
static void prv_gesture(void *data) {
  switch ((unsigned)(uintptr_t)data) {
    case PBL_ADAPTER_GESTURE_WRIST_UP:
      if (backlight_is_motion_enabled()) {
        light_enable_interaction();
        linux_adapter_wake(PBL_ADAPTER_REASON_GESTURE, 0);
      } else {
        // The adapter may already have lit the panel; take it back.
        linux_adapter_display_idle(PBL_ADAPTER_REASON_GESTURE);
      }
      break;
    case PBL_ADAPTER_GESTURE_WRIST_DOWN:
      linux_adapter_display_idle(PBL_ADAPTER_REASON_GESTURE);
      break;
    case PBL_ADAPTER_GESTURE_PALM_COVER:
      linux_adapter_display_idle(PBL_ADAPTER_REASON_PALM);
      break;
    case PBL_ADAPTER_GESTURE_EXTERNAL_WAKE:
      light_enable_interaction();
      linux_adapter_wake(PBL_ADAPTER_REASON_USER, 0);
      break;
    default:
      // EXTERNAL_OFF needs no action: the next wake sees the panel is off.
      break;
  }
}

static void prv_gesture_isr(void *data) {
  system_task_add_callback_from_isr_droppable(prv_gesture, data);
}

void linux_watch_gesture_received(unsigned kind) {
  pbl_posix_irq_run(prv_gesture_isr, (void *)(uintptr_t)kind);
}

static void prv_user_wake(void *unused) {
  light_enable_interaction();
}

static void prv_user_wake_isr(void *unused) {
  system_task_add_callback_from_isr_droppable(prv_user_wake, nullptr);
}

// Input that woke the display never reaches Pebble, so start the backlight timeout here.
void linux_watch_user_wake(void) {
  pbl_posix_irq_run(prv_user_wake_isr, nullptr);
}

static uint8_t s_fb[PBL_DISPLAY_WIDTH * PBL_DISPLAY_HEIGHT];
static bool s_enabled;

static void prv_flush(void) {
  if (s_enabled) {
    time_t now = rtc_get_time();
    struct pbl_tm local;
    pbl_localtime_r(&now, &local);
    display_linux_bottom_set_clock_time((int64_t)now + local.tm_gmtoff);
    display_linux_bottom_set_ambient_clock(shell_prefs_get_ambient_clock_mode(),
                                           clock_is_24h_style());
    display_linux_bottom_update(s_fb, PBL_DISPLAY_WIDTH, PBL_DISPLAY_HEIGHT);
  }
}

void display_init(void) {
  s_enabled = true;
}

void display_clear(void) {
  memset(s_fb, 0, sizeof(s_fb));
  prv_flush();
}

void display_set_enabled(bool enabled) {
  s_enabled = enabled;
}

void display_set_rotated(bool rotated) {
}

bool display_update_in_progress(void) {
  return false;
}

void display_update(NextRowCallback nrcb, UpdateCompleteCallback uccb) {
  PBL_ASSERTN(nrcb != nullptr);
  PBL_ASSERTN(uccb != nullptr);

  DisplayRow row;
  while (nrcb(&row)) {
    uint8_t *dst = &s_fb[row.address * PBL_DISPLAY_WIDTH];
#if PBL_BW
    // 1 bit per pixel, white set, to ARGB2222
    for (int x = 0; x < PBL_DISPLAY_WIDTH; x++) {
      dst[x] = (row.data[x / 8] & (1 << (x % 8))) ? 0xff : 0xc0;
    }
#else
    memcpy(dst, row.data, PBL_DISPLAY_WIDTH);
#endif
  }
  prv_flush();
  uccb(true);
}

void display_update_boot_frame(uint8_t *framebuffer) {
  memcpy(s_fb, framebuffer, sizeof(s_fb));
  prv_flush();
}

#ifdef CONFIG_SHELL
#include <errno.h>
#include <stdlib.h>
#include <pbl/shell/shell.h>
static int prv_ambient_clock(const struct pbl_shell *sh, size_t argc, char **argv) {
  if (argc == 2) {
    int mode = atoi(argv[1]);
    if (mode < 0 || mode >= AmbientClockModeCount)
      return -EINVAL;
    shell_prefs_set_ambient_clock_mode((AmbientClockMode)mode);
    // The idle display renders no frames; apply now rather than at the next flush.
    display_linux_bottom_set_ambient_clock(mode, clock_is_24h_style());
  }
  pbl_shell_print(sh, "ambient_clock=%d (0 off, 1 analog, 2 digital)",
                  (int)shell_prefs_get_ambient_clock_mode());
  return 0;
}
PBL_SHELL_CMD_REGISTER(ambient_clock, nullptr, "Always-on clock: [0|1|2]", prv_ambient_clock);
#endif
