/* SPDX-License-Identifier: Apache-2.0 */
#include "watch_sensor_bottom.h"

#include <errno.h>
#include <string.h>

#include <pbl/drivers/accel.h>
#include <pbl/drivers/rtc.h>
#include <pbl/kernel/irq.h>
#include <pbl/services/activity/activity.h>
#include <pbl/services/hrm/hrm_manager.h>
#include <pbl/services/system_task.h>

#include <pbl_arch_posix.h>

static unsigned s_interval_us = 40000, s_samples;
static AccelDriverSample s_latest;
static bool s_rotated;
static struct {
  bool busy;
  struct pbl_adapter_sensor packet;
} s_slots[4];
// Last sample of the previous slow packet (CLOCK_BOOTTIME), for interpolating across packets.
static struct {
  bool valid;
  uint64_t timestamp_us;
  int16_t values[3];
} s_previous;
static int16_t s_upsampled[PBL_ADAPTER_SENSOR_SAMPLES][3];
extern const struct pbl_accel pbl_accel_linux;
static void prv_report(const int16_t (*values)[3], unsigned count, uint64_t first_epoch_us,
                       unsigned interval_us) {
  int8_t rotate = s_rotated ? -1 : 1;
  AccelRawBatch batch = {
    .data = (const uint8_t *)values,
    .num_samples = count,
    .stride = 6,
    .axis =
        {{.offset = 0, .sign = rotate},
         {.offset = 2, .sign = rotate},
         {.offset = 4, .sign = 1}},
    .scale_num = 1,
    .scale_den = 1,
    .first_timestamp_us = first_epoch_us,
    .sampling_interval_us = interval_us
  };
  pbl_accel_report_samples(&pbl_accel_linux, &batch);
}
// The adapter slows the accelerometer while the screen is off. The activity algorithm assumes its
// requested rate, so slower packets are linearly interpolated up to it, joins included.
static void prv_report_upsampled(const struct pbl_adapter_sensor *p, unsigned factor,
                                 int64_t epoch_offset_us) {
  unsigned out = 0;
  uint64_t first = 0;
  for (unsigned i = 0; i < p->count; i++) {
    uint64_t t = p->timestamp_us + (uint64_t)i * p->interval_us;
    bool joined = s_previous.valid && t > s_previous.timestamp_us &&
                  t - s_previous.timestamp_us < p->interval_us + p->interval_us / 2;
    for (unsigned k = joined ? 1 : factor; k <= factor; k++) {
      if (!out)
        first = t - (uint64_t)(factor - k) * s_interval_us;
      for (unsigned axis = 0; axis < 3; axis++) {
        int from = joined ? s_previous.values[axis] : p->values[i][axis];
        s_upsampled[out][axis] = from + (p->values[i][axis] - from) * (int)k / (int)factor;
      }
      if (++out == PBL_ADAPTER_SENSOR_SAMPLES) {
        prv_report(s_upsampled, out, first + epoch_offset_us, s_interval_us);
        out = 0;
      }
    }
    s_previous.valid = true;
    s_previous.timestamp_us = t;
    memcpy(s_previous.values, p->values[i], sizeof(s_previous.values));
  }
  if (out)
    prv_report(s_upsampled, out, first + epoch_offset_us, s_interval_us);
}
static void prv_deliver(void *data) {
  typeof(s_slots[0]) *slot = data;
  const struct pbl_adapter_sensor *p = &slot->packet;
  uint64_t boot = linux_watch_sensor_boottime_us();
  uint64_t last = p->timestamp_us + (uint64_t)(p->count - 1) * p->interval_us;
  if (last > boot || boot - last > 30000000) {
    s_previous.valid = false;
    goto done;
  }
  time_t seconds;
  uint16_t ms;
  rtc_get_time_ms(&seconds, &ms);
  uint64_t epoch = (uint64_t)seconds * 1000000 + (uint64_t)ms * 1000;
  int64_t epoch_offset = (int64_t)(epoch - boot);
  uint64_t timestamp = p->timestamp_us + epoch_offset;
  if (p->type == PBL_ADAPTER_SENSOR_ACCEL) {
    pbl_accel_lock(&pbl_accel_linux);
    int8_t rotate = s_rotated ? -1 : 1;
    s_latest = (AccelDriverSample){
      .timestamp_us = last + epoch_offset,
      .x = p->values[p->count - 1][0] * rotate,
      .y = p->values[p->count - 1][1] * rotate,
      .z = p->values[p->count - 1][2]
    };
    unsigned factor = p->interval_us / s_interval_us;
    if (s_samples && factor >= 2 && factor <= 10 && p->interval_us % s_interval_us == 0) {
      prv_report_upsampled(p, factor, epoch_offset);
    } else {
      s_previous.valid = false;
      if (s_samples)
        prv_report(p->values, p->count, timestamp, p->interval_us);
    }
    pbl_accel_unlock(&pbl_accel_linux);
  } else if (p->type == PBL_ADAPTER_SENSOR_STEPS) {
    if (p->values[0][0] >= 0 && p->values[0][1] >= 0)
      activity_handle_external_steps(p->values[0][0], (uint32_t)p->values[0][1] * 1000);
  } else if (p->type == PBL_ADAPTER_SENSOR_HRM) {
    int bpm = p->values[0][0];
    int quality = p->values[0][1];
    quality = quality < HRMQuality_OffWrist ? HRMQuality_OffWrist
              : quality > HRMQuality_Excellent ? HRMQuality_Excellent : quality;
    static_assert(HRMQuality_OffWrist == -1 && HRMQuality_Worst == 0 &&
                  HRMQuality_Acceptable == 2 && HRMQuality_Good == 3 && HRMQuality_Excellent == 4);
    HRMData hrm = {
      .features = HRMFeature_BPM,
      .hrm_quality = quality
    };
    if (bpm >= 25 && bpm <= 240 && hrm.hrm_quality > HRMQuality_Worst)
      hrm.hrm_bpm = bpm;
    hrm_manager_new_data_cb(&hrm);
  }
done:
  pbl_irq_lock();
  slot->busy = false;
  pbl_irq_unlock();
}
static void prv_received_isr(void *data) {
  for (unsigned i = 0; i < 4; i++) {
    if (s_slots[i].busy)
      continue;
    s_slots[i].busy = true;
    s_slots[i].packet = *(const struct pbl_adapter_sensor *)data;
    if (!system_task_add_callback_from_isr_droppable(prv_deliver, &s_slots[i]))
      s_slots[i].busy = false;
    break;
  }
}
void linux_watch_sensor_received(const struct pbl_adapter_sensor *packet) {
  pbl_posix_irq_run(prv_received_isr, (void *)packet);
}
static int prv_init(const struct pbl_accel *accel) {
  (void)accel;
  return 0;
}
static int prv_rate(const struct pbl_accel *accel, uint32_t us, uint32_t *actual) {
  (void)accel;
  if (us < 10000 || us > 1000000)
    return -EINVAL;
  s_interval_us = (us + 999) / 1000 * 1000;
  *actual = s_interval_us;
  linux_watch_sensor_config(s_interval_us, s_samples);
  return 0;
}
static uint32_t prv_get_rate(const struct pbl_accel *accel) {
  (void)accel;
  return s_interval_us;
}
static int prv_batch(const struct pbl_accel *accel, uint32_t samples) {
  (void)accel;
  if (samples > PBL_ADAPTER_SENSOR_SAMPLES)
    return -EINVAL;
  s_samples = samples;
  linux_watch_sensor_config(s_interval_us, s_samples);
  return 0;
}
static uint32_t prv_max(const struct pbl_accel *accel) {
  (void)accel;
  return PBL_ADAPTER_SENSOR_SAMPLES;
}
static int prv_peek(const struct pbl_accel *accel, AccelDriverSample *sample) {
  (void)accel;
  if (!s_latest.timestamp_us)
    return -ENODATA;
  *sample = s_latest;
  return 0;
}
static int prv_rotate(const struct pbl_accel *accel, bool rotated) {
  (void)accel;
  s_rotated = rotated;
  return 0;
}
static const struct pbl_accel_ops s_ops = {
  .init = prv_init,
  .set_sampling_interval = prv_rate,
  .get_sampling_interval = prv_get_rate,
  .set_num_samples = prv_batch,
  .get_max_num_samples = prv_max,
  .peek = prv_peek,
  .set_rotated = prv_rotate
};
PBL_ACCEL_STATE_DEFINE(pbl_accel_linux);
const struct pbl_accel pbl_accel_linux = PBL_ACCEL_INIT(pbl_accel_linux, "accel", nullptr, &s_ops);
PBL_DEVICE_REGISTER(pbl_accel_linux, &pbl_accel_linux.dev);

#ifdef CONFIG_SHELL
#include <pbl/services/activity/activity.h>
#include <pbl/shell/shell.h>
static int prv_health_status(const struct pbl_shell *sh, size_t argc, char **argv) {
  (void)argc;
  (void)argv;
  int32_t steps = 0;
  bool valid = activity_get_metric(ActivityMetricStepCount, 1, &steps);
  pbl_shell_print(sh, "tracking=%d preference=%d steps=%ld valid=%d hr_enabled=%d hr_interval=%d",
                  activity_tracking_on(), activity_prefs_tracking_is_enabled(), (long)steps, valid,
                  activity_prefs_heart_rate_is_enabled(),
                  activity_prefs_get_hrm_measurement_interval());
  int32_t bpm = 0;
  bool hr_valid = activity_get_metric(ActivityMetricHeartRateRawBPM, 1, &bpm);
  pbl_shell_print(sh, "heart_rate=%ld valid=%d", (long)bpm, hr_valid);
  return 0;
}
static int prv_health_start(const struct pbl_shell *sh, size_t argc, char **argv) {
  (void)sh;
  (void)argc;
  (void)argv;
  activity_prefs_tracking_set_enabled(true);
  activity_set_enabled(true);
  return activity_start_tracking(false) ? 0 : -EIO;
}
static int prv_health_stop(const struct pbl_shell *sh, size_t argc, char **argv) {
  (void)sh;
  (void)argc;
  (void)argv;
  activity_prefs_tracking_set_enabled(false);
  return activity_stop_tracking() ? 0 : -EIO;
}
static const struct pbl_shell_cmd s_health_commands[] = {
  PBL_SHELL_CMD(status, nullptr, "Show real sensor tracking state", prv_health_status),
  PBL_SHELL_CMD(start, nullptr, "Enable activity tracking", prv_health_start),
  PBL_SHELL_CMD(stop, nullptr, "Disable activity tracking", prv_health_stop),
  PBL_SHELL_SUBCMD_SET_END,
};
PBL_SHELL_CMD_REGISTER(health, s_health_commands, "Samsung sensor integration", nullptr);
#endif
