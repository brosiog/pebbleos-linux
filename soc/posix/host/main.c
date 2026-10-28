/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#define _GNU_SOURCE
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <getopt.h>
#include <libgen.h>
#include <posix_host.h>
#include <pthread.h>
#include <sys/time.h>
#include <unistd.h>

#ifdef __linux__
#include <fcntl.h>
#include <poll.h>
static int s_wake_pipe[2] = {-1, -1};
static void prv_notify(void) {
  if (s_wake_pipe[1] >= 0) {
    (void)write(s_wake_pipe[1], "w", 1);
  }
}
#else
static void prv_notify(void) {}
#endif

#if defined(__linux__) && (defined(__aarch64__) || defined(__arm__))
#include <signal.h>

#include <ucontext.h>

static void prv_fault_hex(const char *label, size_t len, uint64_t value) {
  static const char hex[] = "0123456789abcdef";
  char output[] = "0x0000000000000000\n";
  for (unsigned i = 0; i < 16; ++i) {
    output[17 - i] = hex[value & 15];
    value >>= 4;
  }
  (void)write(STDERR_FILENO, label, len);
  (void)write(STDERR_FILENO, output, sizeof(output) - 1);
}

__attribute__((weak)) uintptr_t posix_host_fault_landing(uintptr_t pc, uintptr_t lr) {
  (void)pc;
  (void)lr;
  return 0;
}

// Resumes the faulting thread at the firmware's landing zone, if it has one.
static bool prv_fault_redirect(ucontext_t *uc) {
#ifdef __aarch64__
  uintptr_t landing = posix_host_fault_landing(uc->uc_mcontext.pc, uc->uc_mcontext.regs[30]);
  if (!landing) {
    return false;
  }
  uc->uc_mcontext.pc = landing;
  uc->uc_mcontext.sp &= ~(uintptr_t)15;
#else
  uintptr_t landing = posix_host_fault_landing(uc->uc_mcontext.arm_pc, uc->uc_mcontext.arm_lr);
  if (!landing) {
    return false;
  }
  // Clear the Thumb and IT state bits, then select the landing zone's instruction set.
  const unsigned long thumb = 1UL << 5;
  const unsigned long it_bits = (0x3FUL << 10) | (0x3UL << 25);
  uc->uc_mcontext.arm_cpsr &= ~(thumb | it_bits);
  if (landing & 1) {
    uc->uc_mcontext.arm_cpsr |= thumb;
  }
  uc->uc_mcontext.arm_pc = landing & ~(uintptr_t)1;
  uc->uc_mcontext.arm_sp &= ~(uintptr_t)7;
#endif
  return true;
}

static void prv_fault(int signal_number, siginfo_t *info, void *context) {
  ucontext_t *uc = context;
  prv_fault_hex("FAULT signal=", 13, signal_number);
  prv_fault_hex("address=", 8, (uintptr_t)info->si_addr);
#ifdef __aarch64__
  prv_fault_hex("pc=", 3, uc->uc_mcontext.pc);
  prv_fault_hex("lr=", 3, uc->uc_mcontext.regs[30]);
  prv_fault_hex("sp=", 3, uc->uc_mcontext.sp);
  for (unsigned i = 0; i < 30; ++i) {
    prv_fault_hex("register=", 9, uc->uc_mcontext.regs[i]);
  }
#else
  prv_fault_hex("pc=", 3, uc->uc_mcontext.arm_pc);
  prv_fault_hex("lr=", 3, uc->uc_mcontext.arm_lr);
  prv_fault_hex("sp=", 3, uc->uc_mcontext.arm_sp);
  const unsigned long regs[] = {
    uc->uc_mcontext.arm_r0, uc->uc_mcontext.arm_r1, uc->uc_mcontext.arm_r2,
    uc->uc_mcontext.arm_r3, uc->uc_mcontext.arm_r4, uc->uc_mcontext.arm_r5,
    uc->uc_mcontext.arm_r6, uc->uc_mcontext.arm_r7, uc->uc_mcontext.arm_r8,
    uc->uc_mcontext.arm_r9, uc->uc_mcontext.arm_r10, uc->uc_mcontext.arm_fp,
    uc->uc_mcontext.arm_ip,
  };
  for (unsigned i = 0; i < 13; ++i) {
    prv_fault_hex("register=", 9, regs[i]);
  }
#endif
  if (prv_fault_redirect(uc)) {
    static const char message[] = "FAULT_RECOVERY killing the faulting app\n";
    (void)write(STDERR_FILENO, message, sizeof(message) - 1);
    return;
  }
  _exit(128 + signal_number);
}
#endif

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#endif

#define MAX_OPTIONS 16
#define MAX_HOOKS   16
#define POLL_MS     10

static const struct posix_host_option *s_options[MAX_OPTIONS];
static size_t s_num_options;
static const struct posix_host_hook *s_hooks[MAX_HOOKS];
static size_t s_num_hooks;

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_cond = PTHREAD_COND_INITIALIZER;
static bool s_woken;
static bool s_quit;
static bool s_reboot;

static char **s_argv;
static char s_exe_dir[PATH_MAX];
static int s_exit_after_s;
static uint64_t s_exit_at_us;

void posix_host_option_register(const struct posix_host_option *option) {
  if (s_num_options < MAX_OPTIONS) {
    s_options[s_num_options++] = option;
  }
}

void posix_host_hook_register(const struct posix_host_hook *hook) {
  if (s_num_hooks < MAX_HOOKS) {
    s_hooks[s_num_hooks++] = hook;
  }
}

void posix_host_wake(void) {
  pthread_mutex_lock(&s_lock);
  s_woken = true;
  pthread_cond_signal(&s_cond);
  prv_notify();
  pthread_mutex_unlock(&s_lock);
}

void posix_host_quit(void) {
  pthread_mutex_lock(&s_lock);
  s_quit = true;
  pthread_cond_signal(&s_cond);
  prv_notify();
  pthread_mutex_unlock(&s_lock);
}

// The main thread restarts the process, once the exit hooks ran there.
void posix_host_reboot(void) {
  pthread_mutex_lock(&s_lock);
  s_reboot = true;
  s_quit = true;
  pthread_cond_signal(&s_cond);
  prv_notify();
  pthread_mutex_unlock(&s_lock);
  for (;;) {
    pause();
  }
}

uint64_t posix_host_monotonic_us(void) {
  struct timespec ts;
#ifdef __linux__
  clock_gettime(CLOCK_BOOTTIME, &ts);
#else
  clock_gettime(CLOCK_MONOTONIC, &ts);
#endif
  return (uint64_t)ts.tv_sec * 1000000 + (uint64_t)ts.tv_nsec / 1000;
}

const char *posix_host_exe_dir(void) {
  return s_exe_dir;
}

static void prv_set_exit_after(const char *value) {
  s_exit_after_s = atoi(value);
}

POSIX_HOST_OPTION(.flag = 't', .arg = "seconds", .help = "quit after this long",
                  .set = prv_set_exit_after)

static void prv_usage(const char *argv0) {
  fprintf(stderr, "usage: %s [options]\n", argv0);
  for (size_t i = 0; i < s_num_options; i++) {
    const struct posix_host_option *o = s_options[i];
    fprintf(stderr, "  -%c %-10s %s\n", o->flag, o->arg ? o->arg : "", o->help);
  }
}

static void prv_parse_options(int argc, char **argv) {
  char optstring[2 * MAX_OPTIONS + 2] = "h";
  for (size_t i = 0; i < s_num_options; i++) {
    size_t len = strlen(optstring);
    optstring[len++] = s_options[i]->flag;
    if (s_options[i]->arg != nullptr) {
      optstring[len++] = ':';
    }
    optstring[len] = '\0';
  }

  int opt;
  while ((opt = getopt(argc, argv, optstring)) != -1) {
    const struct posix_host_option *option = nullptr;
    for (size_t i = 0; i < s_num_options; i++) {
      if (s_options[i]->flag == opt) {
        option = s_options[i];
      }
    }
    if (option == nullptr) {
      prv_usage(argv[0]);
      exit(opt == 'h' ? 0 : 1);
    }
    option->set(optarg);
  }
}

// Waits up to POLL_MS for a wake-up; returns whether to quit.
#ifdef __EMSCRIPTEN__
// The browser has the only thread: give it its turn, to draw and to queue
// input, without waiting when there is work.
static bool prv_wait(void) {
  emscripten_sleep(s_woken ? 0 : POLL_MS);
  s_woken = false;
  return s_quit;
}
#elif defined(__linux__)
static bool prv_wait(void) {
  struct pollfd descriptors[64] = {{.fd = s_wake_pipe[0], .events = POLLIN}};
  unsigned count = 1, timeout = 60000;
  for (size_t i = 0; i < s_num_hooks; i++) {
    const struct posix_host_hook *hook = s_hooks[i];
    if (hook->poll) {
      unsigned delay = hook->poll_timeout_ms ? hook->poll_timeout_ms() : POLL_MS;
      if (delay < timeout)
        timeout = delay;
    }
    if (hook->poll_fd) {
      for (unsigned index = 0; count < 64; index++) {
        int fd = hook->poll_fd(index);
        if (fd < 0)
          break;
        descriptors[count++] = (struct pollfd){.fd = fd, .events = POLLIN};
      }
    }
  }
  if (s_exit_after_s > 0) {
    uint64_t now = posix_host_monotonic_us();
    unsigned remaining = now < s_exit_at_us ? (s_exit_at_us - now + 999) / 1000 : 0;
    if (remaining < timeout)
      timeout = remaining;
  }
  pthread_mutex_lock(&s_lock);
  bool ready = s_woken || s_quit;
  s_woken = false;
  pthread_mutex_unlock(&s_lock);
  (void)poll(descriptors, count, ready ? 0 : (int)timeout);
  char bytes[128];
  while (read(s_wake_pipe[0], bytes, sizeof(bytes)) > 0) {}
  pthread_mutex_lock(&s_lock);
  bool quit = s_quit;
  pthread_mutex_unlock(&s_lock);
  return quit;
}
#else
static bool prv_wait(void) {
  struct timeval now;
  gettimeofday(&now, nullptr);
  uint64_t ns = (uint64_t)now.tv_usec * 1000 + (uint64_t)POLL_MS * 1000000;
  struct timespec deadline = {
    .tv_sec = now.tv_sec + (time_t)(ns / 1000000000),
    .tv_nsec = (long)(ns % 1000000000),
  };

  pthread_mutex_lock(&s_lock);
  while (!s_woken && !s_quit) {
    if (pthread_cond_timedwait(&s_cond, &s_lock, &deadline) != 0) {
      break;
    }
  }
  s_woken = false;
  bool quit = s_quit;
  pthread_mutex_unlock(&s_lock);
  return quit;
}
#endif

int main(int argc, char **argv) {
#ifdef __linux__
  if (pipe2(s_wake_pipe, O_NONBLOCK | O_CLOEXEC)) {
    perror("host wake pipe");
    return 1;
  }
#endif
#if defined(__linux__) && (defined(__aarch64__) || defined(__arm__))
  struct sigaction fault_action = {.sa_sigaction = prv_fault, .sa_flags = SA_SIGINFO};
  sigemptyset(&fault_action.sa_mask);
  sigaction(SIGSEGV, &fault_action, nullptr);
  sigaction(SIGBUS, &fault_action, nullptr);
  sigaction(SIGILL, &fault_action, nullptr);
#endif
  s_argv = argv;
  char exe[PATH_MAX];
  if (realpath(argv[0], exe) != nullptr) {
    snprintf(s_exe_dir, sizeof(s_exe_dir), "%s", dirname(exe));
  }

  prv_parse_options(argc, argv);

  for (size_t i = 0; i < s_num_hooks; i++) {
    if (s_hooks[i]->init != nullptr) {
      s_hooks[i]->init();
    }
  }

  posix_fw_start();

  s_exit_at_us = posix_host_monotonic_us() + (uint64_t)s_exit_after_s * 1000000;
  do {
    if (s_exit_after_s > 0 && posix_host_monotonic_us() >= s_exit_at_us) {
      break;
    }
    for (size_t i = 0; i < s_num_hooks; i++) {
      if (s_hooks[i]->poll != nullptr) {
        s_hooks[i]->poll();
      }
    }
  } while (!prv_wait());

  for (size_t i = 0; i < s_num_hooks; i++) {
    if (s_hooks[i]->exit != nullptr) {
      s_hooks[i]->exit();
    }
  }

  if (s_reboot) {
    fprintf(stderr, "\n*** reboot ***\n\n");
#ifdef __EMSCRIPTEN__
    emscripten_run_script("location.reload()");
#endif
    execv(s_argv[0], s_argv);
    perror("execv");
    exit(1);
  }
  exit(0);
}
