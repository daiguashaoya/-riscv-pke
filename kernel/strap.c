/*
 * Utility functions for trap handling in Supervisor mode.
 */

#include "strap.h"
#include "memlayout.h"
#include "pmm.h"
#include "process.h"
#include "riscv.h"
#include "sched.h"
#include "string.h"
#include "syscall.h"
#include "util/functions.h"
#include "vfs.h"
#include "vmm.h"

#include "spike_interface/spike_utils.h"

//
// handling the syscalls. will call do_syscall() defined in kernel/syscall.c
//
static void handle_syscall(trapframe *tf) {
  // tf->epc points to the address that our computer will jump to after the trap
  // handling. for a syscall, we should return to the NEXT instruction after its
  // handling. in RV64G, each instruction occupies exactly 32 bits (i.e., 4
  // Bytes)
  tf->epc += 4;

  // TODO (lab1_1): remove the panic call below, and call do_syscall (defined in
  // kernel/syscall.c) to conduct real operations of the kernel side for a
  // syscall. IMPORTANT: return value should be returned to user app, or else,
  // you will encounter problems in later experiments!
  tf->regs.a0 = do_syscall(tf->regs.a0, tf->regs.a1, tf->regs.a2, tf->regs.a3,
                           tf->regs.a4, tf->regs.a5, tf->regs.a6, tf->regs.a7);
}

//
// global variable that store the recorded "ticks". added @lab1_3
static uint64 g_ticks = 0;
//
// added @lab1_3
//
void handle_mtimer_trap() {
  sprint("Ticks %d\n", g_ticks);
  // TODO (lab1_3): increase g_ticks to record this "tick", and then clear the
  // "SIP" field in sip register. hint: use write_csr to disable the SIP_SSIP
  // bit in sip.
  g_ticks += 1;
  write_csr(sip, read_csr(sip) & ~SIP_SSIP);
}

//
// the page fault handler. added @lab2_3. parameters:
// sepc: the pc when fault happens;
// stval: the virtual address that causes pagefault when being accessed.
//
void handle_user_page_fault(uint64 mcause, uint64 sepc, uint64 stval) {
  sprint("handle_page_fault: %lx\n", stval);
  switch (mcause) {
  case CAUSE_STORE_PAGE_FAULT:
  case CAUSE_LOAD_PAGE_FAULT:
    if (mcause == CAUSE_STORE_PAGE_FAULT) {
      pte_t *pte = page_walk((pagetable_t)current->pagetable, stval, 0);
      if (pte != NULL && (*pte & PTE_COW)) {
        sprint("handle_page_fault: COW page copy for stval %lx\n", stval);
        void *old_pa = (void *)PTE2PA(*pte);
        void *new_pa = alloc_page();
        if (new_pa == 0)
          panic("Cant alloc more pages for COW!");
        memcpy(new_pa, old_pa, PGSIZE);

        *pte = PA2PTE((uint64)new_pa) | PTE_FLAGS(*pte);
        *pte = (*pte & ~PTE_COW) | PTE_W;

        free_page(old_pa);
        flush_tlb();
        return;
      }
    }

    // TODO (lab2_3): implement the operations that solve the page fault to
    // dynamically increase application stack.
    // hint: first allocate a new physical page, and then, maps the new page to
    // the virtual address that causes the page fault.
    if (stval < USER_STACK_TOP && stval >= USER_STACK_TOP - 20 * PGSIZE) {
      void *pa = alloc_page();
      if (pa == 0)
        panic("Cant alloc more pages!");
      // 设置为页尾的地址
      uint64 va = stval - (stval % PGSIZE);
      user_vm_map((pagetable_t)current->pagetable, va, PGSIZE, (uint64)pa,
                  prot_to_type(PROT_WRITE | PROT_READ, 1));
    } else {
      sprint("this address is not available!\n");
      sys_user_exit(-1);
    }

    break;
  default:
    sprint("unknown page fault.\n");
    break;
  }
}

//
// implements round-robin scheduling. added @lab3_3
//
void rrsched() {
  // TODO (lab3_3): implements round-robin scheduling.
  // hint: increase the tick_count member of current process by one, if it is
  // bigger than TIME_SLICE_LEN (means it has consumed its time slice), change
  // its status into READY, place it in the rear of ready queue, and finally
  // schedule next process to run.

  if (++current->tick_count >= TIME_SLICE_LEN) {
    current->status = READY;
    current->tick_count = 0;
    insert_to_ready_queue(current);
    schedule();
  }
  return;
}

// print the source file name and the code line where exception happened.
void print_errorline(uint64 mepc) {
  if (!current->line)
    return;

  int best = -1;
  for (int i = 0; i < current->line_ind; i++) {
    if (current->line[i].addr <= mepc) {
      if (best == -1 || current->line[i].addr > current->line[best].addr)
        best = i;
    }
  }
  if (best != -1) {
    int i = best;
    uint64 file_idx = current->line[i].file;
    uint64 line_no = current->line[i].line;
    char *filename = current->file[file_idx].file;
    uint64 dir_idx = current->file[file_idx].dir;
    char *dir = (dir_idx != (uint64)-1) ? current->dir[dir_idx] : NULL;

    char fullpath[128];
    if (dir && dir[0] != '\0') {
      strcpy(fullpath, dir);
      fullpath[strlen(dir)] = '/';
      strcpy(fullpath + strlen(dir) + 1, filename);
    } else {
      strcpy(fullpath, filename);
    }

    sprint("Runtime error at %s:%d\n", fullpath, (int)line_no);

    struct file *f = vfs_open(fullpath, O_RDONLY);
    if (IS_ERR_VALUE(f))
      return;

    char c;
    uint64 cur_line = 1;
    while (cur_line < line_no) {
      if (vfs_read(f, &c, 1) <= 0)
        break;
      if (c == '\n')
        cur_line++;
    }

    char buf[256];
    int len = 0;
    while (vfs_read(f, &c, 1) > 0 && c != '\n' && len < 255) {
      buf[len++] = c;
    }
    buf[len] = '\0';

    if (len > 0)
      sprint("%s\n", buf);

    vfs_close(f);
  }
}

//
// kernel/smode_trap.S will pass control to smode_trap_handler, when a trap
// happens in S-mode.
//
void smode_trap_handler(void) {
  // make sure we are in User mode before entering the trap handling.
  // we will consider other previous case in lab1_3 (interrupt).
  if ((read_csr(sstatus) & SSTATUS_SPP) != 0)
    panic("usertrap: not from user mode");

  assert(current);
  // save user process counter.
  current->trapframe->epc = read_csr(sepc);

  // if the cause of trap is syscall from user application.
  // read_csr() and CAUSE_USER_ECALL are macros defined in kernel/riscv.h
  uint64 cause = read_csr(scause);

  // use switch-case instead of if-else, as there are many cases since lab2_3.
  switch (cause) {
  case CAUSE_USER_ECALL:
    handle_syscall(current->trapframe);
    break;
  case CAUSE_MTIMER_S_TRAP:
    handle_mtimer_trap();
    // invoke round-robin scheduler. added @lab3_3
    rrsched();
    break;
  case CAUSE_STORE_PAGE_FAULT:
  case CAUSE_LOAD_PAGE_FAULT:
    // the address of missing page is stored in stval
    // call handle_user_page_fault to process page faults
    handle_user_page_fault(cause, read_csr(sepc), read_csr(stval));
    break;
  default:
    sprint("smode_trap_handler(): unexpected scause %p\n", read_csr(scause));
    sprint("            sepc=%p stval=%p\n", read_csr(sepc), read_csr(stval));
    print_errorline(read_csr(sepc));
    panic("unexpected exception happened.\n");
    break;
  }

  // continue (come back to) the execution of current process.
  switch_to(current);
}
