/* Copyright (c) 2026 Ambiq Micro, Inc. */
/* SPDX-License-Identifier: Apache-2.0 */

/*
 * Minimal Cortex-M55 Corstone-300 UART retargeting.  This follows the
 * CMSDK APB UART contract. Test success is determined from the output,
 * not the model exit code, since UART shutdown does not carry a status.
 */

#include <stddef.h>
#include <stdint.h>

#define REGRESSION_C300_UART0_BASE       0x49303000UL
#define REGRESSION_C300_UART_DATA        (*(volatile uint32_t *)(REGRESSION_C300_UART0_BASE + 0x00UL))
#define REGRESSION_C300_UART_STATE       (*(volatile uint32_t *)(REGRESSION_C300_UART0_BASE + 0x04UL))
#define REGRESSION_C300_UART_CTRL        (*(volatile uint32_t *)(REGRESSION_C300_UART0_BASE + 0x08UL))
#define REGRESSION_C300_UART_BAUDDIV     (*(volatile uint32_t *)(REGRESSION_C300_UART0_BASE + 0x10UL))
#define REGRESSION_C300_UART_STATE_TXFULL (1UL << 0)

void regression_console_init(void)
{
    REGRESSION_C300_UART_BAUDDIV = 25000000UL / 115200UL;
    REGRESSION_C300_UART_CTRL = (1UL << 0) | (1UL << 1);
}

static void regression_putchar(char character)
{
    while ((REGRESSION_C300_UART_STATE & REGRESSION_C300_UART_STATE_TXFULL) != 0UL) {}
    if (character == '\n')
    {
        REGRESSION_C300_UART_DATA = '\r';
        while ((REGRESSION_C300_UART_STATE & REGRESSION_C300_UART_STATE_TXFULL) != 0UL) {}
    }
    REGRESSION_C300_UART_DATA = (uint32_t)(uint8_t)character;
}

int _write(int file, const char *buffer, int length)
{
    int index;

    (void)file;
    for (index = 0; index < length; ++index)
    {
        regression_putchar(buffer[index]);
    }
    return length;
}

void _exit(int status)
{
    (void)status;
    _write(1, "\004\nEXITTHESIM\n", 12);
    while (1) {}
}
