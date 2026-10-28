/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdint.h>

//! Starts the firmware: @p entry runs as soon as the CPU is free, with the CPU
//! held, and is expected to start the kernel.
void pbl_posix_start(int (*entry)(void));

//! Runs @p isr as an interrupt, from the host: whenever no kernel thread holds
//! the CPU.
void pbl_posix_irq_run(void (*isr)(void *), void *arg);

//! Idle: lets go of the CPU until an interrupt ran or @p timeout_us passed.
void pbl_posix_cpu_wait(uint64_t timeout_us);

#ifndef CONFIG_ARCH_POSIX_FIBERS
//! Parks the tick producer while the firmware CPU is idle.
void pbl_posix_tick_wait(void);

//! Updates firmware time with the CPU held, before delivering an idle wake interrupt.
void pbl_posix_tick_sync(bool idle);
#endif

#ifdef CONFIG_ARCH_POSIX_FIBERS
//! Runs the kernel on the calling host thread until it goes idle.
void pbl_posix_run(void);
#endif
