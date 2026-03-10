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
#undef current
process *current_proc[NCPU] = {NULL};
#define current (current_proc[get_hartid()])

//
// switch to a user-mode process
//
void switch_to(process *proc) {
  assert(proc);
  int hartid = get_hartid();
  current_proc[hartid] = proc;

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
  proc->trapframe->hartid = hartid;

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
  procs[i].user_heap.heap_head = NULL;
  procs[i].user_heap.heap_pages_cnt = 0;

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
  // 检查父进程是否在等待这个子进程 (包括等待任意子进程情况: -1)
  if (proc->parent != NULL && proc->parent->status == BLOCKED &&
      (proc->parent->waiting_for_pid == (int)proc->pid ||
       proc->parent->waiting_for_pid == -1)) {
    proc->parent->waiting_for_pid = -1;
    proc->parent->trapframe->regs.a0 =
        proc->pid; // 唤醒时设置返回值为退出的子进程PID
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
    // COW shared mapping for Heap
    case HEAP_SEGMENT: {
      for (int h = 0; h < parent->user_heap.heap_pages_cnt; h++) {
        uint64 hb = parent->user_heap.heap_pages_va[h];
        void *pa = (void *)lookup_pa(parent->pagetable, hb);
        inc_page_ref(pa);

        pte_t *pte_parent = page_walk(parent->pagetable, hb, 0);
        *pte_parent = (*pte_parent & ~(PTE_W | PTE_D)) | PTE_COW | PTE_A;

        user_vm_map((pagetable_t)child->pagetable, hb, PGSIZE, (uint64)pa,
                    prot_to_type(PROT_READ, 1));

        pte_t *pte_child = page_walk((pagetable_t)child->pagetable, hb, 0);
        *pte_child |= PTE_COW | PTE_A;

        child->user_heap.heap_pages_pa[h] =
            (uint64)pa; // It will be the same pa
        child->user_heap.heap_pages_va[h] = hb;
      }
      child->user_heap.heap_pages_cnt = parent->user_heap.heap_pages_cnt;
      child->user_heap.heap_bottom = parent->user_heap.heap_bottom;
      child->user_heap.heap_top = parent->user_heap.heap_top;
      child->user_heap.heap_head = parent->user_heap.heap_head;
      child->mapped_info[HEAP_SEGMENT].npages =
          parent->user_heap.heap_pages_cnt;
      flush_tlb();
    } break;
    case CODE_SEGMENT: {
      // map child code to parent's physical code pages (shared, not copied)
      uint64 code_pa = lookup_pa(parent->pagetable, parent->mapped_info[i].va);
      map_pages(child->pagetable, parent->mapped_info[i].va,
                parent->mapped_info[i].npages * PGSIZE, code_pa,
                prot_to_type(PROT_EXEC | PROT_READ, 1));
      sprint(
          "do_fork map code segment at pa:%lx of parent to child at va:%lx.\n",
          code_pa, parent->mapped_info[i].va);

      // after mapping, register the vm region (do not delete codes below!)
      child->mapped_info[child->total_mapped_region].va =
          parent->mapped_info[i].va;
      child->mapped_info[child->total_mapped_region].npages =
          parent->mapped_info[i].npages;
      child->mapped_info[child->total_mapped_region].seg_type = CODE_SEGMENT;
      child->total_mapped_region++;
    } break;
    // 复制数据段 (added @lab4_challenge3)
    // COW shared mapping for Data
    case DATA_SEGMENT: {
      for (int pg = 0; pg < (int)parent->mapped_info[i].npages; pg++) {
        uint64 data_va = parent->mapped_info[i].va + pg * PGSIZE;
        void *pa = (void *)lookup_pa(parent->pagetable, data_va);
        inc_page_ref(pa);

        pte_t *pte_parent = page_walk(parent->pagetable, data_va, 0);
        *pte_parent = (*pte_parent & ~(PTE_W | PTE_D)) | PTE_COW | PTE_A;

        user_vm_map((pagetable_t)child->pagetable, data_va, PGSIZE, (uint64)pa,
                    prot_to_type(PROT_READ, 1));

        pte_t *pte_child = page_walk((pagetable_t)child->pagetable, data_va, 0);
        *pte_child |= PTE_COW | PTE_A;
      }
      child->mapped_info[child->total_mapped_region].va =
          parent->mapped_info[i].va;
      child->mapped_info[child->total_mapped_region].npages =
          parent->mapped_info[i].npages;
      child->mapped_info[child->total_mapped_region].seg_type = DATA_SEGMENT;
      child->total_mapped_region++;
      flush_tlb();
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
  if (path == NULL)
    return -1;
  if (para == NULL)
    para = "";
  if (para[0] == '\0')
    para = "/RAMDISK0";

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
  for (int h = 0; h < current->user_heap.heap_pages_cnt; h++) {
    user_vm_unmap(current->pagetable, current->user_heap.heap_pages_va[h],
                  PGSIZE, 1);
  }
  current->user_heap.heap_top = USER_FREE_ADDRESS_START;
  current->user_heap.heap_bottom = USER_FREE_ADDRESS_START;
  current->user_heap.heap_head = NULL;
  current->user_heap.heap_pages_cnt = 0;
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
int do_wait(int pid) {
  // 遍历所有进程，寻找属于当前进程的子进程
  int has_child = 0;

  for (int i = 0; i < NPROC; i++) {
    // 筛选条件: 是当前进程的子进程
    if (procs[i].parent == current) {
      // 筛选 PID: pid==-1 (任意) 或 pid匹配
      if (pid == -1 || (int)procs[i].pid == pid) {
        has_child = 1;

        // 情况 1: 发现僵尸子进程 (已退出)
        if (procs[i].status == ZOMBIE) {
          int zpid = procs[i].pid;
          // 回收资源
          procs[i].status = FREE;
          return zpid;
        }
      }
    }
  }

  // 情况 2: 还有符合条件的子进程在运行，父进程进入阻塞状态
  if (has_child) {
    current->status = BLOCKED;      // 设为阻塞
    current->waiting_for_pid = pid; // 记录在等谁
    schedule();                     // 让出 CPU
    // 注意：当被唤醒时，返回值由唤醒者(子进程exit)直接写入 trapframe->a0
    return 0; // 这里的返回值实际上会被覆盖
  }

  // 情况 3: 没有找到任何符合条件的子进程
  return -1;
}
