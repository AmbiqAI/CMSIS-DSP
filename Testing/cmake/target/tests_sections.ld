/*
 * Sections shared by the CMSIS-DSP test images. The target's linker script defines the CODE and DATA
 * regions, __HEAP_SIZE and __STACK_SIZE, then includes this file.
 * Based on Cortex_DFP Device/ARMCM55/Config/ARMCM55_gcc.ld (Arm, Apache-2.0).
 *
 * Code goes in CODE, which starts at the reset vector table. Pattern data, the framework's working
 * memory (malloc) and the stack go in DATA. The model loads every ELF segment at its address, so
 * nothing is copied at startup.
 */
ENTRY(Reset_Handler)

SECTIONS
{
  .text :
  {
    KEEP(*(.vectors))
    *(.text*)
    KEEP(*(.init))
    KEEP(*(.fini))
    *crtbegin.o(.ctors)
    *crtbegin?.o(.ctors)
    *(EXCLUDE_FILE(*crtend?.o *crtend.o) .ctors)
    *(SORT(.ctors.*))
    *(.ctors)
    *crtbegin.o(.dtors)
    *crtbegin?.o(.dtors)
    *(EXCLUDE_FILE(*crtend?.o *crtend.o) .dtors)
    *(SORT(.dtors.*))
    *(.dtors)
    KEEP(*(.eh_frame*))
  } > CODE

  .ARM.extab :
  {
    *(.ARM.extab* .gnu.linkonce.armextab.*)
  } > CODE

  __exidx_start = .;
  .ARM.exidx :
  {
    *(.ARM.exidx* .gnu.linkonce.armexidx.*)
  } > CODE
  __exidx_end = .;

  .copy.table :
  {
    . = ALIGN(4);
    __copy_table_start__ = .;
    __copy_table_end__ = .;
  } > CODE

  /* The C run-time startup clears .bss. */
  .zero.table :
  {
    . = ALIGN(4);
    __zero_table_start__ = .;
    __zero_table_end__ = .;
  } > CODE

  .rodata :
  {
    *(.rodata*)
  } > DATA

  .data : ALIGN(4)
  {
    __data_start__ = .;
    *(vtable)
    *(.data)
    *(.data.*)
    . = ALIGN(4);
    PROVIDE_HIDDEN (__preinit_array_start = .);
    KEEP(*(.preinit_array))
    PROVIDE_HIDDEN (__preinit_array_end = .);
    . = ALIGN(4);
    PROVIDE_HIDDEN (__init_array_start = .);
    KEEP(*(SORT(.init_array.*)))
    KEEP(*(.init_array))
    PROVIDE_HIDDEN (__init_array_end = .);
    . = ALIGN(4);
    PROVIDE_HIDDEN (__fini_array_start = .);
    KEEP(*(SORT(.fini_array.*)))
    KEEP(*(.fini_array))
    PROVIDE_HIDDEN (__fini_array_end = .);
    KEEP(*(.jcr*))
    . = ALIGN(4);
    __data_end__ = .;
  } > DATA
  PROVIDE(__etext = LOADADDR(.data));

  .bss (NOLOAD) :
  {
    . = ALIGN(4);
    __bss_start__ = .;
    *(.bss)
    *(.bss.*)
    *(COMMON)
    . = ALIGN(4);
    __bss_end__ = .;
  } > DATA

  .heap (NOLOAD) :
  {
    . = ALIGN(8);
    __end__ = .;
    PROVIDE(end = .);
    . = . + __HEAP_SIZE;
    . = ALIGN(8);
    __HeapLimit = .;
  } > DATA

  .stack (ORIGIN(DATA) + LENGTH(DATA) - __STACK_SIZE) (NOLOAD) :
  {
    . = ALIGN(8);
    __StackLimit = .;
    . = . + __STACK_SIZE;
    . = ALIGN(8);
    __StackTop = .;
  } > DATA
  PROVIDE(__stack = __StackTop);

  ASSERT(__StackLimit >= __HeapLimit, "region DATA overflowed with stack")
}
