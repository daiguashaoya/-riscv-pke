/*
 * Utility functions for process management.
 *
 * Note: in Lab1, only one process (i.e., our user application) exists.
 * Therefore, PKE OS at this stage will set "current" to the loaded user
 * application, and also switch to the old "current" process after trap
 * handling.
 */

#include "process.h"
#include "config.h"
#include "elf.h"
#include "memlayout.h"
#include "pmm.h"
#include "riscv.h"
#include "sched.h"
#include "spike_interface/spike_utils.h"
#include "strap.h"
#include "string.h"
#include "util/functions.h" // for ROUNDDOWN, added @lab4_challenge3
#include "vmm.h"

// Two functions defined in kernel/usertrap.S
extern char smode_trap_vector[];
extern void return_to_user(trapframe *, uint64 satp);

// trap_sec_start points to the beginning of S-mode trap segment (i.e., the
// entry point of S-mode trap vector).
extern char trap_sec_start[];

// process pool. added @lab3_1
process procs[NPROC];

// current points to the currently running user-mode application.
process *current = NULL;

//
// switch to a user-mode process
//
void switch_to(process *proc) {
  assert(proc);
  current = proc;

  // write the smode_trap_vector (64-bit func. address) defined in
  // kernel/strap_vector.S to the stvec privilege register, such that trap
  // handler pointed by smode_trap_vector will be triggered when an interrupt
  // occurs in S mode.
  write_csr(stvec, (uint64)smode_trap_vector);

  // set up trapframe values (in process structure) that smode_trap_vector will
  // need when the process next re-enters the kernel.
  proc->trapframe->kernel_sp = proc->kstack;     // process's kernel stack
  proc->trapframe->kernel_satp = read_csr(satp); // kernel page table
  proc->trapframe->kernel_trap = (uint64)smode_trap_handler;

  // SSTATUS_SPP and SSTATUS_SPIE are defined in kernel/riscv.h
  // set S Previous Privilege mode (the SSTATUS_SPP bit in sstatus register) to
  // User mode.
  unsigned long x = read_csr(sstatus);
  x &= ~SSTATUS_SPP; // clear SPP to 0 for user mode
  x |= SSTATUS_SPIE; // enable interrupts in user mode

  // write x back to 'sstatus' register to enable interrupts, and sret
  // destination mode.
  write_csr(sstatus, x);

  // set S Exception Program Counter (sepc register) to the elf entry pc.
  write_csr(sepc, proc->trapframe->epc);

  // make user page table. macro MAKE_SATP is defined in kernel/riscv.h. added
  // @lab2_1
  uint64 user_satp = MAKE_SATP(proc->pagetable);

  // return_to_user() is defined in kernel/strap_vector.S. switch to user mode
  // with sret. note, return_to_user takes two parameters @ and after lab2_1.
  return_to_user(proc->trapframe, user_satp);
}

//
// initialize process pool (the procs[] array). added @lab3_1
//
void init_proc_pool() {
  memset(procs, 0, sizeof(process) * NPROC);

  for (int i = 0; i < NPROC; ++i) {
    procs[i].status = FREE;
    procs[i].pid = i;
  }
}

//
// allocate an empty process, init its vm space. returns the pointer to
// process strcuture. added @lab3_1
//
process *alloc_process() {
  // locate the first usable process structure
  int i;

  for (i = 0; i < NPROC; i++)
    if (procs[i].status == FREE)
      break;

  if (i >= NPROC) {
    panic("cannot find any free process structure.\n");
    return 0;
  }

  // init proc[i]'s vm space
  procs[i].trapframe =
      (trapframe *)alloc_page(); // trapframe, used to save context
  memset(procs[i].trapframe, 0, sizeof(trapframe));

  // page directory
  procs[i].pagetable = (pagetable_t)alloc_page();
  memset((void *)procs[i].pagetable, 0, PGSIZE);

  procs[i].kstack = (uint64)alloc_page() + PGSIZE; // user kernel stack top
  uint64 user_stack =
      (uint64)alloc_page(); // phisical address of user stack bottom
  procs[i].trapframe->regs.sp =
      USER_STACK_TOP; // virtual address of user stack top

  // allocates a page to record memory regions (segments)
  procs[i].mapped_info = (mapped_region *)alloc_page();
  memset(procs[i].mapped_info, 0, PGSIZE);

  // map user stack in userspace
  user_vm_map((pagetable_t)procs[i].pagetable, USER_STACK_TOP - PGSIZE, PGSIZE,
              user_stack, prot_to_type(PROT_WRITE | PROT_READ, 1));
  procs[i].mapped_info[STACK_SEGMENT].va = USER_STACK_TOP - PGSIZE;
  procs[i].mapped_info[STACK_SEGMENT].npages = 1;
  procs[i].mapped_info[STACK_SEGMENT].seg_type = STACK_SEGMENT;

  // map trapframe in user space (direct mapping as in kernel space).
  user_vm_map((pagetable_t)procs[i].pagetable, (uint64)procs[i].trapframe,
              PGSIZE, (uint64)procs[i].trapframe,
              prot_to_type(PROT_WRITE | PROT_READ, 0));
  procs[i].mapped_info[CONTEXT_SEGMENT].va = (uint64)procs[i].trapframe;
  procs[i].mapped_info[CONTEXT_SEGMENT].npages = 1;
  procs[i].mapped_info[CONTEXT_SEGMENT].seg_type = CONTEXT_SEGMENT;

  // map S-mode trap vector section in user space (direct mapping as in kernel
  // space) we assume that the size of usertrap.S is smaller than a page.
  user_vm_map((pagetable_t)procs[i].pagetable, (uint64)trap_sec_start, PGSIZE,
              (uint64)trap_sec_start, prot_to_type(PROT_READ | PROT_EXEC, 0));
  procs[i].mapped_info[SYSTEM_SEGMENT].va = (uint64)trap_sec_start;
  procs[i].mapped_info[SYSTEM_SEGMENT].npages = 1;
  procs[i].mapped_info[SYSTEM_SEGMENT].seg_type = SYSTEM_SEGMENT;

  sprint(
      "in alloc_proc. user frame 0x%lx, user stack 0x%lx, user kstack 0x%lx \n",
      procs[i].trapframe, procs[i].trapframe->regs.sp, procs[i].kstack);

  // initialize the process's heap manager
  procs[i].user_heap.heap_top = USER_FREE_ADDRESS_START;
  procs[i].user_heap.heap_bottom = USER_FREE_ADDRESS_START;
  procs[i].user_heap.free_pages_count = 0;

  // map user heap in userspace
  procs[i].mapped_info[HEAP_SEGMENT].va = USER_FREE_ADDRESS_START;
  procs[i].mapped_info[HEAP_SEGMENT].npages =
      0; // no pages are mapped to heap yet.
  procs[i].mapped_info[HEAP_SEGMENT].seg_type = HEAP_SEGMENT;

  procs[i].total_mapped_region = 4;

  // initialize files_struct
  procs[i].pfiles = init_proc_file_management();
  sprint("in alloc_proc. build proc_file_management successfully.\n");

  procs[i].waiting_for_pid = -1; // added @lab4_challenge3

  // return after initialization.
  return &procs[i];
}

//
// reclaim a process. added @lab3_1
//
int free_process(process *proc) {
  // we set the status to ZOMBIE, but cannot destruct its vm space immediately.
  // since proc can be current process, and its user kernel stack is currently
  // in use! but for proxy kernel, it (memory leaking) may NOT be a really
  // serious issue, as it is different from regular OS, which needs to run 7x24.
  proc->status = ZOMBIE;

  // if the parent process is waiting for this child, wake it up. added
  // @lab4_challenge3
  if (proc->parent != NULL && proc->parent->status == BLOCKED &&
      proc->parent->waiting_for_pid == (int)proc->pid) {
    proc->parent->waiting_for_pid = -1;
    insert_to_ready_queue(proc->parent);
  }

  return 0;
}

//
// implements fork syscal in kernel. added @lab3_1
// basic idea here is to first allocate an empty process (child), then duplicate
// the context and data segments of parent process to the child, and lastly, map
// other segments (code, system) of the parent to child. the stack segment
// remains unchanged for the child.
//
int do_fork(process *parent) {
  sprint("will fork a child from parent %d.\n", parent->pid);
  process *child = alloc_process();

  for (int i = 0; i < parent->total_mapped_region; i++) {
    // browse parent's vm space, and copy its trapframe and data segments,
    // map its code segment.
    switch (parent->mapped_info[i].seg_type) {
    // 复制中断帧
    case CONTEXT_SEGMENT:
      *child->trapframe = *parent->trapframe;
      break;
    // 复制用户栈内容
    case STACK_SEGMENT:
      memcpy((void *)lookup_pa(child->pagetable,
                               child->mapped_info[STACK_SEGMENT].va),
             (void *)lookup_pa(parent->pagetable, parent->mapped_info[i].va),
             PGSIZE);
      break;
    // 复制用了的堆页 (added @lab4_challenge3)
    case HEAP_SEGMENT: {
      // build a same heap for child by copying every live heap page
      int free_block_filter[MAX_HEAP_PAGES];
      memset(free_block_filter, 0, sizeof(free_block_filter));
      uint64 heap_bottom = parent->user_heap.heap_bottom;
      for (int j = 0; j < (int)parent->user_heap.free_pages_count; j++) {
        int idx =
            (int)((parent->user_heap.free_pages_address[j] - heap_bottom) /
                  PGSIZE);
        free_block_filter[idx] = 1;
      }
      for (uint64 hb = parent->user_heap.heap_bottom;
           hb < parent->user_heap.heap_top; hb += PGSIZE) {
        int idx = (int)((hb - heap_bottom) / PGSIZE);
        if (free_block_filter[idx])
          continue; // skip freed pages
        void *child_pa = alloc_page();
        memcpy(child_pa, (void *)lookup_pa(parent->pagetable, hb), PGSIZE);
        user_vm_map((pagetable_t)child->pagetable, hb, PGSIZE, (uint64)child_pa,
                    prot_to_type(PROT_WRITE | PROT_READ, 1));
      }
      child->mapped_info[HEAP_SEGMENT].npages =
          parent->mapped_info[HEAP_SEGMENT].npages;
      memcpy((void *)&child->user_heap, (void *)&parent->user_heap,
             sizeof(parent->user_heap));
    } break;
    case CODE_SEGMENT:
      // map child code to parent's physical code pages (shared, not copied)
      map_pages(child->pagetable, parent->mapped_info[i].va,
                parent->mapped_info[i].npages * PGSIZE,
                lookup_pa(parent->pagetable, parent->mapped_info[i].va),
                prot_to_type(PROT_EXEC | PROT_READ, 1));

      // after mapping, register the vm region (do not delete codes below!)
      child->mapped_info[child->total_mapped_region].va =
          parent->mapped_info[i].va;
      child->mapped_info[child->total_mapped_region].npages =
          parent->mapped_info[i].npages;
      child->mapped_info[child->total_mapped_region].seg_type = CODE_SEGMENT;
      child->total_mapped_region++;
      break;
    // 复制数据段 (added @lab4_challenge3)
    case DATA_SEGMENT: {
      // DATA pages are per-process, so copy them (like STACK)
      for (int pg = 0; pg < (int)parent->mapped_info[i].npages; pg++) {
        uint64 data_va = parent->mapped_info[i].va + pg * PGSIZE;
        void *child_pa = alloc_page();
        memcpy(child_pa, (void *)lookup_pa(parent->pagetable, data_va), PGSIZE);
        user_vm_map((pagetable_t)child->pagetable, data_va, PGSIZE,
                    (uint64)child_pa, prot_to_type(PROT_WRITE | PROT_READ, 1));
      }
      child->mapped_info[child->total_mapped_region].va =
          parent->mapped_info[i].va;
      child->mapped_info[child->total_mapped_region].npages =
          parent->mapped_info[i].npages;
      child->mapped_info[child->total_mapped_region].seg_type = DATA_SEGMENT;
      child->total_mapped_region++;
    } break;
    }
  }

  child->status = READY;
  child->trapframe->regs.a0 = 0;
  child->parent = parent;
  insert_to_ready_queue(child);

  return child->pid;
}

//
// do_exec: replace the current process's ELF image with a new one.
// path: absolute path to the new ELF file (in VFS)
// para: a single string argument to pass to the new program's main()
// added @lab4_challenge3
//
int do_exec(char *path, char *para) {
  // Step 1: unmap old CODE and DATA segments
  for (int i = 0; i < current->total_mapped_region; i++) {
    if (current->mapped_info[i].seg_type == CODE_SEGMENT) {
      // CODE pages are shared with the parent — do NOT free them (free=0)
      user_vm_unmap(current->pagetable, current->mapped_info[i].va,
                    current->mapped_info[i].npages * PGSIZE, 0);
      current->mapped_info[i].va = 0;
      current->mapped_info[i].npages = 0;
      current->mapped_info[i].seg_type = 0;
    } else if (current->mapped_info[i].seg_type == DATA_SEGMENT) {
      // DATA pages were copied during fork — safe to free (free=1)
      user_vm_unmap(current->pagetable, current->mapped_info[i].va,
                    current->mapped_info[i].npages * PGSIZE, 1);
      current->mapped_info[i].va = 0;
      current->mapped_info[i].npages = 0;
      current->mapped_info[i].seg_type = 0;
    }
  }

  // Step 2: reset total_mapped_region to the 4 fixed segments
  // (STACK=0, CONTEXT=1, SYSTEM=2, HEAP=3)
  current->total_mapped_region = 4;

  // NOTE: do NOT free heap pages yet — `path` and `para` point into heap
  // physical memory and must remain valid until we finish using them.

  // Step 3: load the new ELF into the current process
  // this sets current->trapframe->epc to the new entry point
  load_bincode_from_host_elf(current, path);

  // Step 4: clear the user stack
  uint64 stack_va =
      current->mapped_info[STACK_SEGMENT].va; // USER_STACK_TOP - PGSIZE
  uint64 stack_pa = lookup_pa(current->pagetable, stack_va);
  memset((void *)stack_pa, 0, PGSIZE);

  // Step 5: build argument layout on the user stack.
  uint64 sp = USER_STACK_TOP; // will be decremented before first use

  // Place the para string (grows downward)
  int para_len = strlen(para) + 1; // including null terminator
  sp -= para_len;
  sp = ROUNDDOWN(sp, 8); // 8-byte align the string start
  kassert(sp >= stack_va);
  char *pa_str = (char *)(stack_pa + (sp - stack_va));
  memcpy(pa_str, para, para_len);
  uint64 para_va = sp; // VA of para string in user space

  // Build argv[] array on the stack (two entries: argv[0] and NULL terminator)
  sp -= sizeof(uint64); // slot for argv[1] = NULL
  kassert(sp >= stack_va);
  *(uint64 *)(stack_pa + (sp - stack_va)) = 0UL;

  sp -= sizeof(uint64); // slot for argv[0] = &para string
  kassert(sp >= stack_va);
  *(uint64 *)(stack_pa + (sp - stack_va)) = para_va;

  uint64 argv_va = sp; // VA of argv[0] in user space (a1)

  // 16-byte align sp for RISC-V ABI
  sp = ROUNDDOWN(sp, 16);

  // Step 6: now that path and para have been fully consumed, clean up old heap
  for (uint64 hb = current->user_heap.heap_bottom;
       hb < current->user_heap.heap_top; hb += PGSIZE) {
    user_vm_unmap(current->pagetable, hb, PGSIZE, 1);
  }
  current->user_heap.heap_top = USER_FREE_ADDRESS_START;
  current->user_heap.heap_bottom = USER_FREE_ADDRESS_START;
  current->user_heap.free_pages_count = 0;
  current->mapped_info[HEAP_SEGMENT].npages = 0;

  // Step 7: set up trapframe registers for the new program entry
  // a0 = argc = 1, a1 = argv (pointer to argv[0])
  current->trapframe->regs.sp = sp;
  current->trapframe->regs.a0 = 1;       // argc
  current->trapframe->regs.a1 = argv_va; // argv

  return 0;
}

//
// do_wait: block current process until child (pid) exits.
// added @lab4_challenge3
//
int do_wait(int pid) {
  // find the target child process
  process *child = NULL;
  for (int i = 0; i < NPROC; i++) {
    if ((int)procs[i].pid == pid && procs[i].parent == current) {
      child = &procs[i];
      break;
    }
  }
  if (child == NULL)
    return -1;

  // if child already exited, return immediately
  if (child->status == ZOMBIE) {
    child->status = FREE;
    return pid;
  }

  // block current process and wait for child to exit
  current->waiting_for_pid = pid;
  current->status = BLOCKED;
  schedule(); // switch to another process; free_process() will re-queue us

  return pid;
}
