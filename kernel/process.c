/*
 * Utility functions for process management.
 *
 * Note: in Lab2_challenge3, two processes exist (one per hart).
 * current[] and g_ufree_page[] are per-hart arrays.
 */

#include "process.h"
#include "config.h"
#include "elf.h"
#include "memlayout.h"
#include "pmm.h"
#include "riscv.h"
#include "spike_interface/spike_utils.h"
#include "strap.h"
#include "string.h"
#include "vmm.h"

// Two functions defined in kernel/usertrap.S
extern char smode_trap_vector[];
extern void return_to_user(trapframe *, uint64 satp);

// Per-hart current running process. added @lab2_challenge3
process *current[NCPU];

// Per-hart heap free-page virtual address pointer. added @lab2_challenge3
// Each process gets its own independent virtual address space for naive_malloc.
uint64 g_ufree_page[NCPU] = {[0 ... NCPU - 1] = USER_FREE_ADDRESS_START};

//
// switch to a user-mode process
//
void switch_to(process *proc) {
  assert(proc);

  // Read hartid from tp register. added @lab2_challenge3
  uint64 hartid;
  asm volatile("mv %0, tp" : "=r"(hartid));

  current[hartid] = proc;

  // Store hartid in trapframe so strap_vector.S can restore tp from it.
  // (User code may freely overwrite tp as a TLS pointer.) added
  // @lab2_challenge3
  proc->trapframe->hartid = hartid;
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
  // with sret.
  return_to_user(proc->trapframe, user_satp);
}
