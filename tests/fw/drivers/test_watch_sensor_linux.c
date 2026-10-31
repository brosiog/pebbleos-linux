/* SPDX-License-Identifier: Apache-2.0 */

#include "../../../drivers/imu/linux/watch_sensor_bottom.h"

#include <stdint.h>
#include <string.h>

#include <pbl/drivers/accel.h>
#include <pbl/drivers/linux_adapter.h>
#include <pbl/services/hrm/hrm_manager.h>

#include <clar.h>
#include <fake_mutex.h>
#include <fake_rtc.h>
#include <fake_system_task.h>

extern const struct pbl_accel pbl_accel_linux;

// Host half
static uint64_t s_boot_us;
uint64_t linux_watch_sensor_boottime_us(void) {
  return s_boot_us;
}
void linux_watch_sensor_config(unsigned interval_us, unsigned samples) {
}
void pbl_posix_irq_run(void (*isr)(void *), void *arg) {
  isr(arg);
}
static unsigned s_hrm_reports;
static HRMData s_hrm;
void hrm_manager_new_data_cb(const HRMData *data) {
  s_hrm_reports++;
  s_hrm = *data;
}

static int s_step_reports;
static uint16_t s_steps;
static uint32_t s_step_elapsed_ms;
void activity_handle_external_steps(uint16_t steps, uint32_t elapsed_ms) {
  s_step_reports++;
  s_steps = steps;
  s_step_elapsed_ms = elapsed_ms;
}

// Listener
#define MAX_RECORDED 2000
static struct {
  int16_t x;
  uint64_t timestamp_us;
} s_recorded[MAX_RECORDED];
static unsigned s_recorded_count, s_batches;
static uint32_t s_last_interval_us;

static void prv_samples(const struct pbl_accel *accel, const AccelRawBatch *batch,
                        void *user_data) {
  s_batches++;
  s_last_interval_us = batch->sampling_interval_us;
  cl_assert(batch->num_samples <= PBL_ADAPTER_SENSOR_SAMPLES);
  for (unsigned i = 0; i < batch->num_samples && s_recorded_count < MAX_RECORDED; i++) {
    int16_t raw;
    memcpy(&raw, batch->data + i * batch->stride + batch->axis[0].offset, sizeof(raw));
    s_recorded[s_recorded_count].x = raw * batch->axis[0].sign;
    s_recorded[s_recorded_count].timestamp_us =
        batch->first_timestamp_us + (uint64_t)i * batch->sampling_interval_us;
    s_recorded_count++;
  }
}

static const struct pbl_accel_listener s_listener = {.samples = prv_samples};

// Boot time 1000 s maps to the epoch time below.
#define BOOT_NOW_US 1000000000ull
#define EPOCH_NOW_S 1700000000

static void prv_send_accel(uint64_t first_boot_us, unsigned interval_us, unsigned count,
                           int16_t first_x, int16_t step_x) {
  struct pbl_adapter_sensor packet = {
    .type = PBL_ADAPTER_SENSOR_ACCEL,
    .count = count,
    .interval_us = interval_us,
    .timestamp_us = first_boot_us,
  };
  for (unsigned i = 0; i < count; i++) {
    packet.values[i][0] = first_x + step_x * (int)i;
    packet.values[i][2] = 1000;
  }
  linux_watch_sensor_received(&packet);
  fake_system_task_callbacks_invoke_pending();
}

void test_watch_sensor_linux__initialize(void) {
  s_boot_us = BOOT_NOW_US;
  fake_rtc_init(0, EPOCH_NOW_S);
  s_recorded_count = 0;
  s_batches = 0;
  s_step_reports = 0;
  s_hrm_reports = 0;
  cl_assert_equal_i(pbl_device_init(&pbl_accel_linux.dev), 0);
  cl_assert_equal_i(pbl_accel_set_listener(&pbl_accel_linux, &s_listener), 0);
  uint32_t actual;
  cl_assert_equal_i(pbl_accel_set_sampling_interval(&pbl_accel_linux, 40000, &actual), 0);
  cl_assert_equal_i(pbl_accel_set_num_samples(&pbl_accel_linux, 125), 0);
}

void test_watch_sensor_linux__cleanup(void) {
  pbl_accel_set_num_samples(&pbl_accel_linux, 0);
  // Forget the previous slow packet between tests.
  prv_send_accel(0, 40000, 1, 0, 0);
  pbl_accel_set_listener(&pbl_accel_linux, nullptr);
  fake_system_task_callbacks_cleanup();
  fake_mutex_reset(true /* assert_all_unlocked */);
}

void test_watch_sensor_linux__native_rate_passes_through(void) {
  prv_send_accel(BOOT_NOW_US - 5000000, 40000, 125, 0, 1);
  cl_assert_equal_i(s_recorded_count, 125);
  cl_assert_equal_i(s_last_interval_us, 40000);
  cl_assert_equal_i(s_recorded[124].x, 124);
  cl_assert(s_recorded[0].timestamp_us == (EPOCH_NOW_S - 5) * 1000000ull);
}

void test_watch_sensor_linux__slow_rate_is_interpolated(void) {
  // 5 Hz: 0, 100, 200 mg. The first sample starts the run; each later one adds 5 samples.
  prv_send_accel(BOOT_NOW_US - 1000000, 200000, 3, 0, 100);
  cl_assert_equal_i(s_recorded_count, 11);
  cl_assert_equal_i(s_last_interval_us, 40000);
  for (unsigned i = 0; i < 11; i++) {
    cl_assert_equal_i(s_recorded[i].x, 20 * i);
    cl_assert(s_recorded[i].timestamp_us == (EPOCH_NOW_S - 1) * 1000000ull + i * 40000ull);
  }
  // The next packet continues from the previous one, across the join.
  prv_send_accel(BOOT_NOW_US - 400000, 200000, 2, 300, 100);
  cl_assert_equal_i(s_recorded_count, 21);
  for (unsigned i = 11; i < 21; i++) {
    cl_assert_equal_i(s_recorded[i].x, 20 * i);
    cl_assert(s_recorded[i].timestamp_us == (EPOCH_NOW_S - 1) * 1000000ull + i * 40000ull);
  }
}

void test_watch_sensor_linux__gap_restarts_interpolation(void) {
  prv_send_accel(BOOT_NOW_US - 10000000, 200000, 2, 0, 100);
  cl_assert_equal_i(s_recorded_count, 6);
  // Three seconds later: nothing is invented across the gap.
  prv_send_accel(BOOT_NOW_US - 6800000, 200000, 2, 500, 100);
  cl_assert_equal_i(s_recorded_count, 12);
  cl_assert_equal_i(s_recorded[6].x, 500);
}

void test_watch_sensor_linux__large_slow_packet_is_chunked(void) {
  // 125 slow samples become 621 at the requested rate, reported in batches of at most 125.
  prv_send_accel(BOOT_NOW_US - 25000000, 200000, 125, 0, 5);
  cl_assert_equal_i(s_recorded_count, 621);
  cl_assert_equal_i(s_batches, 5);
  cl_assert_equal_i(s_recorded[620].x, 620);
  for (unsigned i = 1; i < 621; i++)
    cl_assert(s_recorded[i].timestamp_us - s_recorded[i - 1].timestamp_us == 40000);
}

void test_watch_sensor_linux__staleness_uses_newest_sample(void) {
  // First sample 40 s old, but the newest is recent: delivered.
  prv_send_accel(BOOT_NOW_US - 40000000, 320000, 125, 0, 1);
  cl_assert(s_recorded_count > 0);
  s_recorded_count = 0;
  // Everything older than 30 s: dropped.
  prv_send_accel(BOOT_NOW_US - 40000000, 40000, 125, 0, 1);
  cl_assert_equal_i(s_recorded_count, 0);
  // From the future: dropped.
  prv_send_accel(BOOT_NOW_US + 1000000, 40000, 1, 0, 0);
  cl_assert_equal_i(s_recorded_count, 0);
}

void test_watch_sensor_linux__not_subscribed(void) {
  pbl_accel_set_num_samples(&pbl_accel_linux, 0);
  prv_send_accel(BOOT_NOW_US - 1000000, 200000, 3, 0, 100);
  cl_assert_equal_i(s_recorded_count, 0);
}

static void prv_send_steps(int16_t steps, int16_t elapsed_s) {
  struct pbl_adapter_sensor packet = {
    .type = PBL_ADAPTER_SENSOR_STEPS,
    .count = 1,
    .interval_us = 1000000,
    .timestamp_us = BOOT_NOW_US,
  };
  packet.values[0][0] = steps;
  packet.values[0][1] = elapsed_s;
  linux_watch_sensor_received(&packet);
  fake_system_task_callbacks_invoke_pending();
}

void test_watch_sensor_linux__steps_reach_activity(void) {
  prv_send_steps(30, 15);
  cl_assert_equal_i(s_step_reports, 1);
  cl_assert_equal_i(s_steps, 30);
  cl_assert_equal_i(s_step_elapsed_ms, 15000);
  prv_send_steps(0, 0);
  cl_assert_equal_i(s_step_reports, 2);
  prv_send_steps(-1, 5);
  prv_send_steps(5, -1);
  cl_assert_equal_i(s_step_reports, 2);
  cl_assert_equal_i(s_recorded_count, 0);
}

static void prv_send_hrm(int16_t bpm, int16_t quality) {
  struct pbl_adapter_sensor packet = {
    .type = PBL_ADAPTER_SENSOR_HRM,
    .count = 1,
    .interval_us = 1000000,
    .timestamp_us = BOOT_NOW_US,
  };
  packet.values[0][0] = bpm;
  packet.values[0][1] = quality;
  linux_watch_sensor_received(&packet);
  fake_system_task_callbacks_invoke_pending();
}

void test_watch_sensor_linux__hrm_quality_arrives_in_pebble_units(void) {
  // The adapter normalizes vendor accuracy; the OS uses the quality as sent.
  prv_send_hrm(72, HRMQuality_Good);
  cl_assert_equal_i(s_hrm_reports, 1);
  cl_assert_equal_i(s_hrm.hrm_quality, HRMQuality_Good);
  cl_assert_equal_i(s_hrm.hrm_bpm, 72);
  prv_send_hrm(72, HRMQuality_Excellent);
  cl_assert_equal_i(s_hrm.hrm_quality, HRMQuality_Excellent);
}

void test_watch_sensor_linux__hrm_unusable_readings_carry_no_bpm(void) {
  prv_send_hrm(-3, HRMQuality_OffWrist);
  cl_assert_equal_i(s_hrm.hrm_quality, HRMQuality_OffWrist);
  cl_assert_equal_i(s_hrm.hrm_bpm, 0);
  prv_send_hrm(72, HRMQuality_Worst);
  cl_assert_equal_i(s_hrm.hrm_bpm, 0);
  prv_send_hrm(10, HRMQuality_Good);
  cl_assert_equal_i(s_hrm.hrm_bpm, 0);
  prv_send_hrm(300, HRMQuality_Good);
  cl_assert_equal_i(s_hrm.hrm_bpm, 0);
}

void test_watch_sensor_linux__hrm_quality_out_of_range_is_clamped(void) {
  prv_send_hrm(72, 40);
  cl_assert_equal_i(s_hrm.hrm_quality, HRMQuality_Excellent);
  prv_send_hrm(72, -40);
  cl_assert_equal_i(s_hrm.hrm_quality, HRMQuality_OffWrist);
}
