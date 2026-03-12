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

int main(int argc, char *argv[]) {

  printu("\n======== ShellX Start ========\n\n");

  int MAXBUF = 1024;
  char buf[MAXBUF];

  char *command = naive_malloc();
  char *para = naive_malloc();
  char *real_cmd = naive_malloc();   // 实际执行的路径

  char *token;
  char delim[3] = " \t";

  while (1) {

    // prompt
    printu("\033[1;32mpke:/ $\033[0m ");

    int n = 0;
    while (n == 0) {
      n = read_u(0, buf, MAXBUF - 1);
    }
    if (n < 0) continue;

    buf[n] = '\0';
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) {
      buf[n - 1] = '\0';
      n--;
    }
    if (n == 0) continue;

    // 解析 command
    token = strtok(buf, delim);
    if (token == 0) continue;

    strcpy(command, token);

    // exit
    if (strcmp(command, "exit") == 0)
      break;

    // 解析参数
    token = strtok(NULL, delim);
    if (token)
      strcpy(para, token);
    else
      para[0] = '\0';

    /*
     * 自动补全命令路径
     * 例如：
     * ls  -> /bin/app_ls
     */
    strcpy(real_cmd, "/bin/app_");
    strcat(real_cmd, command);

    printu("\n========== Command Start ==========\n\n");

    int pid = fork();

    if (pid == 0) {

      int ret = exec(real_cmd, para);

      if (ret == -1)
        printu("exec %s failed!\n", real_cmd);
    }
    else {
      wait(pid);
      printu("\n========== Command End ==========\n\n");
    }
  }

  printu("\nShellX exit.\n");

  exit(0);
  return 0;
}
