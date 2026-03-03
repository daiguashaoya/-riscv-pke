/*
 * contains the implementation of all syscalls.
 */

#include <errno.h>
#include <stdint.h>

#include "config.h"
#include "process.h"
#include "string.h"
#include "syscall.h"
#include "util/functions.h"
#include "util/types.h"

#include "spike_interface/spike_utils.h"

//
// implement the SYS_user_print syscall
//
ssize_t sys_user_print(const char *buf, size_t n) {
  int hartid = get_hartid();
  sprint("hartid = %d: %s", hartid, buf);
  return 0;
}

//
// atomic counter: number of harts that have called exit.
// when all NCPU harts have exited, hart0 will call shutdown.
//
static volatile int g_finished_harts = 0;
static volatile int g_exit_code = 0;

//
// implement the SYS_user_exit syscall
//
ssize_t sys_user_exit(uint64 code) {
  int hartid = get_hartid();
  sprint("hartid = %d: User exit with code:%d.\n", hartid, code);
  g_exit_code = (int)code;

  // Atomically increment finished hart count using AMO instruction.
  asm volatile("amoadd.w zero, %0, (%1)" ::"r"(1), "r"(&g_finished_harts)
               : "memory");

  // Spin-wait until ALL harts have reached the exit point.
  while (g_finished_harts < NCPU)
    ;

  // Only hart0 performs the actual shutdown to avoid race condition.
  if (hartid == 0) {
    sprint("hartid = 0: shutdown with code:%d.\n", g_exit_code);
    shutdown(g_exit_code);
  } else {
    // Other harts spin-wait for hart0 to shut down the system.
    while (1)
      ;
  }
  return 0; // unreachable
}

//
// [a0]: the syscall number; [a1] ... [a7]: arguments to the syscalls.
// returns the code of success, (e.g., 0 means success, fail for otherwise)
//
long do_syscall(long a0, long a1, long a2, long a3, long a4, long a5, long a6,
                long a7) {
  switch (a0) {
  case SYS_user_print:
    return sys_user_print((const char *)a1, a2);
  case SYS_user_exit:
    return sys_user_exit(a1);
  default:
    panic("Unknown syscall %ld \n", a0);
  }
}
