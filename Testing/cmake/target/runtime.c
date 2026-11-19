/* Run-time support shared by the Cortex-M test images. */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>

void *_sbrk(ptrdiff_t increment);
void HardFault_Handler(void);
int _write(int file, const char *buffer, int length);
void _exit(int status);

extern char __end__[];
extern char __HeapLimit[];

/* Heap bounded by the linker script, so an allocation that does not fit fails instead of overwriting
 * whatever follows the heap. */
void *_sbrk(ptrdiff_t increment)
{
    static char *heap_end = __end__;
    char *previous = heap_end;

    if ((increment > 0 && (uintptr_t)increment > (uintptr_t)(__HeapLimit - heap_end)) ||
        (increment < 0 && (uintptr_t)(-increment) > (uintptr_t)(heap_end - __end__)))
    {
        errno = ENOMEM;
        return (void *)-1;
    }
    heap_end += increment;
    return previous;
}

/* A fault ends the run at once, instead of spinning in the default handler until the run times out. */
void HardFault_Handler(void)
{
    static const char message[] = "\nHardFault\n";

    _write(1, message, (int)sizeof message - 1);
    _exit(1);
}
