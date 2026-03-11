/*
 * contains the implementation of all syscalls.
 */

#include <errno.h>
#include <stdint.h>

#include "pmm.h"
#include "proc_file.h"
#include "elf.h"
#include "process.h"
#include "sched.h"
#include "string.h"
#include "syscall.h"
#include "util/functions.h"
#include "util/types.h"
#include "vmm.h"
#include "semaphore.h"

#include "spike_interface/spike_utils.h"

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
  // reclaim the current process, and reschedule. added @lab3_1
  free_process(current);
  schedule();
  return 0;
}

//
// maybe, the simplest implementation of malloc in the world ... added @lab2_2
//
uint64 sys_user_allocate_page() {
  void *pa = alloc_page();
  uint64 va = g_ufree_page;
  g_ufree_page += PGSIZE;
  // if there are previously reclaimed pages, use them first (this does not
  // change the size of the heap)
  if (current->user_heap.free_pages_count > 0) {
    va = current->user_heap
             .free_pages_address[--current->user_heap.free_pages_count];
    assert(va < current->user_heap.heap_top);
  } else {
    // otherwise, allocate a new page (this increases the size of the heap by
    // one page)
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
  current->user_heap.free_pages_address[current->user_heap.free_pages_count++] =
      va;
  return 0;
}

static uint64 align8(uint64 n) { return (n + 7UL) & ~7UL; }

static mem_block *heap_block_from_va(uint64 va) {
  return (mem_block *)user_va_to_pa((pagetable_t)current->pagetable, (void *)va);
}

static uint64 heap_map_one_page(process *p) {
  if (p->mapped_info[HEAP_SEGMENT].npages >= MAX_HEAP_PAGES)
    panic("better_malloc: heap reaches MAX_HEAP_PAGES.\n");

  void *pa = alloc_page();
  if (pa == 0)
    panic("better_malloc: out of physical memory.\n");

  memset(pa, 0, PGSIZE);
  uint64 va = p->user_heap.heap_top;
  user_vm_map((pagetable_t)p->pagetable, va, PGSIZE, (uint64)pa,
              prot_to_type(PROT_WRITE | PROT_READ, 1));
  p->user_heap.heap_top += PGSIZE;
  p->mapped_info[HEAP_SEGMENT].npages++;
  return va;
}

static void try_split_block(uint64 block_va, mem_block *block, uint64 want_size) {
  if (block->size < want_size + sizeof(mem_block) + 8)
    return;

  uint64 split_va = block_va + sizeof(mem_block) + want_size;
  uint64 split_page_end = ROUNDDOWN(split_va, PGSIZE) + PGSIZE;
  if (split_va + sizeof(mem_block) > split_page_end)
    return;
  if (split_va + sizeof(mem_block) > current->user_heap.heap_top)
    return;

  mem_block *split = heap_block_from_va(split_va);
  if (split == 0)
    return;

  split->size = block->size - want_size - sizeof(mem_block);
  split->used = 0;
  split->next = block->next;

  block->size = want_size;
  block->next = split_va;
}

static void merge_with_next_free(uint64 block_va, mem_block *block) {
  while (block->next != 0) {
    uint64 next_va = block->next;
    mem_block *next = heap_block_from_va(next_va);
    if (next == 0 || next->used)
      break;

    uint64 expected_next_va = block_va + sizeof(mem_block) + block->size;
    if (expected_next_va != next_va)
      break;

    block->size += sizeof(mem_block) + next->size;
    block->next = next->next;
  }
}

// variable-size allocator merged from lab2_challenge2 design.
uint64 sys_user_better_malloc(uint64 n) {
  if (n == 0)
    return 0;
  n = align8(n);

  if (current->heap_block_head == 0) {
    uint64 first_block_va = heap_map_one_page(current);
    mem_block *first = heap_block_from_va(first_block_va);
    kassert(first != 0);
    first->size = PGSIZE - sizeof(mem_block);
    first->used = 0;
    first->next = 0;
    current->heap_block_head = first_block_va;
  }

  for (;;) {
    uint64 cur_va = current->heap_block_head;
    uint64 tail_va = 0;

    while (cur_va != 0) {
      mem_block *cur = heap_block_from_va(cur_va);
      if (cur == 0)
        panic("better_malloc: invalid heap block header.\n");
      tail_va = cur_va;

      if (!cur->used && cur->size >= n) {
        try_split_block(cur_va, cur, n);
        cur->used = 1;
        return cur_va + sizeof(mem_block);
      }

      cur_va = cur->next;
    }

    kassert(tail_va != 0);
    mem_block *tail = heap_block_from_va(tail_va);
    kassert(tail != 0);

    // If the tail block is free, grow it in-place by mapping more pages.
    if (!tail->used && tail->next == 0) {
      while (tail->size < n) {
        heap_map_one_page(current);
        tail->size += PGSIZE;
      }
      continue;
    }

    // Otherwise append a brand-new free block at current heap top.
    uint64 need_bytes = n + sizeof(mem_block);
    uint64 pages_needed = (need_bytes + PGSIZE - 1) / PGSIZE;
    uint64 new_block_va = current->user_heap.heap_top;
    for (uint64 i = 0; i < pages_needed; i++)
      heap_map_one_page(current);

    mem_block *new_block = heap_block_from_va(new_block_va);
    kassert(new_block != 0);
    new_block->size = pages_needed * PGSIZE - sizeof(mem_block);
    new_block->used = 0;
    new_block->next = 0;
    tail->next = new_block_va;
  }
}

uint64 sys_user_better_free(uint64 va) {
  if (va == 0)
    return 0;
  if (current->heap_block_head == 0)
    return -1;
  if (va < current->user_heap.heap_bottom + sizeof(mem_block) ||
      va >= current->user_heap.heap_top)
    return -1;

  uint64 target_va = va - sizeof(mem_block);
  uint64 prev_va = 0;
  uint64 cur_va = current->heap_block_head;
  mem_block *cur = 0;
  while (cur_va != 0) {
    cur = heap_block_from_va(cur_va);
    if (cur == 0)
      return -1;
    if (cur_va == target_va)
      break;
    prev_va = cur_va;
    cur_va = cur->next;
  }
  if (cur_va == 0 || cur == 0)
    return -1;

  cur->used = 0;
  merge_with_next_free(cur_va, cur);

  if (prev_va != 0) {
    mem_block *prev = heap_block_from_va(prev_va);
    if (prev != 0 && !prev->used) {
      uint64 expected_cur_va = prev_va + sizeof(mem_block) + prev->size;
      if (expected_cur_va == cur_va) {
        prev->size += sizeof(mem_block) + cur->size;
        prev->next = cur->next;
      }
    }
  }

  return 0;
}

//
// kerenl entry point of naive_fork
//
ssize_t sys_user_fork() {
  sprint("User call fork.\n");
  return do_fork(current);
}

//
// kerenl entry point of yield. added @lab3_2
//
ssize_t sys_user_yield() {
  // TODO (lab3_2): implment the syscall of yield.
  // hint: the functionality of yield is to give up the processor. therefore,
  // we should set the status of currently running process to READY, insert it
  // in the rear of ready queue, and finally, schedule a READY process to run.
  current->status = READY;
  insert_to_ready_queue(current);
  schedule();
  return 0;
}

//
// open file
//
ssize_t sys_user_open(char *pathva, int flags) {
  char *pathpa =
      (char *)user_va_to_pa((pagetable_t)(current->pagetable), pathva);
  return do_open(pathpa, flags);
}

//
// read file
//
ssize_t sys_user_read(int fd, char *bufva, uint64 count) {
  int i = 0;
  while (i < count) { // count can be greater than page size
    uint64 addr = (uint64)bufva + i;
    uint64 pa = lookup_pa((pagetable_t)current->pagetable, addr);
    uint64 off = addr - ROUNDDOWN(addr, PGSIZE);
    uint64 len = count - i < PGSIZE - off ? count - i : PGSIZE - off;
    uint64 r = do_read(fd, (char *)pa + off, len);
    i += r;
    if (r < len)
      return i;
  }
  return count;
}

//
// write file
//
ssize_t sys_user_write(int fd, char *bufva, uint64 count) {
  int i = 0;
  while (i < count) { // count can be greater than page size
    uint64 addr = (uint64)bufva + i;
    uint64 pa = lookup_pa((pagetable_t)current->pagetable, addr);
    uint64 off = addr - ROUNDDOWN(addr, PGSIZE);
    uint64 len = count - i < PGSIZE - off ? count - i : PGSIZE - off;
    uint64 r = do_write(fd, (char *)pa + off, len);
    i += r;
    if (r < len)
      return i;
  }
  return count;
}

//
// lseek file
//
ssize_t sys_user_lseek(int fd, int offset, int whence) {
  return do_lseek(fd, offset, whence);
}

//
// read vinode
//
ssize_t sys_user_stat(int fd, struct istat *istat) {
  struct istat *pistat =
      (struct istat *)user_va_to_pa((pagetable_t)(current->pagetable), istat);
  return do_stat(fd, pistat);
}

//
// read disk inode
//
ssize_t sys_user_disk_stat(int fd, struct istat *istat) {
  struct istat *pistat =
      (struct istat *)user_va_to_pa((pagetable_t)(current->pagetable), istat);
  return do_disk_stat(fd, pistat);
}

//
// close file
//
ssize_t sys_user_close(int fd) { return do_close(fd); }

//
// lib call to opendir
//
ssize_t sys_user_opendir(char *pathva) {
  char *pathpa =
      (char *)user_va_to_pa((pagetable_t)(current->pagetable), pathva);
  return do_opendir(pathpa);
}

//
// lib call to readdir
//
ssize_t sys_user_readdir(int fd, struct dir *vdir) {
  struct dir *pdir =
      (struct dir *)user_va_to_pa((pagetable_t)(current->pagetable), vdir);
  return do_readdir(fd, pdir);
}

//
// lib call to mkdir
//
ssize_t sys_user_mkdir(char *pathva) {
  char *pathpa =
      (char *)user_va_to_pa((pagetable_t)(current->pagetable), pathva);
  return do_mkdir(pathpa);
}

//
// lib call to closedir
//
ssize_t sys_user_closedir(int fd) { return do_closedir(fd); }

//
// lib call to link
//
ssize_t sys_user_link(char *vfn1, char *vfn2) {
  char *pfn1 =
      (char *)user_va_to_pa((pagetable_t)(current->pagetable), (void *)vfn1);
  char *pfn2 =
      (char *)user_va_to_pa((pagetable_t)(current->pagetable), (void *)vfn2);
  return do_link(pfn1, pfn2);
}

//
// lib call to unlink
//
ssize_t sys_user_unlink(char *vfn) {
  char *pfn =
      (char *)user_va_to_pa((pagetable_t)(current->pagetable), (void *)vfn);
  return do_unlink(pfn);
}

//
// kernel entry point of exec. added @lab4_challenge3
//
ssize_t sys_user_exec(char *pathva, char *parava) {
  char *path =
      (char *)user_va_to_pa((pagetable_t)(current->pagetable), (void *)pathva);
  char *para =
      (char *)user_va_to_pa((pagetable_t)(current->pagetable), (void *)parava);
  int ret = do_exec(path, para);
  if (ret < 0)
    return -1;
  // IMPORTANT: handle_syscall in strap.c will write the return value of
  // do_syscall back into tf->regs.a0. In do_exec, we set a0 = argc = 1. To
  // prevent handle_syscall from overwriting it with 0, we must return the value
  // we just set in the trapframe.
  return (ssize_t)current->trapframe->regs.a0;
}

//
// kernel entry point of wait. added @lab4_challenge3
//
ssize_t sys_user_wait(int pid) { return do_wait(pid); }

//
// [a0]: the syscall number; [a1] ... [a7]: arguments to the syscalls.
// returns the code of success, (e.g., 0 means success, fail for otherwise)
//
static int read_user_u64(uint64 uva, uint64 *out) {
  void *pa =
      user_va_to_pa((pagetable_t)(current->pagetable), (void *)uva);
  if (pa == NULL)
    return -1;
  *out = *(uint64 *)pa;
  return 0;
}

ssize_t sys_user_print_backtrace(uint64 depth) {
  // At ecall we are in do_user_call(); start from its caller's frame pointer.
  uint64 fp = 0;
  if (read_user_u64(current->trapframe->regs.sp + 24, &fp) != 0)
    return 0;

  for (uint64 i = 0; i < depth && fp != 0; i++) {
    if (fp & 0x7)
      break;

    uint64 ra = 0;
    if (read_user_u64(fp - 8, &ra) != 0)
      break;

    char *func_name = find_symbol_name_by_addr(ra);
    sprint("%s\n", func_name ? func_name : "unknown");

    uint64 prev_fp = 0;
    if (read_user_u64(fp - 16, &prev_fp) != 0)
      break;

    // stack grows downward, so caller frame pointer should be larger.
    if (prev_fp <= fp)
      break;
    fp = prev_fp;
  }
  return 0;
}

ssize_t sys_user_sem_new(int value) { return do_sem_new(value); }

ssize_t sys_user_sem_P(int sem_id) { return do_sem_P(sem_id); }

ssize_t sys_user_sem_V(int sem_id) { return do_sem_V(sem_id); } 

long do_syscall(long a0, long a1, long a2, long a3, long a4, long a5, long a6,
                long a7) {
  switch (a0) {
  case SYS_user_print:
    return sys_user_print((const char *)a1, a2);
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
  case SYS_user_better_malloc:
    return sys_user_better_malloc(a1);
  case SYS_user_better_free:
    return sys_user_better_free(a1);
  // added @lab4_1
  case SYS_user_open:
    return sys_user_open((char *)a1, a2);
  case SYS_user_read:
    return sys_user_read(a1, (char *)a2, a3);
  case SYS_user_write:
    return sys_user_write(a1, (char *)a2, a3);
  case SYS_user_lseek:
    return sys_user_lseek(a1, a2, a3);
  case SYS_user_stat:
    return sys_user_stat(a1, (struct istat *)a2);
  case SYS_user_disk_stat:
    return sys_user_disk_stat(a1, (struct istat *)a2);
  case SYS_user_close:
    return sys_user_close(a1);
  // added @lab4_2
  case SYS_user_opendir:
    return sys_user_opendir((char *)a1);
  case SYS_user_readdir:
    return sys_user_readdir(a1, (struct dir *)a2);
  case SYS_user_mkdir:
    return sys_user_mkdir((char *)a1);
  case SYS_user_closedir:
    return sys_user_closedir(a1);
  // added @lab4_3
  case SYS_user_link:
    return sys_user_link((char *)a1, (char *)a2);
  case SYS_user_unlink:
    return sys_user_unlink((char *)a1);
  // added @lab4_challenge3
  case SYS_user_exec:
    return sys_user_exec((char *)a1, (char *)a2);
  case SYS_user_wait:
    return sys_user_wait((int)a1);
  case SYS_user_print_backtrace:
    return sys_user_print_backtrace(a1);
  case SYS_user_sem_new:
    return sys_user_sem_new(a1);
  case SYS_user_sem_P:
    return sys_user_sem_P(a1);
  case SYS_user_sem_V:
    return sys_user_sem_V(a1);
  default:
    panic("Unknown syscall %ld \n", a0);
  }
}
