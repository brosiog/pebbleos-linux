/* SPDX-License-Identifier: Apache-2.0 */
#include "mic_linux_bottom.h"

#include <inttypes.h>
#include <string.h>

#include <pbl/drivers/mic.h>
#include <pbl/drivers/mic/linux/mic_definitions.h>
#include <pbl/kernel/assert.h>
#include <pbl/kernel/irq.h>
#include <pbl/logging/logging.h>
#include <pbl/services/system_task.h>
#include <pbl/util/circular_buffer.h>

#include <kernel/pbl_malloc.h>
#include <pbl_arch_posix.h>

PBL_LOG_MODULE_DEFINE(driver_mic_linux, CONFIG_DRIVER_MIC_LOG_LEVEL);

// Half a second of audio absorbs system task stalls while the phone link is busy.
#define CIRC_BUFFER_BYTES                   (MIC_SAMPLE_RATE * sizeof(int16_t) / 2)
#define MAX_FRAMES_PER_SYSTEM_TASK_CALLBACK 64

// The device being captured, for the callbacks that carry no context.
static MicDevice *s_device;

static void prv_dispatch(void *data) {
  MicDeviceState *state = s_device->state;
  pbl_mutex_lock(&state->mutex, PBL_FOREVER);
  const size_t frame_bytes = state->audio_buffer_len * sizeof(int16_t);
  int frames = 0;
  while (state->is_running && frames < MAX_FRAMES_PER_SYSTEM_TASK_CALLBACK) {
    pbl_irq_lock();
    bool ready = circular_buffer_get_read_space_remaining(&state->circ_buffer) >= frame_bytes;
    if (ready)
      circular_buffer_copy(&state->circ_buffer, state->audio_buffer, frame_bytes);
    pbl_irq_unlock();
    if (!ready)
      break;
    state->data_handler(state->audio_buffer, state->audio_buffer_len, state->handler_context);
    pbl_irq_lock();
    // A stop and restart inside the handler resets the buffer; never consume the new one.
    if (state->is_running && state->audio_buffer)
      circular_buffer_consume(&state->circ_buffer, frame_bytes);
    pbl_irq_unlock();
    frames++;
    system_task_watchdog_feed();
  }
  bool more = state->is_running &&
              circular_buffer_get_read_space_remaining(&state->circ_buffer) >= frame_bytes;
  state->main_pending = more && system_task_add_callback(prv_dispatch, nullptr);
  pbl_mutex_unlock(&state->mutex);
}

typedef struct {
  uint32_t generation;
  const int16_t *samples;
  size_t count;
} Chunk;

static void prv_received_isr(void *data) {
  const Chunk *chunk = data;
  if (!s_device) {
    return;
  }
  MicDeviceState *state = s_device->state;
  if (!state->is_running || chunk->generation != state->generation)
    return;
  const uint16_t bytes = chunk->count * sizeof(int16_t);
  if (!circular_buffer_write(&state->circ_buffer, chunk->samples, bytes))
    state->dropped_samples += chunk->count;
  const size_t frame_bytes = state->audio_buffer_len * sizeof(int16_t);
  if (!state->main_pending &&
      circular_buffer_get_read_space_remaining(&state->circ_buffer) >= frame_bytes) {
    state->main_pending = system_task_add_callback_from_isr_droppable(prv_dispatch, nullptr);
  }
}

void linux_mic_received(uint32_t generation, const int16_t *samples, size_t count) {
  Chunk chunk = {.generation = generation, .samples = samples, .count = count};
  pbl_posix_irq_run(prv_received_isr, &chunk);
}

void mic_init(MicDevice *this) {
  PBL_ASSERTN(this && this->state);
  MicDeviceState *state = this->state;
  if (state->is_initialized)
    return;
  pbl_mutex_init(&state->mutex);
  state->is_initialized = true;
}

void mic_set_volume(MicDevice *this, uint16_t volume) {
  // Samsung's voice capture path applies its own gain.
}

bool mic_start(MicDevice *this, MicDataHandlerCB data_handler, void *context, int16_t *audio_buffer,
               size_t audio_buffer_len) {
  PBL_ASSERTN(this && this->state && data_handler && audio_buffer);
  PBL_ASSERTN(audio_buffer_len > 0 && audio_buffer_len * sizeof(int16_t) <= CIRC_BUFFER_BYTES);
  MicDeviceState *state = this->state;
  PBL_ASSERTN(state->is_initialized);
  pbl_mutex_lock(&state->mutex, PBL_FOREVER);
  if (state->is_running) {
    pbl_mutex_unlock(&state->mutex);
    return false;
  }
  state->circ_buffer_storage = kernel_malloc(CIRC_BUFFER_BYTES);
  if (!state->circ_buffer_storage) {
    pbl_mutex_unlock(&state->mutex);
    return false;
  }
  circular_buffer_init(&state->circ_buffer, state->circ_buffer_storage, CIRC_BUFFER_BYTES);
  state->data_handler = data_handler;
  state->handler_context = context;
  state->audio_buffer = audio_buffer;
  state->audio_buffer_len = audio_buffer_len;
  state->dropped_samples = 0;
  state->main_pending = false;
  s_device = this;
  if (++state->generation == 0)
    state->generation = 1;
  pbl_irq_lock();
  state->is_running = true;
  pbl_irq_unlock();
  if (!linux_mic_open(state->generation)) {
    pbl_irq_lock();
    state->is_running = false;
    pbl_irq_unlock();
    kernel_free(state->circ_buffer_storage);
    state->circ_buffer_storage = nullptr;
    pbl_mutex_unlock(&state->mutex);
    return false;
  }
  PBL_LOG_DBG("Microphone started");
  pbl_mutex_unlock(&state->mutex);
  return true;
}

void mic_stop(MicDevice *this) {
  PBL_ASSERTN(this && this->state);
  MicDeviceState *state = this->state;
  pbl_mutex_lock(&state->mutex, PBL_FOREVER);
  if (!state->is_running) {
    pbl_mutex_unlock(&state->mutex);
    return;
  }
  pbl_irq_lock();
  state->is_running = false;
  pbl_irq_unlock();
  linux_mic_close(state->generation);
  if (state->dropped_samples)
    PBL_LOG_WRN("Microphone dropped %" PRIu32 " samples", state->dropped_samples);
  kernel_free(state->circ_buffer_storage);
  state->circ_buffer_storage = nullptr;
  state->data_handler = nullptr;
  state->handler_context = nullptr;
  state->audio_buffer = nullptr;
  state->audio_buffer_len = 0;
  state->main_pending = false;
  PBL_LOG_DBG("Microphone stopped");
  pbl_mutex_unlock(&state->mutex);
}

bool mic_is_running(MicDevice *this) {
  PBL_ASSERTN(this && this->state);
  return this->state->is_running;
}

uint32_t mic_get_channels(MicDevice *this) {
  PBL_ASSERTN(this);
  return this->channels ? this->channels : 1;
}
