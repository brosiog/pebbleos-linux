/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#define BT_VENDOR_ID   0x0EEA
#define BT_VENDOR_NAME "Native"

extern UARTDevice *const DBG_UART;
extern UARTDevice *const QEMU_UART;
#if defined(CONFIG_BT_HCI_UART) || defined(CONFIG_BT_HCI_LINUX)
extern UARTDevice *const BT_HCI_UART;
#endif
extern DisplayDevice *const DISPLAY;
extern MicDevice *const MIC;
#ifdef CONFIG_HRM_LINUX_WATCH
extern HRMDevice *const HRM;
#endif
extern const BoardConfigPower BOARD_CONFIG_POWER;
extern const BoardConfig BOARD_CONFIG;
extern const BoardConfigButton BOARD_CONFIG_BUTTON;

static const BoardConfigAccel BOARD_CONFIG_ACCEL = {
  .default_motion_sensitivity = 85U,
};
