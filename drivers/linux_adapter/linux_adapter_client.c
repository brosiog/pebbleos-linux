/* SPDX-License-Identifier: Apache-2.0 */

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include <pbl/drivers/linux_adapter_client.h>
#include <pbl/drivers/linux_profile.h>

#include <pthread.h>

#ifndef LINUX_ADAPTER_STANDALONE
#include <posix_host.h>
#define WAKE_MAIN_LOOP() posix_host_wake()
#else
#define WAKE_MAIN_LOOP() ((void)0)
#endif

#ifndef LINUX_ADAPTER_OS_BUILD
#define LINUX_ADAPTER_OS_BUILD "dev"
#endif

#define HELLO_BACKOFF_MIN_US 1000000ULL
#define HELLO_BACKOFF_MAX_US 60000000ULL
#define SILENCE_DEFAULT_US   (3ULL * PBL_ADAPTER_HEARTBEAT_S * 1000000ULL)
// A busy report makes the adapter hold a lock that expires on its own; repeat it while busy.
#define BUSY_REFRESH_US 2000000ULL
#define CONFIG_RETRY_US 100000ULL

enum config_pending {
  CONFIG_ACCEL = 1u << 0,
  CONFIG_HRM = 1u << 1,
  CONFIG_BACKLIGHT = 1u << 2,
  CONFIG_DISPLAY = 1u << 3,
  CONFIG_IDLE = 1u << 4,
};
static unsigned s_config_pending;
static uint64_t s_config_retry_us;

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t s_dirs_once = PTHREAD_ONCE_INIT;
static const char *s_runtime_override;
static char s_runtime_dir[128];
static char s_data_dir[128];
static int s_fd = -1;
static struct sockaddr_un s_peer;
static _Atomic uint32_t s_seq;
static uint64_t s_silence_limit_us = SILENCE_DEFAULT_US;
static bool s_display_changed;

static struct {
  bool online;
  struct pbl_adapter_caps caps;
  struct pbl_adapter_state state;
  bool have_state;
  uint64_t last_rx_us;
  uint64_t next_hello_us;
  uint64_t backoff_us;
  uint8_t faults[8];
} s_link;

// What the OS wants, resent in full whenever the adapter appears.
static struct {
  unsigned accel_samples, accel_interval_us;
  bool hrm_on, hrm_low_latency;
  int ambient_clock;
  bool backlight_set;
  unsigned backlight;
  bool display_set;
  unsigned display_mode, display_reason;
  uint32_t display_seq;
  bool idle_set, idle_busy;
  uint64_t idle_deadline_us;
  uint64_t idle_sent_us;
} s_want;

static uint64_t prv_now_us(void) {
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return (uint64_t)now.tv_sec * 1000000u + (uint64_t)now.tv_nsec / 1000u;
}

static uint32_t prv_next_seq(void) {
  uint32_t seq = atomic_fetch_add(&s_seq, 1) + 1;
  return seq ? seq : atomic_fetch_add(&s_seq, 1) + 1;
}

static void prv_resolve_dirs(void) {
  const struct linux_profile *profile = linux_profile_get();
  if (pbl_adapter_runtime_dir(s_runtime_dir, sizeof(s_runtime_dir), s_runtime_override,
                              profile->runtime_dir))
    s_runtime_dir[0] = 0;
  const char *data = getenv("PBL_DATA_DIR");
  snprintf(s_data_dir, sizeof(s_data_dir), "%s", data && *data ? data : profile->data_dir);
}

const char *linux_adapter_runtime_dir(void) {
  pthread_once(&s_dirs_once, prv_resolve_dirs);
  return s_runtime_dir;
}

const char *linux_adapter_data_dir(void) {
  pthread_once(&s_dirs_once, prv_resolve_dirs);
  return s_data_dir;
}

void linux_adapter_set_runtime_dir(const char *dir) {
  s_runtime_override = dir;
}

#ifdef LINUX_ADAPTER_STANDALONE
void linux_adapter_test_set_silence_us(uint64_t limit_us) {
  s_silence_limit_us = limit_us;
}
#endif

// Must hold s_lock.
static int prv_send(const void *message, size_t size) {
  return pbl_adapter_send(s_fd, &s_peer, message, size);
}

static int prv_send_config_message(const void *message, size_t size, unsigned pending) {
  int rc = prv_send(message, size);
  if (rc) {
    s_config_pending |= pending;
    s_config_retry_us = prv_now_us() + CONFIG_RETRY_US;
    WAKE_MAIN_LOOP();
  } else {
    s_config_pending &= ~pending;
  }
  return rc;
}

// Must hold s_lock.
static bool prv_has(unsigned cap) {
  return s_link.online && (s_link.caps.caps & cap);
}

static void prv_hello(uint64_t now) {
  struct pbl_adapter_hello hello;
  pbl_adapter_init(&hello, PBL_ADAPTER_HELLO, sizeof(hello), prv_next_seq());
  pbl_adapter_copy_name(hello.os_build, sizeof(hello.os_build), LINUX_ADAPTER_OS_BUILD);
  hello.max_minor = PBL_ADAPTER_MINOR;
  prv_send(&hello, sizeof(hello));
  s_link.backoff_us = s_link.backoff_us ? s_link.backoff_us * 2 : HELLO_BACKOFF_MIN_US;
  if (s_link.backoff_us > HELLO_BACKOFF_MAX_US)
    s_link.backoff_us = HELLO_BACKOFF_MAX_US;
  s_link.next_hello_us = now + s_link.backoff_us;
}

// Must hold s_lock.
static int prv_send_accel(void) {
  struct pbl_adapter_accel_config message;
  pbl_adapter_init(&message, PBL_ADAPTER_ACCEL_CONFIG, sizeof(message), prv_next_seq());
  unsigned maximum = s_link.caps.accel_max_samples;
  if (maximum > PBL_ADAPTER_SENSOR_SAMPLES)
    maximum = PBL_ADAPTER_SENSOR_SAMPLES;
  message.samples = s_want.accel_samples > maximum ? maximum : s_want.accel_samples;
  message.interval_us = s_want.accel_interval_us;
  if (message.samples) {
    unsigned minimum = s_link.caps.accel_min_interval_us;
    unsigned maximum_interval = s_link.caps.accel_max_interval_us;
    if (minimum < PBL_ADAPTER_ACCEL_MIN_INTERVAL_US || minimum > maximum_interval ||
        maximum_interval > PBL_ADAPTER_ACCEL_MAX_INTERVAL_US)
      return -EINVAL;
    if (message.interval_us < minimum)
      message.interval_us = minimum;
    if (message.interval_us > maximum_interval)
      message.interval_us = maximum_interval;
  }
  return prv_send_config_message(&message, sizeof(message), CONFIG_ACCEL);
}

static int prv_send_hrm(void) {
  struct pbl_adapter_hrm_config message;
  pbl_adapter_init(&message, PBL_ADAPTER_HRM_CONFIG, sizeof(message), prv_next_seq());
  message.mode = s_want.hrm_on ? PBL_ADAPTER_HRM_BPM : PBL_ADAPTER_HRM_OFF;
  message.low_latency = s_want.hrm_low_latency;
  return prv_send_config_message(&message, sizeof(message), CONFIG_HRM);
}

static int prv_send_backlight(void) {
  struct pbl_adapter_backlight message;
  pbl_adapter_init(&message, PBL_ADAPTER_BACKLIGHT, sizeof(message), prv_next_seq());
  message.level = (uint8_t)s_want.backlight;
  return prv_send_config_message(&message, sizeof(message), CONFIG_BACKLIGHT);
}

static int prv_send_display(void) {
  struct pbl_adapter_display message;
  s_want.display_seq = prv_next_seq();
  pbl_adapter_init(&message, PBL_ADAPTER_DISPLAY, sizeof(message), s_want.display_seq);
  message.mode = (uint8_t)s_want.display_mode;
  message.reason = (uint8_t)s_want.display_reason;
  return prv_send_config_message(&message, sizeof(message), CONFIG_DISPLAY);
}

static int prv_send_idle(uint64_t now) {
  struct pbl_adapter_idle message;
  pbl_adapter_init(&message, PBL_ADAPTER_IDLE, sizeof(message), prv_next_seq());
  message.busy = s_want.idle_busy;
  message.next_deadline_us = s_want.idle_deadline_us;
  s_want.idle_sent_us = now;
  return prv_send_config_message(&message, sizeof(message), CONFIG_IDLE);
}

// Must hold s_lock.
static void prv_send_config(uint64_t now) {
  s_config_pending = 0;
  if (prv_has(PBL_ADAPTER_CAP_ACCEL))
    prv_send_accel();
  if (prv_has(PBL_ADAPTER_CAP_HRM))
    prv_send_hrm();
  if (prv_has(PBL_ADAPTER_CAP_BACKLIGHT) && s_want.backlight_set)
    prv_send_backlight();
  if (prv_has(PBL_ADAPTER_CAP_DISPLAY_POWER) && s_want.display_set)
    prv_send_display();
  if (prv_has(PBL_ADAPTER_CAP_SUSPEND) && s_want.idle_set)
    prv_send_idle(now);
}

static void prv_go_offline(const char *why) {
  if (s_link.online)
    fprintf(stderr, "adapter: %s; running without it\n", why);
  s_link.online = false;
  s_config_pending = 0;
  s_link.have_state = false;
  s_display_changed = true;
  memset(s_link.faults, 0, sizeof(s_link.faults));
  s_link.backoff_us = 0;
}

// Must hold s_lock. Returns true when a sink must run after unlocking.
static bool prv_handle(const uint8_t *data, size_t size, uint64_t now,
                       struct pbl_adapter_sensor *sensor, unsigned *gesture) {
  struct pbl_adapter_hdr hdr;
  if (!pbl_adapter_accept(data, size, &hdr))
    return false;
  s_link.last_rx_us = now;
  switch (hdr.type) {
    case PBL_ADAPTER_ADAPTER_READY:
      // The adapter (re)started: forget what it declared and introduce ourselves again.
      prv_go_offline("adapter restarted");
      s_link.next_hello_us = now;
      return false;
    case PBL_ADAPTER_CAPS: {
      memcpy(&s_link.caps, data, sizeof(s_link.caps));
      s_link.caps.adapter_name[sizeof(s_link.caps.adapter_name) - 1] = 0;
      s_link.caps.adapter_version[sizeof(s_link.caps.adapter_version) - 1] = 0;
      if (!s_link.online)
        fprintf(stderr, "adapter: %s %s, capabilities 0x%03x\n", s_link.caps.adapter_name,
                s_link.caps.adapter_version, s_link.caps.caps);
      s_link.online = true;
      s_link.backoff_us = 0;
      prv_send_config(now);
      return false;
    }
    default:
      break;
  }
  // Everything else is trusted only after the handshake.
  if (!s_link.online)
    return false;
  switch (hdr.type) {
    case PBL_ADAPTER_STATE: {
      uint8_t previous = s_link.have_state ? s_link.state.display_actual : 0;
      memcpy(&s_link.state, data, sizeof(s_link.state));
      s_link.have_state = true;
      // The display driver follows display_actual from its own hook; make it run now.
      if (previous != s_link.state.display_actual)
        s_display_changed = true;
      return false;
    }
    case PBL_ADAPTER_FAULT: {
      struct pbl_adapter_fault fault;
      memcpy(&fault, data, sizeof(fault));
      if (fault.component < sizeof(s_link.faults) && fault.state <= PBL_ADAPTER_FAULT_FAILED &&
          s_link.faults[fault.component] != fault.state) {
        s_link.faults[fault.component] = fault.state;
        fprintf(stderr, "adapter: component %u fault state %u\n", fault.component, fault.state);
      }
      return false;
    }
    case PBL_ADAPTER_SENSOR: {
      memcpy(sensor, data, sizeof(*sensor));
      bool known =
          sensor->type >= PBL_ADAPTER_SENSOR_ACCEL && sensor->type <= PBL_ADAPTER_SENSOR_STEPS;
      return known && sensor->count > 0 && sensor->count <= PBL_ADAPTER_SENSOR_SAMPLES &&
             sensor->interval_us >= PBL_ADAPTER_ACCEL_MIN_INTERVAL_US &&
             sensor->interval_us <= PBL_ADAPTER_ACCEL_MAX_INTERVAL_US;
    }
    case PBL_ADAPTER_GESTURE: {
      struct pbl_adapter_gesture gesture_message;
      memcpy(&gesture_message, data, sizeof(gesture_message));
      if (gesture_message.kind < PBL_ADAPTER_GESTURE_WRIST_UP ||
          gesture_message.kind > PBL_ADAPTER_GESTURE_EXTERNAL_OFF)
        return false;
      *gesture = gesture_message.kind;
      return true;
    }
    default:
      return false;
  }
}

void linux_adapter_poll(void) {
  uint8_t buffer[sizeof(struct pbl_adapter_sensor) + 64];
  for (unsigned i = 0; s_fd >= 0 && i < 32; i++) {
    ssize_t count = recv(s_fd, buffer, sizeof(buffer), MSG_DONTWAIT);
    if (count < 0)
      break;
    struct pbl_adapter_sensor sensor;
    unsigned gesture = 0;
    uint64_t now = prv_now_us();
    pthread_mutex_lock(&s_lock);
    bool deliver = prv_handle(buffer, (size_t)count, now, &sensor, &gesture);
    pthread_mutex_unlock(&s_lock);
    // Sinks run unlocked: they call back into this client.
    if (deliver && gesture)
      linux_watch_gesture_received(gesture);
    else if (deliver)
      linux_watch_sensor_received(&sensor);
  }
  uint64_t now = prv_now_us();
  pthread_mutex_lock(&s_lock);
  bool display_changed = s_display_changed;
  s_display_changed = false;
  if (s_link.online && now - s_link.last_rx_us > s_silence_limit_us) {
    prv_go_offline("adapter went silent");
    s_link.next_hello_us = now;
  }
  if (!s_link.online && s_fd >= 0 && now >= s_link.next_hello_us)
    prv_hello(now);
  if (s_link.online && s_config_pending && now >= s_config_retry_us) {
    unsigned pending = s_config_pending;
    if (pending & CONFIG_ACCEL)
      prv_send_accel();
    if (pending & CONFIG_HRM)
      prv_send_hrm();
    if (pending & CONFIG_BACKLIGHT)
      prv_send_backlight();
    if (pending & CONFIG_DISPLAY)
      prv_send_display();
    if (pending & CONFIG_IDLE)
      prv_send_idle(now);
  }
  if (s_want.idle_set && s_want.idle_busy && prv_has(PBL_ADAPTER_CAP_SUSPEND) &&
      now - s_want.idle_sent_us >= BUSY_REFRESH_US)
    prv_send_idle(now);
  pthread_mutex_unlock(&s_lock);
  if (display_changed)
    WAKE_MAIN_LOOP();
}

int linux_adapter_poll_fd(void) {
  return s_fd;
}

unsigned linux_adapter_timeout_ms(void) {
  uint64_t now = prv_now_us(), next = now + 60000000ULL;
  pthread_mutex_lock(&s_lock);
  if (s_fd < 0) {
    pthread_mutex_unlock(&s_lock);
    return 60000;
  }
  if (!s_link.online) {
    next = s_link.next_hello_us < next ? s_link.next_hello_us : next;
  } else {
    uint64_t silent = s_link.last_rx_us + s_silence_limit_us;
    next = silent < next ? silent : next;
    if (s_config_pending && s_config_retry_us < next)
      next = s_config_retry_us;
    if (s_want.idle_set && s_want.idle_busy && prv_has(PBL_ADAPTER_CAP_SUSPEND)) {
      uint64_t refresh = s_want.idle_sent_us + BUSY_REFRESH_US;
      next = refresh < next ? refresh : next;
    }
  }
  pthread_mutex_unlock(&s_lock);
  return next <= now ? 0 : (unsigned)((next - now + 999) / 1000);
}

void linux_adapter_init(void) {
  const char *dir = linux_adapter_runtime_dir();
  if (!*dir || pbl_adapter_socket_address(&s_peer, dir, PBL_ADAPTER_ADAPTER_SOCK)) {
    fprintf(stderr, "adapter: no usable runtime directory; running without an adapter\n");
    return;
  }
  mkdir(dir, 0700);
  s_fd = pbl_adapter_bind(dir, PBL_ADAPTER_OS_SOCK);
  if (s_fd < 0) {
    fprintf(stderr, "adapter: cannot bind %s/%s: %s; running without an adapter\n", dir,
            PBL_ADAPTER_OS_SOCK, strerror(-s_fd));
    s_fd = -1;
    return;
  }
  pthread_mutex_lock(&s_lock);
  prv_hello(prv_now_us());
  pthread_mutex_unlock(&s_lock);
}

void linux_adapter_exit(void) {
  pthread_mutex_lock(&s_lock);
  if (s_fd >= 0) {
    s_want.accel_samples = 0;
    s_want.hrm_on = false;
    if (prv_has(PBL_ADAPTER_CAP_ACCEL))
      prv_send_accel();
    if (prv_has(PBL_ADAPTER_CAP_HRM))
      prv_send_hrm();
    // Hand the panel back to the vendor stack.
    if (prv_has(PBL_ADAPTER_CAP_DISPLAY_POWER)) {
      s_want.display_mode = PBL_ADAPTER_DISPLAY_ON;
      s_want.display_reason = PBL_ADAPTER_REASON_APP;
      prv_send_display();
    }
    if (prv_has(PBL_ADAPTER_CAP_SUSPEND)) {
      s_want.idle_busy = false;
      s_want.idle_deadline_us = 0;
      prv_send_idle(prv_now_us());
    }
    char path[256];
    if (!pbl_adapter_join(path, sizeof(path), s_runtime_dir, PBL_ADAPTER_OS_SOCK))
      unlink(path);
    close(s_fd);
    s_fd = -1;
  }
  s_link.online = false;
  pthread_mutex_unlock(&s_lock);
}

bool linux_adapter_online(void) {
  pthread_mutex_lock(&s_lock);
  bool online = s_link.online;
  pthread_mutex_unlock(&s_lock);
  return online;
}

bool linux_adapter_has(unsigned cap) {
  pthread_mutex_lock(&s_lock);
  bool has = prv_has(cap);
  pthread_mutex_unlock(&s_lock);
  return has;
}

unsigned linux_adapter_display_actual(void) {
  pthread_mutex_lock(&s_lock);
  unsigned mode =
      prv_has(PBL_ADAPTER_CAP_DISPLAY_POWER) && s_link.have_state ? s_link.state.display_actual : 0;
  pthread_mutex_unlock(&s_lock);
  return mode;
}

int linux_adapter_lux(unsigned *lux) {
  pthread_mutex_lock(&s_lock);
  int rc = 0;
  if (!s_link.online || !s_link.have_state)
    rc = -EAGAIN;
  else if (!(s_link.caps.caps & PBL_ADAPTER_CAP_ALS))
    rc = -ENODEV;
  else if (!s_link.state.lux_valid)
    rc = -ENODATA;
  else
    *lux = s_link.state.lux;
  pthread_mutex_unlock(&s_lock);
  return rc;
}

unsigned linux_adapter_fault_state(unsigned component) {
  pthread_mutex_lock(&s_lock);
  unsigned state = component < sizeof(s_link.faults) ? s_link.faults[component] : 0;
  pthread_mutex_unlock(&s_lock);
  return state;
}

// Must hold s_lock. Returns 0, -ENOTCONN with no adapter or -ENOSYS without the capability.
static int prv_gate(unsigned cap) {
  if (!s_link.online)
    return -ENOTCONN;
  return s_link.caps.caps & cap ? 0 : -ENOSYS;
}

int linux_adapter_vibe(unsigned strength, unsigned duration_ms) {
  pthread_mutex_lock(&s_lock);
  int rc = prv_gate(PBL_ADAPTER_CAP_VIBE);
  if (!rc) {
    struct pbl_adapter_vibe message;
    unsigned limit = s_link.caps.vibe_max_duration_ms ? s_link.caps.vibe_max_duration_ms
                                                      : PBL_ADAPTER_VIBE_MAX_MS;
    pbl_adapter_build_vibe(&message, prv_next_seq(), strength,
                           duration_ms > limit ? limit : duration_ms);
    rc = prv_send(&message, sizeof(message));
  }
  pthread_mutex_unlock(&s_lock);
  return rc;
}

int linux_adapter_backlight(unsigned level) {
  pthread_mutex_lock(&s_lock);
  s_want.backlight = level > 100 ? 100 : level;
  s_want.backlight_set = true;
  int rc = prv_gate(PBL_ADAPTER_CAP_BACKLIGHT);
  if (!rc)
    rc = prv_send_backlight();
  pthread_mutex_unlock(&s_lock);
  return rc;
}

// Must hold s_lock. Sends when the request changes or `force` is set.
static int prv_display(unsigned mode, unsigned reason, bool force) {
  bool changed = !s_want.display_set || s_want.display_mode != mode;
  s_want.display_set = true;
  s_want.display_mode = mode;
  s_want.display_reason = reason;
  int rc = prv_gate(PBL_ADAPTER_CAP_DISPLAY_POWER);
  if (!rc && (changed || force || (s_config_pending & CONFIG_DISPLAY)))
    rc = prv_send_display();
  return rc;
}

int linux_adapter_display(unsigned mode, unsigned reason) {
  if (mode < PBL_ADAPTER_DISPLAY_ON || mode > PBL_ADAPTER_DISPLAY_AMBIENT_CLOCK)
    return -EINVAL;
  pthread_mutex_lock(&s_lock);
  int rc = prv_display(mode, reason, false);
  pthread_mutex_unlock(&s_lock);
  return rc;
}

static unsigned prv_idle_mode(void) {
  return s_want.ambient_clock ? PBL_ADAPTER_DISPLAY_AMBIENT_CLOCK : PBL_ADAPTER_DISPLAY_OFF;
}

int linux_adapter_display_idle(unsigned reason) {
  pthread_mutex_lock(&s_lock);
  int rc = prv_display(prv_idle_mode(), reason, false);
  pthread_mutex_unlock(&s_lock);
  return rc;
}

int linux_adapter_wake(unsigned reason, unsigned source) {
  pthread_mutex_lock(&s_lock);
  int rc = s_link.online ? 0 : -ENOTCONN;
  if (!rc && source) {
    struct pbl_adapter_user_activity message;
    pbl_adapter_init(&message, PBL_ADAPTER_USER_ACTIVITY, sizeof(message), prv_next_seq());
    message.source = (uint8_t)source;
    prv_send(&message, sizeof(message));
  }
  // The panel may be off while the OS still believes it asked for it on (a vendor blank).
  bool panel_off = s_link.have_state && s_link.state.display_actual != PBL_ADAPTER_DISPLAY_ON;
  rc = prv_display(PBL_ADAPTER_DISPLAY_ON, reason, panel_off);
  pthread_mutex_unlock(&s_lock);
  return rc;
}

int linux_adapter_trace(bool enable, unsigned seconds) {
  pthread_mutex_lock(&s_lock);
  int rc = s_link.online ? 0 : -ENOTCONN;
  if (!rc) {
    struct pbl_adapter_trace message;
    pbl_adapter_init(&message, PBL_ADAPTER_TRACE, sizeof(message), prv_next_seq());
    message.enable = enable;
    message.seconds = (uint16_t)seconds;
    rc = prv_send(&message, sizeof(message));
  }
  pthread_mutex_unlock(&s_lock);
  return rc;
}

void linux_adapter_set_ambient_clock(int mode) {
  pthread_mutex_lock(&s_lock);
  int previous = s_want.ambient_clock;
  s_want.ambient_clock = mode;
  // A display that is idling switches between the clock and black right away.
  if (previous != mode && s_want.display_set && s_want.display_mode != PBL_ADAPTER_DISPLAY_ON)
    prv_display(prv_idle_mode(), s_want.display_reason, true);
  pthread_mutex_unlock(&s_lock);
}

void linux_adapter_set_accel(unsigned samples, unsigned interval_us) {
  pthread_mutex_lock(&s_lock);
  s_want.accel_samples = samples;
  s_want.accel_interval_us = interval_us;
  if (prv_has(PBL_ADAPTER_CAP_ACCEL))
    prv_send_accel();
  pthread_mutex_unlock(&s_lock);
}

void linux_adapter_set_hrm(bool on, bool low_latency) {
  pthread_mutex_lock(&s_lock);
  s_want.hrm_on = on;
  s_want.hrm_low_latency = low_latency;
  if (prv_has(PBL_ADAPTER_CAP_HRM))
    prv_send_hrm();
  pthread_mutex_unlock(&s_lock);
}

void linux_adapter_idle(bool busy, uint64_t next_deadline_us) {
  pthread_mutex_lock(&s_lock);
  uint64_t deadline = busy ? 0 : next_deadline_us;
  bool changed =
      !s_want.idle_set || s_want.idle_busy != busy || s_want.idle_deadline_us != deadline;
  s_want.idle_set = true;
  s_want.idle_busy = busy;
  s_want.idle_deadline_us = deadline;
  if (changed && prv_has(PBL_ADAPTER_CAP_SUSPEND))
    prv_send_idle(prv_now_us());
  pthread_mutex_unlock(&s_lock);
}

#ifndef LINUX_ADAPTER_STANDALONE
static void prv_set_runtime_dir(const char *value) {
  linux_adapter_set_runtime_dir(value);
}

static void prv_set_profile(const char *value) {
  linux_profile_set_file(value);
}

static int prv_poll_fd(unsigned index) {
  return index ? -1 : linux_adapter_poll_fd();
}

POSIX_HOST_HOOK(.init = linux_adapter_init, .poll = linux_adapter_poll, .exit = linux_adapter_exit,
                .poll_fd = prv_poll_fd, .poll_timeout_ms = linux_adapter_timeout_ms)
POSIX_HOST_OPTION(.flag = 'R', .arg = "dir", .help = "adapter runtime directory",
                  .set = prv_set_runtime_dir)
POSIX_HOST_OPTION(.flag = 'P', .arg = "file", .help = "device profile", .set = prv_set_profile)
#endif
