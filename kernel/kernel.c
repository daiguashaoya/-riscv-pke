/*
 * Supervisor-mode startup codes
 */

#include "elf.h"
#include "memlayout.h"
#include "pmm.h"
#include "process.h"
#include "ramdev.h"
#include "rfs.h"
#include "riscv.h"
#include "sched.h"
#include "spike_interface/spike_utils.h"
#include "string.h"
#include "sync_utils.h"
#include "util/types.h"
#include "vfs.h"
#include "vmm.h"

//
// trap_sec_start points to the beginning of S-mode trap segment (i.e., the
// entry point of S-mode trap vector). added @lab2_1
//
extern char trap_sec_start[];

//
// turn on paging. added @lab2_1
//
void enable_paging() {
  // write the pointer to kernel page (table) directory into the CSR of "satp".
  write_csr(satp, MAKE_SATP(g_kernel_pagetable));

  // refresh tlb to invalidate its content.
  flush_tlb();
}

typedef union {
  uint64 buf[MAX_CMDLINE_ARGS];
  char *argv[MAX_CMDLINE_ARGS];
} arg_buf;

enum boot_mode {
  BOOT_MODE_SINGLE_APP = 0,
  BOOT_MODE_MULTI_APP = 1,
};

static volatile int g_boot_mode = BOOT_MODE_SINGLE_APP;
static volatile int g_boot_args_ready = 0;
static size_t g_boot_argc = 0;
static char *g_boot_argv[MAX_CMDLINE_ARGS];
static char g_boot_arg_storage[MAX_CMDLINE_ARGS][128];
static process *g_multi_boot_proc[NCPU] = {0};

int kernel_is_multi_app_mode(void) {
  return g_boot_mode == BOOT_MODE_MULTI_APP;
}

//
// returns the number (should be 1) of string(s) after PKE kernel in command
// line. and store the string(s) in arg_bug_msg.
//
static size_t parse_args(arg_buf *arg_bug_msg) {
  // HTIFSYS_getmainvars frontend call reads command arguments to (input)
  // *arg_bug_msg
  long r = frontend_syscall(HTIFSYS_getmainvars, (uint64)arg_bug_msg,
                            sizeof(*arg_bug_msg), 0, 0, 0, 0, 0);
  kassert(r == 0);

  size_t pk_argc = arg_bug_msg->buf[0];
  uint64 *pk_argv = &arg_bug_msg->buf[1];

  int arg = 1; // skip the PKE OS kernel string, leave behind only the
               // application name
  for (size_t i = 0; arg + i < pk_argc; i++)
    arg_bug_msg->argv[i] = (char *)(uintptr_t)pk_argv[arg + i];

  // returns the number of strings after PKE kernel in command line
  return pk_argc - arg;
}

//
// capture startup args once on hart0, then share via globals.
//
static void init_boot_args(void) {
  arg_buf arg_bug_msg;
  size_t argc = parse_args(&arg_bug_msg);
  if (!argc)
    panic("You need to specify the application program!\n");

  if (argc > MAX_CMDLINE_ARGS)
    argc = MAX_CMDLINE_ARGS;

  g_boot_argc = argc;
  for (size_t i = 0; i < argc; i++) {
    const char *src = arg_bug_msg.argv[i];
    size_t j = 0;
    for (; src[j] && j + 1 < sizeof(g_boot_arg_storage[i]); j++)
      g_boot_arg_storage[i][j] = src[j];
    g_boot_arg_storage[i][j] = '\0';
    g_boot_argv[i] = g_boot_arg_storage[i];
  }
  for (size_t i = argc; i < MAX_CMDLINE_ARGS; i++)
    g_boot_argv[i] = 0;

  g_boot_mode = (argc >= NCPU) ? BOOT_MODE_MULTI_APP : BOOT_MODE_SINGLE_APP;
  g_boot_args_ready = 1;
}

//
// load the elf, and construct a process.
//
static process *load_user_program(char *app_path, int target_hart) {
  process *proc = alloc_process();
  proc->status = READY;
  sprint("hartid = %d: User application is loading.\n", target_hart);
  load_bincode_from_host_elf(proc, app_path);
  proc->trapframe->regs.tp = target_hart;
  return proc;
}

//
// s_start: S-mode entry point of riscv-pke OS kernel.
//
int s_start(void) {
  int hartid = get_hartid();
  sprint("hartid = %d: Enter supervisor mode...\n", hartid);

  write_csr(satp, 0);

  // only hart0 initializes the core OS structures
  if (hartid == 0) {
    // init phisical memory manager
    pmm_init();
    // build the kernel page table
    kern_vm_init();
  }

  static volatile int s_vm_init_count = 0;
  sync_barrier(&s_vm_init_count, NCPU);

  // now, switch to paging mode by turning on paging (SV39)
  enable_paging();

  if (hartid == 0) {
    sprint("kernel page table is on \n");
    init_proc_pool();
    fs_init();
    init_boot_args();
  }

  static volatile int s_boot_ready_count = 0;
  sync_barrier(&s_boot_ready_count, NCPU);

  if (!g_boot_args_ready)
    panic("Boot args are not initialized.\n");

  if (g_boot_mode == BOOT_MODE_MULTI_APP) {
    if (hartid == 0) {
      for (int i = 0; i < NCPU; i++) {
        if ((size_t)i >= g_boot_argc || g_boot_argv[i] == 0)
          panic("Not enough apps for multicore startup.\n");
        g_multi_boot_proc[i] = load_user_program(g_boot_argv[i], i);
      }
    }

    static volatile int s_multi_load_count = 0;
    sync_barrier(&s_multi_load_count, NCPU);

    if (g_multi_boot_proc[hartid] == 0)
      panic("Missing startup process for hart %d.\n", hartid);

    sprint("hartid = %d: Switch to user mode...\n", hartid);
    switch_to(g_multi_boot_proc[hartid]);
  } else {
    if (hartid == 0) {
      sprint("Switch to user mode...\n");
      insert_to_ready_queue(load_user_program(g_boot_argv[0], 0));
      schedule();
    } else {
      while (1)
        asm volatile("wfi");
    }
  }

  // we should never reach here.
  return 0;
}
