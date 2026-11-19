/*
 * Corstone-300 (Cortex-M55) layout for the CMSIS-DSP test images, run with the FVP's -a loader.
 * Based on Cortex_DFP Device/ARMCM55/Config/ARMCM55_gcc.ld (Arm, Apache-2.0).
 *
 * The vector table must sit at the reset VTOR (0x10000000, ITCM), and code stays in ITCM. Pattern data,
 * the framework's working memory (malloc) and the stack go in DDR, which is large enough for any suite.
 * The FVP loads every segment at its address, so nothing is copied at startup.
 */
__HEAP_SIZE  = 0x04000000;
__STACK_SIZE = 0x00100000;

MEMORY
{
  ITCM (rx)  : ORIGIN = 0x10000000, LENGTH = 0x00080000
  DDR  (rwx) : ORIGIN = 0x70000000, LENGTH = 0x10000000
}

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
  } > ITCM

  .ARM.extab :
  {
    *(.ARM.extab* .gnu.linkonce.armextab.*)
  } > ITCM

  __exidx_start = .;
  .ARM.exidx :
  {
    *(.ARM.exidx* .gnu.linkonce.armexidx.*)
  } > ITCM
  __exidx_end = .;

  .copy.table :
  {
    . = ALIGN(4);
    __copy_table_start__ = .;
    __copy_table_end__ = .;
  } > ITCM

  /* The C run-time startup clears .bss. */
  .zero.table :
  {
    . = ALIGN(4);
    __zero_table_start__ = .;
    __zero_table_end__ = .;
  } > ITCM

  .rodata :
  {
    *(.rodata*)
  } > DDR

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
  } > DDR
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
  } > DDR

  .heap (NOLOAD) :
  {
    . = ALIGN(8);
    __end__ = .;
    PROVIDE(end = .);
    . = . + __HEAP_SIZE;
    . = ALIGN(8);
    __HeapLimit = .;
  } > DDR

  .stack (ORIGIN(DDR) + LENGTH(DDR) - __STACK_SIZE) (NOLOAD) :
  {
    . = ALIGN(8);
    __StackLimit = .;
    . = . + __STACK_SIZE;
    . = ALIGN(8);
    __StackTop = .;
  } > DDR
  PROVIDE(__stack = __StackTop);

  ASSERT(__StackLimit >= __HeapLimit, "region DDR overflowed with stack")
}
