/* SPDX-License-Identifier: Apache-2.0 */

#include <stdbool.h>

#include <pbl/drivers/linux_adapter.h>

#include <poll.h>

static int test_send(int fd, const struct sockaddr_un *peer, const void *message, size_t size);
static int test_poll(struct pollfd *fds, nfds_t count, int timeout);
static ssize_t test_recv(int fd, void *out, size_t capacity, int flags);
static int test_clock_gettime(clockid_t clock, struct timespec *now);
static int test_printf(const char *format, ...) {
  (void)format;
  return 0;
}

#define main             conformance_cli_main
#define pbl_adapter_send test_send
#define poll             test_poll
#define recv             test_recv
#define clock_gettime    test_clock_gettime
#define printf           test_printf
#include "conformance.c"
#undef main
#undef pbl_adapter_send
#undef poll
#undef recv
#undef clock_gettime
#undef printf

static struct pbl_adapter_accel_config s_config;
static struct pbl_adapter_sensor s_sensor;
static bool s_pending;
static bool s_then_accel;

static int test_clock_gettime(clockid_t clock, struct timespec *now) {
  static uint64_t ms;
  (void)clock;
  ms += 10;
  now->tv_sec = ms / 1000;
  now->tv_nsec = (ms % 1000) * 1000000;
  return 0;
}

static int test_send(int fd, const struct sockaddr_un *peer, const void *message, size_t size) {
  (void)fd;
  (void)peer;
  if (size == sizeof(s_config)) {
    struct pbl_adapter_accel_config config;
    memcpy(&config, message, sizeof(config));
    if (config.hdr.type == PBL_ADAPTER_ACCEL_CONFIG && config.samples) {
      s_config = config;
      s_pending = true;
    }
  }
  return 0;
}

static int test_poll(struct pollfd *fds, nfds_t count, int timeout) {
  (void)fds;
  (void)count;
  (void)timeout;
  return s_pending;
}

static ssize_t test_recv(int fd, void *out, size_t capacity, int flags) {
  (void)fd;
  (void)flags;
  if (!s_pending) {
    errno = EAGAIN;
    return -1;
  }
  if (capacity < s_sensor.hdr.length)
    abort();
  s_pending = false;
  memcpy(out, &s_sensor, sizeof(s_sensor));
  memset((char *)out + sizeof(s_sensor), 0, s_sensor.hdr.length - sizeof(s_sensor));
  if (s_then_accel) {
    s_then_accel = false;
    s_pending = true;
    s_sensor.type = PBL_ADAPTER_SENSOR_ACCEL;
  }
  return s_sensor.hdr.length;
}

static bool run_sensor_check(unsigned type, bool extended) {
  s_failed = s_passed = s_skipped = 0;
  memset(&s_config, 0, sizeof(s_config));
  pbl_adapter_init(&s_sensor, PBL_ADAPTER_SENSOR, sizeof(s_sensor), 1);
  s_sensor.type = type;
  s_sensor.count = 1;
  s_sensor.interval_us = 10000;
  s_sensor.timestamp_us = pbl_adapter_boottime_us();
  if (extended)
    s_sensor.hdr.length += 8;
  check_sensors();
  return s_failed == 0;
}

int main(void) {
  s_caps.caps = PBL_ADAPTER_CAP_ACCEL;
  s_caps.accel_min_interval_us = 10000;
  s_caps.accel_max_interval_us = 20000;
  s_caps.accel_max_samples = 4;
  bool limits = run_sensor_check(PBL_ADAPTER_SENSOR_ACCEL, false) &&
                s_config.samples <= s_caps.accel_max_samples &&
                s_config.interval_us >= s_caps.accel_min_interval_us &&
                s_config.interval_us <= s_caps.accel_max_interval_us;
  bool steps = !run_sensor_check(PBL_ADAPTER_SENSOR_STEPS, false);
  bool extended = run_sensor_check(PBL_ADAPTER_SENSOR_ACCEL, true);
  s_then_accel = true;
  bool interleaved = run_sensor_check(PBL_ADAPTER_SENSOR_STEPS, false);
  printf("%s  conformance respects declared accelerometer limits\n", limits ? "PASS" : "FAIL");
  printf("%s  conformance rejects steps as an accelerometer stream\n", steps ? "PASS" : "FAIL");
  printf("%s  conformance accepts appended sensor fields\n", extended ? "PASS" : "FAIL");
  printf("%s  conformance tolerates steps before accelerometer data\n",
         interleaved ? "PASS" : "FAIL");
  return !(limits && steps && extended && interleaved);
}
