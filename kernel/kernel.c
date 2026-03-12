/*
 * Supervisor-mode startup codes
 */

#include "riscv.h"
#include "string.h"
#include "elf.h"
#include "process.h"
#include "pmm.h"
#include "vmm.h"
#include "sched.h"
#include "memlayout.h"
#include "spike_interface/spike_utils.h"
#include "util/types.h"
#include "vfs.h"
#include "rfs.h"
#include "ramdev.h"

//
// trap_sec_start points to the beginning of S-mode trap segment (i.e., the entry point of
// S-mode trap vector). added @lab2_1
//
extern char trap_sec_start[];

// S-mode init barriers (one-shot counters).
static volatile int s_stage1_count = 0;
static volatile int s_stage2_count = 0;
// Serialize boot-time ELF loading across harts to avoid races on global VFS
// caches and process-pool allocation (which are lock-free in this lab code).
static volatile int s_load_turn = 0;
static volatile int s_load_ready_count = 0;

static inline void boot_barrier(volatile int *counter) {
  __sync_fetch_and_add(counter, 1);
  while (*counter < NCPU)
    ;
  __sync_synchronize();
}

static int starts_with(const char *s, const char *prefix) {
  while (*prefix) {
    if (*s == '\0' || *s != *prefix)
      return 0;
    s++;
    prefix++;
  }
  return 1;
}

static const char *path_basename(const char *path) {
  const char *base = path;
  while (*path) {
    if (*path == '/')
      base = path + 1;
    path++;
  }
  return base;
}

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

//
// returns the number (should be 1) of string(s) after PKE kernel in command line.
// and store the string(s) in arg_bug_msg.
//
static size_t parse_args(arg_buf *arg_bug_msg) {
  // HTIFSYS_getmainvars frontend call reads command arguments to (input) *arg_bug_msg
  long r = frontend_syscall(HTIFSYS_getmainvars, (uint64)arg_bug_msg,
      sizeof(*arg_bug_msg), 0, 0, 0, 0, 0);
  kassert(r == 0);

  size_t pk_argc = arg_bug_msg->buf[0];
  uint64 *pk_argv = &arg_bug_msg->buf[1];

  int arg = 1;  // skip the PKE OS kernel string, leave behind only the application name
  for (size_t i = 0; arg + i < pk_argc; i++)
    arg_bug_msg->argv[i] = (char *)(uintptr_t)pk_argv[arg + i];

  //returns the number of strings after PKE kernel in command line
  return pk_argc - arg;
}

//
// load the elf, and construct a "process" (with only a trapframe).
// load_bincode_from_host_elf is defined in elf.c
//
process *load_user_program(int hartid) {
  process* proc;

  proc = alloc_process();
  // Reserve this proc slot immediately so another hart won't allocate the same
  // process entry before we switch to user mode.
  proc->status = READY;
  proc->trapframe->regs.tp = hartid;
  sprint("hartid = %d: User application is loading.\n", hartid);

  arg_buf arg_bug_msg;

  // retrieve command line arguements
  size_t argc = parse_args(&arg_bug_msg);
  if (!argc) panic("You need to specify the application program!\n");

  if ((size_t)hartid >= argc)
    panic("Not enough application programs specified for hart %d!\n", hartid);

  const char *app_arg = arg_bug_msg.argv[hartid];
  char resolved_path[MAX_PATH_LEN];
  const char *load_path = app_arg;

  // compatibility for lab1_challenge3 command style:
  //   spike ... obj/app0 obj/app1
  // challengeX loader opens apps through VFS rooted at hostfs_root, so map
  // obj/<name> to bin/<name>.
  if (starts_with(app_arg, "./obj/") || starts_with(app_arg, "obj/")) {
    const char *base = path_basename(app_arg);
    strcpy(resolved_path, "bin/");
    strcat(resolved_path, base);
    load_path = resolved_path;
  }

  load_bincode_from_host_elf(proc, (char *)load_path);
  return proc;
}

//
// s_start: S-mode entry point of riscv-pke OS kernel.
//
int s_start(void) {
  int hartid = get_hartid();
  sprint("hartid = %d: Enter supervisor mode...\n", hartid);

  if (hartid == 0) {
    // Bare mode first. Paging will be enabled on all harts after page tables
    // are ready.
    write_csr(satp, 0);
    pmm_init();
    kern_vm_init();
  }
  boot_barrier(&s_stage1_count);

  // Every hart must switch its own satp.
  enable_paging();

  if (hartid == 0) {
    sprint("kernel page table is on \n");
    init_proc_pool();
    fs_init();
  }
  boot_barrier(&s_stage2_count);

  if (NCPU == 1) {
    sprint("hartid = %d: Switch to user mode...\n", hartid);
    insert_to_ready_queue(load_user_program(hartid));
    schedule();
  } else {
    // boot-time serialization: hart0 loads first, then hart1, ...
    while (s_load_turn != hartid)
      ;
    process *proc = load_user_program(hartid);
    __sync_synchronize();
    s_load_turn++;

    // Keep both harts at the same startup phase so one hart doesn't run too far
    // ahead and recycle pages before the other hart even starts user code.
    boot_barrier(&s_load_ready_count);
    sprint("hartid = %d: Switch to user mode...\n", hartid);

    proc->status = RUNNING;
    switch_to(proc);
  }

  // we should never reach here.
  return 0;
}
