# SPDX-FileCopyrightText: 2026 Core Devices LLC
# SPDX-License-Identifier: Apache-2.0

file(GLOB sources CONFIGURE_DEPENDS samsa/src/*.c)
if(CONFIG_LINUX_WATCH)
  set(sources watch_unity.c)
endif()
pbl_include_directories(samsa/include)
pbl_library_named(samsa)
pbl_library_kind(stlib)
pbl_library_sources(${sources})
pbl_library_include_directories(samsa/src)

if(CONFIG_LINUX_WATCH)
  # Keep the interpreter's memory and ALU helpers visible to the optimizer.
  # This affects active guest execution only; idle scheduling is unchanged.
  target_compile_options(samsa PRIVATE -O3)
endif()
