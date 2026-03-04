/*
 * contains the implementation of all syscalls.
 */

#include <errno.h>
#include <stdint.h>

#include "config.h"
#include "pmm.h"
#include "process.h"
#include "spike_interface/spike_utils.h"
#include "string.h"
#include "syscall.h"
#include "util/functions.h"
#include "util/types.h"
#include "vmm.h"

// Helper: read hartid from tp register (set in mentry.S). added
// @lab2_challenge3
static inline uint64 get_hartid(void) {
  uint64 hartid;
  asm volatile("mv %0, tp" : "=r"(hartid));
  return hartid;
}

//
// implement the SYS_user_print syscall
//
ssize_t sys_user_print(const char *buf, size_t n) {
  uint64 hartid = get_hartid();
  // buf is now an address in user space of the given app's user stack,
  // so we have to transfer it into phisical address (kernel is running in
  // direct mapping).
  assert(current[hartid]);
  char *pa = (char *)user_va_to_pa((pagetable_t)(current[hartid]->pagetable),
                                   (void *)buf);
  sprint(pa);
  return 0;
}

//
// Per-hart exit flag: set to 1 when a hart has counted its exit.
// Prevents timer-interrupt-driven re-entry from double-counting.
// added @lab2_challenge3
//
static volatile int hart_exited[NCPU] = {0};
// Set to 1 once all NCPU harts have exited. Hart 0 watches this to call
// shutdown. added @lab2_challenge3
static volatile int all_done = 0;
// Total count of harts that completed exit.  added @lab2_challenge3
static volatile int exit_count = 0;

//
// implement the SYS_user_exit syscall
//
ssize_t sys_user_exit(uint64 code) {
  uint64 hartid = get_hartid();
  sprint("hartid = %d: User exit with code: %d.\n", hartid, code);

  // Only count each hart once (timer interrupts can re-enter this function).
  if (!hart_exited[hartid]) {
    hart_exited[hartid] = 1;

    int local;
    asm volatile("amoadd.w %0, %2, (%1)\n"
                 : "=r"(local)
                 : "r"(&exit_count), "r"(1)
                 : "memory");

    if (local + 1 == NCPU) {
      // All harts done — signal hart 0 to shut down.
      all_done = 1;
    }
  }

  if (hartid == 0) {
    // Hart 0 waits until all harts have exited, then calls shutdown.
    while (!all_done)
      asm volatile("wfi");
    sprint("hartid = 0: shutdown with code: %d.\n", code);
    shutdown(code);
  } else {
    // Non-zero harts simply spin and wait for hart 0 to shut everything down.
    while (1)
      asm volatile("wfi");
  }

  return 0; // unreachable
}

//
// maybe, the simplest implementation of malloc in the world ... added @lab2_2
//
uint64 sys_user_allocate_page() {
  uint64 hartid = get_hartid();
  void *pa = alloc_page();
  uint64 va = g_ufree_page[hartid]; // per-hart virtual address
  g_ufree_page[hartid] += PGSIZE;
  user_vm_map((pagetable_t)current[hartid]->pagetable, va, PGSIZE, (uint64)pa,
              prot_to_type(PROT_WRITE | PROT_READ, 1));
  sprint("hartid = %ld: vaddr 0x%x is mapped to paddr 0x%x\n", hartid, va, pa);
  return va;
}

//
// reclaim a page, indicated by "va". added @lab2_2
//
uint64 sys_user_free_page(uint64 va) {
  uint64 hartid = get_hartid();
  user_vm_unmap((pagetable_t)current[hartid]->pagetable, va, PGSIZE, 1);
  return 0;
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
  // added @lab2_2
  case SYS_user_allocate_page:
    return sys_user_allocate_page();
  case SYS_user_free_page:
    return sys_user_free_page(a1);
  default:
    panic("Unknown syscall %ld \n", a0);
  }
}
