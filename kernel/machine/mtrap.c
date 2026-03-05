#include "kernel/process.h"
#include "kernel/riscv.h"
#include "kernel/strap.h"
#include "spike_interface/spike_utils.h"
#include "string.h"

static void handle_instruction_access_fault() {
  panic("Instruction access fault!");
}

static void handle_load_access_fault() { panic("Load access fault!"); }

static void handle_store_access_fault() { panic("Store/AMO access fault!"); }

//
// print the source file name and the code line where exception happened.
// mepc: the address of the instruction that triggered the exception.
//
void print_errorline(uint64 mepc) {
  // check that debug info has been loaded
  if (!current->line)
    return;

  // Find the entry whose address is the largest one that is <= mepc.
  // DWARF line info: each entry covers instructions from its addr up to
  // (but not including) the next entry's addr.
  int best = -1;
  // 查找到出错的行entry
  for (int i = 0; i < current->line_ind; i++) {
    if (current->line[i].addr <= mepc) {
      if (best == -1 || current->line[i].addr > current->line[best].addr)
        best = i;
    }
  }
  // 找到该行的行号、文件、目录
  if (best != -1) {
    int i = best;
    if (1) {
      uint64 file_idx = current->line[i].file;
      uint64 line_no = current->line[i].line;
      char *filename = current->file[file_idx].file;
      uint64 dir_idx = current->file[file_idx].dir;
      char *dir = (dir_idx != (uint64)-1) ? current->dir[dir_idx] : NULL;

      // 拼接成完整路径 打印
      char fullpath[128];
      if (dir && dir[0] != '\0') {
        strcpy(fullpath, dir);
        fullpath[strlen(dir)] = '/';
        strcpy(fullpath + strlen(dir) + 1, filename);
      } else {
        strcpy(fullpath, filename);
      }

      sprint("Runtime error at %s:%d\n", fullpath, (int)line_no);

      // 打开源文件并跳转到目标行
      spike_file_t *f = spike_file_open(fullpath, O_RDONLY, 0);
      if (IS_ERR_VALUE(f))
        return;

      // 跳过(line_no - 1)个换行符到达目标行
      char c;
      uint64 cur_line = 1;
      while (cur_line < line_no) {
        if (spike_file_read(f, &c, 1) <= 0)
          break;
        if (c == '\n')
          cur_line++;
      }

      // 读出这一行的代码内容
      char buf[256];
      int len = 0;
      while (spike_file_read(f, &c, 1) > 0 && c != '\n' && len < 255) {
        buf[len++] = c;
      }
      buf[len] = '\0';

      if (len > 0)
        sprint("%s\n", buf);

      spike_file_close(f);
      return;
    }
  }
}

static void handle_illegal_instruction() {
  // print the source file and line number before panicking
  print_errorline(read_csr(mepc));
  panic("Illegal instruction!");
}

static void handle_misaligned_load() { panic("Misaligned Load!"); }

static void handle_misaligned_store() { panic("Misaligned AMO!"); }

// added @lab1_3
static void handle_timer() {
  int cpuid = 0;
  // setup the timer fired at next time (TIMER_INTERVAL from now)
  *(uint64 *)CLINT_MTIMECMP(cpuid) =
      *(uint64 *)CLINT_MTIMECMP(cpuid) + TIMER_INTERVAL;

  // setup a soft interrupt in sip (S-mode Interrupt Pending) to be handled in
  // S-mode
  write_csr(sip, SIP_SSIP);
}

//
// handle_mtrap calls a handling function according to the type of a machine
// mode interrupt (trap).
//
void handle_mtrap() {
  uint64 mcause = read_csr(mcause);
  switch (mcause) {
  case CAUSE_MTIMER:
    handle_timer();
    break;
  case CAUSE_FETCH_ACCESS:
    handle_instruction_access_fault();
    break;
  case CAUSE_LOAD_ACCESS:
    handle_load_access_fault();
  case CAUSE_STORE_ACCESS:
    handle_store_access_fault();
    break;
  case CAUSE_ILLEGAL_INSTRUCTION:
    handle_illegal_instruction();
    break;
  case CAUSE_MISALIGNED_LOAD:
    handle_misaligned_load();
    break;
  case CAUSE_MISALIGNED_STORE:
    handle_misaligned_store();
    break;

  default:
    sprint("machine trap(): unexpected mscause %p\n", mcause);
    sprint("            mepc=%p mtval=%p\n", read_csr(mepc), read_csr(mtval));
    panic("unexpected exception happened in M-mode.\n");
    break;
  }
}
