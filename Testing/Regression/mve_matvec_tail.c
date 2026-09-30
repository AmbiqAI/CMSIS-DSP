/* Copyright (c) 2026 Ambiq Micro, Inc.
 * SPDX-License-Identifier: Apache-2.0
 */
/* Cortex-M55 regression: matrix storage ends at an inaccessible MPU region.
 * This tests removal of the matrix-tail padding requirement; upstream's
 * general vector-buffer padding guidance otherwise permits these reads.
 */
#include "ARMCM55.h"
#include "dsp/matrix_functions.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#if !defined(ARM_MATH_MVEF) || defined(ARM_MATH_AUTOVECTORIZE)
#error This regression requires the handwritten MVE floating-point kernel.
#endif

extern void regression_console_init(void);
static float32_t arena[128] __ALIGNED(32);
static float32_t vector[16] __ALIGNED(16);
static float32_t output[9];
static volatile unsigned current_rows, current_cols;

void MemManage_Handler(void)
{
    ARM_MPU_Disable();
    printf("FAIL: matrix tail access rows=%u cols=%u\n", current_rows, current_cols);
    exit(1);
}

static void protect_tail(uintptr_t guard)
{
    ARM_MPU_Disable();
    for (uint32_t region = 0; region < ((MPU->TYPE >> 8) & 0xffU); ++region)
    {
        ARM_MPU_ClrRegion(region);
    }
    ARM_MPU_SetMemAttr(0, ARM_MPU_ATTR(ARM_MPU_ATTR_NON_CACHEABLE,
                                     ARM_MPU_ATTR_NON_CACHEABLE));
    /* Map everything except the aligned 32-byte guard. No background map. */
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
    /* Last 32 bytes of arena form the guard; each matrix ends before it. */
    float32_t *end = arena + 120;
    for (unsigned rows = 1; rows <= 7; ++rows)
    {
        for (unsigned cols = 1; cols <= 9; ++cols)
        {
            float32_t *matrix = end - rows * cols;
            for (unsigned i = 0; i < rows * cols; ++i)
                matrix[i] = (float32_t)((int)(i % 7) - 3) * 0.25f;
            for (unsigned i = 0; i < 16; ++i)
                vector[i] = (float32_t)((int)(i % 5) - 2) * 0.5f;
            for (unsigned i = 0; i < 9; ++i)
                output[i] = 12345.0f;
            arm_matrix_instance_f32 instance = {rows, cols, matrix};
            current_rows = rows;
            current_cols = cols;
            protect_tail((uintptr_t)end);
            arm_mat_vec_mult_f32(&instance, vector, output + 1);
            ARM_MPU_Disable();
            if (output[0] != 12345.0f || output[rows + 1] != 12345.0f)
            {
                puts("FAIL: output canary");
                return 1;
            }
            for (unsigned r = 0; r < rows; ++r)
            {
                double reference = 0;
                for (unsigned c = 0; c < cols; ++c)
                    reference += (double)matrix[r * cols + c] * vector[c];
                if (fabs((double)output[r + 1] - reference) > 1e-6)
                {
                    printf("FAIL: numerical result rows=%u cols=%u\n", rows, cols);
                    return 1;
                }
            }
            ++checks;
        }
    }
    printf("PASS: %u MVE matrix-tail cases\n", checks);
    return 0;
}
