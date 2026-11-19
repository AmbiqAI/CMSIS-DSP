/* MPS2 AN386 (QEMU mps2-an386): code in SSRAM1 at the reset vector table (0x0), everything else in
 * the 16 MB PSRAM. */
__HEAP_SIZE  = 0x00800000;
__STACK_SIZE = 0x00100000;

MEMORY
{
  CODE (rx)  : ORIGIN = 0x00000000, LENGTH = 0x00400000
  DATA (rwx) : ORIGIN = 0x21000000, LENGTH = 0x01000000
}

INCLUDE tests_sections.ld
