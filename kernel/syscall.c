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
// 扩展 n_pages 个连续虚拟页（物理页可不连续），返回第一页的物理地址
static void *heap_expand_pages(int n_pages) {
  if (current->heap_pages_cnt + n_pages > MAX_HEAP_PAGES)
    panic("heap_expand: heap too large\n");
  void *first_pa = NULL;
  for (int i = 0; i < n_pages; i++) {
    void *pa = alloc_page();
    if (!pa)
      panic("heap_expand: out of memory\n");
    memset(pa, 0, PGSIZE);
    uint64 va = current->heap_top;
    current->heap_top += PGSIZE;
    user_vm_map((pagetable_t)current->pagetable, va, PGSIZE, (uint64)pa,
                prot_to_type(PROT_WRITE | PROT_READ, 1));
    int idx = current->heap_pages_cnt++;
    current->heap_pages_pa[idx] = (uint64)pa;
    current->heap_pages_va[idx] = va;
    if (i == 0)
      first_pa = pa;
  }
  return first_pa;
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
  // 将 n 对齐到 8 字节，保证 MCB 的 next 指针始终 8 字节对齐
  n = (n + 7) & ~7;
  mem_block *blk = current->heap_head;
  mem_block *prev = NULL;

  // First-fit：查找第一个足够大的空闲块
  while (blk != NULL) {
    if (blk->free && blk->size >= n) {
      // 计算 split MCB 的物理地址
      uint64 blk_pa = (uint64)blk;
      uint64 blk_page_end = (blk_pa & ~(uint64)(PGSIZE - 1)) + PGSIZE;
      uint64 split_pa = blk_pa + sizeof(mem_block) + n;
      // 仅当 split MCB 仍在同一物理页内时才分裂，避免跨页 PA 指针运算
      if (split_pa + sizeof(mem_block) <= blk_page_end &&
          blk->size >= n + (int)sizeof(mem_block) + 1) {
        mem_block *split = (mem_block *)split_pa;
        split->size = blk->size - n - (int)sizeof(mem_block);
        split->free = 1;
        split->next = blk->next;
        blk->next = split;
        blk->size = n;
      }
      blk->free = 0;
      return heap_pa_to_va(blk_pa) + sizeof(mem_block);
    }
    prev = blk;
    blk = blk->next;
  }

  // 未找到合适块，按需扩展足够的物理页
  int pages_needed = (n + (int)sizeof(mem_block) + PGSIZE - 1) / PGSIZE;
  mem_block *new_blk = (mem_block *)heap_expand_pages(pages_needed);
  new_blk->size = pages_needed * PGSIZE - (int)sizeof(mem_block);
  new_blk->free = 1;
  new_blk->next = NULL;

  if (current->heap_head == NULL) {
    current->heap_head = new_blk;
  } else {
    prev->next = new_blk; // prev 指向链表最后一个节点
  }
  // 再次尝试分配（此时一定找得到）
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
