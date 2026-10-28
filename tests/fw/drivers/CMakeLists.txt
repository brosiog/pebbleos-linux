# SPDX-FileCopyrightText: 2026 Core Devices LLC
# SPDX-License-Identifier: Apache-2.0

pbl_clar_test(test_flash_api
  SOURCES
    drivers/flash/flash_api.c
  OVERRIDES dummy_board
)

pbl_clar_test(test_flash_erase
  SOURCES
    drivers/flash/flash_erase.c
  PLATFORMS emery
  OVERRIDES dummy_board
)

pbl_clar_test(test_accel_qemu
  SOURCES
    drivers/base/device.c
    drivers/imu/accel.c
    drivers/imu/qemu.c
    tests/fakes/fake_rtc.c
  DEFINES
    CONFIG_DEVICE_LOG_LEVEL=0
    PBL_NO_LINKER_SCRIPT
  OVERRIDES dummy_board
)

pbl_clar_test(test_watch_sensor_linux
  SOURCES
    drivers/base/device.c
    drivers/imu/accel.c
    drivers/imu/linux/watch_sensor.c
    tests/fakes/fake_rtc.c
  DEFINES
    CONFIG_DEVICE_LOG_LEVEL=0
    PBL_NO_LINKER_SCRIPT
  OVERRIDES dummy_board
)

pbl_clar_test(test_mic_linux
  SOURCES
    drivers/mic/linux/mic.c
    lib/util/circular_buffer.c
  DEFINES
    CONFIG_MIC_LINUX=1
    CONFIG_DRIVER_MIC_LOG_LEVEL=0
  OVERRIDES dummy_board
)
