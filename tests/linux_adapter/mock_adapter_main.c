/* SPDX-License-Identifier: Apache-2.0 */

/* Runs the mock adapter as a process, so the OS or the conformance checker can be pointed at it.
 *
 *   mock_adapter --dir D [--caps 0x3ff] [--latency MS] [--fast-path] [--silent]
 *                [--replay trace.bin [--loop]]
 *
 * --replay feeds a recorded sensor trace. The file is either a v1 trace (concatenated 776-byte
 * packets with version 2) or concatenated v2 SENSOR messages. */

#include "mock_adapter.h"

#include <signal.h>
#include <stdlib.h>
#include <string.h>

static volatile sig_atomic_t s_stop;

static void on_signal(int sig) {
  (void)sig;
  s_stop = 1;
}

// The v1 packet, kept only to read old recordings.
struct v1_packet {
  uint32_t version, type, count, interval_us;
  uint64_t timestamp_us;
  int16_t values[125][3];
};

// Tizen SensorDataAccuracy 0..3 to Pebble HRMQuality.
static int16_t v1_quality(int bpm, int accuracy) {
  if (bpm == -3)
    return -1;
  if (bpm < 25 || bpm > 240)
    return 0;
  return accuracy >= 3 ? 4 : accuracy == 2 ? 3 : accuracy == 1 ? 2 : 0;
}

static void sleep_ms(unsigned ms) {
  struct timespec t = {.tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L};
  nanosleep(&t, NULL);
}

// Sends one recording; returns false when interrupted.
static bool replay(struct mock *mock, const char *path) {
  FILE *file = fopen(path, "rb");
  if (!file) {
    fprintf(stderr, "mock: cannot read %s\n", path);
    return false;
  }
  uint8_t record[sizeof(struct pbl_adapter_sensor)];
  uint64_t previous_us = 0;
  size_t first = fread(record, 1, sizeof(struct v1_packet), file);
  bool v1 = first == sizeof(struct v1_packet) && ((struct v1_packet *)record)->version == 2;
  rewind(file);
  while (!s_stop) {
    struct pbl_adapter_sensor out;
    uint64_t stamp;
    if (v1) {
      struct v1_packet in;
      if (fread(&in, sizeof(in), 1, file) != 1)
        break;
      if (in.type < 1 || in.type > 4 || in.type == 3 || in.count == 0 || in.count > 125)
        continue;
      pbl_adapter_init(&out, PBL_ADAPTER_SENSOR, sizeof(out), 0);
      out.type = in.type == 4 ? PBL_ADAPTER_SENSOR_STEPS : in.type;
      out.count = in.count;
      out.interval_us = in.interval_us;
      memcpy(out.values, in.values, sizeof(out.values));
      if (in.type == 2)
        out.values[0][1] = v1_quality(in.values[0][0], in.values[0][1]);
      stamp = in.timestamp_us;
    } else {
      if (fread(&out, sizeof(out), 1, file) != 1)
        break;
      stamp = out.timestamp_us;
    }
    if (previous_us && stamp > previous_us)
      sleep_ms((unsigned)((stamp - previous_us) / 1000));
    previous_us = stamp;
    out.timestamp_us = pbl_adapter_boottime_us();
    mock_send_raw(mock, &out, sizeof(out));
  }
  fclose(file);
  return !s_stop;
}

int main(int argc, char **argv) {
  struct mock_options options = {.caps = 0x3ff, .log = stderr};
  const char *trace = NULL;
  bool loop = false;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--dir") && i + 1 < argc)
      options.dir = argv[++i];
    else if (!strcmp(argv[i], "--caps") && i + 1 < argc)
      options.caps = (uint32_t)strtoul(argv[++i], NULL, 0);
    else if (!strcmp(argv[i], "--latency") && i + 1 < argc)
      options.display_latency_ms = (unsigned)atoi(argv[++i]);
    else if (!strcmp(argv[i], "--fast-path"))
      options.fast_path = true;
    else if (!strcmp(argv[i], "--silent"))
      options.silent = true;
    else if (!strcmp(argv[i], "--replay") && i + 1 < argc)
      trace = argv[++i];
    else if (!strcmp(argv[i], "--loop"))
      loop = true;
    else {
      fprintf(stderr,
              "usage: %s --dir D [--caps HEX] [--latency MS] [--fast-path] [--silent] "
              "[--replay FILE [--loop]]\n",
              argv[0]);
      return 2;
    }
  }
  if (!options.dir) {
    fprintf(stderr, "--dir is required\n");
    return 2;
  }
  signal(SIGTERM, on_signal);
  signal(SIGINT, on_signal);
  struct mock *mock;
  int rc = mock_start(&options, &mock);
  if (rc) {
    fprintf(stderr, "mock: cannot start: %s\n", strerror(-rc));
    return 1;
  }
  fprintf(stderr, "mock: listening in %s, capabilities 0x%x\n", options.dir, options.caps);
  while (!s_stop) {
    if (trace) {
      if (!replay(mock, trace) || !loop)
        trace = NULL;
    } else {
      sleep_ms(100);
    }
  }
  mock_stop(mock);
  return 0;
}
