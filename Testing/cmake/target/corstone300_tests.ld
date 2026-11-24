/* Corstone-300: code in ITCM at the reset VTOR (0x10000000), everything else in DDR. */
__HEAP_SIZE  = 0x04000000;
__STACK_SIZE = 0x00100000;

MEMORY
{
  CODE (rx)  : ORIGIN = 0x10000000, LENGTH = 0x00080000
  DATA (rwx) : ORIGIN = 0x70000000, LENGTH = 0x10000000
}

INCLUDE tests_sections.ld
