#include "user_lib.h"
#include "util/string.h"
#include "util/types.h"

int main(int argc, char *argv[]) {
  char *path = "/RAMDISK0";

  if (argc >= 1 && argv && argv[0] && argv[0][0] != '\0') {
    // 一参 exec("/bin/app_ls") 时，argv[0] 是程序名本身
    if (strcmp(argv[0], "/bin/app_ls") != 0 &&
        strcmp(argv[0], "bin/app_ls") != 0) {
      path = argv[0];  // 两参 exec("/bin/app_ls", "/xxx") 走这里
    }
  }
  int dir_fd = opendir_u(path);
  printu("---------- ls command -----------\n");
  printu("ls \"%s\":\n", path);
  printu("[name]               [inode_num]\n");
  struct dir dir;
  int width = 20;
  while(readdir_u(dir_fd, &dir) == 0) {
    // we do not have %ms :(
    char name[width + 1];
    memset(name, ' ', width + 1);
    name[width] = '\0';
    if (strlen(dir.name) < width) {
      strcpy(name, dir.name);
      name[strlen(dir.name)] = ' ';
      printu("%s %d\n", name, dir.inum);
    }
    else
      printu("%s %d\n", dir.name, dir.inum);
  }
  printu("------------------------------\n");
  closedir_u(dir_fd);

  exit(0);
  return 0;
}
