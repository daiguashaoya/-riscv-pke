/*
 * contains the implementation of all syscalls.
 */

#include <stdint.h>
#include <errno.h>

#include "util/types.h"
#include "syscall.h"
#include "string.h"
#include "process.h"
#include "util/functions.h"
#include "pmm.h"
#include "vmm.h"
#include "sched.h"

#include "spike_interface/spike_utils.h"

extern process procs[NPROC];
//
// implement the SYS_user_print syscall
//
ssize_t sys_user_print(const char* buf, size_t n) {
  // buf is now an address in user space of the given app's user stack,
  // so we have to transfer it into phisical address (kernel is running in direct mapping).
  assert( current );
  char* pa = (char*)user_va_to_pa((pagetable_t)(current->pagetable), (void*)buf);
  sprint(pa);
  return 0;
}

//
// implement the SYS_user_exit syscall
//
extern void insert_to_ready_queue(process *proc); // 确保有此声明

ssize_t sys_user_exit(uint64 code)
{
  sprint("User exit with code:%d.\n", code);

  // 1. 将自身设为 ZOMBIE
  current->status = ZOMBIE;

  // 2. 检查父进程是否在等待自己
  if (current->parent && current->parent->status == BLOCKED)
  {
    int64 wait_pid = current->parent->waiting_pid;

    // 如果父进程在等我 (wait_pid == current->pid)
    // 或者父进程在等任意子进程 (wait_pid == -1)
    if (wait_pid == -1 || wait_pid == current->pid)
    {

      // A. 修改父进程状态为 READY
      current->parent->status = READY;

      // B. 将父进程的 a0 寄存器设置为子进程的 PID (作为 wait 的返回值)
      current->parent->trapframe->regs.a0 = current->pid;

      // [关键修复] C. 将父进程加入就绪队列！
      // 缺少这一步会导致 schedule() 找不到父进程，从而报 "ready queue empty" 错误
      insert_to_ready_queue(current->parent);

      // D. 将自己设为 FREE (资源回收)
      current->status = FREE;
    }
  }

  // 3. 转调度
  schedule();
  return 0;
}

//
// maybe, the simplest implementation of malloc in the world ... added @lab2_2
//
uint64 sys_user_allocate_page() {
  void* pa = alloc_page();
  uint64 va;
  // if there are previously reclaimed pages, use them first (this does not change the
  // size of the heap)
  if (current->user_heap.free_pages_count > 0) {
    va =  current->user_heap.free_pages_address[--current->user_heap.free_pages_count];
    assert(va < current->user_heap.heap_top);
  } else {
    // otherwise, allocate a new page (this increases the size of the heap by one page)
    va = current->user_heap.heap_top;
    current->user_heap.heap_top += PGSIZE;

    current->mapped_info[HEAP_SEGMENT].npages++;
  }
  user_vm_map((pagetable_t)current->pagetable, va, PGSIZE, (uint64)pa,
         prot_to_type(PROT_WRITE | PROT_READ, 1));

  return va;
}

//
// reclaim a page, indicated by "va". added @lab2_2
//
uint64 sys_user_free_page(uint64 va) {
  user_vm_unmap((pagetable_t)current->pagetable, va, PGSIZE, 1);
  // add the reclaimed page to the free page list
  current->user_heap.free_pages_address[current->user_heap.free_pages_count++] = va;
  return 0;
}

//
// kerenl entry point of naive_fork
//
ssize_t sys_user_fork() {
  sprint("User call fork.\n");
  return do_fork( current );
}
//
// kerenl entry point of yield. added @lab3_2
//
ssize_t sys_user_yield()
{
  // TODO (lab3_2): implment the syscall of yield.
  // hint: the functionality of yield is to give up the processor. therefore,
  // we should set the status of currently running process to READY, insert it in
  // the rear of ready queue, and finally, schedule a READY process to run.
  // 1. 将当前进程状态设置为 READY (就绪态)
  current->status = READY;

  // 2. 将当前进程加入就绪队列的队尾
  // insert_to_ready_queue 定义在 kernel/sched.c 中
  insert_to_ready_queue(current);

  // 3. 转进程调度，选择下一个进程运行
  // schedule 定义在 kernel/sched.c 中
  schedule();

  return 0;
}

ssize_t sys_user_wait(ssize_t pid)
{
  // 遍历所有进程，寻找属于当前进程的子进程
  int has_child = 0;

  for (int i = 0; i < NPROC; i++)
  {
    // 筛选条件: 是当前进程的子进程
    if (procs[i].parent == current)
    {

      // 筛选 PID: pid==-1 (任意) 或 pid匹配
      if (pid == -1 || procs[i].pid == pid)
      {
        has_child = 1;

        // 情况 1: 发现僵尸子进程 (已退出)
        if (procs[i].status == ZOMBIE)
        {
          // 回收资源
          procs[i].status = FREE;
          return procs[i].pid;
        }
      }
    }
  }

  // 情况 2: 还有符合条件的子进程在运行，父进程进入阻塞状态
  if (has_child)
  {
    current->status = BLOCKED;  // 设为阻塞
    current->waiting_pid = pid; // 记录在等谁
    schedule();                 // 让出 CPU
    // 注意：当被唤醒时，返回值由唤醒者(子进程exit)直接写入 trapframe->a0
    return 0; // 这里的返回值实际上会被覆盖
  }

  // 情况 3: 没有找到任何符合条件的子进程
  return -1;
}

//
// [a0]: the syscall number; [a1] ... [a7]: arguments to the syscalls.
// returns the code of success, (e.g., 0 means success, fail for otherwise)
//
long do_syscall(long a0, long a1, long a2, long a3, long a4, long a5, long a6, long a7) {
  switch (a0) {
    case SYS_user_print:
      return sys_user_print((const char*)a1, a2);
    case SYS_user_exit:
      return sys_user_exit(a1);
    // added @lab2_2
    case SYS_user_allocate_page:
      return sys_user_allocate_page();
    case SYS_user_free_page:
      return sys_user_free_page(a1);
    case SYS_user_fork:
      return sys_user_fork();
    case SYS_user_yield:
      return sys_user_yield();
    case SYS_user_wait:
      return sys_user_wait(a1);
    default:
      panic("Unknown syscall %ld \n", a0);
  }
}
