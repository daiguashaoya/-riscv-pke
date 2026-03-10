#include "../process.h"
#include "kernel/config.h"
#include "kernel/riscv.h"
#include "spike_interface/spike_utils.h"

static void handle_instruction_access_fault() {
  panic("Instruction access fault!");
}

static void handle_load_access_fault() { panic("Load access fault!"); }

static void handle_store_access_fault() { panic("Store/AMO access fault!"); }

extern void print_errorline(uint64 mepc);

extern void sys_user_exit(uint64 code);
extern uint64 g_itrframe[NCPU][64];

static void handle_illegal_instruction() {
  print_errorline(read_csr(mepc));
  sprint("Illegal instruction!\n");

  if (current) {
    write_csr(mepc, (uint64)sys_user_exit);
    uint64 mstat = read_csr(mstatus);
    mstat = (mstat & ~(3ULL << 11)) | (1ULL << 11); // MPP = S-mode
    write_csr(mstatus, mstat);
    g_itrframe[get_hartid()][9] = -1;              // a0
    g_itrframe[get_hartid()][1] = current->kstack; // sp
    write_csr(satp, current->trapframe->kernel_satp);
    flush_tlb();
  } else {
    panic("Illegal instruction!");
  }
}

static void handle_misaligned_load() { panic("Misaligned Load!"); }

static void handle_misaligned_store() {
  unsigned long mepc = read_csr(mepc);
  unsigned long mtval = read_csr(mtval);
  unsigned long sscratch_val = read_csr(sscratch);
  unsigned long sepc = read_csr(sepc);
  unsigned long stval = read_csr(stval);
  sprint("mepc=0x%016lx mtval=0x%016lx sscratch=0x%0lx\n", mepc, mtval,
         sscratch_val);
  sprint("sepc=0x%016lx stval=0x%016lx\n", sepc, stval);
  // extern process *current; // Removed because current is a macro now
  if (current) {
    sprint("current=%p, pid=%d, trapframe=%p\n", current, current->pid,
           current->trapframe);
  }
  panic("Misaligned AMO!");
}

// added @lab1_3
static void handle_timer() {
  int cpuid = read_csr(mhartid);
  // setup the timer fired at next time (TIMER_INTERVAL from now)
  *(uint64 *)CLINT_MTIMECMP(cpuid) =
      *(uint64 *)CLINT_MTIMECMP(cpuid) + TIMER_INTERVAL;

  // setup a soft interrupt in sip (S-mode Interrupt Pending) to be handled in
  // S-mode
  write_csr(sip, SIP_SSIP);
}

//
// handle_mtrap calls a handling function according to the type of a machine
// mode interrupt (trap).
//
void handle_mtrap() {
  uint64 mcause = read_csr(mcause);
  switch (mcause) {
  case CAUSE_MTIMER:
    handle_timer();
    break;
  case CAUSE_FETCH_ACCESS:
    handle_instruction_access_fault();
    break;
  case CAUSE_LOAD_ACCESS:
    handle_load_access_fault();
  case CAUSE_STORE_ACCESS:
    handle_store_access_fault();
    break;
  case CAUSE_ILLEGAL_INSTRUCTION:
    handle_illegal_instruction();
    break;
  case CAUSE_MISALIGNED_LOAD:
    handle_misaligned_load();
    break;
  case CAUSE_MISALIGNED_STORE:
    handle_misaligned_store();
    break;

  default:
    sprint("machine trap(): unexpected mscause %p\n", mcause);
    sprint("            mepc=%p mtval=%p\n", read_csr(mepc), read_csr(mtval));
    panic("unexpected exception happened in M-mode.\n");
    break;
  }
}
