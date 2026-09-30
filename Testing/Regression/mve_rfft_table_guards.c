/* Copyright (c) 2026 Ambiq Micro, Inc.
 * SPDX-License-Identifier: Apache-2.0
 */
/* Exercise the actual forward split kernels with each library-owned table
 * copied immediately before an MPU-inaccessible region. Input/output have
 * vector tail storage; this test isolates coefficient-table reads.
 */
#include "ARMCM55.h"
#include "arm_math.h"
#include "arm_common_tables.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(ARM_MATH_MVEI) || defined(ARM_MATH_AUTOVECTORIZE)
#error This regression requires the handwritten MVE integer kernels.
#endif

extern void regression_console_init(void);
extern void arm_split_rfft_q15(q15_t *, uint32_t, const q15_t *, const q15_t *, q15_t *, uint32_t);
extern void arm_split_rfft_q31(q31_t *, uint32_t, const q31_t *, const q31_t *, q31_t *, uint32_t);
static unsigned char arena[32832] __ALIGNED(32);
static q15_t input15[8200], reference15[8200], output15[8200];
static q31_t input31[8200], reference31[8200], output31[8200];
static volatile unsigned current_bits, current_length, current_table;

void MemManage_Handler(void)
{
    ARM_MPU_Disable();
    printf("FAIL: Q%u length=%u table=%c guard access\n",
           current_bits, current_length, 'A' + current_table);
    exit(1);
}

static void protect_tail(uintptr_t guard)
{
    ARM_MPU_Disable();
    for (uint32_t region = 0; region < ((MPU->TYPE >> 8) & 0xffU); ++region)
        ARM_MPU_ClrRegion(region);
    ARM_MPU_SetMemAttr(0, ARM_MPU_ATTR(ARM_MPU_ATTR_NON_CACHEABLE,
                                     ARM_MPU_ATTR_NON_CACHEABLE));
    ARM_MPU_SetRegion(0, ARM_MPU_RBAR(0, ARM_MPU_SH_NON, 0, 1, 0),
                     ARM_MPU_RLAR(guard - 1U, 0));
    ARM_MPU_SetRegion(1, ARM_MPU_RBAR(guard + 32U, ARM_MPU_SH_NON, 0, 1, 0),
                     ARM_MPU_RLAR(UINT32_MAX, 0));
    ARM_MPU_Enable(0);
}

int main(void)
{
    regression_console_init();
    unsigned checks = 0;
    unsigned char *end = arena + sizeof(arena) - 32;
    for (unsigned i = 0; i < 8200; ++i)
    {
        input15[i] = (q15_t)((int)(i % 31) * 127 - 1905);
        input31[i] = (q31_t)((int)(i % 31) * 1048576 - 15728640);
    }
    for (unsigned n = 32; n <= 8192; n *= 2)
    {
        current_length = n;
        arm_split_rfft_q15(input15, n / 2, realCoefAQ15, realCoefBQ15, reference15, 8192 / n);
        arm_split_rfft_q31(input31, n / 2, realCoefAQ31, realCoefBQ31, reference31, 8192 / n);
        for (unsigned table = 0; table < 2; ++table)
        {
            current_table = table;
            current_bits = 15;
            size_t bytes15 = table ? sizeof(realCoefBQ15) : sizeof(realCoefAQ15);
            q15_t *copy15 = (q15_t *)(end - bytes15);
            memcpy(copy15, table ? realCoefBQ15 : realCoefAQ15, bytes15);
            protect_tail((uintptr_t)end);
            arm_split_rfft_q15(input15, n / 2, table ? realCoefAQ15 : copy15,
                              table ? copy15 : realCoefBQ15, output15, 8192 / n);
            ARM_MPU_Disable();
            if (memcmp(output15, reference15, (n + 2) * sizeof(q15_t)))
            {
                puts("FAIL: Q15 split output mismatch");
                return 1;
            }
            ++checks;
            current_bits = 31;
            size_t bytes31 = table ? sizeof(realCoefBQ31) : sizeof(realCoefAQ31);
            q31_t *copy31 = (q31_t *)(end - bytes31);
            memcpy(copy31, table ? realCoefBQ31 : realCoefAQ31, bytes31);
            protect_tail((uintptr_t)end);
            arm_split_rfft_q31(input31, n / 2, table ? realCoefAQ31 : copy31,
                              table ? copy31 : realCoefBQ31, output31, 8192 / n);
            ARM_MPU_Disable();
            if (memcmp(output31, reference31, (n + 2) * sizeof(q31_t)))
            {
                puts("FAIL: Q31 split output mismatch");
                return 1;
            }
            ++checks;
        }
    }
    printf("PASS: %u MVE RFFT table-guard cases\n", checks);
    return 0;
}
