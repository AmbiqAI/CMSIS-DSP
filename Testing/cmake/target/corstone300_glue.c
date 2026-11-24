/* Corstone-300 run-time support for the CMSIS-DSP test images. */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>

void regression_console_init(void);
void testmain_hook(void);
void *_sbrk(ptrdiff_t increment);

/* testmain.cpp calls this weak hook before any output. */
void testmain_hook(void)
{
    regression_console_init();
}

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
