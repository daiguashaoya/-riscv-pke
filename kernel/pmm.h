#ifndef _PMM_H_
#define _PMM_H_

#include "config.h"

// Initialize phisical memeory manager
void pmm_init();
// Allocate a free phisical page
void* alloc_page();
// Free an allocated page
void free_page(void* pa);

// Per-hart: 0 for kernel alloc stage, 1 for user alloc stage.
extern int vm_alloc_stage[NCPU];

void inc_page_ref(void *pa);
int get_page_ref(void *pa);

#endif
