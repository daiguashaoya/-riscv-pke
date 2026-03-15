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

#define SHELL_LINE_LEN 1024
#define SHELL_TEXT_LEN 256
#define SHELL_ENV_KEY_LEN 64
#define SHELL_ENV_VAL_LEN 256
#define MAX_HISTORY 20
#define MAX_ENV 32

#define PARSE_OK 1
#define PARSE_EMPTY 0
#define PARSE_INVALID -1
#define PARSE_TOO_LONG -2

#define RESOLVE_OK 0
#define RESOLVE_NOT_FOUND -1
#define RESOLVE_TOO_LONG -2

static char history[MAX_HISTORY][SHELL_LINE_LEN];
static int history_count = 0;
static int history_head = 0;

static char env_keys[MAX_ENV][SHELL_ENV_KEY_LEN];
static char env_vals[MAX_ENV][SHELL_ENV_VAL_LEN];
static int env_count = 0;

static int is_ws(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static int is_digit_char(char c) { return c >= '0' && c <= '9'; }

static int is_var_start_char(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

static int is_var_char(char c) {
  return is_var_start_char(c) || is_digit_char(c);
}

static int safe_copy(char *dst, int cap, const char *src) {
  int i = 0;

  if (cap <= 0)
    return -1;

  while (src[i] != '\0') {
    if (i >= cap - 1) {
      dst[0] = '\0';
      return -1;
    }
    dst[i] = src[i];
    i++;
  }

  dst[i] = '\0';
  return 0;
}

static int safe_copy_n(char *dst, int cap, const char *src, int n) {
  int i;

  if (cap <= 0 || n < 0 || n >= cap) {
    if (cap > 0)
      dst[0] = '\0';
    return -1;
  }

  for (i = 0; i < n; i++)
    dst[i] = src[i];
  dst[n] = '\0';
  return 0;
}

static int safe_append_n(char *dst, int cap, const char *src, int n) {
  int len = strlen(dst);
  int i;

  if (len + n >= cap)
    return -1;

  for (i = 0; i < n; i++)
    dst[len + i] = src[i];
  dst[len + n] = '\0';
  return 0;
}

static int safe_append(char *dst, int cap, const char *src) {
  return safe_append_n(dst, cap, src, strlen(src));
}

static int safe_append_char(char *dst, int cap, char c) {
  int len = strlen(dst);

  if (len + 1 >= cap)
    return -1;

  dst[len] = c;
  dst[len + 1] = '\0';
  return 0;
}

static void print_chunked(const char *text) {
  char chunk[201];
  int len = strlen(text);
  int off = 0;

  while (off < len) {
    int n = len - off;
    if (n > 200)
      n = 200;
    memcpy(chunk, text + off, n);
    chunk[n] = '\0';
    printu("%s", chunk);
    off += n;
  }
}

static void print_key_value(const char *key, const char *value) {
  printu("%s=", key);
  print_chunked(value);
  printu("\n");
}

static void trim_trailing_ws(char *s) {
  int len = strlen(s);
  while (len > 0 && is_ws(s[len - 1])) {
    s[len - 1] = '\0';
    len--;
  }
}

static char *trim_ws(char *s) {
  while (is_ws(*s))
    s++;
  trim_trailing_ws(s);
  return s;
}

static int env_find(const char *key) {
  int i;
  for (i = 0; i < env_count; i++) {
    if (strcmp(env_keys[i], key) == 0)
      return i;
  }
  return -1;
}

static const char *env_get(const char *key) {
  int idx = env_find(key);
  if (idx < 0)
    return "";
  return env_vals[idx];
}

static int is_valid_var_name(const char *name) {
  int i;

  if (name[0] == '\0' || !is_var_start_char(name[0]))
    return 0;

  for (i = 1; name[i] != '\0'; i++) {
    if (!is_var_char(name[i]))
      return 0;
  }
  return 1;
}

static int env_set(const char *key, const char *value) {
  int idx;

  if (!is_valid_var_name(key))
    return -2;
  if (strlen(key) >= SHELL_ENV_KEY_LEN || strlen(value) >= SHELL_ENV_VAL_LEN)
    return -2;

  idx = env_find(key);
  if (idx < 0) {
    if (env_count >= MAX_ENV)
      return -1;
    idx = env_count;
  }

  if (safe_copy(env_keys[idx], SHELL_ENV_KEY_LEN, key) < 0 ||
      safe_copy(env_vals[idx], SHELL_ENV_VAL_LEN, value) < 0)
    return -2;

  if (idx == env_count)
    env_count++;

  return 0;
}

static void init_shell_env(void) {
  env_count = 0;
  env_set("PATH", "/bin");
}

static void print_env(void) {
  int i;
  for (i = 0; i < env_count; i++)
    print_key_value(env_keys[i], env_vals[i]);
}

static int history_slot_for(int visible_index) {
  int oldest;

  if (visible_index < 1 || visible_index > history_count)
    return -1;

  oldest = (history_head - history_count + MAX_HISTORY) % MAX_HISTORY;
  return (oldest + visible_index - 1) % MAX_HISTORY;
}

static void history_push(const char *line) {
  safe_copy(history[history_head], SHELL_LINE_LEN, line);
  history_head = (history_head + 1) % MAX_HISTORY;
  if (history_count < MAX_HISTORY)
    history_count++;
}

static int history_lookup(int visible_index, char *dst, int cap) {
  int slot = history_slot_for(visible_index);
  if (slot < 0)
    return -1;
  return safe_copy(dst, cap, history[slot]);
}

static void print_history(void) {
  int i;
  int oldest = (history_head - history_count + MAX_HISTORY) % MAX_HISTORY;

  for (i = 0; i < history_count; i++) {
    int slot = (oldest + i) % MAX_HISTORY;
    printu("%d  ", i + 1);
    print_chunked(history[slot]);
    printu("\n");
  }
}

static int resolve_history_reference(const char *line, char *dst, int cap) {
  int i = 1;
  int value = 0;

  if (line[0] != '!')
    return safe_copy(dst, cap, line);

  if (!is_digit_char(line[i])) {
    printu("shellX: invalid history reference, use !n\n");
    return -1;
  }

  while (is_digit_char(line[i])) {
    value = value * 10 + (line[i] - '0');
    i++;
  }

  while (is_ws(line[i]))
    i++;

  if (line[i] != '\0') {
    printu("shellX: invalid history reference, use !n\n");
    return -1;
  }

  if (history_lookup(value, dst, cap) < 0) {
    printu("shellX: history entry not found: !%d\n", value);
    return -1;
  }

  return 0;
}

static int expand_variables(const char *src, char *dst, int cap) {
  int i = 0;

  dst[0] = '\0';
  while (src[i] != '\0') {
    if (src[i] == '$' && is_var_start_char(src[i + 1])) {
      char name[SHELL_ENV_KEY_LEN];
      int j = i + 1;
      int name_len = 0;

      while (is_var_char(src[j])) {
        if (name_len >= SHELL_ENV_KEY_LEN - 1)
          return -1;
        name[name_len++] = src[j++];
      }
      name[name_len] = '\0';

      if (safe_append(dst, cap, env_get(name)) < 0)
        return -1;
      i = j;
      continue;
    }

    if (safe_append_char(dst, cap, src[i]) < 0)
      return -1;
    i++;
  }

  return 0;
}

static int parse_one_command(char *segment, char *command, int command_cap,
                             char *para, int para_cap) {
  char *cursor = segment;
  char *start;
  int len;

  while (is_ws(*cursor))
    cursor++;
  if (*cursor == '\0')
    return PARSE_EMPTY;

  start = cursor;
  while (*cursor != '\0' && !is_ws(*cursor))
    cursor++;
  len = cursor - start;
  if (safe_copy_n(command, command_cap, start, len) < 0)
    return PARSE_TOO_LONG;

  while (is_ws(*cursor))
    cursor++;
  if (*cursor == '\0') {
    para[0] = '\0';
    return PARSE_OK;
  }

  start = cursor;
  while (*cursor != '\0' && !is_ws(*cursor))
    cursor++;
  len = cursor - start;
  if (safe_copy_n(para, para_cap, start, len) < 0)
    return PARSE_TOO_LONG;

  while (is_ws(*cursor))
    cursor++;
  if (*cursor != '\0')
    return PARSE_INVALID;

  return PARSE_OK;
}

static int is_builtin_command(const char *command) {
  return strcmp(command, "exit") == 0 || strcmp(command, "history") == 0 ||
         strcmp(command, "env") == 0 || strcmp(command, "set") == 0 ||
         strcmp(command, "export") == 0;
}

static int run_builtin_command(const char *command, const char *para) {
  if (strcmp(command, "exit") == 0)
    return 1;

  if (strcmp(command, "history") == 0) {
    print_history();
    return 0;
  }

  if (strcmp(command, "env") == 0 || strcmp(command, "set") == 0) {
    print_env();
    return 0;
  }

  if (strcmp(command, "export") == 0) {
    char key[SHELL_ENV_KEY_LEN];
    char *eq;
    int ret;

    if (para[0] == '\0') {
      printu("shellX: usage: export NAME=value\n");
      return 0;
    }

    eq = strchr(para, '=');
    if (eq == 0 || eq == para) {
      printu("shellX: usage: export NAME=value\n");
      return 0;
    }

    if (safe_copy_n(key, SHELL_ENV_KEY_LEN, para, eq - para) < 0 ||
        !is_valid_var_name(key)) {
      printu("shellX: invalid variable name\n");
      return 0;
    }

    ret = env_set(key, eq + 1);
    if (ret == -1)
      printu("shellX: environment table is full\n");
    else if (ret == -2)
      printu("shellX: variable name or value is too long\n");
    return 0;
  }

  return 0;
}

static void reap_background_children(void) {
  int pid = wait(0);
  while (pid > 0) {
    printu("[bg] pid %d finished.\n", pid);
    pid = wait(0);
  }
}

static void reap_foreground_children(int pid1, int pid2) {
  int pid = wait(0);
  while (pid > 0) {
    if (pid != pid1 && pid != pid2)
      printu("[bg] pid %d finished.\n", pid);
    pid = wait(0);
  }
}

static int path_exists(const char *path) {
  int fd = open(path, O_RDONLY);
  if (fd < 0)
    return 0;
  close(fd);
  return 1;
}

static int build_candidate_path(char *dst, int cap, const char *dir,
                                const char *prefix, const char *command) {
  int len;

  if (safe_copy(dst, cap, dir) < 0)
    return -1;

  len = strlen(dst);
  if (len == 0)
    return -1;

  if (dst[len - 1] != '/' && safe_append_char(dst, cap, '/') < 0)
    return -1;
  if (safe_append(dst, cap, prefix) < 0)
    return -1;
  if (safe_append(dst, cap, command) < 0)
    return -1;
  return 0;
}

static int has_slash(const char *text) { return strchr(text, '/') != 0; }

static int try_path_candidate(const char *dir, const char *prefix,
                              const char *command, char *real_cmd, int cap,
                              int *saw_too_long) {
  char candidate[SHELL_ENV_VAL_LEN];

  if (build_candidate_path(candidate, sizeof(candidate), dir, prefix, command) <
      0) {
    *saw_too_long = 1;
    return 0;
  }

  if (strlen(candidate) >= MAX_PATH_LEN) {
    *saw_too_long = 1;
    return 0;
  }

  if (!path_exists(candidate))
    return 0;

  safe_copy(real_cmd, cap, candidate);
  return 1;
}

static int resolve_command_path(const char *command, char *real_cmd, int cap) {
  const char *path_env = env_get("PATH");
  int start = 0;
  int end = 0;
  int saw_too_long = 0;

  if (command[0] == '\0')
    return RESOLVE_NOT_FOUND;

  if (has_slash(command))
    return safe_copy(real_cmd, cap, command) < 0 ? RESOLVE_TOO_LONG
                                                  : RESOLVE_OK;

  while (1) {
    char dir[SHELL_ENV_VAL_LEN];

    while (path_env[end] != '\0' && path_env[end] != ':')
      end++;

    if (end > start) {
      if (safe_copy_n(dir, sizeof(dir), path_env + start, end - start) < 0) {
        saw_too_long = 1;
      } else {
        if (try_path_candidate(dir, "app_", command, real_cmd, cap,
                               &saw_too_long))
          return RESOLVE_OK;
        if (try_path_candidate(dir, "", command, real_cmd, cap, &saw_too_long))
          return RESOLVE_OK;
      }
    }

    if (path_env[end] == '\0')
      break;
    start = end + 1;
    end = start;
  }

  return saw_too_long ? RESOLVE_TOO_LONG : RESOLVE_NOT_FOUND;
}

static int validate_exec_input(const char *para) {
  if (strlen(para) >= MAX_PATH_LEN) {
    printu("shellX: argument exceeds exec limit (%d)\n", MAX_PATH_LEN - 1);
    return -1;
  }
  return 0;
}

static int prepare_external_command(const char *command, const char *para,
                                    char *real_cmd, int cap) {
  int ret;

  ret = resolve_command_path(command, real_cmd, cap);
  if (ret == RESOLVE_TOO_LONG) {
    printu("shellX: command path exceeds exec limit (%d)\n",
           MAX_PATH_LEN - 1);
    return -1;
  }
  if (ret == RESOLVE_NOT_FOUND) {
    printu("shellX: command not found or not executable: %s\n", command);
    return -1;
  }

  return validate_exec_input(para);
}

static void exec_or_exit(const char *command, const char *para,
                         const char *real_cmd) {
  int ret = exec(real_cmd, para);
  if (ret == -1)
    printu("shellX: command not found or not executable: %s\n", command);
  exit(-1);
}

static int run_pipeline_segment(char *segment, int background) {
  char *pipe_pos = strchr(segment, '|');
  char left_cmd[SHELL_TEXT_LEN], left_para[SHELL_TEXT_LEN];
  char right_cmd[SHELL_TEXT_LEN], right_para[SHELL_TEXT_LEN];
  char left_real_cmd[MAX_PATH_LEN], right_real_cmd[MAX_PATH_LEN];
  char *left;
  char *right;
  int parse_left;
  int parse_right;
  int fd[2];
  int pid1;
  int pid2;

  if (pipe_pos == 0)
    return 0;
  if (strchr(pipe_pos + 1, '|') != 0) {
    printu("shellX: only one pipe is supported now\n");
    return 0;
  }

  *pipe_pos = '\0';
  left = trim_ws(segment);
  right = trim_ws(pipe_pos + 1);

  if (*left == '\0' || *right == '\0') {
    printu("shellX: invalid pipe format, use cmd1 | cmd2\n");
    return 0;
  }

  parse_left =
      parse_one_command(left, left_cmd, sizeof(left_cmd), left_para,
                        sizeof(left_para));
  parse_right =
      parse_one_command(right, right_cmd, sizeof(right_cmd), right_para,
                        sizeof(right_para));
  if (parse_left == PARSE_TOO_LONG || parse_right == PARSE_TOO_LONG) {
    printu("shellX: command or argument is too long\n");
    return 0;
  }
  if (parse_left != PARSE_OK || parse_right != PARSE_OK) {
    printu("shellX: invalid pipe format, use cmd [arg] | cmd [arg]\n");
    return 0;
  }

  if (is_builtin_command(left_cmd) || is_builtin_command(right_cmd)) {
    printu("shellX: builtins are not supported in pipelines\n");
    return 0;
  }

  if (prepare_external_command(left_cmd, left_para, left_real_cmd,
                               sizeof(left_real_cmd)) < 0)
    return 0;
  if (prepare_external_command(right_cmd, right_para, right_real_cmd,
                               sizeof(right_real_cmd)) < 0)
    return 0;

  if (pipe(fd) < 0) {
    printu("shellX: pipe creation failed\n");
    return 0;
  }

  pid1 = fork();
  if (pid1 == 0) {
    dup2(fd[1], 1);
    close(fd[0]);
    close(fd[1]);
    exec_or_exit(left_cmd, left_para, left_real_cmd);
  }
  if (pid1 < 0) {
    close(fd[0]);
    close(fd[1]);
    printu("shellX: fork failed for left command\n");
    return 0;
  }

  pid2 = fork();
  if (pid2 == 0) {
    dup2(fd[0], 0);
    close(fd[1]);
    close(fd[0]);
    exec_or_exit(right_cmd, right_para, right_real_cmd);
  }

  close(fd[0]);
  close(fd[1]);

  if (pid2 < 0) {
    printu("shellX: fork failed for right command\n");
    wait(pid1);
    return 0;
  }

  if (background) {
    printu("[bg] pipeline started: %d | %d\n", pid1, pid2);
  } else {
    printu("\n========== Command Start ==========\n\n");
    wait(pid1);
    wait(pid2);
    reap_foreground_children(pid1, pid2);
    printu("\n========== Command End ==========\n\n");
  }

  return 0;
}

static int run_one_segment(char *segment, int background) {
  char command[SHELL_TEXT_LEN], para[SHELL_TEXT_LEN];
  char real_cmd[MAX_PATH_LEN];
  int parse_ret;
  int pid;

  if (strchr(segment, '|') != 0)
    return run_pipeline_segment(segment, background);

  parse_ret =
      parse_one_command(segment, command, sizeof(command), para, sizeof(para));
  if (parse_ret == PARSE_TOO_LONG) {
    printu("shellX: command or argument is too long\n");
    return 0;
  }
  if (parse_ret == PARSE_INVALID) {
    printu("shellX: invalid format, use cmd [arg] [&]\n");
    return 0;
  }
  if (parse_ret == PARSE_EMPTY)
    return 0;

  if (is_builtin_command(command))
    return run_builtin_command(command, para);

  if (prepare_external_command(command, para, real_cmd, sizeof(real_cmd)) < 0)
    return 0;

  pid = fork();
  if (pid == 0) {
    exec_or_exit(command, para, real_cmd);
  } else if (pid > 0) {
    if (background) {
      printu("[bg] pid %d started: %s\n", pid, real_cmd);
    } else {
      printu("\n========== Command Start ==========\n\n");
      wait(pid);
      reap_foreground_children(pid, -1);
      printu("\n========== Command End ==========\n\n");
    }
  } else {
    printu("fork failed for %s\n", real_cmd);
  }

  return 0;
}

static int process_one_line(char *line) {
  char *cursor = line;

  while (1) {
    char *amp;
    int background;

    cursor = trim_ws(cursor);
    if (*cursor == '\0')
      break;

    amp = strchr(cursor, '&');
    background = (amp != 0);
    if (amp)
      *amp = '\0';

    trim_trailing_ws(cursor);
    if (*cursor != '\0') {
      int should_exit = run_one_segment(cursor, background);
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
  char *buf = naive_malloc();
  char *history_line = naive_malloc();
  char *expanded_line = naive_malloc();

  printu("\n======== ShellX Start ========\n\n");
  init_shell_env();

  while (1) {
    int n = 0;
    int should_exit = 0;
    char *line;

    reap_background_children();

    printu("\033[1;32mpke:/ $\033[0m ");

    while (n == 0)
      n = read_u(0, buf, SHELL_LINE_LEN - 1);
    if (n < 0)
      continue;

    reap_background_children();
    buf[n] = '\0';

    line = buf;
    while (*line != '\0') {
      char *next = line;

      while (*next != '\0' && *next != '\n' && *next != '\r')
        next++;
      if (*next != '\0') {
        *next = '\0';
        next++;
        while (*next == '\n' || *next == '\r')
          next++;
      }

      line = trim_ws(line);
      if (*line != '\0') {
        char *expanded;

        if (resolve_history_reference(line, history_line, SHELL_LINE_LEN) < 0) {
          line = next;
          continue;
        }

        if (expand_variables(history_line, expanded_line, SHELL_LINE_LEN) < 0) {
          printu("shellX: expanded line is too long\n");
          line = next;
          continue;
        }

        expanded = trim_ws(expanded_line);
        if (*expanded != '\0') {
          history_push(expanded);
          should_exit = process_one_line(expanded);
          if (should_exit)
            break;
        }
      }

      line = next;
    }

    if (should_exit) {
      while (wait(-1) > 0) {
      }
      break;
    }
  }

  printu("\nShellX exit.\n");
  exit(0);
  return 0;
}
