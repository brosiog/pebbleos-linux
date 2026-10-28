/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include <pbl/drivers/mic.h>
#include <pbl/kernel/mutex.h>
#include <pbl/util/circular_buffer.h>

/**
 * @defgroup drivers_mic_linux Linux watch microphone
 * @ingroup drivers_mic
 * @brief @ref drivers_mic implementation capturing through the host's PulseAudio server.
 * @{
 */

/** @cond INTERNAL_HIDDEN */
typedef struct {
  struct pbl_mutex mutex;
  CircularBuffer circ_buffer;
  uint8_t *circ_buffer_storage;
  MicDataHandlerCB data_handler;
  void *handler_context;
  int16_t *audio_buffer;
  size_t audio_buffer_len;
  uint32_t generation;
  uint32_t dropped_samples;
  bool main_pending;
  bool is_initialized;
  bool is_running;
} MicDeviceState;
/** @endcond */

/** @brief Linux watch microphone device. */
typedef const struct MicDevice {
  /** Driver runtime state. */
  MicDeviceState *state;
  /** Number of channels. */
  uint32_t channels;
} MicDevice;

/** @} */
