#ifndef _ELF_H_
#define _ELF_H_

#include "util/types.h"
#include "process.h"

#define MAX_CMDLINE_ARGS 64

// elf header structure
typedef struct elf_header_t {
  uint32 magic;
  uint8 elf[12];
  uint16 type;      /* Object file type */
  uint16 machine;   /* Architecture */
  uint32 version;   /* Object file version */
  uint64 entry;     /* Entry point virtual address */
  uint64 phoff;     /* Program header table file offset */
  uint64 shoff;     /* Section header table file offset */
  uint32 flags;     /* Processor-specific flags */
  uint16 ehsize;    /* ELF header size in bytes */
  uint16 phentsize; /* Program header table entry size */
  uint16 phnum;     /* Program header table entry count */
  uint16 shentsize; /* Section header table entry size */
  uint16 shnum;     /* Section header table entry count */
  uint16 shstrndx;  /* Section header string table index */
} elf_header;

// segment types, attributes of elf_prog_header_t.flags
#define SEGMENT_READABLE   0x4
#define SEGMENT_EXECUTABLE 0x1
#define SEGMENT_WRITABLE   0x2

// Program segment header.
typedef struct elf_prog_header_t {
  uint32 type;   /* Segment type */
  uint32 flags;  /* Segment flags */
  uint64 off;    /* Segment file offset */
  uint64 vaddr;  /* Segment virtual address */
  uint64 paddr;  /* Segment physical address */
  uint64 filesz; /* Segment size in file */
  uint64 memsz;  /* Segment size in memory */
  uint64 align;  /* Segment alignment */
} elf_prog_header;

typedef struct elf_section_header_t {
  uint32 name;      //  0 -  3: Section 名称偏移
  uint32 type;      //  4 -  7: Section 类型
  uint64 flags;     //  8 - 15: Section 标志
  uint64 addr;      // 16 - 23: 虚拟地址
  uint64 offset;    // 24 - 31: 文件偏移
  uint64 size;      // 32 - 39: Section 大小
  uint32 link;      // 40 - 43: 链接信息
  uint32 info;      // 44 - 47: 额外信息
  uint64 addralign; // 48 - 55: 对齐
  uint64 entsize;   // 56 - 63: 条目大小
} elf_section_header;

// Symbol Entry 结构 (24字节)
typedef struct elf_symbol_t {
  uint32 name;  //  0 -  3: 符号名偏移
  uint8 info;   //  4:      类型和绑定
  uint8 other;  //  5:      可见性
  uint16 shndx; //  6 -  7: Section 索引
  uint64 value; //  8 - 15: 符号值/地址
  uint64 size; // 16 - 23: 符号大小，指的是该符号对应的代码的实际长度（字节数）
} elf_symbol;

char *find_symbol_name_by_addr(uint64 addr); // 符号是函数

#define ELF_MAGIC 0x464C457FU  // "\x7FELF" in little endian
#define ELF_PROG_LOAD 1

typedef enum elf_status_t {
  EL_OK = 0,

  EL_EIO,
  EL_ENOMEM,
  EL_NOTELF,
  EL_ERR,

} elf_status;

typedef struct elf_ctx_t {
  void *info;
  elf_header ehdr;
} elf_ctx;

typedef struct __attribute__((packed)) {
    uint32 length;
    uint16 version;
    uint32 header_length;
    uint8 min_instruction_length;
    uint8 default_is_stmt;
    int8 line_base;
    uint8 line_range;
    uint8 opcode_base;
    uint8 std_opcode_lengths[12];
} debug_header;

elf_status elf_init(elf_ctx *ctx, void *info);
elf_status elf_load(elf_ctx *ctx);

void load_bincode_from_host_elf(process *p, char *filename);

#endif
