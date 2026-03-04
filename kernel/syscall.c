/*
 * contains the implementation of all syscalls.
 */

#include <errno.h>
#include <stdint.h>

#include "memlayout.h"
#include "pmm.h"
#include "process.h"
#include "spike_interface/spike_utils.h"
#include "string.h"
#include "syscall.h"
#include "util/functions.h"
#include "util/types.h"
#include "vmm.h"

// 将 pa 转换为 va
static uint64 heap_pa_to_va(uint64 pa) {
  for (int i = 0; i < current->heap_pages_cnt; i++) {
    uint64 base_pa = current->heap_pages_pa[i];
    if (pa >= base_pa && pa < base_pa + PGSIZE) {
      return current->heap_pages_va[i] + (pa - base_pa);
    }
  }
  panic("heap_pa_to_va: pa not found\n");
  return 0;
}
// 当前页空间不足时，需要扩展
static void *heap_expand() {
  if (current->heap_pages_cnt >= MAX_HEAP_PAGES)
    panic("heap_expand: heap too large\n");
  void *pa = alloc_page();
  if (!pa)
    panic("heap_expand: out of memory\n");
  memset(pa, 0, PGSIZE);
  uint64 va = current->heap_top;
  current->heap_top += PGSIZE;
  user_vm_map((pagetable_t)current->pagetable, va, PGSIZE, (uint64)pa,
              prot_to_type(PROT_WRITE | PROT_READ, 1));
  // 记录 pa <-> va 的对应关系
  int idx = current->heap_pages_cnt++;
  current->heap_pages_pa[idx] = (uint64)pa;
  current->heap_pages_va[idx] = va;
  return pa;
}

//
// implement the SYS_user_print syscall
//
ssize_t sys_user_print(const char *buf, size_t n) {
  // buf is now an address in user space of the given app's user stack,
  // so we have to transfer it into phisical address (kernel is running in
  // direct mapping).
  assert(current);
  char *pa =
      (char *)user_va_to_pa((pagetable_t)(current->pagetable), (void *)buf);
  sprint(pa);
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

uint64 sys_user_allocate_page(int n) {
  // 将 n 对齐到 8 字节，保证后续 MCB 的 next 指针始终 8 字节对齐
  n = (n + 7) & ~7;
  mem_block *blk = current->heap_head;
  mem_block *prev = NULL;
  // First-fit：查找第一个足够大的空闲块
  while (blk != NULL) {
    if (blk->free && blk->size >= n) {
      // 分裂：若剩余空间还能再放一个 MCB + 至少 1 字节
      if (blk->size >= n + (int)sizeof(mem_block) + 1) {
        mem_block *split = (mem_block *)((char *)blk + sizeof(mem_block) + n);
        split->size = blk->size - n - (int)sizeof(mem_block);
        split->free = 1;
        split->next = blk->next;
        blk->next = split;
        blk->size = n;
      }
      blk->free = 0;
      uint64 blk_va = heap_pa_to_va((uint64)blk);
      return blk_va + sizeof(mem_block);
    }
    prev = blk;
    blk = blk->next;
  }
  // 未找到，扩展一页
  mem_block *new_blk = (mem_block *)heap_expand();
  new_blk->size = PGSIZE - (int)sizeof(mem_block);
  new_blk->free = 1;
  new_blk->next = NULL;
  if (current->heap_head == NULL) {
    current->heap_head = new_blk;
  } else {
    // 把 new_blk 接到链表末尾
    // prev 此时指向链表最后一个节点
    prev->next = new_blk;
  }
  // 再次尝试分配
  return sys_user_allocate_page(n);
}
uint64 sys_user_free_page(uint64 va) {
  uint64 mcb_va = va - sizeof(mem_block);
  mem_block *blk = (mem_block *)user_va_to_pa((pagetable_t)current->pagetable,
                                              (void *)mcb_va);
  if (!blk)
    panic("better_free: invalid pointer 0x%lx\n", va);
  blk->free = 1;
  // 合并相邻空闲块（减少碎片）
  mem_block *cur = current->heap_head;
  while (cur != NULL && cur->next != NULL) {
    if (cur->free && cur->next->free) {
      cur->size += (int)sizeof(mem_block) + cur->next->size;
      cur->next = cur->next->next;
    } else {
      cur = cur->next;
    }
  }
  return 0;
}

//
// [a0]: the syscall number; [a1] ... [a7]: arguments to the syscalls.
// returns the code of success, (e.g., 0 means success, fail for otherwise)
//
long do_syscall(long a0, long a1, long a2, long a3, long a4, long a5, long a6,
                long a7) {
  switch (a0) {
  case SYS_user_print:
    return sys_user_print((const char *)a1, a2);
  case SYS_user_exit:
    return sys_user_exit(a1);
  // added @lab2_2
  case SYS_user_allocate_page:
    return sys_user_allocate_page((int)a1);
  case SYS_user_free_page:
    return sys_user_free_page(a1);
  default:
    panic("Unknown syscall %ld \n", a0);
  }
}
