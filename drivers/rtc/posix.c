/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#define _GNU_SOURCE
#include <string.h>
#include <time.h>

#include <pbl/drivers/rtc.h>
#ifdef CONFIG_LINUX_WATCH
#include <pbl/drivers/linux_adapter_client.h>
#endif
#include <pbl/services/time.h>

#include <sys/time.h>

// Wall time follows the host clock, plus whatever offset the firmware sets.

#define NUM_BACKUP_REGS 16

static int64_t s_offset_s;
static uint32_t s_backup[NUM_BACKUP_REGS];
static uint64_t s_boot_us;
static TimezoneInfo s_tzinfo;
static bool s_tz_set;

static uint64_t prv_ticks_us(void) {
  struct timespec now;
#ifdef __linux__
  clock_gettime(CLOCK_BOOTTIME, &now);
#else
  clock_gettime(CLOCK_MONOTONIC, &now);
#endif
  return (uint64_t)now.tv_sec * 1000000 + now.tv_nsec / 1000;
}

#ifdef CONFIG_LINUX_WATCH
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#include <fcntl.h>
#include <unistd.h>

typedef struct {
  uint32_t magic, size;
  int64_t offset_s;
  char tm_zone[PBL_TZ_LEN - 1];
  uint8_t dst_id;
  int16_t timezone_id;
  int32_t tm_gmtoff;
  uint32_t reserved;
  int64_t dst_start, dst_end;
  uint32_t backup[NUM_BACKUP_REGS];
  uint32_t timezone_set, checksum;
} RtcState;
static_assert(sizeof(RtcState) == 120 && offsetof(RtcState, checksum) == 116,
              "RTC wire format must be independent of the host ABI");
// Version 1 stored the host's 32-bit time_t directly in TimezoneInfo.
typedef struct {
  uint32_t magic, size;
  int64_t offset_s;
  char tm_zone[PBL_TZ_LEN - 1];
  uint8_t dst_id;
  int16_t timezone_id;
  int32_t tm_gmtoff, dst_start, dst_end;
  uint32_t backup[NUM_BACKUP_REGS];
  uint32_t timezone_set, checksum;
} RtcStateLegacy32;
static_assert(sizeof(RtcStateLegacy32) == 112 && offsetof(RtcStateLegacy32, checksum) == 104,
              "Legacy ARM32 RTC layout changed");
// Where the clock and time zone persist, or NULL for nowhere. PBL_RTC_STATE overrides; a Linux
// watch keeps it in its data directory.
static const char *prv_state_path(void) {
  const char *path = getenv("PBL_RTC_STATE");
  if (path)
    return path;
#ifdef CONFIG_LINUX_WATCH
  static char s_path[256];
  if (!s_path[0])
    snprintf(s_path, sizeof(s_path), "%s/rtc-state.bin", linux_adapter_data_dir());
  return s_path;
#else
  return NULL;
#endif
}
static uint32_t prv_checksum(const void *state, size_t length) {
  uint32_t result = 2166136261u;
  const uint8_t *bytes = (const void *)state;
  for (size_t i = 0; i < length; i++)
    result = (result ^ bytes[i]) * 16777619u;
  return result;
}
static void prv_save(void) {
  RtcState state = {0};
  state.magic = 0x52544332;
  state.size = sizeof(state);
  state.offset_s = s_offset_s;
  memcpy(state.tm_zone, s_tzinfo.tm_zone, sizeof(state.tm_zone));
  state.dst_id = s_tzinfo.dst_id;
  state.timezone_id = s_tzinfo.timezone_id;
  state.tm_gmtoff = s_tzinfo.tm_gmtoff;
  state.dst_start = s_tzinfo.dst_start;
  state.dst_end = s_tzinfo.dst_end;
  state.timezone_set = s_tz_set;
  memcpy(state.backup, s_backup, sizeof(s_backup));
  state.checksum = prv_checksum(&state, offsetof(RtcState, checksum));
  const char *destination = prv_state_path();
  if (!destination)
    return;
  char temporary[4096];
  int length = snprintf(temporary, sizeof(temporary), "%s.new", destination);
  if (length < 0 || (size_t)length >= sizeof(temporary))
    return;
  int fd = open(temporary, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (fd < 0)
    return;
  bool ok = write(fd, &state, sizeof(state)) == sizeof(state) && !fsync(fd);
  close(fd);
  if (!ok || rename(temporary, destination))
    unlink(temporary);
}
static bool prv_restore(void) {
  uint8_t bytes[sizeof(RtcState) + 1];
  RtcState state = {0};
  const char *source = prv_state_path();
  int fd = source ? open(source, O_RDONLY | O_CLOEXEC | O_NOFOLLOW) : -1;
  if (fd < 0)
    return false;
  ssize_t count = read(fd, bytes, sizeof(bytes));
  close(fd);
  if (count == sizeof(RtcStateLegacy32)) {
    RtcStateLegacy32 old;
    memcpy(&old, bytes, sizeof(old));
    if (old.magic != 0x52544331 || old.size != sizeof(old) ||
        old.checksum != prv_checksum(&old, offsetof(RtcStateLegacy32, checksum)))
      return false;
    state.offset_s = old.offset_s;
    memcpy(state.tm_zone, old.tm_zone, sizeof(state.tm_zone));
    state.dst_id = old.dst_id;
    state.timezone_id = old.timezone_id;
    state.tm_gmtoff = old.tm_gmtoff;
    state.dst_start = old.dst_start;
    state.dst_end = old.dst_end;
    state.timezone_set = old.timezone_set;
    memcpy(state.backup, old.backup, sizeof(state.backup));
  } else if (count == sizeof(state)) {
    memcpy(&state, bytes, sizeof(state));
    // Version 1's ARM64 layout has the same field offsets as version 2.
    if ((state.magic != 0x52544331 && state.magic != 0x52544332) || state.size != sizeof(state) ||
        state.checksum != prv_checksum(&state, offsetof(RtcState, checksum)))
      return false;
  } else {
    return false;
  }
  if (state.timezone_set > 1 || state.tm_gmtoff < -86400 || state.tm_gmtoff > 86400 ||
      (sizeof(time_t) == 4 && (state.dst_start < INT32_MIN || state.dst_start > INT32_MAX ||
                               state.dst_end < INT32_MIN || state.dst_end > INT32_MAX)))
    return false;
  s_offset_s = state.offset_s;
  s_tzinfo = (TimezoneInfo){
    .dst_id = state.dst_id,
    .timezone_id = state.timezone_id,
    .tm_gmtoff = state.tm_gmtoff,
    .dst_start = state.dst_start,
    .dst_end = state.dst_end
  };
  memcpy(s_tzinfo.tm_zone, state.tm_zone, sizeof(s_tzinfo.tm_zone));
  s_tz_set = state.timezone_set;
  memcpy(s_backup, state.backup, sizeof(s_backup));
  return true;
}

#else
static void prv_save(void) {
}
#endif

void RTC_WriteBackupRegister(uint32_t reg_id, uint32_t value) {
  if (reg_id < NUM_BACKUP_REGS) {
    s_backup[reg_id] = value;
    prv_save();
  }
}

uint32_t RTC_ReadBackupRegister(uint32_t reg_id) {
  return reg_id < NUM_BACKUP_REGS ? s_backup[reg_id] : 0;
}

void rtc_init(void) {
  s_boot_us = prv_ticks_us();
#ifdef CONFIG_LINUX_WATCH
  if (!prv_restore()) {
    time_t now = time(NULL);
    struct tm host;
    if (localtime_r(&now, &host)) {
      s_tzinfo = (TimezoneInfo){.timezone_id = -1, .tm_gmtoff = host.tm_gmtoff};
      if (host.tm_zone)
        strncpy(s_tzinfo.tm_zone, host.tm_zone, sizeof(s_tzinfo.tm_zone));
      s_tz_set = true;
    }
    prv_save();
  }
#endif
}

void rtc_calibrate_frequency(uint32_t frequency) {
}

void rtc_init_timers(void) {
}

void rtc_set_time(time_t time) {
  struct timeval now;
  gettimeofday(&now, nullptr);
  s_offset_s = (int64_t)time - now.tv_sec;
  prv_save();
}

void rtc_get_time_ms(time_t *out_seconds, uint16_t *out_ms) {
  struct timeval now;
  gettimeofday(&now, nullptr);
  *out_seconds = (time_t)(now.tv_sec + s_offset_s);
  *out_ms = (uint16_t)(now.tv_usec / 1000);
}

time_t rtc_get_time(void) {
  time_t t;
  uint16_t ms;
  rtc_get_time_ms(&t, &ms);
  return t;
}

void rtc_set_time_tm(struct pbl_tm *time_tm) {
  rtc_set_time(pbl_mktime(time_tm));
}

void rtc_get_time_tm(struct pbl_tm *time_tm) {
  time_t t = rtc_get_time();
  pbl_localtime_r(&t, time_tm);
}

bool rtc_sanitize_struct_tm(struct pbl_tm *t) {
  if (t->tm_year < 100) {
    t->tm_year = 100;
    return true;
  } else if (t->tm_year > 137) {
    t->tm_year = 137;
    return true;
  }
  return false;
}

bool rtc_sanitize_time_t(time_t *t) {
  struct pbl_tm time_struct;
  pbl_gmtime_r(t, &time_struct);
  const bool result = rtc_sanitize_struct_tm(&time_struct);
  *t = pbl_mktime(&time_struct);
  return result;
}

RtcTicks rtc_get_ticks(void) {
  return (RtcTicks)((prv_ticks_us() - s_boot_us) * RTC_TICKS_HZ / 1000000);
}

void rtc_alarm_init(void) {
}

void rtc_alarm_set(RtcTicks num_ticks) {
}

RtcTicks rtc_alarm_get_elapsed_ticks(void) {
  return 0;
}

bool rtc_alarm_is_initialized(void) {
  return true;
}

void rtc_set_timezone(TimezoneInfo *tzinfo) {
  s_tzinfo = *tzinfo;
  s_tz_set = true;
  prv_save();
}

void rtc_get_timezone(TimezoneInfo *tzinfo) {
  *tzinfo = s_tzinfo;
}

uint16_t rtc_get_timezone_id(void) {
  return s_tzinfo.timezone_id;
}

bool rtc_is_timezone_set(void) {
  return s_tz_set;
}

void rtc_timezone_clear(void) {
  memset(&s_tzinfo, 0, sizeof(s_tzinfo));
  s_tz_set = false;
  prv_save();
}

const char *rtc_get_time_string(char *buffer) {
  return time_t_to_string(buffer, rtc_get_time());
}

const char *time_t_to_string(char *buffer, time_t t) {
  struct pbl_tm time;
  pbl_localtime_r(&t, &time);
  pbl_strftime_r(buffer, TIME_STRING_BUFFER_SIZE, "%c", &time);
  return buffer;
}
