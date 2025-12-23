#ifndef _PROC_H_
#define _PROC_H_

#include "riscv.h"

typedef struct trapframe_t {
  // space to store context (all common registers)
  /* offset:0   */ riscv_regs regs;

  // process's "user kernel" stack
  // 内核栈顶地址
  /* offset:248 */ uint64 kernel_sp;
  // pointer to smode_trap_handler
  // 指向内核陷阱处理函数的指针smode_handler
  /* offset:256 */ uint64 kernel_trap;
  // saved user process counter
  /* offset:264 */ uint64 epc;
}trapframe;

// the extremely simple definition of process, used for begining labs of PKE
typedef struct process_t {
  // pointing to the stack used in trap handling.
  uint64 kstack;
  // trapframe storing the context of a (User mode) process.
  trapframe* trapframe;
}process;

void switch_to(process*);

extern process* current;

#endif
