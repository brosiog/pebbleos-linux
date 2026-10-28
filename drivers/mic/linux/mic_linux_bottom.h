/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LINUX_MIC_CHUNK_SAMPLES 320

//! Start a capture thread for @p generation; false if the host has no audio library.
bool linux_mic_open(uint32_t generation);
//! Ask the thread for @p generation to exit. It never blocks: the thread may be waiting for the
//! CPU the caller holds, so it notices on its next chunk and closes the stream itself.
void linux_mic_close(uint32_t generation);
//! Called by the capture thread, outside the firmware, for every captured chunk.
void linux_mic_received(uint32_t generation, const int16_t *samples, size_t count);
