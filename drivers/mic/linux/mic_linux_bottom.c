/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include "mic_linux_bottom.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

// Tizen's PulseAudio server grants the recorder privilege to the supervised UI's root/User
// context, so the firmware records directly with libpulse-simple, loaded only when needed.
typedef struct {
  int format;
  uint32_t rate;
  uint8_t channels;
} pa_spec;

static void *(*s_new)(const char *, const char *, int, const char *, const char *,
                      const pa_spec *, const void *, const void *, int *);
static int (*s_read)(void *, void *, size_t, int *);
static void (*s_free)(void *);
static const char *(*s_strerror)(int);
static _Atomic uint32_t s_active;

static bool prv_load(void) {
  if (s_new)
    return true;
  void *simple = dlopen("libpulse-simple.so.0", RTLD_NOW);
  void *core = dlopen("libpulse.so.0", RTLD_NOW);
  if (!simple || !core) {
    fprintf(stderr, "mic: PulseAudio unavailable\n");
    return false;
  }
  s_read = dlsym(simple, "pa_simple_read");
  s_free = dlsym(simple, "pa_simple_free");
  s_strerror = dlsym(core, "pa_strerror");
  s_new = dlsym(simple, "pa_simple_new");
  return s_new && s_read && s_free && s_strerror;
}

static void *prv_capture(void *arg) {
  uint32_t generation = (uint32_t)(uintptr_t)arg;
  const pa_spec spec = {.format = 3 /* PA_SAMPLE_S16LE */, .rate = 16000, .channels = 1};
  int error = 0;
  void *stream = s_new(NULL, "pebble", 2 /* PA_STREAM_RECORD */, NULL, "voice", &spec, NULL,
                       NULL, &error);
  if (!stream) {
    fprintf(stderr, "mic: open failed: %s\n", s_strerror(error));
    return NULL;
  }
  fprintf(stderr, "mic: capturing (session %u)\n", (unsigned)generation);
  int16_t chunk[LINUX_MIC_CHUNK_SAMPLES];
  while (atomic_load(&s_active) == generation) {
    if (s_read(stream, chunk, sizeof(chunk), &error) < 0) {
      fprintf(stderr, "mic: read failed: %s\n", s_strerror(error));
      break;
    }
    linux_mic_received(generation, chunk, LINUX_MIC_CHUNK_SAMPLES);
  }
  s_free(stream);
  fprintf(stderr, "mic: stopped (session %u)\n", (unsigned)generation);
  return NULL;
}

bool linux_mic_open(uint32_t generation) {
  if (!prv_load())
    return false;
  atomic_store(&s_active, generation);
  pthread_t thread;
  if (pthread_create(&thread, NULL, prv_capture, (void *)(uintptr_t)generation)) {
    atomic_store(&s_active, 0);
    return false;
  }
  pthread_detach(thread);
  return true;
}

void linux_mic_close(uint32_t generation) {
  uint32_t expected = generation;
  atomic_compare_exchange_strong(&s_active, &expected, 0);
}
