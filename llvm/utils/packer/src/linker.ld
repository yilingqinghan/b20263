/* 链接时还要加：-Wl,-z,max-page-size=1 -Wl,--no-rosegment */
OUTPUT_ARCH("riscv")
ENTRY(_start)

MEMORY
{
  ROM (rx)  : ORIGIN = 0x40070000, LENGTH = 512K
  RAM (rwx) : ORIGIN = 0x40090000, LENGTH = 512K
}

PHDRS
{
  text PT_LOAD FILEHDR;  /* 包含 ELF 头，但只按1字节对齐 */
  data PT_LOAD;          /* 数据区按1字节对齐即可 */
}

SECTIONS
{
  /* 跳过真实头部长度，不填满到下一页 */
  . = SIZEOF_HEADERS;

  /* code段直接映射到 text PT_LOAD */
  .text :
  {
    *(.text*)
  } > ROM :text

  /* data/bss 合并映射到 data PT_LOAD */
  .data :
  {
    *(.data*)
    *(.bss*)
    *(COMMON)
  } > RAM :data

  /* 丢弃多余的 comment/note */
  /DISCARD/ : { *(.riscv.attributes) }
  /DISCARD/ : { *(.comment*) *(.note*) }
}
