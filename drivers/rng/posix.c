/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include <stddef.h>

#include <pbl/drivers/rng.h>

#include <sys/random.h>
#include <unistd.h>
#ifdef __linux__
#include <errno.h>

#include <sys/syscall.h>
#endif

bool rng_rand(uint32_t *rand_out) {
#ifdef __linux__
  // The watch's glibc predates getentropy; its kernel provides getrandom.
  size_t done = 0;
  while (done < sizeof(*rand_out)) {
    ssize_t n = syscall(SYS_getrandom, (char *)rand_out + done, sizeof(*rand_out) - done, 0);
    if (n < 0 && errno == EINTR) {
      continue;
    }
    if (n <= 0) {
      return false;
    }
    done += n;
  }
  return true;
#else
  return getentropy(rand_out, sizeof(*rand_out)) == 0;
#endif
}
