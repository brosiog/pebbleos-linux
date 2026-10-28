/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include <stdint.h>

#include <pbl/kernel/idle.h>
#include <pbl/kernel/init.h>
#include <pbl/kernel/sched.h>

#include <kernel/util/idle.h>
#include <pbl_arch_posix.h>
#include <posix_host.h>
#include <pthread.h>
#include <unistd.h>

#ifdef CONFIG_LINUX_WATCH
#include <pbl/drivers/linux_adapter_client.h>
#endif

uint32_t SystemCoreClock = 64000000;

static uint64_t s_tick_start_ms;
static uint64_t s_ticks_announced;

static uint64_t prv_monotonic_ms(void) {
  return posix_host_monotonic_us() / 1000;
}

// Catches up on every tick that passed while the kernel held the CPU.
static void prv_tick_sync(bool idle) {
  if (!pbl_kernel_is_started()) {
    return;
  }
  uint64_t now = prv_monotonic_ms() - s_tick_start_ms;
  if (idle && now > s_ticks_announced) {
    uint64_t elapsed = now - s_ticks_announced;
    s_ticks_announced = now;
    pbl_idle_slept(elapsed);
    return;
  }
  while (s_ticks_announced < now) {
    s_ticks_announced++;
    pbl_kernel_tick_isr();
  }
}

static void prv_tick_isr(void *arg) {
  prv_tick_sync(false);
}

#ifdef CONFIG_ARCH_POSIX_FIBERS
// One host thread: the main loop ticks, then runs the kernel until it idles.
static void prv_kernel_poll(void) {
  pbl_posix_irq_run(prv_tick_isr, nullptr);
  pbl_posix_run();
}

POSIX_HOST_HOOK(.poll = prv_kernel_poll)

void pbl_soc_early_init(void) {
  s_tick_start_ms = prv_monotonic_ms();
}
#else
void pbl_posix_tick_sync(bool idle) {
  prv_tick_sync(idle);
}

static void *prv_tick_thread(void *arg) {
  for (;;) {
    pbl_posix_tick_wait();
    usleep(1000000 / PBL_TICK_HZ);
    pbl_posix_irq_run(prv_tick_isr, nullptr);
  }
  return nullptr;
}

void pbl_soc_early_init(void) {
  s_tick_start_ms = prv_monotonic_ms();
  pthread_t tid;
  pthread_create(&tid, nullptr, prv_tick_thread, nullptr);
}
#endif

#ifdef CONFIG_LINUX_WATCH
// Only sleeps of a quarter second or more are reported; shorter naps happen hundreds of times a
// second while the UI animates. Skipping the report of a nap leaves the OS marked busy, so the
// threshold stays well below the gaps between timer events of an idle watch.
#define IDLE_REPORT_MIN_US 250000ULL
static bool s_idle_reported;

static void prv_idle_begin(uint64_t duration_us) {
  if (duration_us >= IDLE_REPORT_MIN_US) {
    // Anything beyond a day is "no timer".
    linux_adapter_idle(false, duration_us > 86400ULL * 1000000ULL
                                  ? 0 : pbl_adapter_boottime_us() + duration_us);
    s_idle_reported = true;
  }
}

static void prv_idle_end(void) {
  if (s_idle_reported) {
    linux_adapter_idle(true, 0);
    s_idle_reported = false;
  }
}
#else
static void prv_idle_begin(uint64_t duration_us) {
}
static void prv_idle_end(void) {
}
#endif

void pbl_soc_idle(pbl_tick_t max_ticks) {
  if (!idle_is_allowed() || !pbl_idle_confirm()) {
    return;
  }
  uint64_t duration_us = (uint64_t)max_ticks * 1000000 / PBL_TICK_HZ;
  prv_idle_begin(duration_us);
  pbl_posix_cpu_wait(duration_us);
  prv_idle_end();
}

bool pbl_soc_tick_enable(void) {
  return true;
}

void pbl_analytics_external_collect_cpu_stats(void) {
}

void posix_fw_start(void) {
  extern int pbl_fw_main(void);
  pbl_posix_start(pbl_fw_main);
}
