/*
 * contains the implementation of all syscalls.
 */

#include <stdint.h>
#include <errno.h>

#include "util/types.h"
#include "syscall.h"
#include "string.h"
#include "process.h"
#include "util/functions.h"
#include "elf.h"

#include "spike_interface/spike_utils.h"

//
// implement the SYS_user_print syscall
//
ssize_t sys_user_print(const char* buf, size_t n) {
  sprint(buf);
  return 0;
}

void print_symbol(spike_file_t *f, elf_sect_header *symtab, elf_sect_header *strtab, uint64_t addr) {
  elf_sym sym;
  for (uint64_t off = 0; off < symtab->size; off += sizeof(sym)) {
    spike_file_pread(f, &sym, sizeof(sym), symtab->offset + off);
    
    // Check if it's a function (STT_FUNC = 2)
    if ((sym.info & 0xf) != 2) continue;

    if (addr >= sym.value && addr < sym.value + sym.size) {
      char name[64];
      // Read symbol name from string table
      spike_file_pread(f, name, sizeof(name), strtab->offset + sym.name);
      name[63] = '\0'; // Ensure null termination
      sprint("%s\n", name);
      return;
    }
  }
  sprint("???\n");
}

//
// implement the SYS_user_exit syscall
//
ssize_t sys_user_exit(uint64 code) {
  sprint("User exit with code:%d.\n", code);
  // in lab1, PKE considers only one app (one process). 
  // therefore, shutdown the system when the app calls exit()
  shutdown(code);
}

ssize_t sys_user_backtrace(int depth) {
  spike_file_t *f = spike_file_open(current->app_name, O_RDONLY, 0);
  if (IS_ERR_VALUE(f)) {
    sprint("Failed to open ELF file: %s\n", current->app_name);
    return -1;
  }

  elf_header ehdr;
  spike_file_pread(f, &ehdr, sizeof(ehdr), 0);

  // Read section header string table header
  elf_sect_header shstrtab_hdr;
  spike_file_pread(f, &shstrtab_hdr, sizeof(shstrtab_hdr), ehdr.shoff + ehdr.shstrndx * ehdr.shentsize);

  // Read section header string table
  char shstrtab_buf[4096]; 
  if (shstrtab_hdr.size > sizeof(shstrtab_buf)) {
      sprint("shstrtab too big\n");
      spike_file_close(f);
      return -1;
  }
  spike_file_pread(f, shstrtab_buf, shstrtab_hdr.size, shstrtab_hdr.offset);

  elf_sect_header symtab_hdr, strtab_hdr;
  int found_symtab = 0, found_strtab = 0;

  for (int i = 0; i < ehdr.shnum; i++) {
    elf_sect_header shdr;
    spike_file_pread(f, &shdr, sizeof(shdr), ehdr.shoff + i * ehdr.shentsize);
    
    char *name = shstrtab_buf + shdr.name;
    if (strcmp(name, ".symtab") == 0) {
      symtab_hdr = shdr;
      found_symtab = 1;
    } else if (strcmp(name, ".strtab") == 0) {
      strtab_hdr = shdr;
      found_strtab = 1;
    }
  }

  if (!found_symtab || !found_strtab) {
    sprint("Symbol table or string table not found\n");
    spike_file_close(f);
    return -1;
  }
  
  uint64_t fp = current->trapframe->regs.s0;
  
  // Skip do_user_call frame
  fp = *(uint64_t*)(fp - 8);
  // Get return address to f8 (from print_backtrace frame)
  uint64_t ra = *(uint64_t*)(fp - 8);
  // Go to f8 frame
  fp = *(uint64_t*)(fp - 16);

  // Print f8
  print_symbol(f, &symtab_hdr, &strtab_hdr, ra);

  for (int i = 0; i < depth - 1; i++) {
    if (fp == 0) break; 
    ra = *(uint64_t*)(fp - 8);
    fp = *(uint64_t*)(fp - 16);
    print_symbol(f, &symtab_hdr, &strtab_hdr, ra);
  }

  spike_file_close(f);
  return 0;
}

//
// [a0]: the syscall number; [a1] ... [a7]: arguments to the syscalls.
// returns the code of success, (e.g., 0 means success, fail for otherwise)
//
long do_syscall(long a0, long a1, long a2, long a3, long a4, long a5, long a6, long a7) {
  switch (a0) {
    case SYS_user_print:
      return sys_user_print((const char*)a1, a2);
    case SYS_user_exit:
      return sys_user_exit(a1);
    case SYS_user_backtrace:
      return sys_user_backtrace(a1);
    default:
      panic("Unknown syscall %ld \n", a0);
  }
}
