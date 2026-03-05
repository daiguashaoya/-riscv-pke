#ifndef _PROC_H_
#define _PROC_H_

#include "riscv.h"

#define MAX_HEAP_PAGES 64

// 内存块结构体(mcb)
typedef struct mem_block_t {
  int size;
  int free;// 0: free, 1: used
  struct mem_block_t *next;
} mem_block;

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
} trapframe;

// the extremely simple definition of process, used for begining labs of PKE
typedef struct process_t {
  // pointing to the stack used in trap handling.
  uint64 kstack;
  // user page table
  pagetable_t pagetable;
  // trapframe storing the context of a (User mode) process.
  trapframe *trapframe;

  // 堆管理
  uint64 heap_bottom;
  uint64 heap_top;
  mem_block *heap_head;
  uint64 heap_pages_pa[MAX_HEAP_PAGES];
  uint64 heap_pages_va[MAX_HEAP_PAGES];
  int heap_pages_cnt;
} process;

// switch to run user app
void switch_to(process *);

// current running process
extern process *current;

// address of the first free page in our simple heap. added @lab2_2
extern uint64 g_ufree_page;

#endif
