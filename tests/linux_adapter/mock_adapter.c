/* SPDX-License-Identifier: Apache-2.0 */

#include "mock_adapter.h"

#include <stdlib.h>
#include <string.h>

#include <poll.h>
#include <pthread.h>

#define TYPES 512

struct mock {
  struct mock_options options;
  char dir[128];
  int fd;
  struct sockaddr_un os;
  pthread_t thread;
  pthread_mutex_t lock;
  bool stop;
  uint32_t seq;
  unsigned counts[TYPES];
  uint8_t last[TYPES][sizeof(struct pbl_adapter_sensor)];
  unsigned display_actual;
  uint32_t ack_seq;
  unsigned lux;
  bool lux_valid;
  bool display_pending;
  unsigned pending_mode;
  uint32_t pending_seq;
  uint64_t pending_due_us;
  uint64_t next_heartbeat_us;
};

static unsigned prv_slot(unsigned type) {
  return type < 0x200 ? type : (type & 0xff) | 0x100;
}

static uint64_t prv_now_us(void) {
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return (uint64_t)now.tv_sec * 1000000u + (uint64_t)now.tv_nsec / 1000u;
}

// Must hold the lock.
static int prv_send(struct mock *m, const void *message, size_t size) {
  return pbl_adapter_send(m->fd, &m->os, message, size);
}

static void prv_send_state(struct mock *m) {
  struct pbl_adapter_state state;
  pbl_adapter_init(&state, PBL_ADAPTER_STATE, sizeof(state), ++m->seq);
  state.ack_seq = m->ack_seq;
  state.display_actual = (uint8_t)m->display_actual;
  state.lux = (uint16_t)m->lux;
  state.lux_valid = m->lux_valid;
  prv_send(m, &state, sizeof(state));
}

static void prv_send_caps(struct mock *m, uint32_t ack) {
  struct pbl_adapter_caps caps;
  pbl_adapter_init(&caps, PBL_ADAPTER_CAPS, sizeof(caps), ++m->seq);
  caps.ack_seq = ack;
  caps.caps = m->options.caps;
  caps.accel_min_interval_us =
      m->options.accel_min_interval_us ? m->options.accel_min_interval_us : 10000;
  caps.accel_max_interval_us =
      m->options.accel_max_interval_us ? m->options.accel_max_interval_us : 1000000;
  caps.accel_max_samples =
      m->options.accel_max_samples ? m->options.accel_max_samples : PBL_ADAPTER_SENSOR_SAMPLES;
  caps.vibe_max_duration_ms = PBL_ADAPTER_VIBE_MAX_MS;
  pbl_adapter_copy_name(caps.adapter_name, sizeof(caps.adapter_name), "mock");
  pbl_adapter_copy_name(caps.adapter_version, sizeof(caps.adapter_version), "1");
  prv_send(m, &caps, sizeof(caps));
}

static void prv_handle(struct mock *m, const uint8_t *data, size_t size) {
  struct pbl_adapter_hdr hdr;
  if (!pbl_adapter_accept(data, size, &hdr))
    return;
  unsigned slot = prv_slot(hdr.type);
  m->counts[slot]++;
  memcpy(m->last[slot], data,
         hdr.length < sizeof(m->last[slot]) ? hdr.length : sizeof(m->last[slot]));
  if (m->options.log)
    fprintf(m->options.log, "mock: type=0x%04x seq=%u length=%u\n", hdr.type, hdr.seq, hdr.length);
  switch (hdr.type) {
    case PBL_ADAPTER_HELLO:
      if (!m->options.silent) {
        prv_send_caps(m, hdr.seq);
        prv_send_state(m);
      }
      break;
    case PBL_ADAPTER_DISPLAY: {
      struct pbl_adapter_display display;
      memcpy(&display, data, sizeof(display));
      if (display.mode < PBL_ADAPTER_DISPLAY_ON || display.mode > PBL_ADAPTER_DISPLAY_AMBIENT_CLOCK)
        break;
      m->display_pending = true;
      m->pending_mode = display.mode;
      m->pending_seq = hdr.seq;
      m->pending_due_us = prv_now_us() + (uint64_t)m->options.display_latency_ms * 1000u;
      break;
    }
    case PBL_ADAPTER_USER_ACTIVITY:
      if (m->options.fast_path && (m->options.caps & PBL_ADAPTER_CAP_DISPLAY_POWER) &&
          m->display_actual != PBL_ADAPTER_DISPLAY_ON) {
        m->display_actual = PBL_ADAPTER_DISPLAY_ON;
        prv_send_state(m);
      }
      break;
    default:
      break;
  }
}

static void *prv_run(void *arg) {
  struct mock *m = arg;
  uint8_t buffer[sizeof(struct pbl_adapter_sensor) + 64];
  while (true) {
    struct pollfd p = {.fd = m->fd, .events = POLLIN};
    poll(&p, 1, 5);
    pthread_mutex_lock(&m->lock);
    if (m->stop) {
      pthread_mutex_unlock(&m->lock);
      return NULL;
    }
    ssize_t count;
    while ((count = recv(m->fd, buffer, sizeof(buffer), MSG_DONTWAIT)) >= 0)
      prv_handle(m, buffer, (size_t)count);
    uint64_t now = prv_now_us();
    if (m->display_pending && now >= m->pending_due_us) {
      m->display_pending = false;
      m->display_actual = m->pending_mode;
      m->ack_seq = m->pending_seq;
      prv_send_state(m);
    }
    unsigned period = m->options.heartbeat_ms ? m->options.heartbeat_ms : 10000;
    if (!m->options.silent && now >= m->next_heartbeat_us) {
      m->next_heartbeat_us = now + (uint64_t)period * 1000u;
      prv_send_state(m);
    }
    pthread_mutex_unlock(&m->lock);
  }
}

int mock_start(const struct mock_options *options, struct mock **out) {
  struct mock *m = calloc(1, sizeof(*m));
  if (!m)
    return -ENOMEM;
  m->options = *options;
  snprintf(m->dir, sizeof(m->dir), "%s", options->dir);
  pthread_mutex_init(&m->lock, NULL);
  m->display_actual = PBL_ADAPTER_DISPLAY_ON;
  mkdir(m->dir, 0700);
  int rc = pbl_adapter_socket_address(&m->os, m->dir, PBL_ADAPTER_OS_SOCK);
  m->fd = rc ? rc : pbl_adapter_bind(m->dir, PBL_ADAPTER_ADAPTER_SOCK);
  if (m->fd < 0) {
    rc = m->fd;
    free(m);
    return rc;
  }
  m->next_heartbeat_us = prv_now_us();
  pthread_create(&m->thread, NULL, prv_run, m);
  if (!options->silent)
    mock_restart(m);
  *out = m;
  return 0;
}

void mock_stop(struct mock *m) {
  pthread_mutex_lock(&m->lock);
  m->stop = true;
  pthread_mutex_unlock(&m->lock);
  pthread_join(m->thread, NULL);
  char path[256];
  if (!pbl_adapter_join(path, sizeof(path), m->dir, PBL_ADAPTER_ADAPTER_SOCK))
    unlink(path);
  close(m->fd);
  free(m);
}

void mock_restart(struct mock *m) {
  struct pbl_adapter_hdr ready;
  pthread_mutex_lock(&m->lock);
  pbl_adapter_init(&ready, PBL_ADAPTER_ADAPTER_READY, sizeof(ready), ++m->seq);
  prv_send(m, &ready, sizeof(ready));
  pthread_mutex_unlock(&m->lock);
}

void mock_send_gesture(struct mock *m, unsigned kind) {
  struct pbl_adapter_gesture gesture;
  pthread_mutex_lock(&m->lock);
  pbl_adapter_init(&gesture, PBL_ADAPTER_GESTURE, sizeof(gesture), ++m->seq);
  gesture.kind = (uint8_t)kind;
  gesture.timestamp_us = pbl_adapter_boottime_us();
  // A real adapter with a fast path lights the panel before the OS has decided anything.
  if (m->options.fast_path && kind == PBL_ADAPTER_GESTURE_WRIST_UP &&
      (m->options.caps & PBL_ADAPTER_CAP_DISPLAY_POWER)) {
    m->display_actual = PBL_ADAPTER_DISPLAY_ON;
    prv_send_state(m);
  }
  if (kind == PBL_ADAPTER_GESTURE_EXTERNAL_OFF) {
    m->display_actual = PBL_ADAPTER_DISPLAY_OFF;
    prv_send_state(m);
  }
  prv_send(m, &gesture, sizeof(gesture));
  pthread_mutex_unlock(&m->lock);
}

void mock_send_fault(struct mock *m, unsigned component, unsigned state) {
  struct pbl_adapter_fault fault;
  pthread_mutex_lock(&m->lock);
  pbl_adapter_init(&fault, PBL_ADAPTER_FAULT, sizeof(fault), ++m->seq);
  fault.component = (uint8_t)component;
  fault.state = (uint8_t)state;
  prv_send(m, &fault, sizeof(fault));
  pthread_mutex_unlock(&m->lock);
}

void mock_send_sensor(struct mock *m, unsigned type, unsigned count, unsigned interval_us,
                      int16_t first, int16_t second) {
  struct pbl_adapter_sensor sensor;
  pthread_mutex_lock(&m->lock);
  pbl_adapter_init(&sensor, PBL_ADAPTER_SENSOR, sizeof(sensor), ++m->seq);
  sensor.type = type;
  sensor.count = count;
  sensor.interval_us = interval_us;
  sensor.timestamp_us = pbl_adapter_boottime_us();
  for (unsigned i = 0; i < count && i < PBL_ADAPTER_SENSOR_SAMPLES; i++) {
    sensor.values[i][0] = first;
    sensor.values[i][1] = second;
    sensor.values[i][2] = (int16_t)i;
  }
  prv_send(m, &sensor, sizeof(sensor));
  pthread_mutex_unlock(&m->lock);
}

void mock_set_lux(struct mock *m, unsigned lux, bool valid) {
  pthread_mutex_lock(&m->lock);
  m->lux = lux;
  m->lux_valid = valid;
  prv_send_state(m);
  pthread_mutex_unlock(&m->lock);
}

int mock_send_raw(struct mock *m, const void *data, size_t size) {
  pthread_mutex_lock(&m->lock);
  int rc = prv_send(m, data, size);
  pthread_mutex_unlock(&m->lock);
  return rc;
}

unsigned mock_count(struct mock *m, unsigned type) {
  pthread_mutex_lock(&m->lock);
  unsigned count = m->counts[prv_slot(type)];
  pthread_mutex_unlock(&m->lock);
  return count;
}

bool mock_last(struct mock *m, unsigned type, void *out, size_t size) {
  pthread_mutex_lock(&m->lock);
  unsigned slot = prv_slot(type);
  bool have = m->counts[slot] != 0;
  if (have)
    memcpy(out, m->last[slot], size);
  pthread_mutex_unlock(&m->lock);
  return have;
}

unsigned mock_display_actual(struct mock *m) {
  pthread_mutex_lock(&m->lock);
  unsigned mode = m->display_actual;
  pthread_mutex_unlock(&m->lock);
  return mode;
}

bool mock_wait_count(struct mock *m, unsigned type, unsigned count, unsigned timeout_ms) {
  for (unsigned waited = 0; waited <= timeout_ms; waited += 2) {
    if (mock_count(m, type) >= count)
      return true;
    usleep(2000);
  }
  return false;
}
