/* SPDX-License-Identifier: Apache-2.0 */
#include "battery_linux_bottom.h"

#include <stdio.h>
#include <string.h>

#include <linux/netlink.h>
#include <posix_host.h>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>

// Kernel power_supply uevents announce plug and level changes; polling is only a fallback.
#define POLL_US        15000000ULL
#define UEVENT_POLL_US 60000000ULL

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static struct linux_battery_sample s_sample = {.percent = -1};
static uint64_t s_next_read;
// Samsung shows its charging screen over Pebble on plug changes; provided by the display backend.
void display_linux_recheck_foreground(void) __attribute__((weak));
static int s_uevents = -1;

static void prv_uevents_open(void) {
  int fd = socket(AF_NETLINK, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, NETLINK_KOBJECT_UEVENT);
  struct sockaddr_nl address = {.nl_family = AF_NETLINK, .nl_groups = 1};
  if (fd >= 0 && bind(fd, (struct sockaddr *)&address, sizeof(address))) {
    close(fd);
    fd = -1;
  }
  s_uevents = fd;
  fprintf(stderr, "battery: %s\n", fd >= 0 ? "power supply uevents" : "polling");
}

// Returns whether a power_supply uevent arrived.
static bool prv_uevents_drain(void) {
  bool changed = false;
  char message[2048];
  ssize_t count;
  while (s_uevents >= 0 && (count = recv(s_uevents, message, sizeof(message) - 1, 0)) > 0) {
    message[count] = 0;
    for (char *field = message; field < message + count; field += strlen(field) + 1) {
      if (!strcmp(field, "SUBSYSTEM=power_supply"))
        changed = true;
    }
  }
  return changed;
}

static int prv_read_integer(const char *path, int *value) {
  FILE *file = fopen(path, "r");
  if (!file)
    return -1;
  int result = fscanf(file, "%d", value) == 1 ? 0 : -1;
  fclose(file);
  return result;
}

static void prv_poll(void) {
  uint64_t now = posix_host_monotonic_us();
  if (!prv_uevents_drain() && now < s_next_read)
    return;
  s_next_read = now + (s_uevents >= 0 ? UEVENT_POLL_US : POLL_US);
  struct linux_battery_sample sample = {.percent = -1};
  int voltage, percent;
  if (prv_read_integer("/sys/class/power_supply/battery/voltage_now", &voltage) ||
      prv_read_integer("/sys/class/power_supply/battery/capacity", &percent) || voltage < 2000000 ||
      voltage > 6000000 || percent < 0 || percent > 100) {
    if (!s_sample.millivolts)
      fprintf(stderr, "battery: live measurements unavailable\n");
    return;
  }
  sample.millivolts = voltage / 1000;
  sample.percent = percent;
  char status[32] = {0};
  FILE *file = fopen("/sys/class/power_supply/battery/status", "r");
  if (file) {
    if (fgets(status, sizeof(status), file)) {
      sample.charging = !strncmp(status, "Charging", 8);
      sample.full = !strncmp(status, "Full", 4);
    }
    fclose(file);
  }
  const char *sources[] = {
    "/sys/class/power_supply/usb/online", "/sys/class/power_supply/wireless/online"
  };
  for (unsigned i = 0; i < sizeof(sources) / sizeof(sources[0]); i++) {
    int online;
    if (!prv_read_integer(sources[i], &online) && online > 0)
      sample.powered = true;
  }
  pthread_mutex_lock(&s_lock);
  bool first = !s_sample.millivolts;
  bool plug_changed = !first && sample.powered != s_sample.powered;
  s_sample = sample;
  pthread_mutex_unlock(&s_lock);
  if (plug_changed && display_linux_recheck_foreground) {
    display_linux_recheck_foreground();
  }
  if (first) {
    fprintf(stderr, "battery: live %dmV %d%% charging=%d powered=%d\n", sample.millivolts,
            sample.percent, sample.charging, sample.powered);
  }
}

struct linux_battery_sample battery_linux_sample(void) {
  pthread_mutex_lock(&s_lock);
  struct linux_battery_sample sample = s_sample;
  pthread_mutex_unlock(&s_lock);
  return sample;
}

static void prv_init(void) {
  prv_uevents_open();
  prv_poll();
}

static void prv_exit(void) {
  if (s_uevents >= 0)
    close(s_uevents);
}

static int prv_poll_fd(unsigned index) {
  return index ? -1 : s_uevents;
}

static unsigned prv_poll_timeout_ms(void) {
  uint64_t now = posix_host_monotonic_us();
  return now >= s_next_read ? 0 : (unsigned)((s_next_read - now + 999) / 1000);
}

POSIX_HOST_HOOK(.init = prv_init, .poll = prv_poll, .exit = prv_exit, .poll_fd = prv_poll_fd,
                .poll_timeout_ms = prv_poll_timeout_ms)
