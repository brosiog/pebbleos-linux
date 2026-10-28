/* SPDX-License-Identifier: Apache-2.0 */

/* The OS-side adapter client against the mock adapter. Each case runs in its own process, because
 * the client keeps its state in globals. */

#include "mock_adapter.h"

#include <signal.h>

#include <pbl/drivers/linux_adapter_client.h>
#include <pbl/drivers/linux_profile.h>

#include <sys/wait.h>

void linux_adapter_test_set_silence_us(uint64_t limit_us);

#define CHECK(test)                                                     \
  do {                                                                  \
    if (!(test)) {                                                      \
      fprintf(stderr, "  CHECK FAILED line %d: %s\n", __LINE__, #test); \
      _exit(1);                                                         \
    }                                                                   \
  } while (0)

#define ALL_CAPS 0x3ffu

static char s_dir[128];
static struct mock *s_mock;

// Sink state.
static unsigned s_sensor_count, s_gesture_count, s_last_gesture;
static struct pbl_adapter_sensor s_last_sensor;

void linux_watch_sensor_received(const struct pbl_adapter_sensor *packet) {
  s_sensor_count++;
  s_last_sensor = *packet;
}
void linux_watch_gesture_received(unsigned kind) {
  s_gesture_count++;
  s_last_gesture = kind;
}

static uint64_t prv_wall_ms(void) {
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

// Polls for `ms` of wall time.
static void pump(unsigned ms) {
  uint64_t end = prv_wall_ms() + ms;
  do {
    linux_adapter_poll();
    usleep(2000);
  } while (prv_wall_ms() < end);
}

#define PUMP_UNTIL(cond, ms)                \
  do {                                      \
    uint64_t end_ = prv_wall_ms() + (ms);   \
    while (!(cond) && prv_wall_ms() < end_) \
      pump(4);                              \
  } while (0)

static void start(uint32_t caps, unsigned latency_ms, bool silent, bool fast_path) {
  snprintf(s_dir, sizeof(s_dir), "/tmp/pbl-adapter-test-%d", (int)getpid());
  linux_adapter_set_runtime_dir(s_dir);
  struct mock_options options = {
    .dir = s_dir,
    .caps = caps,
    .display_latency_ms = latency_ms,
    .silent = silent,
    .fast_path = fast_path,
    .heartbeat_ms = 20
  };
  CHECK(mock_start(&options, &s_mock) == 0);
  linux_adapter_init();
}

static void finish(void) {
  linux_adapter_exit();
  if (s_mock)
    mock_stop(s_mock);
  char command[200];
  snprintf(command, sizeof(command), "rm -rf %s", s_dir);
  (void)!system(command);
}

static void test_no_adapter(void) {
  snprintf(s_dir, sizeof(s_dir), "/tmp/pbl-adapter-test-%d", (int)getpid());
  linux_adapter_set_runtime_dir(s_dir);
  linux_adapter_init();
  pump(50);
  CHECK(!linux_adapter_online());
  CHECK(!linux_adapter_has(PBL_ADAPTER_CAP_VIBE));
  CHECK(linux_adapter_display_actual() == 0);
  unsigned lux;
  CHECK(linux_adapter_lux(&lux) == -EAGAIN);
  CHECK(linux_adapter_vibe(50, 100) == -ENOTCONN);
  CHECK(linux_adapter_backlight(50) == -ENOTCONN);
  CHECK(linux_adapter_display_idle(PBL_ADAPTER_REASON_TIMEOUT) == -ENOTCONN);
  linux_adapter_set_accel(25, 40000);
  linux_adapter_idle(true, 0);
  finish();
}

static void test_handshake_and_config(void) {
  start(ALL_CAPS, 0, false, false);
  linux_adapter_set_accel(25, 40000);
  linux_adapter_set_hrm(true, false);
  PUMP_UNTIL(linux_adapter_online(), 500);
  CHECK(linux_adapter_online());
  CHECK(mock_count(s_mock, PBL_ADAPTER_HELLO) >= 1);
  struct pbl_adapter_hello hello;
  CHECK(mock_last(s_mock, PBL_ADAPTER_HELLO, &hello, sizeof(hello)));
  CHECK(hello.hdr.major == PBL_ADAPTER_MAJOR && hello.max_minor == PBL_ADAPTER_MINOR);
  // Config cached before the handshake is sent once the capabilities arrive.
  CHECK(mock_wait_count(s_mock, PBL_ADAPTER_ACCEL_CONFIG, 1, 500));
  CHECK(mock_wait_count(s_mock, PBL_ADAPTER_HRM_CONFIG, 1, 500));
  struct pbl_adapter_accel_config accel;
  CHECK(mock_last(s_mock, PBL_ADAPTER_ACCEL_CONFIG, &accel, sizeof(accel)));
  CHECK(accel.samples == 25 && accel.interval_us == 40000);
  struct pbl_adapter_hrm_config hrm;
  CHECK(mock_last(s_mock, PBL_ADAPTER_HRM_CONFIG, &hrm, sizeof(hrm)));
  CHECK(hrm.mode == PBL_ADAPTER_HRM_BPM);
  finish();
}

static void test_missing_capabilities(void) {
  start(PBL_ADAPTER_CAP_ACCEL, 0, false, false);
  PUMP_UNTIL(linux_adapter_online(), 500);
  CHECK(linux_adapter_online());
  CHECK(!linux_adapter_has(PBL_ADAPTER_CAP_VIBE));
  CHECK(linux_adapter_vibe(80, 100) == -ENOSYS);
  CHECK(linux_adapter_backlight(40) == -ENOSYS);
  CHECK(linux_adapter_display(PBL_ADAPTER_DISPLAY_OFF, PBL_ADAPTER_REASON_TIMEOUT) == -ENOSYS);
  // With no display capability the OS must read the panel as interactive.
  CHECK(linux_adapter_display_actual() == 0);
  linux_adapter_set_hrm(true, false);
  unsigned lux;
  CHECK(linux_adapter_lux(&lux) == -ENODEV);
  pump(40);
  CHECK(mock_count(s_mock, PBL_ADAPTER_VIBE) == 0);
  CHECK(mock_count(s_mock, PBL_ADAPTER_BACKLIGHT) == 0);
  CHECK(mock_count(s_mock, PBL_ADAPTER_DISPLAY) == 0);
  CHECK(mock_count(s_mock, PBL_ADAPTER_HRM_CONFIG) == 0);
  finish();
}

static void test_vibe_deadline(void) {
  start(ALL_CAPS, 0, false, false);
  PUMP_UNTIL(linux_adapter_online(), 500);
  CHECK(linux_adapter_vibe(80, 60) == 0);
  CHECK(mock_wait_count(s_mock, PBL_ADAPTER_VIBE, 1, 500));
  struct pbl_adapter_vibe vibe;
  CHECK(mock_last(s_mock, PBL_ADAPTER_VIBE, &vibe, sizeof(vibe)));
  CHECK(vibe.strength == 80 && vibe.duration_ms == 60);
  uint64_t now_ms = pbl_adapter_boottime_us() / 1000;
  CHECK(pbl_adapter_vibe_remaining_ms(&vibe, now_ms, 5000) > 0);
  // Replayed after a stall, the same message does nothing.
  CHECK(pbl_adapter_vibe_remaining_ms(&vibe, now_ms + 200, 5000) == 0);
  // Durations are clamped to what the adapter declared.
  CHECK(linux_adapter_vibe(100, 60000) == 0);
  CHECK(mock_wait_count(s_mock, PBL_ADAPTER_VIBE, 2, 500));
  CHECK(mock_last(s_mock, PBL_ADAPTER_VIBE, &vibe, sizeof(vibe)));
  CHECK(vibe.duration_ms == PBL_ADAPTER_VIBE_MAX_MS);
  // A stop carries no strength.
  CHECK(linux_adapter_vibe(0, 0) == 0);
  CHECK(mock_wait_count(s_mock, PBL_ADAPTER_VIBE, 3, 500));
  CHECK(mock_last(s_mock, PBL_ADAPTER_VIBE, &vibe, sizeof(vibe)));
  CHECK(vibe.strength == 0 && vibe.duration_ms == 0);
  finish();
}

static void test_display_flow(void) {
  start(ALL_CAPS, 10, false, false);
  PUMP_UNTIL(linux_adapter_online(), 500);
  CHECK(linux_adapter_backlight(70) == 0);
  CHECK(linux_adapter_display(PBL_ADAPTER_DISPLAY_ON, PBL_ADAPTER_REASON_USER) == 0);
  // Repeating the same request does not resend it.
  CHECK(linux_adapter_display(PBL_ADAPTER_DISPLAY_ON, PBL_ADAPTER_REASON_USER) == 0);
  CHECK(mock_wait_count(s_mock, PBL_ADAPTER_DISPLAY, 1, 500));
  pump(30);
  CHECK(mock_count(s_mock, PBL_ADAPTER_DISPLAY) == 1);
  struct pbl_adapter_backlight backlight;
  CHECK(mock_last(s_mock, PBL_ADAPTER_BACKLIGHT, &backlight, sizeof(backlight)));
  CHECK(backlight.level == 70);

  // Idle with the clock off goes dark; STATE reports it after the adapter's latency.
  CHECK(linux_adapter_display_idle(PBL_ADAPTER_REASON_TIMEOUT) == 0);
  PUMP_UNTIL(linux_adapter_display_actual() == PBL_ADAPTER_DISPLAY_OFF, 500);
  CHECK(linux_adapter_display_actual() == PBL_ADAPTER_DISPLAY_OFF);
  struct pbl_adapter_display display;
  CHECK(mock_last(s_mock, PBL_ADAPTER_DISPLAY, &display, sizeof(display)));
  CHECK(display.mode == PBL_ADAPTER_DISPLAY_OFF && display.reason == PBL_ADAPTER_REASON_TIMEOUT);

  // Turning the clock on while idle switches mode without another timeout.
  linux_adapter_set_ambient_clock(2);
  PUMP_UNTIL(linux_adapter_display_actual() == PBL_ADAPTER_DISPLAY_AMBIENT_CLOCK, 500);
  CHECK(linux_adapter_display_actual() == PBL_ADAPTER_DISPLAY_AMBIENT_CLOCK);

  // Input reports activity and asks for the panel.
  CHECK(linux_adapter_wake(PBL_ADAPTER_REASON_USER, PBL_ADAPTER_ACTIVITY_TOUCH) == 0);
  CHECK(mock_wait_count(s_mock, PBL_ADAPTER_USER_ACTIVITY, 1, 500));
  PUMP_UNTIL(linux_adapter_display_actual() == PBL_ADAPTER_DISPLAY_ON, 500);
  CHECK(linux_adapter_display_actual() == PBL_ADAPTER_DISPLAY_ON);
  finish();
}

static void test_vendor_blank_then_wake(void) {
  start(ALL_CAPS, 0, false, false);
  PUMP_UNTIL(linux_adapter_online(), 500);
  CHECK(linux_adapter_display(PBL_ADAPTER_DISPLAY_ON, PBL_ADAPTER_REASON_USER) == 0);
  CHECK(mock_wait_count(s_mock, PBL_ADAPTER_DISPLAY, 1, 500));
  pump(60);
  unsigned before = mock_count(s_mock, PBL_ADAPTER_DISPLAY);
  // The vendor stack blanks the panel; the OS still believes it asked for ON.
  mock_send_gesture(s_mock, PBL_ADAPTER_GESTURE_EXTERNAL_OFF);
  PUMP_UNTIL(linux_adapter_display_actual() == PBL_ADAPTER_DISPLAY_OFF, 500);
  CHECK(linux_adapter_display_actual() == PBL_ADAPTER_DISPLAY_OFF);
  CHECK(s_gesture_count == 1 && s_last_gesture == PBL_ADAPTER_GESTURE_EXTERNAL_OFF);
  // The next wake resends ON even though the requested mode did not change.
  CHECK(linux_adapter_wake(PBL_ADAPTER_REASON_USER, 0) == 0);
  CHECK(mock_wait_count(s_mock, PBL_ADAPTER_DISPLAY, before + 1, 500));
  PUMP_UNTIL(linux_adapter_display_actual() == PBL_ADAPTER_DISPLAY_ON, 500);
  CHECK(linux_adapter_display_actual() == PBL_ADAPTER_DISPLAY_ON);
  finish();
}

static void test_input_validation(void) {
  start(ALL_CAPS, 0, false, false);
  PUMP_UNTIL(linux_adapter_online(), 500);
  struct pbl_adapter_sensor sensor;

  // Wrong magic, wrong major, short datagram, unknown type: all dropped.
  pbl_adapter_init(&sensor, PBL_ADAPTER_SENSOR, sizeof(sensor), 1);
  sensor.type = PBL_ADAPTER_SENSOR_ACCEL;
  sensor.count = 1;
  sensor.interval_us = 40000;
  struct pbl_adapter_sensor bad = sensor;
  bad.hdr.magic = 0x1234;
  mock_send_raw(s_mock, &bad, sizeof(bad));
  bad = sensor;
  bad.hdr.major = PBL_ADAPTER_MAJOR + 1;
  mock_send_raw(s_mock, &bad, sizeof(bad));
  mock_send_raw(s_mock, &sensor, sizeof(sensor) - 10);
  struct pbl_adapter_hdr unknown;
  pbl_adapter_init(&unknown, 0x00ee, sizeof(unknown), 1);
  mock_send_raw(s_mock, &unknown, sizeof(unknown));
  mock_send_raw(s_mock, "x", 1);
  // Out-of-range contents.
  bad = sensor;
  bad.count = 0;
  mock_send_raw(s_mock, &bad, sizeof(bad));
  bad = sensor;
  bad.count = PBL_ADAPTER_SENSOR_SAMPLES + 1;
  mock_send_raw(s_mock, &bad, sizeof(bad));
  bad = sensor;
  bad.interval_us = 10;
  mock_send_raw(s_mock, &bad, sizeof(bad));
  bad = sensor;
  bad.type = 9;
  mock_send_raw(s_mock, &bad, sizeof(bad));
  pump(60);
  CHECK(s_sensor_count == 0);
  CHECK(linux_adapter_online());

  // A valid packet, with trailing bytes from a newer minor version, is accepted.
  uint8_t extended[sizeof(sensor) + 8];
  memset(extended, 0xaa, sizeof(extended));
  sensor.hdr.minor = 7;
  memcpy(extended, &sensor, sizeof(sensor));
  mock_send_raw(s_mock, extended, sizeof(extended));
  PUMP_UNTIL(s_sensor_count == 1, 500);
  CHECK(s_sensor_count == 1);
  CHECK(s_last_sensor.type == PBL_ADAPTER_SENSOR_ACCEL && s_last_sensor.count == 1);

  mock_send_sensor(s_mock, PBL_ADAPTER_SENSOR_HRM, 1, 1000000, 72, 3);
  PUMP_UNTIL(s_sensor_count == 2, 500);
  CHECK(s_sensor_count == 2);
  CHECK(s_last_sensor.values[0][0] == 72 && s_last_sensor.values[0][1] == 3);
  finish();
}

static void test_data_ignored_before_handshake(void) {
  start(ALL_CAPS, 0, true, false);
  mock_send_sensor(s_mock, PBL_ADAPTER_SENSOR_ACCEL, 4, 40000, 10, 20);
  mock_send_gesture(s_mock, PBL_ADAPTER_GESTURE_WRIST_UP);
  pump(60);
  CHECK(!linux_adapter_online());
  CHECK(s_sensor_count == 0 && s_gesture_count == 0);
  finish();
}

static void test_gestures_and_faults(void) {
  start(ALL_CAPS, 0, false, true);
  PUMP_UNTIL(linux_adapter_online(), 500);
  mock_send_gesture(s_mock, PBL_ADAPTER_GESTURE_WRIST_UP);
  PUMP_UNTIL(s_gesture_count == 1, 500);
  CHECK(s_last_gesture == PBL_ADAPTER_GESTURE_WRIST_UP);
  mock_send_gesture(s_mock, 99);
  pump(40);
  CHECK(s_gesture_count == 1);
  CHECK(linux_adapter_fault_state(PBL_ADAPTER_COMPONENT_ACCEL) == PBL_ADAPTER_FAULT_OK);
  mock_send_fault(s_mock, PBL_ADAPTER_COMPONENT_ACCEL, PBL_ADAPTER_FAULT_RECOVERING);
  PUMP_UNTIL(linux_adapter_fault_state(PBL_ADAPTER_COMPONENT_ACCEL) != PBL_ADAPTER_FAULT_OK, 500);
  CHECK(linux_adapter_fault_state(PBL_ADAPTER_COMPONENT_ACCEL) == PBL_ADAPTER_FAULT_RECOVERING);
  mock_send_fault(s_mock, PBL_ADAPTER_COMPONENT_ACCEL, PBL_ADAPTER_FAULT_OK);
  PUMP_UNTIL(linux_adapter_fault_state(PBL_ADAPTER_COMPONENT_ACCEL) == PBL_ADAPTER_FAULT_OK, 500);
  CHECK(linux_adapter_fault_state(PBL_ADAPTER_COMPONENT_ACCEL) == PBL_ADAPTER_FAULT_OK);
  mock_set_lux(s_mock, 321, true);
  unsigned lux = 0;
  PUMP_UNTIL(linux_adapter_lux(&lux) == 0 && lux == 321, 500);
  CHECK(linux_adapter_lux(&lux) == 0 && lux == 321);
  mock_set_lux(s_mock, 0, false);
  PUMP_UNTIL(linux_adapter_lux(&lux) == -ENODATA, 500);
  CHECK(linux_adapter_lux(&lux) == -ENODATA);
  finish();
}

static void test_adapter_restart(void) {
  start(ALL_CAPS, 0, false, false);
  linux_adapter_set_accel(25, 40000);
  PUMP_UNTIL(linux_adapter_online(), 500);
  CHECK(mock_wait_count(s_mock, PBL_ADAPTER_ACCEL_CONFIG, 1, 500));
  unsigned hellos = mock_count(s_mock, PBL_ADAPTER_HELLO);
  mock_restart(s_mock);
  // READY drops the link, then the OS says HELLO again and resends its config.
  PUMP_UNTIL(mock_count(s_mock, PBL_ADAPTER_HELLO) > hellos, 500);
  CHECK(mock_count(s_mock, PBL_ADAPTER_HELLO) > hellos);
  PUMP_UNTIL(mock_count(s_mock, PBL_ADAPTER_ACCEL_CONFIG) >= 2, 500);
  CHECK(mock_count(s_mock, PBL_ADAPTER_ACCEL_CONFIG) >= 2);
  CHECK(linux_adapter_online());
  finish();
}

static void test_silent_adapter_backoff(void) {
  start(ALL_CAPS, 0, true, false);
  pump(2600);
  unsigned hellos = mock_count(s_mock, PBL_ADAPTER_HELLO);
  // At start, +1 s, +3 s: two or three by 2.6 s, never a flood.
  if (hellos != 2)
    fprintf(stderr, "  hellos=%u\n", hellos);
  CHECK(hellos == 2);
  CHECK(!linux_adapter_online());
  finish();
}

static void test_silence_drops_link(void) {
  start(ALL_CAPS, 0, false, false);
  linux_adapter_test_set_silence_us(150000);
  PUMP_UNTIL(linux_adapter_online(), 500);
  CHECK(linux_adapter_online());
  mock_stop(s_mock);
  s_mock = NULL;
  PUMP_UNTIL(!linux_adapter_online(), 1000);
  CHECK(!linux_adapter_online());
  CHECK(linux_adapter_display_actual() == 0);
  finish();
}

static void test_suspend_contract(void) {
  start(ALL_CAPS, 0, false, false);
  PUMP_UNTIL(linux_adapter_online(), 500);
  linux_adapter_idle(false, 5000000);
  CHECK(mock_wait_count(s_mock, PBL_ADAPTER_IDLE, 1, 500));
  struct pbl_adapter_idle idle;
  CHECK(mock_last(s_mock, PBL_ADAPTER_IDLE, &idle, sizeof(idle)));
  CHECK(!idle.busy && idle.next_deadline_us == 5000000);
  // Repeating the same state does not resend it.
  linux_adapter_idle(false, 5000000);
  pump(20);
  CHECK(mock_count(s_mock, PBL_ADAPTER_IDLE) == 1);
  linux_adapter_idle(true, 0);
  CHECK(mock_wait_count(s_mock, PBL_ADAPTER_IDLE, 2, 500));
  CHECK(mock_last(s_mock, PBL_ADAPTER_IDLE, &idle, sizeof(idle)));
  CHECK(idle.busy && idle.next_deadline_us == 0);
  finish();
}

static void test_runtime_dir_resolution(void) {
  char out[128];
  setenv("PBL_RUNTIME_DIR", "/tmp/env-dir", 1);
  setenv("XDG_RUNTIME_DIR", "/tmp/xdg", 1);
  CHECK(!pbl_adapter_runtime_dir(out, sizeof(out), "/tmp/flag-dir", "/tmp/profile"));
  CHECK(!strcmp(out, "/tmp/flag-dir"));
  CHECK(!pbl_adapter_runtime_dir(out, sizeof(out), NULL, "/tmp/profile"));
  CHECK(!strcmp(out, "/tmp/env-dir"));
  unsetenv("PBL_RUNTIME_DIR");
  CHECK(!pbl_adapter_runtime_dir(out, sizeof(out), NULL, "/tmp/profile"));
  CHECK(!strcmp(out, "/tmp/xdg/pebble"));
  unsetenv("XDG_RUNTIME_DIR");
  CHECK(!pbl_adapter_runtime_dir(out, sizeof(out), NULL, "/tmp/profile"));
  CHECK(!strcmp(out, "/tmp/profile"));
  CHECK(pbl_adapter_runtime_dir(out, sizeof(out), NULL, NULL) == -ENOENT);
  char tiny[8];
  CHECK(pbl_adapter_runtime_dir(tiny, sizeof(tiny), "/tmp/too-long-for-it", NULL) == -ENAMETOOLONG);
  struct sockaddr_un address;
  char long_dir[128];
  memset(long_dir, 'a', sizeof(long_dir));
  long_dir[0] = '/';
  long_dir[120] = 0;
  CHECK(pbl_adapter_socket_address(&address, long_dir, "os.sock") == -ENAMETOOLONG);
}

static void test_live_endpoint_refused(void) {
  snprintf(s_dir, sizeof(s_dir), "/tmp/pbl-adapter-test-%d", (int)getpid());
  mkdir(s_dir, 0700);
  int first = pbl_adapter_bind(s_dir, PBL_ADAPTER_OS_SOCK);
  CHECK(first >= 0);
  CHECK(pbl_adapter_bind(s_dir, PBL_ADAPTER_OS_SOCK) == -EADDRINUSE);
  close(first);
  // The file is now stale: nothing listens, so it is replaced.
  int second = pbl_adapter_bind(s_dir, PBL_ADAPTER_OS_SOCK);
  CHECK(second >= 0);
  close(second);
  char command[200];
  snprintf(command, sizeof(command), "rm -rf %s", s_dir);
  (void)!system(command);
}

static void test_profile(void) {
  const char *path = "/tmp/pbl-profile-test.conf";
  FILE *file = fopen(path, "w");
  CHECK(file);
  fputs(
      "# test device\n"
      "runtime_dir = /run/pebble-test\n"
      "input.keys = *side-keys, crown\n"
      "input.touch = ts0\n"
      "input.bezel =\n"
      "foreground_reclaim_ms = 100, 250\n"
      "bogus = 1\n",
      file);
  fclose(file);
  linux_profile_set_file(path);
  const struct linux_profile *p = linux_profile_get();
  CHECK(!strcmp(p->runtime_dir, "/run/pebble-test"));
  CHECK(linux_profile_match(p->keys, "gpio-side-keys-1"));
  CHECK(linux_profile_match(p->keys, "crown"));
  CHECK(!linux_profile_match(p->keys, "crown2"));
  CHECK(linux_profile_match(p->touch, "ts0"));
  CHECK(!linux_profile_match(p->bezel, "tizen_detent"));
  CHECK(p->reclaim_delays == 2 && p->reclaim_delays_ms[1] == 250);
  unlink(path);
  linux_profile_set_file(NULL);
  p = linux_profile_get();
  CHECK(linux_profile_match(p->bezel, "tizen_detent"));
  CHECK(linux_profile_match(p->keys, "s2mpw02-power-keys"));
  CHECK(p->reclaim_delays == 2 && p->reclaim_delays_ms[0] == 700);
}

struct testcase {
  const char *name;
  void (*run)(void);
};

static const struct testcase s_cases[] = {
  {"no adapter present", test_no_adapter},
  {"handshake and cached config", test_handshake_and_config},
  {"missing capabilities", test_missing_capabilities},
  {"vibe deadline and clamp", test_vibe_deadline},
  {"display flow", test_display_flow},
  {"vendor blank then wake", test_vendor_blank_then_wake},
  {"malformed and out-of-range input", test_input_validation},
  {"data ignored before handshake", test_data_ignored_before_handshake},
  {"gestures, faults, light", test_gestures_and_faults},
  {"adapter restart", test_adapter_restart},
  {"silent adapter backoff", test_silent_adapter_backoff},
  {"silence drops the link", test_silence_drops_link},
  {"suspend contract", test_suspend_contract},
  {"runtime directory resolution", test_runtime_dir_resolution},
  {"live endpoint refused", test_live_endpoint_refused},
  {"device profile", test_profile},
};

int main(void) {
  signal(SIGPIPE, SIG_IGN);
  unsigned failed = 0;
  for (unsigned i = 0; i < sizeof(s_cases) / sizeof(*s_cases); i++) {
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
      alarm(20);
      s_cases[i].run();
      _exit(0);
    }
    int status;
    waitpid(pid, &status, 0);
    bool ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    printf("%s  %s\n", ok ? "PASS" : "FAIL", s_cases[i].name);
    failed += !ok;
  }
  printf("%u of %zu failed\n", failed, sizeof(s_cases) / sizeof(*s_cases));
  return failed != 0;
}
