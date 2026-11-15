ENTRY(_start)
MEMORY { RAM (rwx) : ORIGIN = 0x20000000, LENGTH = 1M }
SECTIONS {
  .text : { *(.text*) *(.rodata*) . = ALIGN(4); } > RAM
  .init_array : { __init_array_start = .; *(.init_array*) __init_array_end = .; } > RAM
  .data : { *(.data*) . = ALIGN(4); } > RAM
  .bss : { __bss_start__ = .; *(.bss*) *(COMMON) . = ALIGN(4); __bss_end__ = .; end = .; } > RAM
  /DISCARD/ : { *(.ARM.exidx*) }
}
