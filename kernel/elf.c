/*
 * routines that scan and load a (host) Executable and Linkable Format (ELF) file
 * into the (emulated) memory.
 */

#include "elf.h"
#include "string.h"
#include "riscv.h"
#include "vmm.h"
#include "pmm.h"
#include "vfs.h"
#include "spike_interface/spike_utils.h"
#include "process.h"

typedef struct elf_info_t {
  struct file *f;
  process *p;
} elf_info;

static elf_section_header shstrtab_header;
static uint64 symtab_off = 0, symtab_sz = 0;
static uint64 strtab_off = 0, strtab_sz = 0;
static elf_info g_elf_info;
static elf_ctx g_elfloader;

char debug_line_data[128 * 1024];


// leb128 (little-endian base 128) is a variable-length
// compression algoritm in DWARF
void read_uleb128(uint64 *out, char **off) {
  uint64 value = 0;
  int shift = 0;
  uint8 b;
  for (;;) {
    b = *(uint8 *)(*off);
    (*off)++;
    value |= ((uint64)b & 0x7F) << shift;
    shift += 7;
    if ((b & 0x80) == 0)
      break;
  }
  if (out)
    *out = value;
}
void read_sleb128(int64 *out, char **off) {
  int64 value = 0;
  int shift = 0;
  uint8 b;
  for (;;) {
    b = *(uint8 *)(*off);
    (*off)++;
    value |= ((uint64_t)b & 0x7F) << shift;
    shift += 7;
    if ((b & 0x80) == 0)
      break;
  }
  if (shift < 64 && (b & 0x40))
    value |= -(1 << shift);
  if (out)
    *out = value;
}
// Since reading below types through pointer cast requires aligned address,
// so we can only read them byte by byte
void read_uint64(uint64 *out, char **off) {
  *out = 0;
  for (int i = 0; i < 8; i++) {
    *out |= (uint64)(**off) << (i << 3);
    (*off)++;
  }
}
void read_uint32(uint32 *out, char **off) {
  *out = 0;
  for (int i = 0; i < 4; i++) {
    *out |= (uint32)(**off) << (i << 3);
    (*off)++;
  }
}
void read_uint16(uint16 *out, char **off) {
  *out = 0;
  for (int i = 0; i < 2; i++) {
    *out |= (uint16)(**off) << (i << 3);
    (*off)++;
  }
}

void make_addr_line(elf_ctx *ctx, char *debug_line, uint64 length) {
  process *p = ((elf_info *)ctx->info)->p;
  p->debugline = debug_line;
  // directory name char pointer array
  p->dir = (char **)((((uint64)debug_line + length + 7) >> 3) << 3);
  int dir_ind = 0, dir_base;
  // file name char pointer array
  p->file = (code_file *)(p->dir + 64);
  int file_ind = 0, file_base;
  // table array
  p->line = (addr_line *)(p->file + 64);
  p->line_ind = 0;
  char *off = debug_line;
  while (off < debug_line + length) { // iterate each compilation unit(CU)
    debug_header *dh = (debug_header *)off;
    off += sizeof(debug_header);
    dir_base = dir_ind;
    file_base = file_ind;
    // get directory name char pointer in this CU
    while (*off != 0) {
      p->dir[dir_ind++] = off;
      while (*off != 0)
        off++;
      off++;
    }
    off++;
    // get file name char pointer in this CU
    while (*off != 0) {
      p->file[file_ind].file = off;
      while (*off != 0)
        off++;
      off++;
      uint64 dir;
      read_uleb128(&dir, &off);
      p->file[file_ind++].dir = dir - 1 + dir_base;
      read_uleb128(NULL, &off);
      read_uleb128(NULL, &off);
    }
    off++;
    addr_line regs;
    regs.addr = 0;
    regs.file = 1;
    regs.line = 1;
    // simulate the state machine op code
    for (;;) {
      uint8 op = *(off++);
      switch (op) {
      case 0: // Extended Opcodes
        read_uleb128(NULL, &off);
        op = *(off++);
        switch (op) {
        case 1: // DW_LNE_end_sequence
          if (p->line_ind > 0 && p->line[p->line_ind - 1].addr == regs.addr)
            p->line_ind--;
          p->line[p->line_ind] = regs;
          p->line[p->line_ind].file += file_base - 1;
          p->line_ind++;
          goto endop;
        case 2: // DW_LNE_set_address
          read_uint64(&regs.addr, &off);
          break;
        // ignore DW_LNE_define_file
        case 4: // DW_LNE_set_discriminator
          read_uleb128(NULL, &off);
          break;
        }
        break;
      case 1: // DW_LNS_copy
        if (p->line_ind > 0 && p->line[p->line_ind - 1].addr == regs.addr)
          p->line_ind--;
        p->line[p->line_ind] = regs;
        p->line[p->line_ind].file += file_base - 1;
        p->line_ind++;
        break;
      case 2: { // DW_LNS_advance_pc
        uint64 delta;
        read_uleb128(&delta, &off);
        regs.addr += delta * dh->min_instruction_length;
        break;
      }
      case 3: { // DW_LNS_advance_line
        int64 delta;
        read_sleb128(&delta, &off);
        regs.line += delta;
        break;
      }
      case 4: // DW_LNS_set_file
        read_uleb128(&regs.file, &off);
        break;
      case 5: // DW_LNS_set_column
        read_uleb128(NULL, &off);
        break;
      case 6: // DW_LNS_negate_stmt
      case 7: // DW_LNS_set_basic_block
        break;
      case 8: { // DW_LNS_const_add_pc
        int adjust = 255 - dh->opcode_base;
        int delta = (adjust / dh->line_range) * dh->min_instruction_length;
        regs.addr += delta;
        break;
      }
      case 9: { // DW_LNS_fixed_advanced_pc
        uint16 delta;
        read_uint16(&delta, &off);
        regs.addr += delta;
        break;
      }
        // ignore 10, 11 and 12
      default: { // Special Opcodes
        int adjust = op - dh->opcode_base;
        int addr_delta = (adjust / dh->line_range) * dh->min_instruction_length;
        int line_delta = dh->line_base + (adjust % dh->line_range);
        regs.addr += addr_delta;
        regs.line += line_delta;
        if (p->line_ind > 0 && p->line[p->line_ind - 1].addr == regs.addr)
          p->line_ind--;
        p->line[p->line_ind] = regs;
        p->line[p->line_ind].file += file_base - 1;
        p->line_ind++;
        break;
      }
      }
    }
  endop:;
  }
  // for (int i = 0; i < p->line_ind; i++)
  //     sprint("%p %d %d\n", p->line[i].addr, p->line[i].line,
  //     p->line[i].file);
}

//
// the implementation of allocater. allocates memory space for later segment loading.
// this allocater is heavily modified @lab2_1, where we do NOT work in bare mode.
//
static void *elf_alloc_mb(elf_ctx *ctx, uint64 elf_pa, uint64 elf_va, uint64 size) {
  elf_info *msg = (elf_info *)ctx->info;
  // we assume that size of proram segment is smaller than a page.
  kassert(size < PGSIZE);
  void *pa = alloc_page();
  if (pa == 0) panic("uvmalloc mem alloc falied\n");

  memset((void *)pa, 0, PGSIZE);
  user_vm_map((pagetable_t)msg->p->pagetable, elf_va, PGSIZE, (uint64)pa,
         prot_to_type(PROT_WRITE | PROT_READ | PROT_EXEC, 1));

  return pa;
}

//
// actual file reading, using the vfs file interface.
//
static uint64 elf_fpread(elf_ctx *ctx, void *dest, uint64 nb, uint64 offset) {
  elf_info *msg = (elf_info *)ctx->info;
  vfs_lseek(msg->f, offset, SEEK_SET);
  return vfs_read(msg->f, dest, nb);
}

char *find_symbol_name_by_addr(uint64 addr) {
  if (g_elf_info.f == NULL || symtab_sz == 0 || strtab_off == 0)
    return "unknown";

  elf_symbol sym;
  // 计算有多少个符号
  int num_symbols = symtab_sz / sizeof(elf_symbol);

  for (int i = 0; i < num_symbols; i++) {
    // 1. 读取第 i 个符号
    // 偏移量 = 符号表起点 + i * 单个符号大小
    uint64 current_sym_offset = symtab_off + i * sizeof(elf_symbol);
    elf_fpread(&g_elfloader, &sym, sizeof(sym), current_sym_offset);
    // 2. 核心判断：地址匹配
    // 如果 ra 落在 [sym.value, sym.value + sym.size) 区间
    if (addr >= sym.value && addr < (sym.value + sym.size)) {
      // 3. 找到了！去 .strtab 读名字
      // 名字位置 = 字符串表起点 + 符号中的 name 偏移
      static char func_name_buf[64]; // 静态buffer返回（或者你可以malloc）
      memset(func_name_buf, 0, sizeof(func_name_buf));
      elf_fpread(&g_elfloader, func_name_buf, sizeof(func_name_buf) - 1,
                 strtab_off + sym.name);

      return func_name_buf;
    }
  }
  return "unknown";
}

//
// init elf_ctx, a data structure that loads the elf.
//
elf_status elf_init(elf_ctx *ctx, void *info) {
  ctx->info = info;

  // load the elf header
  if (elf_fpread(ctx, &ctx->ehdr, sizeof(ctx->ehdr), 0) != sizeof(ctx->ehdr)) return EL_EIO;

  // check the signature (magic value) of the elf
  if (ctx->ehdr.magic != ELF_MAGIC) return EL_NOTELF;

  return EL_OK;
}

//
// load the elf segments to memory regions.
//
elf_status elf_load(elf_ctx *ctx) {
  // elf_prog_header structure is defined in kernel/elf.h
  elf_prog_header ph_addr;
  int i, off;

  // traverse the elf program segment headers
  for (i = 0, off = ctx->ehdr.phoff; i < ctx->ehdr.phnum; i++, off += sizeof(ph_addr)) {
    // read segment headers
    if (elf_fpread(ctx, (void *)&ph_addr, sizeof(ph_addr), off) != sizeof(ph_addr)) return EL_EIO;

    if (ph_addr.type != ELF_PROG_LOAD) continue;
    if (ph_addr.memsz < ph_addr.filesz) return EL_ERR;
    if (ph_addr.vaddr + ph_addr.memsz < ph_addr.vaddr) return EL_ERR;

    // allocate memory block before elf loading
    void *dest = elf_alloc_mb(ctx, ph_addr.vaddr, ph_addr.vaddr, ph_addr.memsz);

    // actual loading
    if (elf_fpread(ctx, dest, ph_addr.memsz, ph_addr.off) != ph_addr.memsz)
      return EL_EIO;

    // record the vm region in proc->mapped_info. added @lab3_1
    int j;
    for( j=0; j<PGSIZE/sizeof(mapped_region); j++ ) //seek the last mapped region
      if( (process*)(((elf_info*)(ctx->info))->p)->mapped_info[j].va == 0x0 ) break;

    ((process*)(((elf_info*)(ctx->info))->p))->mapped_info[j].va = ph_addr.vaddr;
    ((process*)(((elf_info*)(ctx->info))->p))->mapped_info[j].npages = 1;

    // SEGMENT_READABLE, SEGMENT_EXECUTABLE, SEGMENT_WRITABLE are defined in kernel/elf.h
    if( ph_addr.flags == (SEGMENT_READABLE|SEGMENT_EXECUTABLE) ){
      ((process*)(((elf_info*)(ctx->info))->p))->mapped_info[j].seg_type = CODE_SEGMENT;
      sprint( "CODE_SEGMENT added at mapped info offset:%d\n", j );
    }else if ( ph_addr.flags == (SEGMENT_READABLE|SEGMENT_WRITABLE) ){
      ((process*)(((elf_info*)(ctx->info))->p))->mapped_info[j].seg_type = DATA_SEGMENT;
      sprint( "DATA_SEGMENT added at mapped info offset:%d\n", j );
    }else
      panic( "unknown program segment encountered, segment flag:%d.\n", ph_addr.flags );

    ((process*)(((elf_info*)(ctx->info))->p))->total_mapped_region ++;
  }

  return EL_OK;
}

//
// load the elf of user application, by using the spike file interface.
//
void load_bincode_from_host_elf(process *p, char *filename) {
  sprint("Application: %s\n", filename);

  if (g_elf_info.f != NULL) {
    vfs_close(g_elf_info.f);
    g_elf_info.f = NULL;
  }

  g_elf_info.f = vfs_open(filename, O_RDONLY);
  g_elf_info.p = p;
  symtab_off = symtab_sz = 0;
  strtab_off = strtab_sz = 0;
  // IS_ERR_VALUE is a macro defined in spike_interface/spike_htif.h
  if (g_elf_info.f == NULL || IS_ERR_VALUE((uint64)g_elf_info.f))
    panic("Fail on opening the input application program.");

  // init elfloader context. elf_init() is defined above.
  if (elf_init(&g_elfloader, &g_elf_info) != EL_OK)
    panic("fail to init elfloader.\n");

  // load elf. elf_load() is defined above.
  if (elf_load(&g_elfloader) != EL_OK) panic("Fail on loading elf.\n");

  // entry (virtual, also physical in lab1_x) address
  p->trapframe->epc = g_elfloader.ehdr.entry;


  // 找到shstrtab_header
  uint64 shstrtab_header_offset =
      g_elfloader.ehdr.shoff +
      g_elfloader.ehdr.shstrndx * g_elfloader.ehdr.shentsize;
  elf_fpread(&g_elfloader, &shstrtab_header, sizeof(shstrtab_header),
             shstrtab_header_offset);
  uint64 shstrtab_file_off = shstrtab_header.offset;

  elf_section_header sh;
  char name_buf[32]; // 用来存读取到的 Section 名字

  // 确定symtab和strtab的文件偏移
  for (int i = 0; i < g_elfloader.ehdr.shnum; i++) {
    // 1. 读取第 i 个 Section Header
    uint64 sh_offset = g_elfloader.ehdr.shoff + i * sizeof(elf_section_header);
    elf_fpread(&g_elfloader, &sh, sizeof(sh), sh_offset);

    // 2. 读取这个 Section 的名字
    // 名字位于：Section名字字符串表基址 + 当前Section的name偏移
    memset(name_buf, 0, sizeof(name_buf));
    elf_fpread(&g_elfloader, name_buf, sizeof(name_buf) - 1,
               shstrtab_file_off + sh.name);

    // 3. 比对名字
    if (strcmp(name_buf, ".symtab") == 0) {
      symtab_off = sh.offset; // 找到了符号表的文件偏移
      symtab_sz = sh.size;    // 找到了符号表的大小
    } else if (strcmp(name_buf, ".strtab") == 0) {
      strtab_off = sh.offset; // 找到了字符串表的文件偏移
      strtab_sz = sh.size;
    }
  }

  // 确定debug_line的内容，并调用make_addr_line建立地址到行号的映射
  for (int i = 0; i < g_elfloader.ehdr.shnum; i++) {
    // 1. 读取第 i 个 Section Header
    uint64 sh_offset = g_elfloader.ehdr.shoff + i * sizeof(elf_section_header);
    elf_fpread(&g_elfloader, &sh, sizeof(sh), sh_offset);

    // 2. 读取这个 Section 的名字
    // 名字位于：Section名字字符串表基址 + 当前Section的name偏移
    elf_fpread(&g_elfloader, name_buf, 32, shstrtab_file_off + sh.name);

    // 3. 比对名字
    if (strcmp(name_buf, ".debug_line") == 0) {
      elf_fpread(&g_elfloader, debug_line_data, sh.size, sh.offset);
      make_addr_line(&g_elfloader, debug_line_data, sh.size);
    }
  }

  sprint("Application program entry point (virtual address): 0x%lx\n", p->trapframe->epc);
}
