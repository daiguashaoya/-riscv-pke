#include "user/user_lib.h"
#include "util/types.h"

#ifndef STRESS_CHILDREN
#define STRESS_CHILDREN 28
#endif

#ifndef STRESS_ROUNDS
#define STRESS_ROUNDS 64
#endif

#ifndef STRESS_COMPUTE_SPINS
#define STRESS_COMPUTE_SPINS 20000UL
#endif

#ifndef STRESS_IO_BURST
#define STRESS_IO_BURST 4
#endif

// 纯计算任务
static uint64 compute_burst(int slot, int round) {
  volatile uint64 acc = 0x9e3779b97f4a7c15ULL ^ (uint64)(slot + 1);

  for (uint64 i = 0; i < STRESS_COMPUTE_SPINS; i++) {
    acc ^= (uint64)(slot + 3) * 0x45d9f3bUL;
    acc += (uint64)(round + 1) * (i + 1);
    acc = (acc << 7) | (acc >> (64 - 7));
    acc ^= acc >> 11;
  }

  return acc;
}

// 一共4次系统调用printu
static void io_burst(int slot, int round, uint64 acc) {
  printu("[stress] slot=%d round=%d acc=%lx ", slot, round, acc & 0xfffffUL);
  for (int i = 0; i < STRESS_IO_BURST; i++) {
    char marker = 'A' + (char)((slot + round + i) % 26);
    printu("%c", marker);
  }
  printu("\n");
}

// 28*64 = 1792次循环调度
static void child_main(int slot) {
  for (int round = 0; round < STRESS_ROUNDS; round++) {
    uint64 acc = compute_burst(slot, round);
    io_burst(slot, round, acc);
    yield();
  }

  printu("[stress] slot=%d exit\n", slot);
  exit(0);
}

static int find_slot_by_pid(const int *pids, int count, int pid) {
  for (int i = 0; i < count; i++) {
    if (pids[i] == pid)
      return i;
  }

  return -1;
}

int main(void) {
  int child_pids[STRESS_CHILDREN];
  int reaped_flags[STRESS_CHILDREN] = {0};
  int created = 0;
  int reaped = 0;

  printu("\n======== stress test start ========\n");
  printu("[stress] target_children=%d rounds=%d compute_spins=%d\n",
         STRESS_CHILDREN, STRESS_ROUNDS, STRESS_COMPUTE_SPINS);
  printu("[stress] child logs use slot ids; parent logs keep pid mapping.\n");

  for (int slot = 0; slot < STRESS_CHILDREN; slot++) {
    int pid = fork();
    if (pid < 0) {
      printu("[stress] fork failed at slot=%d\n", slot);
      break;
    }

    if (pid == 0)
      child_main(slot);

    child_pids[created] = pid;
    created++;
    printu("[stress] spawned slot=%d pid=%d\n", slot, pid);
  }

  printu("[stress] parent waiting for %d children...\n", created);

  // Polling wait(0) avoids the current branch's unstable return value path
  // when a blocking wait(pid) sleeps and later gets rescheduled.
  while (reaped < created) {
    int waited = wait(0);
    int slot;

    if (waited == 0) {
      yield();
      continue;
    }

    if (waited < 0) {
      printu("[stress] wait failed after reaping=%d/%d\n", reaped, created);
      break;
    }

    slot = find_slot_by_pid(child_pids, created, waited);
    if (slot < 0) {
      printu("[stress] reaped unknown pid=%d\n", waited);
      continue;
    }

    if (reaped_flags[slot]) {
      printu("[stress] duplicate reap slot=%d pid=%d\n", slot, waited);
      continue;
    }

    reaped_flags[slot] = 1;
    printu("[stress] reaped slot=%d pid=%d\n", slot, waited);
    reaped++;
  }

  printu("[stress] complete: created=%d reaped=%d\n", created, reaped);
  printu("======== stress test done ========\n\n");
  exit(0);
  return 0;
}
