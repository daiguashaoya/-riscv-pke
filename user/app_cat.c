#include "user_lib.h"
#include "util/string.h"
#include "util/types.h"

int main(int argc, char *argv[]) {
  int MAXBUF = 512;
  char buf[MAXBUF + 1];
  int fd = 0;

  printu("---------- cat command -----------\n");

  if (argc >= 1 && argv && argv[0] && argv[0][0] != '\0') {
    printu("cat: %s\n", argv[0]);
    fd = open(argv[0], O_RDONLY);
    if (fd < 0) {
      printu("cat: open %s failed\n", argv[0]);
      exit(-1);
      return -1;
    }
  } else {
    printu("cat: <stdin>\n");
  }

  while (1) {
    int n = read_u(fd, buf, MAXBUF);
    if (n <= 0)
      break;
    buf[n] = '\0';
    // printu() has a fixed internal buffer (256 bytes), so print in chunks.
    int off = 0;
    while (off < n) {
      int chunk = n - off;
      if (chunk > 200)
        chunk = 200;
      char saved = buf[off + chunk];
      buf[off + chunk] = '\0';
      printu("%s", buf + off);
      buf[off + chunk] = saved;
      off += chunk;
    }
  }

  if (fd != 0)
    close(fd);

  exit(0);
  return 0;
}
