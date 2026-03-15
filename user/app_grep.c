#include "user_lib.h"
#include "util/string.h"
#include "util/types.h"

static int contains_pattern(const char *line, const char *pattern) {
  if (pattern[0] == '\0')
    return 1;

  for (int i = 0; line[i] != '\0'; i++) {
    int j = 0;
    while (pattern[j] != '\0' && line[i + j] == pattern[j])
      j++;
    if (pattern[j] == '\0')
      return 1;
  }

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

int main(int argc, char *argv[]) {
  const int READ_BUF = 256;
  const int MAX_LINE = 1024;
  char read_buf[READ_BUF];
  char *line = naive_malloc();
  int line_len = 0;
  int line_cap = MAX_LINE - 1;

  if (argc < 1 || argv == 0 || argv[0] == 0 || argv[0][0] == '\0') {
    printu("usage: grep PATTERN\n");
    exit(-1);
    return -1;
  }

  char *pattern = argv[0];

  printu("---------- grep command -----------\n");
  printu("grep \"%s\":\n", pattern);

  while (1) {
    int n = read_u(0, read_buf, READ_BUF);
    if (n <= 0)
      break;

    for (int i = 0; i < n; i++) {
      char c = read_buf[i];

      if (c == '\r')
        continue;

      if (c == '\n') {
        line[line_len] = '\0';
        if (contains_pattern(line, pattern)) {
          print_chunked(line);
          printu("\n");
        }
        line_len = 0;
        continue;
      }

      if (line_len < line_cap)
        line[line_len++] = c;
    }
  }

  if (line_len > 0) {
    line[line_len] = '\0';
    if (contains_pattern(line, pattern)) {
      print_chunked(line);
      printu("\n");
    }
  }

  exit(0);
  return 0;
}
