/* SPDX-License-Identifier: Apache-2.0 */

#include "linux_watch_input.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pbl/drivers/display/linux.h>
#include <pbl/drivers/linux_adapter_client.h>
#include <pbl/drivers/linux_profile.h>

#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <posix_host.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#define MAX_INPUTS 8
struct watch_input {
  int fd;
  bool touch;
  bool bezel;
  bool grabbed;
  bool pressed;
  bool wake_touch;
  // Touch frame state: a contact began, the panel flagged a palm, Pebble holds a touch-down.
  bool touch_began;
  bool palm;
  bool palm_acted;
  bool delivered_down;
  unsigned buttons;
  int slot;
  int x;
  int y;
  struct input_absinfo abs_x;
  struct input_absinfo abs_y;
};
static struct watch_input s_inputs[MAX_INPUTS];
static int s_count;
static int s_width = 260;
static int s_height = 260;
static bool s_trace;
static int s_bezel_pending;
static int s_bezel_button = -1;
static uint64_t s_bezel_deadline;
static unsigned s_physical_buttons;
static bool s_debug_chord_active;
static bool s_debug_requested;
static uint64_t s_debug_chord_since;
#define DEBUG_CHORD_US   3000000ULL
#define DEBUG_CHORD_MASK ((1u << LINUX_BUTTON_BACK) | (1u << LINUX_BUTTON_SELECT))
// Input that wakes the idle display only wakes it; covers the wake round trip to the adapter.
#define WAKE_GUARD_US 500000ULL
static uint64_t s_wake_guard_until;
static unsigned s_wake_buttons;

// Autosleep can suspend again between a key interrupt and this process reading the event. Input
// devices sit in an EPOLLWAKEUP set, so the kernel stays awake until they are read; a waking input
// then holds a second EPOLLWAKEUP source until the adapter reports the panel on, or this long.
#define WAKE_HOLD_US 2000000ULL
static int s_epoll = -1;
static int s_hold_epoll = -1;
static int s_hold_event = -1;
static uint64_t s_hold_until;

static void prv_hold_init(void) {
  s_hold_epoll = epoll_create1(EPOLL_CLOEXEC);
  s_hold_event = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  struct epoll_event event = {.events = EPOLLIN | EPOLLWAKEUP};
  if (s_hold_epoll < 0 || s_hold_event < 0 ||
      epoll_ctl(s_hold_epoll, EPOLL_CTL_ADD, s_hold_event, &event)) {
    fprintf(stderr, "evdev: wake hold unavailable\n");
  }
}

static void prv_hold_awake(uint64_t now) {
  if (s_hold_event < 0)
    return;
  if (!s_hold_until) {
    uint64_t one = 1;
    (void)!write(s_hold_event, &one, sizeof(one));
  }
  s_hold_until = now + WAKE_HOLD_US;
}

static void prv_hold_poll(uint64_t now) {
  if (!s_hold_until)
    return;
  bool lit = linux_adapter_display_actual() == PBL_ADAPTER_DISPLAY_ON;
  if (now < s_hold_until && !lit)
    return;
  uint64_t count;
  struct epoll_event event;
  (void)!read(s_hold_event, &count, sizeof(count));
  // Collecting the drained source relaxes its wakeup source.
  epoll_wait(s_hold_epoll, &event, 1, 0);
  s_hold_until = 0;
}

// Samsung touch panels flag a palm on a vendor axis as well as the standard tool type.
#ifndef ABS_MT_PALM
#define ABS_MT_PALM 0x3e
#endif
#ifndef MT_TOOL_PALM
#define MT_TOOL_PALM 2
#endif
// A hand leaving the screen nudges the bezel or brushes the panel. Samsung disables the panel
// once the display is off, so the lift is unseen: for this long after the last palm frame a lone
// detent or touch does not wake the display, while a deliberate turn (2 detents) or a button does.
#define PALM_QUIET_US       10000000ULL
#define PALM_TURN_WINDOW_US 800000ULL
static uint64_t s_palm_quiet_until;
static uint64_t s_palm_detent_at;

// How long the kernel held an input event before it reached this process.
static long prv_event_age_ms(const struct input_event *event) {
  struct timeval now;
  gettimeofday(&now, NULL);
  return (now.tv_sec - event->input_event_sec) * 1000L +
         (now.tv_usec - (long)event->input_event_usec) / 1000L;
}

static void prv_physical_button(struct watch_input *input, int button, bool pressed) {
  unsigned previous = s_physical_buttons;
  if (pressed) {
    input->buttons |= 1u << button;
  } else {
    input->buttons &= ~(1u << button);
  }
  s_physical_buttons = 0;
  for (int i = 0; i < s_count; i++) {
    s_physical_buttons |= s_inputs[i].buttons;
  }
  if ((previous ^ s_physical_buttons) & (1u << button)) {
    linux_watch_button_changed(button, (s_physical_buttons & (1u << button)) != 0);
  }
  bool chord = (s_physical_buttons & DEBUG_CHORD_MASK) == DEBUG_CHORD_MASK;
  if (chord && !s_debug_chord_active) {
    s_debug_chord_since = posix_host_monotonic_us();
  }
  s_debug_chord_active = chord;
}

static void prv_debug_chord_poll(uint64_t now) {
  if (s_debug_chord_active && !s_debug_requested && now - s_debug_chord_since >= DEBUG_CHORD_US) {
    s_debug_requested = true;
    fprintf(stderr, "evdev: both side buttons held; returning to Samsung debug interface\n");
    posix_host_quit();
  }
}

void linux_watch_touch_init(int width, int height) {
  s_width = width;
  s_height = height;
}

static void prv_init(void) {
  s_trace = getenv("PBL_INPUT_TRACE") != NULL;
  s_epoll = epoll_create1(EPOLL_CLOEXEC);
  prv_hold_init();
  for (int index = 0; index < 64 && s_count < MAX_INPUTS; index++) {
    char path[64];
    char name[128] = {0};
    snprintf(path, sizeof(path), "/dev/input/event%d", index);
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
      continue;
    }
    const struct linux_profile *profile = linux_profile_get();
    bool touch = false, bezel = false;
    if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) < 0 ||
        !((touch = linux_profile_match(profile->touch, name)) ||
          (bezel = linux_profile_match(profile->bezel, name)) ||
          linux_profile_match(profile->keys, name))) {
      close(fd);
      continue;
    }
    struct watch_input *input = &s_inputs[s_count++];
    input->fd = fd;
    input->touch = touch;
    input->bezel = bezel;
    if (!input->touch && getenv("PBL_INPUT_GRAB")) {
      input->grabbed = ioctl(fd, EVIOCGRAB, 1) == 0;
      fprintf(stderr, "evdev: exclusive %s %s\n", name, input->grabbed ? "acquired" : "failed");
    }
    if (input->touch) {
      ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &input->abs_x);
      ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &input->abs_y);
    }
    struct epoll_event ready = {.events = EPOLLIN | EPOLLWAKEUP, .data.ptr = input};
    if (s_epoll < 0 || epoll_ctl(s_epoll, EPOLL_CTL_ADD, fd, &ready))
      fprintf(stderr, "evdev: %s wakeup polling unavailable\n", name);
    fprintf(stderr, "evdev: %s (%s)\n", name, path);
  }
}

static void prv_event(struct watch_input *input, const struct input_event *event);

static void prv_read(struct watch_input *input) {
  struct input_event events[32];
  ssize_t count;
  while ((count = read(input->fd, events, sizeof(events))) > 0) {
    for (size_t n = 0; n < (size_t)count / sizeof(events[0]); n++) {
      prv_event(input, &events[n]);
    }
  }
}

static int prv_scale(int value, const struct input_absinfo *axis, int extent) {
  int range = axis->maximum - axis->minimum;
  if (range <= 0) {
    return 0;
  }
  int scaled = (long long)(value - axis->minimum) * (extent - 1) / range;
  return scaled < 0 ? 0 : scaled >= extent ? extent - 1 : scaled;
}

static int prv_button(const struct input_event *event) {
  if (event->code == KEY_POWER || event->code == KEY_HOME)
    return LINUX_BUTTON_SELECT;
  return event->code == KEY_BACK ? LINUX_BUTTON_BACK : -1;
}

static void prv_event(struct watch_input *input, const struct input_event *event) {
  uint64_t now = posix_host_monotonic_us();
  // A touch is only classified (finger or palm) once its frame is complete.
  // One bezel detent also emits a companion REL_X event; only the wheel counts as a detent.
  bool begins = (event->type == EV_KEY && event->value == 1) ||
                (event->type == EV_REL && event->value &&
                 (event->code == REL_WHEEL || event->code == REL_DIAL)) ||
                (input->touch && event->type == EV_SYN && event->code == SYN_REPORT &&
                 input->touch_began && !input->palm);
  if (begins && (input->bezel || input->touch) && now < s_palm_quiet_until &&
      !display_linux_is_interactive()) {
    bool turned = input->bezel && s_palm_detent_at && now - s_palm_detent_at < PALM_TURN_WINDOW_US;
    s_palm_detent_at = input->bezel ? now : 0;
    if (!turned) {
      if (input->touch)
        input->wake_touch = true;
      return;
    }
    s_palm_quiet_until = 0;
  }
  if (begins) {
    unsigned source = input->touch   ? PBL_ADAPTER_ACTIVITY_TOUCH
                      : input->bezel ? PBL_ADAPTER_ACTIVITY_BEZEL
                                     : PBL_ADAPTER_ACTIVITY_BUTTON;
    if (!display_linux_is_interactive()) {
      prv_hold_awake(now);
      if (now >= s_wake_guard_until)
        fprintf(stderr, "evdev: wake %s event age %ldms\n",
                input->touch   ? "touch"
                : input->bezel ? "bezel"
                               : "button",
                prv_event_age_ms(event));
      s_wake_guard_until = now + WAKE_GUARD_US;
      linux_watch_user_wake();
    }
    linux_adapter_wake(PBL_ADAPTER_REASON_USER, source);
  }
  bool waking = now < s_wake_guard_until || !display_linux_is_interactive();
  if (s_trace && !input->touch && (event->type == EV_KEY || event->type == EV_REL)) {
    fprintf(stderr, "evdev event: bezel=%d type=%u code=%u value=%d\n", input->bezel, event->type,
            event->code, event->value);
  }
  if (input->touch) {
    if (begins && waking)
      input->wake_touch = true;
    if (event->type == EV_ABS) {
      if ((event->code == ABS_MT_PALM && event->value) ||
          (event->code == ABS_MT_TOOL_TYPE && event->value == MT_TOOL_PALM)) {
        input->palm = true;
      }
      if (event->code == ABS_MT_SLOT) {
        input->slot = event->value;
      } else if (input->slot == 0) {
        if (event->code == ABS_MT_TRACKING_ID) {
          input->pressed = event->value >= 0;
          input->touch_began |= event->value >= 0;
        }
        if (event->code == ABS_MT_POSITION_X)
          input->x = event->value;
        if (event->code == ABS_MT_POSITION_Y)
          input->y = event->value;
      }
    } else if (event->type == EV_KEY && event->code == BTN_TOUCH) {
      input->pressed = event->value != 0;
    } else if (event->type == EV_SYN && event->code == SYN_REPORT) {
      input->touch_began = false;
      int x = prv_scale(input->x, &input->abs_x, s_width);
      int y = prv_scale(input->y, &input->abs_y, s_height);
      if (input->palm) {
        // Covering the screen with a palm turns it off, like Samsung's cover gesture. Never a tap.
        if (input->delivered_down) {
          linux_watch_touch_changed(false, x, y);
          input->delivered_down = false;
        }
        if (!input->palm_acted && display_linux_is_interactive()) {
          fprintf(stderr, "evdev: palm cover, display off\n");
          linux_adapter_display_idle(PBL_ADAPTER_REASON_PALM);
        }
        input->palm_acted = input->pressed;
        input->wake_touch = input->pressed;
        input->palm = input->pressed;
        // Quiet while the palm covers the screen, then for PALM_QUIET_US after it lifts.
        s_palm_quiet_until = now + PALM_QUIET_US;
        s_palm_detent_at = 0;
        return;
      }
      // Hide a waking touch from Pebble until its finger lifts.
      if (input->wake_touch) {
        input->wake_touch = input->pressed;
        return;
      }
      linux_watch_touch_changed(input->pressed, x, y);
      input->delivered_down = input->pressed;
    }
    return;
  }
  if (input->bezel && event->type == EV_REL &&
      (event->code == REL_WHEEL || event->code == REL_DIAL) && event->value) {
    if (waking)
      return;
    s_bezel_pending += event->value > 0 ? 1 : -1;
    if (s_bezel_pending > 8)
      s_bezel_pending = 8;
    if (s_bezel_pending < -8)
      s_bezel_pending = -8;
  } else if (event->type == EV_KEY && (event->value == 0 || event->value == 1)) {
    int button = prv_button(event);
    if (button < 0)
      return;
    // A press that wakes the display, and its release, never reach Pebble.
    if (event->value && waking) {
      s_wake_buttons |= 1u << button;
      return;
    }
    if (!event->value && (s_wake_buttons & (1u << button))) {
      s_wake_buttons &= ~(1u << button);
      return;
    }
    prv_physical_button(input, button, event->value != 0);
  }
}

static void prv_poll(void) {
  uint64_t now = posix_host_monotonic_us();
  if (s_epoll >= 0) {
    struct pollfd ready = {.fd = s_epoll, .events = POLLIN};
    // Checking readiness keeps the wakeup sources; epoll_wait() below releases them.
    if (poll(&ready, 1, 0) > 0) {
      if (!display_linux_is_interactive())
        prv_hold_awake(now);
      struct epoll_event events[MAX_INPUTS];
      int n = epoll_wait(s_epoll, events, MAX_INPUTS, 0);
      for (int i = 0; i < n; i++) {
        prv_read(events[i].data.ptr);
      }
    }
  } else {
    for (int i = 0; i < s_count; i++) {
      prv_read(&s_inputs[i]);
    }
  }
  prv_hold_poll(now);
  // A detent becomes a paced click, allowing the firmware to drain its queue.
  prv_debug_chord_poll(now);
  if (now < s_bezel_deadline)
    return;
  if (s_bezel_button >= 0) {
    linux_watch_button_changed(s_bezel_button, false);
    s_bezel_button = -1;
    s_bezel_deadline = now + 20000;
  } else if (s_bezel_pending) {
    bool down = s_bezel_pending > 0;
    s_bezel_pending += down ? -1 : 1;
    s_bezel_button = down ? LINUX_BUTTON_DOWN : LINUX_BUTTON_UP;
    linux_watch_button_changed(s_bezel_button, true);
    s_bezel_deadline = now + 20000;
  }
}

static void prv_exit(void) {
  if (s_epoll >= 0)
    close(s_epoll);
  if (s_hold_epoll >= 0)
    close(s_hold_epoll);
  if (s_hold_event >= 0)
    close(s_hold_event);
  for (int i = 0; i < s_count; i++) {
    if (s_inputs[i].grabbed)
      ioctl(s_inputs[i].fd, EVIOCGRAB, 0);
    close(s_inputs[i].fd);
  }
}

static int prv_poll_fd(unsigned index) {
  if (s_epoll >= 0)
    return index ? -1 : s_epoll;
  return index < (unsigned)s_count ? s_inputs[index].fd : -1;
}

static unsigned prv_poll_timeout_ms(void) {
  if (s_bezel_pending || s_bezel_button >= 0 || s_debug_chord_active)
    return 10;
  if (s_hold_until) {
    uint64_t now = posix_host_monotonic_us();
    return now >= s_hold_until ? 0 : (unsigned)((s_hold_until - now + 999) / 1000);
  }
  return 60000;
}

POSIX_HOST_HOOK(.init = prv_init, .poll = prv_poll, .exit = prv_exit, .poll_fd = prv_poll_fd,
                .poll_timeout_ms = prv_poll_timeout_ms)
