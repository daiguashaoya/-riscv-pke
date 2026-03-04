#ifndef _PROC_H_
#define _PROC_H_

#include "config.h"
#include "riscv.h"

typedef struct trapframe_t {
  // space to store context (all common registers)
  /* offset:0   */ riscv_regs regs;

  // process's "user kernel" stack
  /* offset:248 */ uint64 kernel_sp;
  // pointer to smode_trap_handler
  /* offset:256 */ uint64 kernel_trap;
  // saved user process counter
  /* offset:264 */ uint64 epc;

  // kernel page table. added @lab2_1
  /* offset:272 */ uint64 kernel_satp;

  // hart ID of the owning process. added @lab2_challenge3
  // Stored here so strap_vector.S can restore tp before calling
  // smode_trap_handler, because user code may freely overwrite tp (used as TLS
  // pointer in user mode).
  /* offset:280 */ uint64 hartid;
} trapframe;

// the extremely simple definition of process, used for begining labs of PKE
typedef struct process_t {
  // pointing to the stack used in trap handling.
  uint64 kstack;
  // user page table
  pagetable_t pagetable;
  // trapframe storing the context of a (User mode) process.
  trapframe *trapframe;
} process;

// switch to run user app
void switch_to(process *);

// Per-hart current running process. changed to array @lab2_challenge3
extern process *current[NCPU];

// Per-hart address of the first free page in our simple heap. changed to array
// @lab2_challenge3
extern uint64 g_ufree_page[NCPU];

#endif
