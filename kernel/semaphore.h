#include "process.h"

#define MAX_SEMAPHORES 16 // 信号量池大小（不要太大，避免kernel_size问题）

typedef struct semaphore {
  int value;           // 信号量值
  int used;            // 是否被使用
  process *wait_queue; // 等待队列头指针
} semaphore;

int do_sem_new(int value);
int do_sem_P(int sem_id);
int do_sem_V(int sem_id);
