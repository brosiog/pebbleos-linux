/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include <pbl/kernel/irq.h>

/** @cond INTERNAL_HIDDEN */

static inline bool mcu_state_is_isr(void) {
  return pbl_in_isr();
}

static inline uint32_t mcu_state_get_isr_priority(void) {
  // Host interrupts are serialized and run at the kernel's interrupt priority.
  return pbl_in_isr() ? PBL_IRQ_PRIO_KERNEL : ~0U;
}

/** @endcond */
