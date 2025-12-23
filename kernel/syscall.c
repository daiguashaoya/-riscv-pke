/*
 * contains the implementation of all syscalls.
 */

#include <errno.h>
#include <stdint.h>

#include "elf.h"
#include "process.h"
#include "string.h"
#include "syscall.h"
#include "util/functions.h"
#include "util/types.h"

#include "spike_interface/spike_utils.h"

//
// implement the SYS_user_print syscall
//
ssize_t sys_user_print(const char *buf, size_t n) {
  sprint(buf);
  return 0;
}

//
// implement the SYS_user_exit syscall
//
ssize_t sys_user_exit(uint64 code) {
  sprint("User exit with code:%d.\n", code);
  // in lab1, PKE considers only one app (one process).
  // therefore, shutdown the system when the app calls exit()
  shutdown(code);
}

//
// [a0]: the syscall number; [a1] ... [a7]: arguments to the syscalls.
// returns the code of success, (e.g., 0 means success, fail for otherwise)
//
ssize_t sys_user_print_backtrace(uint64 depth) {
  // 这个 fp 指向do_user_call函数的栈顶
  uint64 fp = current->trapframe->regs.s0;
  // 这个是print_backtrace函数的栈顶，注意这里是-8
  fp = *(uint64 *)(fp - 8);
  for (int i = 0; i < depth && fp != 0; i++) {
    // 读取栈上的 Return Address (通常是 fp - 8)
    uint64 ra = *(uint64 *)(fp - 8);
    // sprint("0x%lx\n", ra);
    // 4. 在 ELF 符号表中查找 ra 对应的函数名
    char *func_name = find_symbol_name_by_addr(ra);

    sprint("%s\n", func_name);

    // 移动到上一个栈帧 (通常是 *fp)
    fp = *(uint64 *)(fp - 16);
  }
  return 0;
}
long do_syscall(long a0, long a1, long a2, long a3, long a4, long a5, long a6,
                long a7) {
  switch (a0) {
  case SYS_user_print:
    return sys_user_print((const char *)a1, a2);
  case SYS_user_exit:
    return sys_user_exit(a1);
  case SYS_user_print_backtrace:
    return sys_user_print_backtrace(a1);
  default:
    panic("Unknown syscall %ld \n", a0);
  }
}
