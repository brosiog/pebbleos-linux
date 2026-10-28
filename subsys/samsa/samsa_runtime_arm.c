/* SPDX-License-Identifier: Apache-2.0 */

#include <stdint.h>
#include <stdio.h>
#include <time.h>

#include <pbl/kernel/assert.h>
#include <pbl/logging/logging.h>

#include <process_management/pebble_process_md.h>
#include <process_management/process_runtime.h>
#include <sys/mman.h>
#include <unistd.h>

static_assert(sizeof(void *) == 4, "Native Pebble apps require the 32-bit ARM ABI");
static_assert(sizeof(time_t) == 4, "Native Pebble apps require the 32-bit time ABI");
#if !defined(__arm__) || defined(__ARM_PCS_VFP)
#error "Native Pebble apps require an ARM soft-float host"
#endif

PBL_LOG_MODULE_DEFINE(samsa, CONFIG_SAMSA_LOG_LEVEL);

extern char __APP_RAM__[], __APP_RAM_end__[];
extern char __WORKER_RAM__[], __WORKER_RAM_end__[];

// Cortex-M cycle profiling is unavailable on this host, as with Samsa.
void sys_profiler_init(void) {
}
void sys_profiler_start(void) {
}
void sys_profiler_stop(void) {
}
void sys_profiler_print_stats(void) {
}

void process_runtime_run(const PebbleProcessMd *app_md, PebbleTask task, void *entry,
                         void *stack_top) {
  if (app_md->process_storage != ProcessStorageBuiltin) {
    char *start = task == PebbleTask_Worker ? __WORKER_RAM__ : __APP_RAM__;
    char *end = task == PebbleTask_Worker ? __WORKER_RAM_end__ : __APP_RAM_end__;
    uintptr_t page_size = (uintptr_t)sysconf(_SC_PAGESIZE);
    PBL_ASSERTN(page_size && !(page_size & (page_size - 1)));
    uintptr_t first = (uintptr_t)start & ~(page_size - 1);
    uintptr_t last = ((uintptr_t)end + page_size - 1) & ~(page_size - 1);
    PBL_ASSERTN(mprotect((void *)first, last - first, PROT_READ | PROT_WRITE | PROT_EXEC) == 0);
    __builtin___clear_cache(start, end);
    entry = (void *)((uintptr_t)entry | 1u);
    PBL_LOG_DBG("Running %s directly on ARM", process_metadata_get_name(app_md));
  }
  ((PebbleMain)entry)();
}
