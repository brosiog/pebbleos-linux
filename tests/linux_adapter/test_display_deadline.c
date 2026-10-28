/* SPDX-License-Identifier: Apache-2.0 */

#include "../../drivers/display/linux/display_linux_bottom.c"

static time_t s_now = 1793145659; /* One second before a minute boundary. */
static uint64_t s_monotonic_us = 1000000;
static struct linux_profile s_profile;
static unsigned s_panel = PBL_ADAPTER_DISPLAY_AMBIENT_CLOCK;
static unsigned s_commits;

#define CHECK(condition)                                                   \
  do {                                                                     \
    if (!(condition)) {                                                    \
      fprintf(stderr, "CHECK FAILED line %d: %s\n", __LINE__, #condition); \
      return 1;                                                            \
    }                                                                      \
  } while (0)

time_t time(time_t *out) {
  if (out)
    *out = s_now;
  return s_now;
}

const struct linux_profile *linux_profile_get(void) {
  return &s_profile;
}

unsigned linux_adapter_display_actual(void) {
  return s_panel;
}

void linux_adapter_set_ambient_clock(int mode) {
  (void)mode;
}

const char *linux_adapter_runtime_dir(void) {
  return "/tmp";
}

uint64_t posix_host_monotonic_us(void) {
  return s_monotonic_us;
}

void posix_host_hook_register(const struct posix_host_hook *hook) {
  (void)hook;
}

void posix_host_option_register(const struct posix_host_option *option) {
  (void)option;
}

void posix_host_wake(void) {
}

bool display_linux_request_redraw(void) {
  return true;
}

bool wayland_linux_init(const char *path) {
  (void)path;
  return true;
}

void wayland_linux_poll(void) {
}

int wayland_linux_fd(void) {
  return -1;
}

void wayland_linux_activate(void) {
}

bool wayland_linux_obscured(void) {
  return false;
}

bool wayland_linux_take_resume_redraw(void) {
  return false;
}

bool wayland_linux_update(const uint8_t *pixels, int width, int height) {
  (void)pixels;
  (void)width;
  (void)height;
  s_commits++;
  return true;
}

void wayland_linux_exit(void) {
}

int main(void) {
  s_wayland = s_ambient = true;
  s_width = s_height = 260;
  s_frame = calloc(s_width, s_height);
  CHECK(s_frame);
  atomic_store(&s_clock_mode, 1);
  prv_poll();
  CHECK(s_commits == 1);
  CHECK(prv_poll_timeout_ms() == 1000);
  s_now++;
  s_monotonic_us += 1000000;
  prv_poll();
  CHECK(s_commits == 2);
  CHECK(prv_poll_timeout_ms() == 60000);
  s_now += 30;
  s_profile.reclaim_delays = 1;
  s_profile.reclaim_delays_ms[0] = 500;
  s_foreground_base = s_monotonic_us;
  s_foreground_next = 0;
  CHECK(prv_poll_timeout_ms() == 500);
  s_foreground_next = 1;
  CHECK(prv_poll_timeout_ms() == 30000);
  atomic_store(&s_clock_mode, 0);
  CHECK(prv_poll_timeout_ms() == 60000);
  s_panel = 0;
  prv_poll();
  CHECK(display_linux_is_interactive());
  // fbdev follows the same adapter state and keeps app frames behind the ambient clock.
  s_wayland = false;
  s_var.xres = s_var.yres = 260;
  s_var.bits_per_pixel = 32;
  s_var.red = (struct fb_bitfield){.offset = 16, .length = 8};
  s_var.green = (struct fb_bitfield){.offset = 8, .length = 8};
  s_var.blue = (struct fb_bitfield){.offset = 0, .length = 8};
  s_var.transp = (struct fb_bitfield){.offset = 24, .length = 8};
  s_fix.line_length = 260 * 4;
  s_fix.smem_len = 260 * s_fix.line_length;
  s_memory = calloc(1, s_fix.smem_len);
  uint8_t *previous = malloc(s_fix.smem_len);
  uint8_t *face = malloc(260 * 260);
  CHECK(s_memory && previous && face);
  s_panel = PBL_ADAPTER_DISPLAY_AMBIENT_CLOCK;
  atomic_store(&s_clock_mode, 1);
  prv_poll();
  CHECK(!display_linux_is_interactive());
  CHECK(prv_poll_timeout_ms() == 30000);
  memcpy(previous, s_memory, s_fix.smem_len);
  memset(face, 0xff, 260 * 260);
  display_linux_bottom_update(face, 260, 260);
  CHECK(!memcmp(previous, s_memory, s_fix.smem_len));
  s_panel = PBL_ADAPTER_DISPLAY_ON;
  prv_poll();
  CHECK(display_linux_is_interactive());
  CHECK(memcmp(previous, s_memory, s_fix.smem_len));
  free(previous);
  free(face);
  free(s_memory);
  s_memory = NULL;
  free(s_frame);
  free(s_clock);
  puts("PASS  ambient clock schedules its next minute and preserves earlier wake deadlines");
  puts("PASS  fbdev follows display power and restores app frames after ambient mode");
  return 0;
}
