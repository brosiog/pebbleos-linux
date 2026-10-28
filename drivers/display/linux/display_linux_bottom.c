/* SPDX-License-Identifier: Apache-2.0 */

#include "display_linux_bottom.h"

#include "wayland_linux.h"

#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <pbl/drivers/display/linux.h>
#include <pbl/drivers/linux_adapter_client.h>
#include <pbl/drivers/linux_profile.h>

#include <fcntl.h>
#include <linux/fb.h>
#include <posix_host.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static int s_fd = -1;
static uint8_t *s_memory;
static struct fb_fix_screeninfo s_fix;
static struct fb_var_screeninfo s_var;
static uint8_t *s_frame;
static uint8_t *s_rotated_frame;
static bool s_rotated;
static int s_width;
static int s_height;
static const char *s_capture;
// SIGUSR1 saves the frame on screen (the idle clock included) without stopping the firmware. The
// file is a developer aid of this process and not part of the adapter protocol.
static char s_signal_capture_path[256];
static volatile sig_atomic_t s_capture_requested;

static void prv_capture_signal(int signal) {
  (void)signal;
  s_capture_requested = 1;
}
static bool s_pan_error;
static uint8_t *s_previous;
static bool s_wayland;
static bool s_dirty;
static bool s_ambient;
// Idle clock: 0 off (display off), 1 analog, 2 digital. Set from the firmware preference.
static _Atomic int s_clock_mode;
static atomic_bool s_clock_24h;
static int s_drawn_mode = -1;
static int s_drawn_clock_mode = -1;
static atomic_bool s_interactive = true;
static _Atomic uint64_t s_frame_count;
static bool s_wake_app_pending;
bool display_linux_is_interactive(void) {
  return atomic_load(&s_interactive);
}
uint64_t display_linux_frame_count(void) {
  return atomic_load(&s_frame_count);
}
static uint8_t *s_clock;
// A vendor UI may raise itself at screen-on and charger changes, sometimes after Pebble's raise.
// The device profile says when to check.
static uint64_t s_foreground_base;
static unsigned s_foreground_next;
// When the display last became interactive, until its first frame is committed.
static uint64_t s_wake_at;
static time_t s_clock_minute = -1;
static _Atomic int64_t s_clock_offset;

void display_linux_bottom_set_clock_time(int64_t local_epoch) {
  atomic_store(&s_clock_offset, local_epoch - (int64_t)time(NULL));
}

void display_linux_bottom_set_ambient_clock(int mode, bool is_24h) {
  atomic_store(&s_clock_24h, is_24h);
  if (atomic_exchange(&s_clock_mode, mode) != mode) {
    linux_adapter_set_ambient_clock(mode);
    posix_host_wake();
  }
}

static void prv_dot(int x, int y, int radius, uint8_t color) {
  for (int dy = -radius; dy <= radius; dy++) {
    for (int dx = -radius; dx <= radius; dx++) {
      if (x + dx >= 0 && x + dx < s_width && y + dy >= 0 && y + dy < s_height &&
          dx * dx + dy * dy <= radius * radius)
        s_clock[(y + dy) * s_width + x + dx] = color;
    }
  }
}

static void prv_hand(int cx, int cy, double turns, int from, int to, int thickness) {
  double angle = turns * 6.283185307179586;
  for (int distance = from; distance <= to; distance++)
    prv_dot(cx + lround(sin(angle) * distance), cy - lround(cos(angle) * distance), thickness,
            0xd5);
}

// Seven-segment digits, segments a-g: top, top right, bottom right, bottom, bottom left, top left,
// middle.
static const uint8_t s_segments[10] = {0x3f, 0x06, 0x5b, 0x4f, 0x66, 0x6d, 0x7d, 0x07, 0x7f, 0x6f};

static void prv_line(int x0, int y0, int x1, int y1) {
  int steps = abs(x1 - x0) > abs(y1 - y0) ? abs(x1 - x0) : abs(y1 - y0);
  for (int i = 0; i <= steps; i++)
    prv_dot(x0 + (x1 - x0) * i / (steps ? steps : 1), y0 + (y1 - y0) * i / (steps ? steps : 1), 1,
            0xd5);
}

static void prv_digit(int x, int y, int w, int h, int value) {
  uint8_t on = s_segments[value];
  int g = 3, mid = y + h / 2;
  if (on & 0x01)
    prv_line(x + g, y, x + w - g, y);
  if (on & 0x02)
    prv_line(x + w, y + g, x + w, mid - g);
  if (on & 0x04)
    prv_line(x + w, mid + g, x + w, y + h - g);
  if (on & 0x08)
    prv_line(x + g, y + h, x + w - g, y + h);
  if (on & 0x10)
    prv_line(x, mid + g, x, y + h - g);
  if (on & 0x20)
    prv_line(x, y + g, x, mid - g);
  if (on & 0x40)
    prv_line(x + g, mid, x + w - g, mid);
}

static void prv_draw_digital(int cx, int cy, const struct tm *local) {
  int hour = local->tm_hour;
  if (!atomic_load(&s_clock_24h))
    hour = hour % 12 ? hour % 12 : 12;
  const int w = s_width / 9, h = w * 2, gap = w / 3, colon = w / 2;
  // No leading zero on a 12-hour clock; center what is drawn.
  bool leading = hour >= 10 || atomic_load(&s_clock_24h);
  int x = cx - (4 * w + 3 * gap + colon) / 2 - (leading ? 0 : (w + gap) / 2), y = cy - h / 2;
  if (leading)
    prv_digit(x, y, w, h, hour / 10);
  prv_digit(x + w + gap, y, w, h, hour % 10);
  int colon_x = x + 2 * w + gap + (gap + colon) / 2;
  prv_dot(colon_x, y + h / 3, 2, 0xd5);
  prv_dot(colon_x, y + h * 2 / 3, 2, 0xd5);
  x += 2 * w + 2 * gap + colon;
  prv_digit(x, y, w, h, local->tm_min / 10);
  prv_digit(x + w + gap, y, w, h, local->tm_min % 10);
}

static bool prv_clock_update(void) {
  if (!s_frame)
    return false;
  time_t now = time(NULL) + atomic_load(&s_clock_offset), minute = now / 60;
  int mode = atomic_load(&s_clock_mode);
  if (minute == s_clock_minute && s_clock && mode == s_drawn_mode)
    return false;
  if (!s_clock)
    s_clock = malloc((size_t)s_width * s_height);
  if (!s_clock)
    return false;
  s_clock_minute = minute;
  s_drawn_mode = mode;
  memset(s_clock, 0xc0, (size_t)s_width * s_height);
  struct tm local;
  gmtime_r(&now, &local);
  // Move the sparse clock a few pixels every minute; never draw a seconds hand.
  int cx = s_width / 2 + (int)(minute % 5) - 2;
  int cy = s_height / 2 + (int)((minute / 5) % 5) - 2;
  if (mode == 2) {
    prv_draw_digital(cx, cy, &local);
    return true;
  }
  int radius = (s_width < s_height ? s_width : s_height) * 3 / 8;
  for (int mark = 0; mark < 12; mark++)
    prv_hand(cx, cy, mark / 12.0, radius - (mark % 3 ? 3 : 7), radius, 0);
  prv_hand(cx, cy, ((local.tm_hour % 12) + local.tm_min / 60.0) / 12.0, 0, radius * 3 / 5, 1);
  prv_hand(cx, cy, local.tm_min / 60.0, 0, radius * 9 / 10, 0);
  prv_dot(cx, cy, 2, 0xd5);
  return true;
}

static const uint8_t *prv_visible_frame(void) {
  return s_ambient && s_clock ? s_clock : s_frame;
}

static uint32_t prv_component(unsigned value, struct fb_bitfield field) {
  if (!field.length || field.length > 8 || field.offset + field.length > 32) {
    return 0;
  }
  return ((value * ((1u << field.length) - 1) + 127) / 255) << field.offset;
}

static void prv_init(void) {
  signal(SIGUSR1, prv_capture_signal);
  // Until the firmware applies its preference: PBL_AMBIENT_CLOCK=1 analog, 2 digital.
  const char *ambient_clock = getenv("PBL_AMBIENT_CLOCK");
  atomic_store(&s_clock_mode,
               ambient_clock && (!strcmp(ambient_clock, "1") || !strcmp(ambient_clock, "2"))
                   ? ambient_clock[0] - '0'
                   : 0);
  linux_adapter_set_ambient_clock(atomic_load(&s_clock_mode));
  snprintf(s_signal_capture_path, sizeof(s_signal_capture_path), "%s/screen.ppm",
           linux_adapter_runtime_dir());
  s_foreground_next = linux_profile_get()->reclaim_delays;
  const char *wayland = getenv("PBL_WAYLAND_DISPLAY");
  if (wayland) {
    s_wayland = wayland_linux_init(wayland);
    return;
  }
  const char *device = getenv("PBL_FB_DEVICE");
  if (!device) {
    device = "/dev/fb0";
  }
  s_fd = open(device, O_RDWR | O_CLOEXEC);
  if (s_fd < 0 || ioctl(s_fd, FBIOGET_FSCREENINFO, &s_fix) < 0 ||
      ioctl(s_fd, FBIOGET_VSCREENINFO, &s_var) < 0) {
    fprintf(stderr, "fbdev %s: %s; frame capture remains available\n", device, strerror(errno));
    if (s_fd >= 0) {
      close(s_fd);
      s_fd = -1;
    }
    return;
  }
  unsigned bytes = s_var.bits_per_pixel / 8;
  uint64_t extent = (uint64_t)(s_var.yoffset + s_var.yres) * s_fix.line_length;
  if ((bytes != 2 && bytes != 4) || !s_var.xres || !s_var.yres ||
      (uint64_t)(s_var.xoffset + s_var.xres) * bytes > s_fix.line_length ||
      extent > s_fix.smem_len) {
    fprintf(stderr, "fbdev: unsupported framebuffer layout\n");
    close(s_fd);
    s_fd = -1;
    return;
  }
  void *memory = mmap(nullptr, s_fix.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, s_fd, 0);
  if (memory == MAP_FAILED) {
    fprintf(stderr, "fbdev mmap: %s\n", strerror(errno));
    close(s_fd);
    s_fd = -1;
    return;
  }
  s_memory = memory;
  if (getenv("PBL_FB_RESTORE")) {
    s_previous = malloc(s_fix.smem_len);
    if (s_previous) {
      memcpy(s_previous, s_memory, s_fix.smem_len);
    }
  }
  fprintf(stderr, "fbdev: %ux%u, %u bpp, stride %u, offset %u,%u\n", s_var.xres, s_var.yres,
          s_var.bits_per_pixel, s_fix.line_length, s_var.xoffset, s_var.yoffset);
}

// Must hold s_lock.
static void prv_log_wake_frame(void) {
  if (s_wake_at && !s_dirty) {
    fprintf(stderr, "display: wake frame committed after %llums\n",
            (unsigned long long)((posix_host_monotonic_us() - s_wake_at) / 1000));
    s_wake_at = 0;
  }
}

// Must hold s_lock. Rotate at scanout so app frames and idle clocks share the same orientation.
static const uint8_t *prv_scanout_frame(const uint8_t *fb) {
  if (!s_rotated)
    return fb;
  size_t pixels = (size_t)s_width * s_height;
  if (!s_rotated_frame)
    s_rotated_frame = malloc(pixels);
  if (!s_rotated_frame)
    return nullptr;
  for (size_t i = 0; i < pixels; i++)
    s_rotated_frame[i] = fb[pixels - 1 - i];
  return s_rotated_frame;
}

void display_linux_bottom_set_rotated(bool rotated) {
  pthread_mutex_lock(&s_lock);
  if (s_rotated != rotated) {
    s_rotated = rotated;
    s_dirty = true;
  }
  pthread_mutex_unlock(&s_lock);
  posix_host_wake();
}

// Must hold s_lock. The cached app frame stays separate from the visible ambient frame.
static bool prv_present_frame(const uint8_t *fb) {
  fb = prv_scanout_frame(fb);
  if (!fb)
    return false;
  if (s_wayland)
    return wayland_linux_update(fb, s_width, s_height);
  if (s_memory) {
    unsigned bytes = s_var.bits_per_pixel / 8;
    for (unsigned y = 0; y < s_var.yres; y++) {
      uint8_t *row = s_memory + (y + s_var.yoffset) * s_fix.line_length + s_var.xoffset * bytes;
      unsigned sy = (uint64_t)y * s_height / s_var.yres;
      for (unsigned x = 0; x < s_var.xres; x++) {
        unsigned sx = (uint64_t)x * s_width / s_var.xres;
        uint8_t c = fb[sy * s_width + sx];
        uint32_t pixel = prv_component(((c >> 4) & 3) * 85, s_var.red) |
                         prv_component(((c >> 2) & 3) * 85, s_var.green) |
                         prv_component((c & 3) * 85, s_var.blue) | prv_component(255, s_var.transp);
        memcpy(row + x * bytes, &pixel, bytes);
      }
    }
    if (ioctl(s_fd, FBIOPAN_DISPLAY, &s_var) < 0 && !s_pan_error) {
      fprintf(stderr, "fbdev pan: %s\n", strerror(errno));
      s_pan_error = true;
    }
  }
  return true;
}

void display_linux_bottom_update(const uint8_t *fb, int width, int height) {
  pthread_mutex_lock(&s_lock);
  if (!s_frame) {
    s_frame = malloc((size_t)width * height);
    if (!s_frame) {
      pthread_mutex_unlock(&s_lock);
      return;
    }
    s_width = width;
    s_height = height;
  }
  if (width != s_width || height != s_height) {
    pthread_mutex_unlock(&s_lock);
    return;
  }
  memcpy(s_frame, fb, (size_t)width * height);
  atomic_fetch_add(&s_frame_count, 1);
  if (!s_ambient) {
    s_dirty = !prv_present_frame(fb);
    prv_log_wake_frame();
  }
  pthread_mutex_unlock(&s_lock);
}

// Must hold s_lock.
static void prv_write_frame(const char *path) {
  const uint8_t *fb = prv_scanout_frame(prv_visible_frame());
  if (!fb)
    return;
  FILE *file = fopen(path, "wb");
  if (!file) {
    fprintf(stderr, "frame capture %s: %s\n", path, strerror(errno));
    return;
  }
  fprintf(file, "P6\n%d %d\n255\n", s_width, s_height);
  for (int i = 0; i < s_width * s_height; i++) {
    unsigned char c = fb[i];
    unsigned char rgb[] = {((c >> 4) & 3) * 85, ((c >> 2) & 3) * 85, (c & 3) * 85};
    fwrite(rgb, sizeof(rgb), 1, file);
  }
  fclose(file);
}

static void prv_exit(void) {
  pthread_mutex_lock(&s_lock);
  if (s_capture && s_frame) {
    prv_write_frame(s_capture);
  }
  if (s_memory) {
    if (s_previous) {
      memcpy(s_memory, s_previous, s_fix.smem_len);
      ioctl(s_fd, FBIOPAN_DISPLAY, &s_var);
    }
    munmap(s_memory, s_fix.smem_len);
  }
  if (s_fd >= 0) {
    close(s_fd);
  }
  free(s_frame);
  free(s_rotated_frame);
  free(s_clock);
  free(s_previous);
  if (s_wayland)
    wayland_linux_exit();
  pthread_mutex_unlock(&s_lock);
}

static void prv_capture(const char *value) {
  s_capture = value;
}

static void prv_poll(void) {
  pthread_mutex_lock(&s_lock);
  if (s_capture_requested && s_frame) {
    s_capture_requested = 0;
    prv_write_frame(s_signal_capture_path);
    fprintf(stderr, "display: frame saved to %s\n", s_signal_capture_path);
  }
  uint64_t now = posix_host_monotonic_us();
  int clock_mode = atomic_load(&s_clock_mode);
  bool clock_on = clock_mode != 0;
  if (clock_mode != s_drawn_clock_mode) {
    // Switching the idle clock on or off while idle redraws at once.
    if (s_ambient) {
      s_dirty = true;
      if (!clock_on && s_frame) {
        uint8_t *black = calloc((size_t)s_width * s_height, 1);
        if (black) {
          prv_present_frame(black);
          free(black);
        }
      }
    }
    s_drawn_clock_mode = clock_mode;
  }
  {
    // An adapter that is absent, or cannot power the display, leaves Pebble interactive.
    unsigned actual = linux_adapter_display_actual();
    bool enabled = actual == PBL_ADAPTER_DISPLAY_OFF || actual == PBL_ADAPTER_DISPLAY_AMBIENT_CLOCK;
    if (s_ambient != enabled) {
      s_ambient = enabled;
      atomic_store(&s_interactive, !enabled);
      s_wake_app_pending = !enabled;
      if (!enabled) {
        s_foreground_base = now;
        s_foreground_next = 0;
      }
      s_wake_at = enabled ? 0 : now;
      if (!enabled && s_wayland)
        wayland_linux_activate();
      // The vendor stack may keep the panel lit for a few seconds after an off request: show
      // black (unlit on OLED) at once instead of the last face.
      if (enabled && !clock_on && s_frame) {
        uint8_t *black = calloc((size_t)s_width * s_height, 1);
        if (black) {
          prv_present_frame(black);
          free(black);
        }
      }
      s_clock_minute = -1;
      s_dirty = true;
      fprintf(stderr, "display: %s\n",
              enabled ? (clock_mode == 2 ? "ambient digital clock"
                         : clock_on      ? "ambient analog clock"
                                         : "off")
                      : "PebbleOS interactive");
    }
  }
  if (s_foreground_next < linux_profile_get()->reclaim_delays &&
      now >= s_foreground_base +
                 (uint64_t)linux_profile_get()->reclaim_delays_ms[s_foreground_next] * 1000u) {
    s_foreground_next++;
    if (s_wayland && !s_ambient && wayland_linux_obscured()) {
      fprintf(stderr, "display: reclaiming foreground from the vendor UI\n");
      wayland_linux_activate();
    }
  }
  // Clock time advances independently of watchface redraws and sensor updates.
  if (s_ambient && clock_on && prv_clock_update())
    s_dirty = true;
  if (s_wayland) {
    wayland_linux_poll();
    if (wayland_linux_take_resume_redraw()) {
      s_dirty = true;
    }
  }
  if (s_dirty && s_frame && (!s_ambient || (clock_on && s_clock))) {
    s_dirty = !prv_present_frame(prv_visible_frame());
    prv_log_wake_frame();
  }
  pthread_mutex_unlock(&s_lock);
  // Never enter the kernel while holding the display lock: app flushes take it too.
  if (s_wake_app_pending)
    s_wake_app_pending = !display_linux_request_redraw();
}

static int prv_poll_fd(unsigned index) {
  return index == 0 ? wayland_linux_fd() : -1;
}

void display_linux_recheck_foreground(void) {
  pthread_mutex_lock(&s_lock);
  s_foreground_base = posix_host_monotonic_us();
  s_foreground_next = 0;
  pthread_mutex_unlock(&s_lock);
}

static unsigned prv_poll_timeout_ms(void) {
  if (s_wake_app_pending)
    return 10;
  if (s_dirty && (!s_ambient || atomic_load(&s_clock_mode)))
    return 10;
  unsigned timeout = 60000;
  if (s_ambient && atomic_load(&s_clock_mode)) {
    int64_t seconds = (int64_t)time(NULL) + atomic_load(&s_clock_offset);
    unsigned elapsed = (unsigned)((seconds % 60 + 60) % 60);
    timeout = (60 - elapsed) * 1000;
  }
  if (!s_wayland)
    return timeout;
  // Adapter changes wake through the client; the clock and foreground recheck have deadlines.
  const struct linux_profile *profile = linux_profile_get();
  if (s_foreground_next >= profile->reclaim_delays)
    return timeout;
  uint64_t now = posix_host_monotonic_us();
  uint64_t recheck =
      s_foreground_base + (uint64_t)profile->reclaim_delays_ms[s_foreground_next] * 1000u;
  unsigned foreground = now >= recheck ? 0 : (unsigned)((recheck - now + 999) / 1000);
  return foreground < timeout ? foreground : timeout;
}

POSIX_HOST_HOOK(.init = prv_init, .poll = prv_poll, .exit = prv_exit, .poll_fd = prv_poll_fd,
                .poll_timeout_ms = prv_poll_timeout_ms)
POSIX_HOST_OPTION(.flag = 'S', .arg = "out.ppm", .help = "save the final Pebble frame",
                  .set = prv_capture)
