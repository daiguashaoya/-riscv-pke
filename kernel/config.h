#ifndef _CONFIG_H_
#define _CONFIG_H_

// we use two HART (cpu) in challenge3
#define NCPU 2

// interval of timer interrupt. added @lab1_3
#define TIMER_INTERVAL 1000000

#define DRAM_BASE 0x80000000

/* Per-hart memory layout (64MB offset per hart):
 * hart0: USER_STACK=0x81100000, USER_KSTACK=0x81200000,
 * USER_TRAP_FRAME=0x81300000 hart1: USER_STACK=0x85100000,
 * USER_KSTACK=0x85200000, USER_TRAP_FRAME=0x85300000
 */
#define HART_MEM_OFFSET 0x04000000UL // 64MB gap between harts

#define USER_STACK(id) (0x81100000UL + (id) * HART_MEM_OFFSET)
#define USER_KSTACK(id) (0x81200000UL + (id) * HART_MEM_OFFSET)
#define USER_TRAP_FRAME(id) (0x81300000UL + (id) * HART_MEM_OFFSET)

#endif
