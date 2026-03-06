/*
 * Utility functions for trap handling in Supervisor mode.
 */

#include "strap.h"
#include "memlayout.h"
#include "pmm.h"
#include "process.h"
#include "riscv.h"
#include "sched.h"
#include "syscall.h"
#include "util/functions.h"
#include "vmm.h"
#include <string.h>

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
    // 首先检查是否是因为 COW 引发的页错误
    pte_t *pte = page_walk(current->pagetable, stval, 0);
    if (pte != 0 && (*pte & PTE_V) && (*pte & PTE_COW)) {
      // 这是 COW 造成的异常
      uint64 pa = PTE2PA(*pte); // 之前被共享的旧的物理地址

      // 尝试分配新的一页物理内存
      void *new_pa = alloc_page();
      if (new_pa == 0)
        panic("COW: out of memory");

      // 拷贝原物理页的内容到新的物理页
      memcpy(new_pa, (void *)ROUNDDOWN((uint64)pa, PGSIZE), PGSIZE);

      // 重新设置该页的PTE，去掉 COW 标志，并加上 PTE_W 允许当前进程合法写操作
      *pte = PA2PTE(new_pa) | ((PTE_FLAGS(*pte) | PTE_W) & ~PTE_COW);

      // 最后释放对原来旧共享页面的引用计数，如果只剩1个引用则通过底层自动收回真正空间
      free_page((void *)pa);

      // 刷新 TLB 生效变更
      flush_tlb();
      break; // 放行操作，让其重试缺页触发的当前行指令即可
    }
    if (stval < USER_STACK_TOP && stval >= USER_STACK_TOP - 20 * PGSIZE) {
      void *pa = alloc_page();
      if (pa == 0)
        panic("Cant alloc more pages!");
      // 设置为页尾的地址
      uint64 va = stval - (stval % PGSIZE);
      user_vm_map((pagetable_t)current->pagetable, va, PGSIZE, (uint64)pa,
                  prot_to_type(PROT_WRITE | PROT_READ, 1));
    } else
      panic("Invalid address acess!");

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
    panic("unexpected exception happened.\n");
    break;
  }

  // continue (come back to) the execution of current process.
  switch_to(current);
}
