/* MPS2 AN386 (QEMU mps2-an386) run-time support for the CMSIS-DSP test images: console on CMSDK UART0,
 * exit through semihosting so QEMU stops with the run's status. */

#include <stdint.h>

#define UART0_BASE    0x40004000UL
#define UART_DATA     (*(volatile uint32_t *)(UART0_BASE + 0x00UL))
#define UART_STATE    (*(volatile uint32_t *)(UART0_BASE + 0x04UL))
#define UART_CTRL     (*(volatile uint32_t *)(UART0_BASE + 0x08UL))
#define UART_BAUDDIV  (*(volatile uint32_t *)(UART0_BASE + 0x10UL))
#define UART_STATE_TXFULL (1UL << 0)
#define UART_CTRL_TXEN    (1UL << 0)

#define SEMIHOSTING_SYS_EXIT                0x18UL
#define ADP_STOPPED_APPLICATION_EXIT        0x20026UL
#define ADP_STOPPED_RUN_TIME_ERROR_UNKNOWN  0x20023UL

void testmain_hook(void);
int _write(int file, const char *buffer, int length);
void _exit(int status);

/* testmain.cpp calls this weak hook before any output. */
void testmain_hook(void)
{
    UART_BAUDDIV = 16UL;
    UART_CTRL = UART_CTRL_TXEN;
}

int _write(int file, const char *buffer, int length)
{
    (void)file;
    for (int index = 0; index < length; ++index)
    {
        while ((UART_STATE & UART_STATE_TXFULL) != 0UL) {}
        UART_DATA = (uint32_t)(uint8_t)buffer[index];
    }
    return length;
}

/* QEMU exits with status 0 for ADP_Stopped_ApplicationExit and 1 for any other stop reason. */
void _exit(int status)
{
    register uint32_t operation __asm__("r0") = SEMIHOSTING_SYS_EXIT;
    register uint32_t reason __asm__("r1") =
        status == 0 ? ADP_STOPPED_APPLICATION_EXIT : ADP_STOPPED_RUN_TIME_ERROR_UNKNOWN;

    __asm__ volatile("bkpt 0xab" : : "r"(operation), "r"(reason) : "memory");
    for (;;) {}
}
