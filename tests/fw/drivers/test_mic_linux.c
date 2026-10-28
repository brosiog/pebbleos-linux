/* SPDX-License-Identifier: Apache-2.0 */

#include <stdint.h>
#include <string.h>

#include <pbl/drivers/mic.h>
#include <pbl/drivers/mic/linux/mic_definitions.h>

#include <clar.h>
#include <fake_kernel_malloc.h>
#include <fake_mutex.h>
#include <fake_system_task.h>

#include "../../../drivers/mic/linux/mic_linux_bottom.h"

static MicDeviceState s_state;
static const struct MicDevice s_mic = {.state = &s_state, .channels = 1};
#undef MIC
#define MIC (&s_mic)

// Host half
static int s_opens, s_closes;
static uint32_t s_open_generation, s_close_generation;
static bool s_open_fails;

bool linux_mic_open(uint32_t generation) {
  s_opens++;
  s_open_generation = generation;
  return !s_open_fails;
}

void linux_mic_close(uint32_t generation) {
  s_closes++;
  s_close_generation = generation;
}

void pbl_posix_irq_run(void (*isr)(void *), void *arg) {
  isr(arg);
}

// Consumer
#define MAX_RECORDED 20000
static int16_t s_recorded[MAX_RECORDED];
static size_t s_recorded_count;
static int s_frames;
static size_t s_last_frame_len;
static void *s_last_context;

static void prv_handler(int16_t *samples, size_t count, void *context) {
  s_frames++;
  s_last_frame_len = count;
  s_last_context = context;
  for (size_t i = 0; i < count && s_recorded_count < MAX_RECORDED; i++) {
    s_recorded[s_recorded_count++] = samples[i];
  }
}

static int16_t s_next_sample;

static void prv_chunk(uint32_t generation, size_t count) {
  int16_t chunk[LINUX_MIC_CHUNK_SAMPLES];
  for (size_t i = 0; i < count; i++) {
    chunk[i] = s_next_sample++;
  }
  linux_mic_received(generation, chunk, count);
}

static void prv_assert_sequential(int16_t first) {
  for (size_t i = 0; i < s_recorded_count; i++) {
    cl_assert_equal_i(s_recorded[i], (int16_t)(first + i));
  }
}

static int16_t s_frame[480];

void test_mic_linux__initialize(void) {
  memset(&s_state, 0, sizeof(s_state));
  s_opens = s_closes = 0;
  s_open_generation = s_close_generation = 0;
  s_open_fails = false;
  s_recorded_count = 0;
  s_frames = 0;
  s_last_frame_len = 0;
  s_next_sample = 0;
  fake_kernel_malloc_init();
  fake_kernel_malloc_enable_stats(true);
  mic_init(MIC);
  fake_kernel_malloc_mark();
}

void test_mic_linux__cleanup(void) {
  mic_stop(MIC);
  fake_system_task_callbacks_cleanup();
  fake_kernel_malloc_mark_assert_equal();
  fake_kernel_malloc_deinit();
  fake_mutex_reset(true /* assert_all_unlocked */);
}

void test_mic_linux__start_stop(void) {
  cl_assert(!mic_is_running(MIC));
  cl_assert(mic_start(MIC, prv_handler, (void *)0x1234, s_frame, 320));
  cl_assert(mic_is_running(MIC));
  cl_assert_equal_i(s_opens, 1);
  cl_assert(!mic_start(MIC, prv_handler, nullptr, s_frame, 320));
  cl_assert_equal_i(s_opens, 1);
  mic_stop(MIC);
  cl_assert(!mic_is_running(MIC));
  cl_assert_equal_i(s_closes, 1);
  cl_assert_equal_i(s_close_generation, s_open_generation);
  mic_stop(MIC);
  cl_assert_equal_i(s_closes, 1);
  cl_assert_equal_i(mic_get_channels(MIC), 1);
}

void test_mic_linux__frames_in_order(void) {
  cl_assert(mic_start(MIC, prv_handler, (void *)0x1234, s_frame, 320));
  for (int i = 0; i < 10; i++) {
    prv_chunk(s_open_generation, LINUX_MIC_CHUNK_SAMPLES);
  }
  fake_system_task_callbacks_invoke_pending();
  cl_assert_equal_i(s_frames, 10);
  cl_assert_equal_i(s_last_frame_len, 320);
  cl_assert_equal_p(s_last_context, (void *)0x1234);
  cl_assert_equal_i(s_recorded_count, 3200);
  prv_assert_sequential(0);
}

void test_mic_linux__frame_sizes_not_matching_chunks(void) {
  // 480-sample frames from 320-sample chunks: every 3 chunks make 2 frames.
  cl_assert(mic_start(MIC, prv_handler, nullptr, s_frame, 480));
  for (int i = 0; i < 9; i++) {
    prv_chunk(s_open_generation, LINUX_MIC_CHUNK_SAMPLES);
    fake_system_task_callbacks_invoke_pending();
  }
  cl_assert_equal_i(s_frames, 6);
  prv_assert_sequential(0);
  mic_stop(MIC);
  // Odd sizes and short chunks.
  s_recorded_count = 0;
  s_frames = 0;
  int16_t first = s_next_sample;
  cl_assert(mic_start(MIC, prv_handler, nullptr, s_frame, 7));
  for (int i = 0; i < 50; i++) {
    prv_chunk(s_open_generation, 1 + i % 13);
  }
  fake_system_task_callbacks_invoke_pending();
  size_t total = 0;
  for (int i = 0; i < 50; i++) total += 1 + i % 13;
  cl_assert_equal_i(s_frames, total / 7);
  for (size_t i = 0; i < s_recorded_count; i++) {
    cl_assert_equal_i(s_recorded[i], (int16_t)(first + i));
  }
}

void test_mic_linux__stale_generation_ignored(void) {
  cl_assert(mic_start(MIC, prv_handler, nullptr, s_frame, 320));
  uint32_t first = s_open_generation;
  prv_chunk(first + 1, LINUX_MIC_CHUNK_SAMPLES);
  prv_chunk(0, LINUX_MIC_CHUNK_SAMPLES);
  fake_system_task_callbacks_invoke_pending();
  cl_assert_equal_i(s_frames, 0);
  mic_stop(MIC);
  cl_assert(mic_start(MIC, prv_handler, nullptr, s_frame, 320));
  cl_assert(s_open_generation != first);
  prv_chunk(first, LINUX_MIC_CHUNK_SAMPLES);  // the old thread's last chunk
  fake_system_task_callbacks_invoke_pending();
  cl_assert_equal_i(s_frames, 0);
  prv_chunk(s_open_generation, LINUX_MIC_CHUNK_SAMPLES);
  fake_system_task_callbacks_invoke_pending();
  cl_assert_equal_i(s_frames, 1);
}

void test_mic_linux__no_handler_after_stop(void) {
  cl_assert(mic_start(MIC, prv_handler, nullptr, s_frame, 320));
  prv_chunk(s_open_generation, LINUX_MIC_CHUNK_SAMPLES);
  prv_chunk(s_open_generation, LINUX_MIC_CHUNK_SAMPLES);
  // The dispatch is queued but stop comes first.
  mic_stop(MIC);
  prv_chunk(s_open_generation, LINUX_MIC_CHUNK_SAMPLES);
  fake_system_task_callbacks_invoke_pending();
  cl_assert_equal_i(s_frames, 0);
}

void test_mic_linux__restart_with_dispatch_pending(void) {
  cl_assert(mic_start(MIC, prv_handler, nullptr, s_frame, 320));
  prv_chunk(s_open_generation, LINUX_MIC_CHUNK_SAMPLES);
  mic_stop(MIC);
  cl_assert(mic_start(MIC, prv_handler, nullptr, s_frame, 320));
  int16_t first = s_next_sample;
  prv_chunk(s_open_generation, LINUX_MIC_CHUNK_SAMPLES);
  fake_system_task_callbacks_invoke_pending();
  cl_assert_equal_i(s_frames, 1);
  cl_assert_equal_i(s_recorded[0], first);
}

void test_mic_linux__overflow_drops_whole_chunks(void) {
  cl_assert(mic_start(MIC, prv_handler, nullptr, s_frame, 320));
  // Two seconds with the system task stalled; the ring holds half a second.
  system_task_set_available_space(0);
  for (int i = 0; i < 100; i++) {
    prv_chunk(s_open_generation, LINUX_MIC_CHUNK_SAMPLES);
  }
  system_task_set_available_space(~(uint32_t)0);
  prv_chunk(s_open_generation, LINUX_MIC_CHUNK_SAMPLES);
  fake_system_task_callbacks_invoke_pending();
  cl_assert(s_frames > 0 && s_frames <= 26);
  cl_assert(s_state.dropped_samples > 0);
  // Frames stay chunk-aligned: each starts at a multiple of the chunk size.
  for (int f = 0; f < s_frames; f++) {
    cl_assert_equal_i(s_recorded[f * 320] % LINUX_MIC_CHUNK_SAMPLES, 0);
  }
  // Recovery: new chunks flow again.
  int before = s_frames;
  prv_chunk(s_open_generation, LINUX_MIC_CHUNK_SAMPLES);
  fake_system_task_callbacks_invoke_pending();
  cl_assert_equal_i(s_frames, before + 1);
}

void test_mic_linux__long_backlog_is_rescheduled(void) {
  cl_assert(mic_start(MIC, prv_handler, nullptr, s_frame, 80));
  // 24 chunks make 96 frames, more than one dispatch handles at once.
  for (int i = 0; i < 24; i++) {
    prv_chunk(s_open_generation, LINUX_MIC_CHUNK_SAMPLES);
  }
  fake_system_task_callbacks_invoke(1);
  cl_assert_equal_i(s_frames, 64);
  fake_system_task_callbacks_invoke_pending();
  cl_assert_equal_i(s_frames, 96);
  prv_assert_sequential(0);
}

void test_mic_linux__open_failure(void) {
  s_open_fails = true;
  cl_assert(!mic_start(MIC, prv_handler, nullptr, s_frame, 320));
  cl_assert(!mic_is_running(MIC));
  prv_chunk(s_open_generation, LINUX_MIC_CHUNK_SAMPLES);
  fake_system_task_callbacks_invoke_pending();
  cl_assert_equal_i(s_frames, 0);
  s_open_fails = false;
  cl_assert(mic_start(MIC, prv_handler, nullptr, s_frame, 320));
}

void test_mic_linux__out_of_memory(void) {
  fake_kernel_malloc_set_largest_free_block(100);
  cl_assert(!mic_start(MIC, prv_handler, nullptr, s_frame, 320));
  cl_assert(!mic_is_running(MIC));
  cl_assert_equal_i(s_opens, 0);
  fake_kernel_malloc_set_largest_free_block(~0);
}

void test_mic_linux__many_cycles_do_not_leak(void) {
  for (int i = 0; i < 500; i++) {
    cl_assert(mic_start(MIC, prv_handler, nullptr, s_frame, 320));
    prv_chunk(s_open_generation, (i % 3) * 160);
    if (i % 2) {
      fake_system_task_callbacks_invoke_pending();
    }
    mic_stop(MIC);
  }
  fake_system_task_callbacks_invoke_pending();
  cl_assert_equal_i(s_opens, 500);
  cl_assert_equal_i(s_closes, 500);
}
