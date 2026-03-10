/*
 * This app create two child process.
 * Use semaphores to control the order of
 * the main process and two child processes print info.
 */
#include "user_lib.h"
#include "util/types.h"

int main(void) {
  int mutex = sem_new(0); // 初值为0，所有进程都会阻塞

  // 让多个子进程都尝试 P 操作
  for (int i = 1; i <= 3; i++) {
    if (fork() == 0) {
      sem_P(mutex); // 所有子进程都会阻塞在这里
      printu("Child %d woke up\n", i);
      exit(0);
    }
    yield(); // 让子进程先运行，执行 sem_P 并阻塞
  }
  // 父进程：等所有子进程创建完毕后，逐个唤醒
  for (int i = 0; i < 3; i++) {
    sem_V(mutex); // 每次唤醒一个
    yield();
  }
  exit(0);
  return 0;
}
