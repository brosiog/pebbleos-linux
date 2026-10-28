/* SPDX-License-Identifier: Apache-2.0 */

#include "mock_adapter.h"

#include <signal.h>

#include <pbl/drivers/linux_adapter_client.h>

#include <sys/wait.h>

static char s_dir[] = "/tmp/pbl-client-review.XXXXXX";
static struct mock *s_mock;
static int s_adapter_fd = -1;

#define CHECK(condition)                                                   \
  do {                                                                     \
    if (!(condition)) {                                                    \
      fprintf(stderr, "CHECK FAILED line %d: %s\n", __LINE__, #condition); \
      exit(1);                                                             \
    }                                                                      \
  } while (0)

void linux_watch_sensor_received(const struct pbl_adapter_sensor *sensor) {
  (void)sensor;
}

void linux_watch_gesture_received(unsigned kind) {
  (void)kind;
}

static void cleanup(void) {
  linux_adapter_exit();
  if (s_mock)
    mock_stop(s_mock);
  if (s_adapter_fd >= 0) {
    char path[256];
    close(s_adapter_fd);
    pbl_adapter_join(path, sizeof(path), s_dir, PBL_ADAPTER_ADAPTER_SOCK);
    unlink(path);
  }
  rmdir(s_dir);
}

static uint64_t now_ms(void) {
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

static void pump(unsigned ms) {
  uint64_t end = now_ms() + ms;
  do {
    linux_adapter_poll();
    usleep(2000);
  } while (now_ms() < end);
}

static void start_options(struct mock_options options) {
  CHECK(mkdtemp(s_dir));
  CHECK(atexit(cleanup) == 0);
  linux_adapter_set_runtime_dir(s_dir);
  options.dir = s_dir;
  options.heartbeat_ms = 20;
  CHECK(mock_start(&options, &s_mock) == 0);
  linux_adapter_init();
  uint64_t end = now_ms() + 500;
  while (!linux_adapter_online() && now_ms() < end)
    pump(4);
  CHECK(linux_adapter_online());
}

static void start(unsigned caps) {
  start_options((struct mock_options){.caps = caps});
}

static void test_adapter_accel_limits(void) {
  start_options((struct mock_options){
    .caps = PBL_ADAPTER_CAP_ACCEL,
    .accel_min_interval_us = 20000,
    .accel_max_interval_us = 30000,
    .accel_max_samples = 4
  });
  CHECK(mock_wait_count(s_mock, PBL_ADAPTER_ACCEL_CONFIG, 1, 500));
  linux_adapter_set_accel(125, 40000);
  CHECK(mock_wait_count(s_mock, PBL_ADAPTER_ACCEL_CONFIG, 2, 500));
  struct pbl_adapter_accel_config accel;
  CHECK(mock_last(s_mock, PBL_ADAPTER_ACCEL_CONFIG, &accel, sizeof(accel)));
  CHECK(accel.samples == 4 && accel.interval_us == 30000);
  linux_adapter_set_accel(1, 5000);
  CHECK(mock_wait_count(s_mock, PBL_ADAPTER_ACCEL_CONFIG, 3, 500));
  CHECK(mock_last(s_mock, PBL_ADAPTER_ACCEL_CONFIG, &accel, sizeof(accel)));
  CHECK(accel.samples == 1 && accel.interval_us == 20000);
}

static void test_idle_deadline_update(void) {
  start(PBL_ADAPTER_CAP_SUSPEND);
  linux_adapter_idle(false, 5000000);
  CHECK(mock_wait_count(s_mock, PBL_ADAPTER_IDLE, 1, 500));
  linux_adapter_idle(false, 6000000);
  CHECK(mock_wait_count(s_mock, PBL_ADAPTER_IDLE, 2, 500));
  struct pbl_adapter_idle idle;
  CHECK(mock_last(s_mock, PBL_ADAPTER_IDLE, &idle, sizeof(idle)));
  CHECK(!idle.busy && idle.next_deadline_us == 6000000);
  linux_adapter_idle(false, 6000000);
  pump(30);
  CHECK(mock_count(s_mock, PBL_ADAPTER_IDLE) == 2);
  linux_adapter_idle(true, 0);
  CHECK(mock_wait_count(s_mock, PBL_ADAPTER_IDLE, 3, 500));
  linux_adapter_idle(true, 9000000);
  pump(30);
  CHECK(mock_count(s_mock, PBL_ADAPTER_IDLE) == 3);
  CHECK(mock_last(s_mock, PBL_ADAPTER_IDLE, &idle, sizeof(idle)));
  CHECK(idle.busy && idle.next_deadline_us == 0);
}

static void test_busy_without_suspend(void) {
  start(PBL_ADAPTER_CAP_ACCEL);
  linux_adapter_idle(true, 0);
  pump(2100);
  CHECK(mock_count(s_mock, PBL_ADAPTER_IDLE) == 0);
  CHECK(linux_adapter_timeout_ms() > 1000);
}

static void test_unusable_runtime_directory(void) {
  char dir[128];
  memset(dir, 'x', sizeof(dir) - 1);
  dir[0] = '/';
  dir[sizeof(dir) - 1] = 0;
  linux_adapter_set_runtime_dir(dir);
  linux_adapter_init();
  CHECK(linux_adapter_poll_fd() == -1);
  linux_adapter_poll();
  CHECK(linux_adapter_timeout_ms() > 1000);
}

static void drain_adapter(void) {
  char buffer[2048];
  while (recv(s_adapter_fd, buffer, sizeof(buffer), MSG_DONTWAIT) >= 0) {
  }
}

static void test_full_command_queue(void) {
  CHECK(mkdtemp(s_dir));
  CHECK(atexit(cleanup) == 0);
  s_adapter_fd = pbl_adapter_bind(s_dir, PBL_ADAPTER_ADAPTER_SOCK);
  CHECK(s_adapter_fd >= 0);
  linux_adapter_set_runtime_dir(s_dir);
  linux_adapter_init();
  struct sockaddr_un os, adapter;
  CHECK(!pbl_adapter_socket_address(&os, s_dir, PBL_ADAPTER_OS_SOCK));
  CHECK(!pbl_adapter_socket_address(&adapter, s_dir, PBL_ADAPTER_ADAPTER_SOCK));
  struct pbl_adapter_caps caps;
  pbl_adapter_init(&caps, PBL_ADAPTER_CAPS, sizeof(caps), 1);
  caps.caps = PBL_ADAPTER_CAP_DISPLAY_POWER | PBL_ADAPTER_CAP_BACKLIGHT | PBL_ADAPTER_CAP_ACCEL |
              PBL_ADAPTER_CAP_HRM | PBL_ADAPTER_CAP_SUSPEND;
  caps.accel_min_interval_us = 10000;
  caps.accel_max_interval_us = 1000000;
  caps.accel_max_samples = 125;
  CHECK(!pbl_adapter_send(s_adapter_fd, &os, &caps, sizeof(caps)));
  linux_adapter_poll();
  CHECK(linux_adapter_online());
  drain_adapter();
  char padding[sizeof(struct pbl_adapter_hdr)] = {0};
  int rc = 0;
  for (unsigned i = 0; i < 10000 && !rc; i++)
    rc = pbl_adapter_send(linux_adapter_poll_fd(), &adapter, padding, sizeof(padding));
  CHECK(rc == -EAGAIN || rc == -ENOBUFS);
  CHECK(linux_adapter_display(PBL_ADAPTER_DISPLAY_OFF, PBL_ADAPTER_REASON_USER) < 0);
  CHECK(linux_adapter_backlight(35) < 0);
  linux_adapter_set_accel(23, 40000);
  linux_adapter_set_hrm(true, false);
  linux_adapter_idle(false, 5000000);
  drain_adapter();
  unsigned seen = 0;
  uint64_t end = now_ms() + 700;
  while (seen != 31 && now_ms() < end) {
    pump(4);
    unsigned char buffer[1024];
    ssize_t size;
    while ((size = recv(s_adapter_fd, buffer, sizeof(buffer), MSG_DONTWAIT)) >= 0) {
      struct pbl_adapter_hdr hdr;
      CHECK(pbl_adapter_accept(buffer, size, &hdr));
      if (hdr.type == PBL_ADAPTER_DISPLAY) {
        struct pbl_adapter_display message;
        memcpy(&message, buffer, sizeof(message));
        CHECK(message.mode == PBL_ADAPTER_DISPLAY_OFF);
        seen |= 1;
      } else if (hdr.type == PBL_ADAPTER_BACKLIGHT) {
        struct pbl_adapter_backlight message;
        memcpy(&message, buffer, sizeof(message));
        CHECK(message.level == 35);
        seen |= 2;
      } else if (hdr.type == PBL_ADAPTER_ACCEL_CONFIG) {
        struct pbl_adapter_accel_config message;
        memcpy(&message, buffer, sizeof(message));
        CHECK(message.samples == 23 && message.interval_us == 40000);
        seen |= 4;
      } else if (hdr.type == PBL_ADAPTER_HRM_CONFIG) {
        struct pbl_adapter_hrm_config message;
        memcpy(&message, buffer, sizeof(message));
        CHECK(message.mode == PBL_ADAPTER_HRM_BPM);
        seen |= 8;
      } else if (hdr.type == PBL_ADAPTER_IDLE) {
        struct pbl_adapter_idle message;
        memcpy(&message, buffer, sizeof(message));
        CHECK(!message.busy && message.next_deadline_us == 5000000);
        seen |= 16;
      }
    }
  }
  CHECK(seen == 31);
  CHECK(linux_adapter_timeout_ms() > 1000);
}

int main(void) {
  const struct {
    const char *name;
    void (*run)(void);
  } cases[] = {
    {"updated idle deadline reaches the adapter", test_idle_deadline_update},
    {"busy report without suspend capability does not spin", test_busy_without_suspend},
    {"unusable runtime directory does not spin", test_unusable_runtime_directory},
    {"persistent commands survive a full adapter queue", test_full_command_queue},
    {"accelerometer requests respect adapter limits", test_adapter_accel_limits},
  };
  unsigned failed = 0;
  for (unsigned i = 0; i < sizeof(cases) / sizeof(*cases); i++) {
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
      alarm(10);
      cases[i].run();
      exit(0);
    }
    CHECK(pid > 0);
    int status;
    CHECK(waitpid(pid, &status, 0) == pid);
    bool ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    printf("%s  %s\n", ok ? "PASS" : "FAIL", cases[i].name);
    failed += !ok;
  }
  return failed != 0;
}
