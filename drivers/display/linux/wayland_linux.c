/* SPDX-License-Identifier: Apache-2.0 */

#define _GNU_SOURCE
#include "wayland_linux.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <poll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

// Core Wayland v1 wire protocol. No dependency on the stock 32-bit libraries.
#define EDGE        360
#define FRAME_BYTES (EDGE * EDGE * sizeof(uint32_t))
static int s_socket = -1;
int wayland_linux_fd(void) {
  return s_socket;
}
static uint32_t *s_pixels;
static uint32_t s_compositor_name, s_shm_name, s_shell_name;
static uint32_t s_policy_name;
static uint32_t s_keyrouter_name;
static bool s_ready;
static bool s_busy[2];
static unsigned char s_incoming[65536];
static size_t s_pending;
static unsigned s_frames;
static uint32_t s_visibility = UINT32_MAX;
static bool s_resume_redraw;
enum {
  DISPLAY = 1,
  REGISTRY,
  SYNC,
  COMPOSITOR,
  SHM,
  SHELL,
  SURFACE,
  POOL,
  BUFFER0,
  BUFFER1,
  SHELL_SURFACE,
  POLICY,
  VISIBILITY,
  KEYROUTER
};

static bool prv_send(uint32_t object, unsigned opcode, const uint32_t *args, size_t count, int fd) {
  uint32_t message[128];
  if (s_socket < 0 || count > 126)
    return false;
  message[0] = object;
  message[1] = ((count + 2) * 4) << 16 | opcode;
  if (count)
    memcpy(message + 2, args, count * 4);
  struct iovec vector = {.iov_base = message, .iov_len = (count + 2) * 4};
  union {
    struct cmsghdr align;
    char data[CMSG_SPACE(sizeof(int))];
  } control;
  struct msghdr header = {.msg_iov = &vector, .msg_iovlen = 1};
  if (fd >= 0) {
    memset(&control, 0, sizeof(control));
    header.msg_control = control.data;
    header.msg_controllen = sizeof(control.data);
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&header);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cmsg), &fd, sizeof(fd));
  }
  ssize_t sent = sendmsg(s_socket, &header, MSG_NOSIGNAL);
  if (sent != (ssize_t)vector.iov_len) {
    fprintf(stderr, "wayland send: %s\n", sent < 0 ? strerror(errno) : "short write");
    close(s_socket);
    s_socket = -1;
    return false;
  }
  return true;
}

static bool prv_string(uint32_t object, unsigned opcode, const char *value) {
  uint32_t args[64] = {0};
  size_t length = strlen(value) + 1;
  if (length > sizeof(args) - 4)
    return false;
  args[0] = length;
  memcpy(args + 1, value, length);
  return prv_send(object, opcode, args, 1 + (length + 3) / 4, -1);
}

static bool prv_bind(uint32_t name, const char *interface, uint32_t id) {
  uint32_t args[64] = {0};
  size_t length = strlen(interface) + 1;
  size_t words = (length + 3) / 4;
  args[0] = name;
  args[1] = length;
  memcpy(args + 2, interface, length);
  args[2 + words] = 1;
  args[3 + words] = id;
  return prv_send(REGISTRY, 0, args, 4 + words, -1);
}

static void prv_event(uint32_t object, unsigned opcode, const unsigned char *data, size_t size) {
  if (object == REGISTRY && opcode == 0 && size >= 16) {
    uint32_t name, length;
    memcpy(&name, data, 4);
    memcpy(&length, data + 4, 4);
    size_t padded = ((size_t)length + 3) & ~(size_t)3;
    if (!length || padded > size - 12 || data[8 + length - 1])
      return;
    const char *interface = (const char *)data + 8;
    if (!strcmp(interface, "wl_compositor"))
      s_compositor_name = name;
    if (!strcmp(interface, "wl_shm"))
      s_shm_name = name;
    if (!strcmp(interface, "wl_shell"))
      s_shell_name = name;
    if (!strcmp(interface, "tizen_policy"))
      s_policy_name = name;
    if (!strcmp(interface, "tizen_keyrouter"))
      s_keyrouter_name = name;
  } else if (object == SYNC && opcode == 0) {
    s_ready = true;
  } else if (object == SHELL_SURFACE && opcode == 0 && size >= 4) {
    uint32_t serial;
    memcpy(&serial, data, 4);
    prv_send(SHELL_SURFACE, 0, &serial, 1, -1);
  } else if ((object == BUFFER0 || object == BUFFER1) && opcode == 0) {
    s_busy[object - BUFFER0] = false;
  } else if (object == VISIBILITY && opcode == 0 && size >= 4) {
    uint32_t state;
    memcpy(&state, data, 4);
    fprintf(stderr, "wayland: Samsung visibility state=%u\n", state);
    if (state == 0 && s_visibility != 0 && s_frames) {
      s_resume_redraw = true;
      uint32_t surface = SURFACE;
      prv_send(POLICY, 2, &surface, 1, -1);
    }
    s_visibility = state;
  } else if (object == KEYROUTER && opcode == 0 && size >= 16) {
    uint32_t result[4];
    memcpy(result, data, sizeof(result));
    fprintf(stderr, "wayland: key grab key=%u mode=%u result=%u\n", result[1], result[2],
            result[3]);
  } else if (object == DISPLAY && opcode == 0) {
    uint32_t failed = 0, code = 0, length = 0;
    if (size >= 12) {
      memcpy(&failed, data, 4);
      memcpy(&code, data + 4, 4);
      memcpy(&length, data + 8, 4);
    }
    fprintf(stderr, "wayland protocol error: object=%u code=%u %.*s\n", failed, code,
            (int)(length <= size - (size >= 12 ? 12 : size) ? length : 0),
            size >= 12 ? (const char *)data + 12 : "");
    close(s_socket);
    s_socket = -1;
  }
}

void wayland_linux_poll(void) {
  while (s_socket >= 0) {
    ssize_t count =
        recv(s_socket, s_incoming + s_pending, sizeof(s_incoming) - s_pending, MSG_DONTWAIT);
    if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
      break;
    if (count <= 0) {
      if (count < 0 && errno == EINTR)
        continue;
      fprintf(stderr, "wayland: display connection closed\n");
      close(s_socket);
      s_socket = -1;
      break;
    }
    s_pending += count;
    size_t consumed = 0;
    while (s_pending - consumed >= 8) {
      uint32_t header[2];
      memcpy(header, s_incoming + consumed, 8);
      size_t size = header[1] >> 16;
      if (size < 8 || size % 4) {
        close(s_socket);
        s_socket = -1;
        return;
      }
      if (size > s_pending - consumed)
        break;
      prv_event(header[0], header[1] & 65535, s_incoming + consumed + 8, size - 8);
      consumed += size;
    }
    memmove(s_incoming, s_incoming + consumed, s_pending - consumed);
    s_pending -= consumed;
  }
}

bool wayland_linux_obscured(void) {
  return s_socket >= 0 && s_visibility != 0 && s_visibility != UINT32_MAX;
}

bool wayland_linux_take_resume_redraw(void) {
  bool redraw = s_resume_redraw;
  s_resume_redraw = false;
  return redraw;
}

void wayland_linux_activate(void) {
  if (s_socket < 0)
    return;
  prv_send(SHELL_SURFACE, 3, NULL, 0, -1);
  if (s_policy_name) {
    uint32_t surface = SURFACE;
    prv_send(POLICY, 4, &surface, 1, -1);
    prv_send(POLICY, 2, &surface, 1, -1);
  }
  s_resume_redraw = true;
  fprintf(stderr, "wayland: restoring PebbleOS foreground\n");
}

bool wayland_linux_init(const char *path) {
  struct sockaddr_un address = {.sun_family = AF_UNIX};
  if (strlen(path) >= sizeof(address.sun_path))
    return false;
  strcpy(address.sun_path, path);
  s_socket = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  struct timeval limit = {.tv_sec = 2};
  if (s_socket < 0)
    return false;
  setsockopt(s_socket, SOL_SOCKET, SO_SNDTIMEO, &limit, sizeof(limit));
  if (connect(s_socket, (struct sockaddr *)&address, sizeof(address)))
    goto fail;
  uint32_t args[] = {REGISTRY};
  if (!prv_send(DISPLAY, 1, args, 1, -1))
    goto fail;
  args[0] = SYNC;
  if (!prv_send(DISPLAY, 0, args, 1, -1))
    goto fail;
  for (int tries = 0; tries < 20 && !s_ready && s_socket >= 0; tries++) {
    struct pollfd descriptor = {.fd = s_socket, .events = POLLIN};
    poll(&descriptor, 1, 100);
    wayland_linux_poll();
  }
  if (!s_ready || !s_compositor_name || !s_shm_name || !s_shell_name)
    goto fail;
  if (!prv_bind(s_compositor_name, "wl_compositor", COMPOSITOR) ||
      !prv_bind(s_shm_name, "wl_shm", SHM) || !prv_bind(s_shell_name, "wl_shell", SHELL))
    goto fail;
  args[0] = SURFACE;
  if (!prv_send(COMPOSITOR, 0, args, 1, -1))
    goto fail;
  int memory = (int)syscall(SYS_memfd_create, "pebbleos-display", MFD_CLOEXEC);
  if (memory < 0)
    goto fail;
  if (ftruncate(memory, FRAME_BYTES * 2)) {
    close(memory);
    goto fail;
  }
  void *mapped = mmap(NULL, FRAME_BYTES * 2, PROT_READ | PROT_WRITE, MAP_SHARED, memory, 0);
  if (mapped == MAP_FAILED) {
    close(memory);
    goto fail;
  }
  s_pixels = mapped;
  uint32_t pool[] = {POOL, FRAME_BYTES * 2};
  bool sent = prv_send(SHM, 0, pool, 2, memory);
  close(memory);
  if (!sent)
    goto fail;
  for (unsigned i = 0; i < 2; i++) {
    uint32_t buffer[] = {BUFFER0 + i, i * FRAME_BYTES, EDGE, EDGE, EDGE * 4, 0};
    if (!prv_send(POOL, 0, buffer, 6, -1))
      goto fail;
  }
  uint32_t shell[] = {SHELL_SURFACE, SURFACE};
  if (!prv_send(SHELL, 0, shell, 2, -1) || !prv_string(SHELL_SURFACE, 8, "PebbleOS") ||
      !prv_string(SHELL_SURFACE, 9, "org.pebbleos.galaxywatch3") ||
      !prv_send(SHELL_SURFACE, 3, NULL, 0, -1))
    goto fail;
  if (s_policy_name) {
    if (!prv_bind(s_policy_name, "tizen_policy", POLICY))
      goto fail;
    uint32_t visibility[] = {VISIBILITY, SURFACE};
    if (!prv_send(POLICY, 0, visibility, 2, -1))
      goto fail;
    if (s_keyrouter_name && !prv_bind(s_keyrouter_name, "tizen_keyrouter", KEYROUTER))
      goto fail;
  }
  fprintf(stderr, "wayland: PebbleOS surface created (%dx%d)\n", EDGE, EDGE);
  return true;
fail:
  fprintf(stderr, "wayland init failed: %s\n", strerror(errno));
  wayland_linux_exit();
  return false;
}

bool wayland_linux_update(const uint8_t *pixels, int width, int height) {
  if (s_socket < 0 || !s_pixels || width <= 0 || height <= 0)
    return false;
  int slot = !s_busy[0] ? 0 : !s_busy[1] ? 1 : -1;
  if (slot < 0)
    return false;
  uint32_t *target = s_pixels + slot * EDGE * EDGE;
  for (unsigned y = 0; y < EDGE; y++) {
    for (unsigned x = 0; x < EDGE; x++) {
      uint8_t c = pixels[(size_t)y * height / EDGE * width + (size_t)x * width / EDGE];
      target[y * EDGE + x] =
          0xff000000u | (((c >> 4) & 3) * 85u << 16) | (((c >> 2) & 3) * 85u << 8) | (c & 3) * 85u;
    }
  }
  uint32_t attach[] = {BUFFER0 + slot, 0, 0};
  uint32_t damage[] = {0, 0, EDGE, EDGE};
  if (prv_send(SURFACE, 1, attach, 3, -1) && prv_send(SURFACE, 2, damage, 4, -1) &&
      prv_send(SURFACE, 6, NULL, 0, -1)) {
    s_busy[slot] = true;
    if (++s_frames == 1) {
      fprintf(stderr, "wayland: first PebbleOS frame committed\n");
      wayland_linux_activate();
      if (s_policy_name) {
        if (s_keyrouter_name) {
          // Tizen's keyrouter uses XKB keycodes (Linux evdev code + 8).
          const uint32_t keys[] = {116 + 8, 158 + 8};
          for (unsigned i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
            uint32_t grab[] = {SURFACE, keys[i], 4};
            prv_send(KEYROUTER, 0, grab, 3, -1);
          }
        }
      }
    }
    return true;
  }
  return false;
}

void wayland_linux_exit(void) {
  if (s_socket >= 0)
    close(s_socket);
  s_socket = -1;
  if (s_pixels)
    munmap(s_pixels, FRAME_BYTES * 2);
  s_pixels = NULL;
}
