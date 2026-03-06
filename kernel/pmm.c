#include "pmm.h"
#include "config.h"
#include "memlayout.h"
#include "riscv.h"
#include "spike_interface/spike_utils.h"
#include "util/functions.h"
#include "util/string.h"

// _end is defined in kernel/kernel.lds, it marks the ending (virtual) address
// of PKE kernel
extern char _end[];
// g_mem_size is defined in spike_interface/spike_memory.c, it indicates the
// size of our (emulated) spike machine. g_mem_size's value is obtained when
// initializing HTIF.
extern uint64 g_mem_size;

static uint64 free_mem_start_addr; // beginning address of free memory
static uint64 free_mem_end_addr;   // end address of free memory (not included)

typedef struct node {
  struct node *next;
} list_node;

// g_free_mem_list is the head of the list of free physical memory pages
static list_node g_free_mem_list;

// 假设物理内存页数不会超过一定上限（这里开辟了512MB/4KB = 131072
// 个字节数组来记录引用数）
static uint8 page_ref[131072];

void inc_page_ref(void *pa) {
  uint64 index = ((uint64)pa - free_mem_start_addr) / PGSIZE;
  page_ref[index]++;
}

int get_page_ref(void *pa) {
  uint64 index = ((uint64)pa - free_mem_start_addr) / PGSIZE;
  return page_ref[index];
}

//
// actually creates the freepage list. each page occupies 4KB (PGSIZE), i.e.,
// small page. PGSIZE is defined in kernel/riscv.h, ROUNDUP is defined in
// util/functions.h.
//
static void create_freepage_list(uint64 start, uint64 end) {
  g_free_mem_list.next = 0;
  for (uint64 p = ROUNDUP(start, PGSIZE); p + PGSIZE < end; p += PGSIZE)
    free_page((void *)p);
}

void *alloc_page(void) {
  list_node *n = g_free_mem_list.next;
  if (n) {
    g_free_mem_list.next = n->next;
    // ===== 新增开始 =====
    page_ref[((uint64)n - free_mem_start_addr) / PGSIZE] = 1;
    // ===== 新增结束 =====
  }
  return (void *)n;
}

void free_page(void *pa) {
  if (((uint64)pa % PGSIZE) != 0 || (uint64)pa < free_mem_start_addr ||
      (uint64)pa >= free_mem_end_addr)
    panic("free_page 0x%lx \n", pa);

  // ===== 新增开始 =====
  uint64 index = ((uint64)pa - free_mem_start_addr) / PGSIZE;
  if (page_ref[index] > 0)
    page_ref[index]--;
  if (page_ref[index] > 0)
    return; // 还有其它进程正在使用(COW共享)，不能回收
  // ===== 新增结束 =====

  // insert a physical page to g_free_mem_list
  list_node *n = (list_node *)pa;
  n->next = g_free_mem_list.next;
  g_free_mem_list.next = n;
}

//
// pmm_init() establishes the list of free physical pages according to available
// physical memory space.
//
void pmm_init() {
  // start of kernel program segment
  uint64 g_kernel_start = KERN_BASE;
  uint64 g_kernel_end = (uint64)&_end;

  uint64 pke_kernel_size = g_kernel_end - g_kernel_start;
  sprint("PKE kernel start 0x%lx, PKE kernel end: 0x%lx, PKE kernel size: "
         "0x%lx .\n",
         g_kernel_start, g_kernel_end, pke_kernel_size);

  // free memory starts from the end of PKE kernel and must be page-aligined
  free_mem_start_addr = ROUNDUP(g_kernel_end, PGSIZE);

  // recompute g_mem_size to limit the physical memory space that our riscv-pke
  // kernel needs to manage
  g_mem_size = MIN(PKE_MAX_ALLOWABLE_RAM, g_mem_size);
  if (g_mem_size < pke_kernel_size)
    panic("Error when recomputing physical memory size (g_mem_size).\n");

  free_mem_end_addr = g_mem_size + DRAM_BASE;
  sprint("free physical memory address: [0x%lx, 0x%lx] \n", free_mem_start_addr,
         free_mem_end_addr - 1);

  sprint("kernel memory manager is initializing ...\n");
  // create the list of free pages
  create_freepage_list(free_mem_start_addr, free_mem_end_addr);
}
