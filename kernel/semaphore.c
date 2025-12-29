#include "semaphore.h"
#include "../spike_interface/spike_utils.h"
#include "sched.h"
#include "util/types.h"

semaphore semaphores[MAX_SEMAPHORES];

int do_sem_new(int value) {
  for (int i = 0; i < MAX_SEMAPHORES; i++) {
    if (!semaphores[i].used) {
      semaphores[i].used = 1;
      semaphores[i].value = value;
      semaphores[i].wait_queue = NULL;
      return i; // 返回信号量ID
    }
  }
  return -1; // 分配失败
}

int do_sem_P(int sem_id) {
  if (sem_id < 0 || sem_id >= MAX_SEMAPHORES || !semaphores[sem_id].used)
    return -1;

  semaphores[sem_id].value--;
  if (semaphores[sem_id].value < 0) {
    // 当前进程需要阻塞，加入等待队列
    current->status = BLOCKED;
    // 将current加入semaphores[sem_id].wait_queue队尾
    insert_to_wait_queue(&semaphores[sem_id].wait_queue, current);
    // 触发调度
    schedule();
  }
  return 0;
}

int do_sem_V(int sem_id) {
  if (sem_id < 0 || sem_id >= MAX_SEMAPHORES || !semaphores[sem_id].used)
    return -1;
  process *wait_process;
  sprint("semaphore %d value: %d\n wait queue: ", sem_id,
         semaphores[sem_id].value);
  for (wait_process = semaphores[sem_id].wait_queue; wait_process != NULL;
       wait_process = wait_process->queue_next) {
    sprint("%d  ", wait_process->pid);
  }
  sprint("\n");
  semaphores[sem_id].value++;
  if (semaphores[sem_id].value <= 0) {
    // 唤醒等待队列中的一个进程
    process *wakeup_proc = pop_from_wait_queue(&semaphores[sem_id].wait_queue);
    if (wakeup_proc) {
      insert_to_ready_queue(wakeup_proc);
    }
  }
  return 0;
}