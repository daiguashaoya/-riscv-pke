#ifndef _PROC_H_
#define _PROC_H_

#include "proc_file.h"
#include "riscv.h"
#include "util/types.h"

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

  // owning hart id. used to restore tp in trap entry for multicore mode.
  /* offset:280 */ uint64 hartid;
} trapframe;

// riscv-pke kernel supports at most 32 processes
#define NPROC 32
// maximum number of pages in a process's heap
#define MAX_HEAP_PAGES 64

// Memory block structure for Better Malloc
typedef struct mem_block_t {
  int size;
  int free; // 0: free, 1: used
  struct mem_block_t *next;
} mem_block;

// possible status of a process
enum proc_status {
  FREE,    // unused state
  READY,   // ready state
  RUNNING, // currently running
  BLOCKED, // waiting for something
  ZOMBIE,  // terminated but not reclaimed yet
};

// types of a segment
enum segment_type {
  STACK_SEGMENT = 0, // runtime stack segment
  CONTEXT_SEGMENT,   // trapframe segment
  SYSTEM_SEGMENT,    // system segment
  HEAP_SEGMENT,      // runtime heap segment
  CODE_SEGMENT,      // ELF segment
  DATA_SEGMENT,      // ELF segment
};

// the VM regions mapped to a user process
typedef struct mapped_region {
  uint64 va;       // mapped virtual address
  uint32 npages;   // mapping_info is unused if npages == 0
  uint32 seg_type; // segment type, one of the segment_types
} mapped_region;

// code file struct, including directory index and file name char pointer
typedef struct {
  uint64 dir;
  char *file;
} code_file;

// address-line number-file name table
typedef struct {
  uint64 addr, line, file;
} addr_line;

// the extremely simple definition of process, used for begining labs of PKE
typedef struct process_t {
  // pointing to the stack used in trap handling.
  uint64 kstack;
  // user page table
  pagetable_t pagetable;
  // trapframe storing the context of a (User mode) process.
  // 中断帧指针 用于保护现场（例如pc、寄存器)
  trapframe *trapframe;

  // points to a page that contains mapped_regions. below are added @lab3_1
  mapped_region *mapped_info;
  // next free mapped region in mapped_info
  int total_mapped_region;

  // heap management
  // 用户堆管理
  struct {
    uint64 heap_bottom;
    uint64 heap_top;
    mem_block *heap_head;
    uint64 heap_pages_pa[MAX_HEAP_PAGES];
    uint64 heap_pages_va[MAX_HEAP_PAGES];
    int heap_pages_cnt;
  } user_heap;

  // process id
  uint64 pid;
  // process status
  int status;
  // parent process
  struct process_t *parent;
  // next queue element
  struct process_t *queue_next;

  // for wait syscall: pid of the child this process is waiting for (-1 = none)
  // added @lab4_challenge3
  int waiting_for_pid;

  // accounting. added @lab3_3
  int tick_count;

  // file system. added @lab4_1
  proc_file_management *pfiles;

  // app name for backtrace
  char app_name[128];

  // added @lab1_challenge2
  char *debugline;
  char **dir;
  code_file *file;
  addr_line *line;
  int line_ind;
} process;

// switch to run user app
void switch_to(process *);

// initialize process pool (the procs[] array)
void init_proc_pool();
// allocate an empty process, init its vm space. returns its pid
process *alloc_process();
// reclaim a process, destruct its vm space and free physical pages.
int free_process(process *proc);
// fork a child from parent
int do_fork(process *parent);
// exec: replace current process image with a new ELF
// added @lab4_challenge3
int do_exec(char *path, char *para);
// wait: block until child (pid) exits
// added @lab4_challenge3
int do_wait(int pid);

// current running process array
extern process *current_proc[NCPU];

// returns the hartid of the current hart using the tp register (set in
// mentry.S)
static inline int get_hartid() {
  int id;
  asm volatile("mv %0, tp" : "=r"(id));
  return id;
}

#define current (current_proc[get_hartid()])

#endif
