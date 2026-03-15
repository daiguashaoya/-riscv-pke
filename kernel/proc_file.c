/*
 * Interface functions between file system and kernel/processes. added @lab4_1
 */

#include "proc_file.h"

#include "hostfs.h"
#include "pmm.h"
#include "process.h"
#include "sched.h"
#include "ramdev.h"
#include "rfs.h"
#include "riscv.h"
#include "spike_interface/spike_file.h"
#include "spike_interface/spike_utils.h"
#include "util/functions.h"
#include "util/string.h"

#define PIPE_SIZE 4096

typedef struct pipe_t {
  // data ring buffer (allocated separately for simplicity).
  char *buf;
  uint32 rpos;
  uint32 wpos;
  uint32 len;  // bytes currently in buffer

  // number of opened endpoints
  int readers;
  int writers;

  // wait queues for blocking read/write
  process *read_wait_queue;
  process *write_wait_queue;
} pipe_t;

static void pipe_wake_readers(pipe_t *p) {
  if (!p)
    return;
  process *proc = pop_from_wait_queue(&p->read_wait_queue);
  if (proc)
    insert_to_ready_queue(proc);
}

static void pipe_wake_writers(pipe_t *p) {
  if (!p)
    return;
  process *proc = pop_from_wait_queue(&p->write_wait_queue);
  if (proc)
    insert_to_ready_queue(proc);
}

static void pipe_wake_all_readers(pipe_t *p) {
  if (!p)
    return;
  process *proc = pop_from_wait_queue(&p->read_wait_queue);
  while (proc) {
    insert_to_ready_queue(proc);
    proc = pop_from_wait_queue(&p->read_wait_queue);
  }
}

static void pipe_wake_all_writers(pipe_t *p) {
  if (!p)
    return;
  process *proc = pop_from_wait_queue(&p->write_wait_queue);
  while (proc) {
    insert_to_ready_queue(proc);
    proc = pop_from_wait_queue(&p->write_wait_queue);
  }
}

// 为管道分配内存并初始化
static pipe_t *alloc_pipe(void) {
  pipe_t *p = (pipe_t *)alloc_page();
  if (!p)
    return NULL;
  memset(p, 0, sizeof(*p));

  p->buf = (char *)alloc_page();
  if (!p->buf) {
    free_page(p);
    return NULL;
  }

  p->rpos = 0;
  p->wpos = 0;
  p->len = 0;
  p->readers = 1;
  p->writers = 1;
  p->read_wait_queue = NULL;
  p->write_wait_queue = NULL;
  return p;
}

static int pipe_read(pipe_t *p, char *dst, uint64 count) {
  if (!p || !dst)
    return -1;

  while (p->len == 0) {
    // No buffered data. If there are no writers, report EOF.
    if (p->writers == 0)
      return 0;

    // Otherwise block until data becomes available or writers close.
    current->status = BLOCKED;
    insert_to_wait_queue(&p->read_wait_queue, current);
    schedule();
  }

  uint32 n = (uint32)count;
  if (n > p->len)
    n = p->len;

  uint32 first = n;
  uint32 till_end = PIPE_SIZE - p->rpos;
  if (first > till_end)
    first = till_end;
  memcpy(dst, p->buf + p->rpos, first);

  if (n > first)
    memcpy(dst + first, p->buf, n - first);

  p->rpos = (p->rpos + n) % PIPE_SIZE;
  p->len -= n;

  // If writers were blocked due to full buffer, reading makes room.
  pipe_wake_writers(p);
  return (int)n;
}

static int pipe_write(pipe_t *p, const char *src, uint64 count) {
  if (!p || !src)
    return -1;

  uint64 written = 0;
  while (written < count) {
    // If no readers, pretend the write succeeds (avoid deadlock loops).
    if (p->readers == 0)
      return (int)count;

    while (p->len == PIPE_SIZE) {
      if (p->readers == 0)
        return (int)count;

      current->status = BLOCKED;
      insert_to_wait_queue(&p->write_wait_queue, current);
      schedule();
    }

    uint32 space = PIPE_SIZE - p->len;
    uint32 want = (uint32)(count - written);
    if (want > space)
      want = space;

    uint32 first = want;
    uint32 till_end = PIPE_SIZE - p->wpos;
    if (first > till_end)
      first = till_end;
    memcpy(p->buf + p->wpos, src + written, first);
    if (want > first)
      memcpy(p->buf, src + written + first, want - first);

    p->wpos = (p->wpos + want) % PIPE_SIZE;
    p->len += want;
    written += want;

    // New data is available; wake one reader.
    pipe_wake_readers(p);
  }

  return (int)written;
}

static void pipe_close_end(pipe_t *p, int readable, int writable) {
  if (!p)
    return;

  if (readable && p->readers > 0)
    p->readers--;
  if (writable && p->writers > 0)
    p->writers--;

  // Closing endpoints may unblock the other side.
  if (p->writers == 0)
    pipe_wake_all_readers(p);
  if (p->readers == 0)
    pipe_wake_all_writers(p);

  if (p->readers == 0 && p->writers == 0) {
    // Nobody holds the pipe anymore.
    if (p->buf)
      free_page(p->buf);
    free_page(p);
  }
}

//
// initialize file system
//
void fs_init(void) {
  // initialize the vfs
  vfs_init();

  // register hostfs and mount it as the root
  if( register_hostfs() < 0 ) panic( "fs_init: cannot register hostfs.\n" );
  struct device *hostdev = init_host_device("HOSTDEV");
  vfs_mount("HOSTDEV", MOUNT_AS_ROOT);

  // register and mount rfs
  if( register_rfs() < 0 ) panic( "fs_init: cannot register rfs.\n" );
  struct device *ramdisk0 = init_rfs_device("RAMDISK0");
  rfs_format_dev(ramdisk0);
  vfs_mount("RAMDISK0", MOUNT_DEFAULT);
}

//
// initialize a proc_file_management data structure for a process.
// return the pointer to the page containing the data structure.
//
proc_file_management *init_proc_file_management(void) {
  proc_file_management *pfiles = (proc_file_management *)alloc_page();
  pfiles->cwd = vfs_root_dentry; // by default, cwd is the root
  pfiles->nfiles = 0;

  for (int fd = 0; fd < MAX_FILES; ++fd)
    pfiles->opened_files[fd].status = FD_NONE;

  sprint("FS: created a file management struct for a process.\n");
  return pfiles;
}

//
// reclaim the open-file management data structure of a process.
// note: this function is not used as PKE does not actually reclaim a process.
//
void reclaim_proc_file_management(proc_file_management *pfiles) {
  free_page(pfiles);
  return;
}

// 辅助函数：在 dup 过程中增加引用计数，避免被过早释放
static void bump_ref_on_dup(struct file *pfile) {
  if (!pfile)
    return;

  if (pfile->status == FD_OPENED_PIPE) {
    pipe_t *p = (pipe_t *)pfile->pipe;
    if (p) {
      if (pfile->readable)
        p->readers++;
      if (pfile->writable)
        p->writers++;
    }
    return;
  }

  if (pfile->status == FD_OPENED) {
    // Each proc table entry counts as one opened reference at vfs layer.
    if (pfile->f_dentry)
      pfile->f_dentry->d_ref++;
    return;
  }
}

static void drop_ref_on_close(struct file *pfile) {
  if (!pfile)
    return;

  if (pfile->status == FD_OPENED_PIPE) {
    pipe_close_end((pipe_t *)pfile->pipe, pfile->readable, pfile->writable);
    pfile->status = FD_NONE;
    pfile->readable = 0;
    pfile->writable = 0;
    pfile->offset = 0;
    pfile->f_dentry = NULL;
    pfile->pipe = NULL;
    return;
  }

  if (pfile->status == FD_OPENED) {
    (void)vfs_close(pfile);
    pfile->pipe = NULL;
    return;
  }
}

void close_all_files(proc_file_management *pfiles) {
  if (!pfiles)
    return;
  for (int fd = 0; fd < MAX_FILES; fd++) {
    if (pfiles->opened_files[fd].status != FD_NONE)
      drop_ref_on_close(&pfiles->opened_files[fd]);
  }
  pfiles->nfiles = 0;
}

void dup_proc_file_management(proc_file_management *dst,
                              proc_file_management *src) {
  if (!dst || !src)
    return;

  dst->cwd = src->cwd;
  dst->nfiles = src->nfiles;

  for (int fd = 0; fd < MAX_FILES; fd++) {
    dst->opened_files[fd] = src->opened_files[fd];
    if (dst->opened_files[fd].status != FD_NONE)
      bump_ref_on_dup(&dst->opened_files[fd]);
  }
}

//
// get an opened file from proc->opened_file array.
// return: the pointer to the opened file structure.
//
struct file *get_opened_file(int fd) {
  if (fd < 0 || fd >= MAX_FILES) panic("get_opened_file: invalid fd!\n");
  struct file *pfile = &(current->pfiles->opened_files[fd]);
  if (pfile->status == FD_NONE) panic("get_opened_file: unopened fd!\n");
  return pfile;
}

//
// open a file named as "pathname" with the permission of "flags".
// return: -1 on failure; non-zero file-descriptor on success.
//
int do_open(char *pathname, int flags) {
  struct file *opened_file = NULL;
  if ((opened_file = vfs_open(pathname, flags)) == NULL) return -1;

  // Reserve 0/1/2 for stdin/stdout/stderr.
  int fd = -1;
  struct file *pfile = NULL;
  for (int i = 3; i < MAX_FILES; ++i) {
    if (current->pfiles->opened_files[i].status == FD_NONE) {
      fd = i;
      pfile = &(current->pfiles->opened_files[i]);
      break;
    }
  }
  if (pfile == NULL) panic("do_open: no file entry for current process!\n");

  // initialize this file structure
  memcpy(pfile, opened_file, sizeof(struct file));

  ++current->pfiles->nfiles;
  return fd;
}

//
// read content of a file ("fd") into "buf" for "count".
// return: actual length of data read from the file.
//
int do_read(int fd, char *buf, uint64 count) {
  if (count == 0)
    return 0;

  // fd 0 defaults to stdin from Spike host, but can be redirected (e.g., to a pipe).
  if (fd == 0) {
    struct file *in = &(current->pfiles->opened_files[0]);
    if (in->status == FD_OPENED_PIPE) {
      if (in->readable == 0)
        panic("do_read: pipe not readable!\n");
      return pipe_read((pipe_t *)in->pipe, buf, count);
    }
    if (in->status == FD_OPENED) {
      // redirected stdin to a regular file
      struct file *pfile = get_opened_file(0);
      if (pfile->readable == 0)
        panic("do_read: no readable file!\n");
      return (int)vfs_read(pfile, buf, count);
    }

    spike_file_t *f = spike_file_get(0);
    if (!f)
      return -1;
    int len = spike_file_read(f, buf, count);
    spike_file_decref(f);
    return len;
  }

  struct file *pfile = get_opened_file(fd);

  if (pfile->readable == 0) panic("do_read: no readable file!\n");

  if (pfile->status == FD_OPENED_PIPE)
    return pipe_read((pipe_t *)pfile->pipe, buf, count);

  return (int)vfs_read(pfile, buf, count);
}

//
// write content ("buf") whose length is "count" to a file "fd".
// return: actual length of data written to the file.
//
int do_write(int fd, char *buf, uint64 count) {
  struct file *pfile = get_opened_file(fd);

  if (pfile->writable == 0) panic("do_write: cannot write file!\n");

  if (pfile->status == FD_OPENED_PIPE) {
    return pipe_write((pipe_t *)pfile->pipe, buf, count);
  }

  return (int)vfs_write(pfile, buf, count);
}

//
// reposition the file offset
//
int do_lseek(int fd, int offset, int whence) {
  struct file *pfile = get_opened_file(fd);
  return vfs_lseek(pfile, offset, whence);
}

//
// read the vinode information
//
int do_stat(int fd, struct istat *istat) {
  struct file *pfile = get_opened_file(fd);
  return vfs_stat(pfile, istat);
}

//
// read the inode information on the disk
//
int do_disk_stat(int fd, struct istat *istat) {
  struct file *pfile = get_opened_file(fd);
  return vfs_disk_stat(pfile, istat);
}

//
// close a file
//
int do_close(int fd) {
  struct file *pfile = get_opened_file(fd);
  if (pfile->status == FD_OPENED_PIPE) {
    drop_ref_on_close(pfile);
    if (current->pfiles->nfiles > 0)
      current->pfiles->nfiles--;
    return 0;
  }
  int ret = vfs_close(pfile);
  if (current->pfiles->nfiles > 0)
    current->pfiles->nfiles--;
  return ret;
}

// 创建一个管道，fd[0] 用于读，fd[1] 用于写
int do_pipe(int fd[2]) {
  if (!fd)
    return -1;

  int rfd = -1;
  int wfd = -1;

  // Reserve 0/1/2 for stdio.
  for (int i = 3; i < MAX_FILES; i++) {
    if (current->pfiles->opened_files[i].status != FD_NONE)
      continue;
    if (rfd < 0)
      rfd = i;
    else {
      wfd = i;
      break;
    }
  }

  if (rfd < 0 || wfd < 0)
    return -1;

  pipe_t *p = alloc_pipe();
  if (!p)
    return -1;

  struct file *rf = &current->pfiles->opened_files[rfd];
  memset(rf, 0, sizeof(*rf));
  rf->status = FD_OPENED_PIPE;
  rf->readable = 1;
  rf->writable = 0;
  rf->offset = 0;
  rf->f_dentry = NULL;
  rf->pipe = (void *)p;

  struct file *wf = &current->pfiles->opened_files[wfd];
  memset(wf, 0, sizeof(*wf));
  wf->status = FD_OPENED_PIPE;
  wf->readable = 0;
  wf->writable = 1;
  wf->offset = 0;
  wf->f_dentry = NULL;
  wf->pipe = (void *)p;

  fd[0] = rfd;
  fd[1] = wfd;
  current->pfiles->nfiles += 2;
  return 0;
}

// 重复 oldfd 到 newfd，若 newfd 已经被打开，则先关闭它
int do_dup2(int oldfd, int newfd) {
  if (oldfd < 0 || oldfd >= MAX_FILES || newfd < 0 || newfd >= MAX_FILES)
    return -1;
  if (oldfd == newfd)
    return newfd;

  struct file *old = &(current->pfiles->opened_files[oldfd]);
  if (old->status == FD_NONE)
    return -1;

  if (current->pfiles->opened_files[newfd].status != FD_NONE)
    do_close(newfd);

  struct file *dst = &(current->pfiles->opened_files[newfd]);
  *dst = *old;
  bump_ref_on_dup(dst);
  current->pfiles->nfiles += 1;
  return newfd;
}

//
// open a directory
// return: the fd of the directory file
//
int do_opendir(char *pathname) {
  struct file *opened_file = NULL;
  if ((opened_file = vfs_opendir(pathname)) == NULL) return -1;

  // Reserve 0/1/2 for stdin/stdout/stderr.
  int fd = -1;
  struct file *pfile = NULL;
  for (int i = 3; i < MAX_FILES; ++i) {
    if (current->pfiles->opened_files[i].status == FD_NONE) {
      fd = i;
      pfile = &(current->pfiles->opened_files[i]);
      break;
    }
  }
  if (pfile == NULL) panic("do_opendir: no file entry for current process!\n");

  // initialize this file structure
  memcpy(pfile, opened_file, sizeof(struct file));

  ++current->pfiles->nfiles;
  return fd;
}

//
// read a directory entry
//
int do_readdir(int fd, struct dir *dir) {
  struct file *pfile = get_opened_file(fd);
  return vfs_readdir(pfile, dir);
}

//
// make a new directory
//
int do_mkdir(char *pathname) {
  return vfs_mkdir(pathname);
}

//
// close a directory
//
int do_closedir(int fd) {
  struct file *pfile = get_opened_file(fd);
  return vfs_closedir(pfile);
}

//
// create hard link to a file
//
int do_link(char *oldpath, char *newpath) {
  return vfs_link(oldpath, newpath);
}

//
// remove a hard link to a file
//
int do_unlink(char *path) {
  return vfs_unlink(path);
}

// 经过 user_va_to_pa 函数的转换之后 ，这里的path就是字符串的实际物理地址
int do_rcwd(char *path) {
  // 从当前的 cwd dentry 向上遍历到根目录，构建路径字符串
  struct dentry *d = current->pfiles->cwd;

  // 如果是根目录
  if (d->parent == NULL || d == vfs_root_dentry) {
    strcpy(path, "/");
    return 0;
  }

  // 从当前目录向上遍历，收集路径
  char temp[MAX_PATH_LEN];
  temp[0] = '\0';

  while (d != NULL && d != vfs_root_dentry) {
    char segment[MAX_PATH_LEN];
    strcpy(segment, "/");
    strcat(segment, d->name);
    // temp是目前的目录，segment是当前目录的父目录，所以接到后面
    strcat(segment, temp);
    strcpy(temp, segment);
    d = d->parent;
  }

  if (temp[0] == '\0') {
    strcpy(path, "/");
  } else {
    strcpy(path, temp);
  }

  return 0;
}
//
// 改变当前工作目录
//
int do_ccwd(const char *path) {
  // 默认是./格式，从当前目录开始
  struct dentry *parent = current->pfiles->cwd;
  char miss_name[MAX_PATH_LEN];

  // 如果是绝对路径，从根目录开始
  if (path[0] == '/') {
    parent = vfs_root_dentry;
  }
  // // 如果是../格式，从当前目录的父目录开始
  // if (path[0] == '.' && path[1] == '.') {
  //   parent = parent->parent;
  // }

  // 查找目标目录
  struct dentry *target = lookup_final_dentry(path, &parent, miss_name);

  if (target == NULL) {
    sprint("do_change_cwd: directory not found!\n");
    return -1;
  }

  if (target->dentry_inode->type != DIR_I) {
    sprint("do_change_cwd: not a directory!\n");
    return -1;
  }

  // 更新当前工作目录，在这里真正实现cd 的 功能
  current->pfiles->cwd = target;
  return 0;
}
