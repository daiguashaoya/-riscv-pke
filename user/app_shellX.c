/*
 * Interactive shell version (enhanced).
 * Support commands like:
 *    ls /
 *    cat hostfile.txt
 * Instead of requiring /bin/app_ls
 */

#include "user_lib.h"
#include "string.h"
#include "util/types.h"

static void trim_trailing_ws(char *s) {
  int len = strlen(s);
  while (len > 0 &&
         (s[len - 1] == ' ' || s[len - 1] == '\t' || s[len - 1] == '\n' ||
          s[len - 1] == '\r')) {
    s[len - 1] = '\0';
    len--;
  }
}

static int parse_one_command(char *segment, char *command, char *para) {
  char delim[5] = " \t\r\n";
  char *token = strtok(segment, delim);
  if (token == 0)
    return 0;

  strcpy(command, token);
  token = strtok(NULL, delim);
  if (token)
    strcpy(para, token);
  else
    para[0] = '\0';

  // Keep ShellX simple: support at most one argument.
  if (strtok(NULL, delim) != 0)
    return -1;

  return 1;
}

static void reap_background_children(void) {
  int pid = wait(0); // non-blocking poll
  while (pid > 0) {
    printu("[bg] pid %d finished.\n", pid);
    pid = wait(0);
  }
}

static int run_one_segment(char *segment, int background, char *command,
                           char *para, char *real_cmd) {
  // 解析指令和参数
  int parse_ret = parse_one_command(segment, command, para);
  if (parse_ret < 0) {
    printu("shellX: invalid format, use cmd [arg] [&]\n");
    return 0;
  }
  if (parse_ret == 0)
    return 0;

  if (strcmp(command, "exit") == 0)
    return 1;

  /*
   * Automatic command path completion:
   *   ls   -> /bin/app_ls
   * Fallback:
   *   app0 -> /bin/app0
   */
  strcpy(real_cmd, "/bin/app_");
  strcat(real_cmd, command);

  int pid = fork();
  if (pid == 0) {
    int ret = exec(real_cmd, para);
    if (ret == -1 && command[0] != '/') {
      strcpy(real_cmd, "/bin/");
      strcat(real_cmd, command);
      ret = exec(real_cmd, para);
    }
    if (ret == -1)
      printu("exec %s failed!\n", real_cmd);
    exit(-1);
  } else if (pid > 0) {
    if (background) {
      printu("[bg] pid %d started: %s\n", pid, real_cmd);
    } else {
      printu("\n========== Command Start ==========\n\n");
      wait(pid);
      printu("\n========== Command End ==========\n\n");
    }
  } else {
    printu("fork failed for %s\n", real_cmd);
  }

  return 0;
}

static int process_one_line(char *line, char *command, char *para,
                            char *real_cmd) {
  char *cursor = line;

  // Support line format: cmd [arg] [& cmd [arg] ...]
  while (1) {
    while (*cursor == ' ' || *cursor == '\t' || *cursor == '\n' ||
           *cursor == '\r')
      cursor++;
    if (*cursor == '\0')
      break;

    char *amp = strchr(cursor, '&');
    // 如果有 &，则将其替换为字符串结束符，并标记当前命令为后台运行
    int background = (amp != 0);
    if (amp)
      *amp = '\0';

    trim_trailing_ws(cursor);
    if (*cursor != '\0') {
      int should_exit = run_one_segment(cursor, background, command, para, real_cmd);
      if (should_exit)
        return 1;
    }

    if (!amp)
      break;
    cursor = amp + 1;
  }

  return 0;
}

int main(int argc, char *argv[]) {

  printu("\n======== ShellX Start ========\n\n");

  int MAXBUF = 1024;
  char buf[MAXBUF];

  char *command = naive_malloc();
  char *para = naive_malloc();
  char *real_cmd = naive_malloc(); // 实际执行的路径

  while (1) {
    // 处理上一次终端的后台运行结果
    reap_background_children();

    // prompt
    printu("\033[1;32mpke:/ $\033[0m ");

    int n = 0;
    while (n == 0) {
      n = read_u(0, buf, MAXBUF - 1);
    }
    if (n < 0) continue;

    // Reap any children that finished while waiting for input.
    reap_background_children();

    buf[n] = '\0';

    int should_exit = 0;
    char *line = buf;
    while (*line != '\0') {
      // Split input into lines so pasting multiple lines works.
      char *next = line;
      while (*next != '\0' && *next != '\n' && *next != '\r')
        next++;
      if (*next != '\0') {
        *next = '\0';
        next++;
        // Skip consecutive line breaks.
        while (*next == '\n' || *next == '\r')
          next++;
      }

      trim_trailing_ws(line);
      if (*line != '\0') {
        // 处理一行指令（可能包含多个用 & 分隔的子指令）
        should_exit = process_one_line(line, command, para, real_cmd);
        if (should_exit)
          break;
      }

      line = next;
    }

    if (should_exit) {
      // Wait for all remaining children before exit.
      while (wait(-1) > 0) {
      }
      break;
    }
  }

  printu("\nShellX exit.\n");

  exit(0);
  return 0;
}
