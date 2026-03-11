#ifndef _CONFIG_H_
#define _CONFIG_H_

// number of harts (can be overridden by compiler flag: -DNCPU=<n>)
#ifndef NCPU
#define NCPU 1
#endif

//interval of timer interrupt. added @lab1_3
#define TIMER_INTERVAL 1000000

// the maximum memory space that PKE is allowed to manage. added @lab2_1
#define PKE_MAX_ALLOWABLE_RAM 128 * 1024 * 1024

// the ending physical address that PKE observes. added @lab2_1
#define PHYS_TOP (DRAM_BASE + PKE_MAX_ALLOWABLE_RAM)

#define HART_MEM_OFFSET 0x04000000UL // 64MB gap between harts

#define USER_STACK(id) (0x81100000UL + (id) * HART_MEM_OFFSET)
#define USER_KSTACK(id) (0x81200000UL + (id) * HART_MEM_OFFSET)
#define USER_TRAP_FRAME(id) (0x81300000UL + (id) * HART_MEM_OFFSET)

#endif
