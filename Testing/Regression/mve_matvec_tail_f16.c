/* Copyright (c) 2026 Ambiq Micro, Inc.
 * SPDX-License-Identifier: Apache-2.0
 */
/* Cortex-M55 regression: f16 matrix storage ends at an inaccessible MPU
 * region. Companion to mve_matvec_tail.c (f32) and mve_matvec_tail_fixed.c
 * (Q7/Q15/Q31) for the f16 kernel.
 */
#include "ARMCM55.h"
#include "dsp/matrix_functions_f16.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(ARM_MATH_MVE_FLOAT16) || defined(ARM_MATH_AUTOVECTORIZE)
#error This regression requires the handwritten MVE float16 kernel.
#endif

#define MAX_ROWS 7U
#define MAX_COLS 17U
#define GUARD_BYTES 32U
#define F16_INF 0x7c00U

extern void regression_console_init(void);
static unsigned char arena[512 + GUARD_BYTES] __ALIGNED(32);
static float16_t padded[MAX_ROWS * MAX_COLS + 16];
static float16_t vector[MAX_COLS + 16];
static float16_t plain[MAX_ROWS + 2], guarded[MAX_ROWS + 2];
static volatile unsigned current_rows, current_cols;

void MemManage_Handler(void)
{
    ARM_MPU_Disable();
    printf("FAIL: f16 matrix tail access rows=%u cols=%u\n", current_rows, current_cols);
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
    ARM_MPU_SetRegion(1, ARM_MPU_RBAR(guard + GUARD_BYTES, ARM_MPU_SH_NON, 0, 1, 0),
                     ARM_MPU_RLAR(UINT32_MAX, 0));
    ARM_MPU_Enable(0);
}

static void set_bits(float16_t *p, uint16_t bits)
{
    memcpy(p, &bits, sizeof(bits));
}

/* Bit pattern with the two zeros identified: under -ffast-math the reference
 * sum may come out as -0.0 where the kernel gives +0.0. */
static uint16_t bits_of(const float16_t *p)
{
    uint16_t bits;
    memcpy(&bits, p, sizeof(bits));
    return bits == 0x8000U ? 0U : bits;
}

/* Each case runs the kernel twice: on a matrix followed by readable padding
 * holding f16 +Inf, and on the same matrix ending at the guard. The inputs are
 * multiples of 1/8 and 1/4 with |x| <= 3/8, so every partial sum is exact in
 * f16; both outputs must equal the double-precision reference bit for bit. A read
 * of the padding multiplies Inf by a zeroed vector lane and gives NaN.
 */
int main(void)
{
    regression_console_init();
    float16_t *end = (float16_t *)(arena + sizeof(arena) - GUARD_BYTES);
    unsigned checks = 0;
    for (unsigned rows = 1; rows <= MAX_ROWS; ++rows)
    {
        for (unsigned cols = 1; cols <= MAX_COLS; ++cols)
        {
            float16_t *matrix = end - rows * cols;
            for (unsigned i = 0; i < sizeof(padded) / sizeof(padded[0]); ++i)
                set_bits(&padded[i], F16_INF);
            for (unsigned i = 0; i < rows * cols; ++i)
                padded[i] = matrix[i] = (float16_t)(((int)(i % 7) - 3) * 0.125f);
            for (unsigned i = 0; i < MAX_COLS + 16; ++i)
                vector[i] = (float16_t)(((int)(i % 5) - 2) * 0.25f);
            for (unsigned i = 0; i < MAX_ROWS + 2; ++i)
            {
                set_bits(&plain[i], 0x2a2aU);
                set_bits(&guarded[i], 0x2a2aU);
            }
            arm_matrix_instance_f16 plain_matrix = {rows, cols, padded};
            arm_matrix_instance_f16 guarded_matrix = {rows, cols, matrix};
            current_rows = rows;
            current_cols = cols;
            arm_mat_vec_mult_f16(&plain_matrix, vector, plain + 1);
            protect_tail((uintptr_t)end);
            arm_mat_vec_mult_f16(&guarded_matrix, vector, guarded + 1);
            ARM_MPU_Disable();
            uint16_t first, last;
            memcpy(&first, &guarded[0], sizeof(first));
            memcpy(&last, &guarded[rows + 1], sizeof(last));
            if (first != 0x2a2aU || last != 0x2a2aU)
            {
                printf("FAIL: f16 output canary rows=%u cols=%u\n", rows, cols);
                return 1;
            }
            for (unsigned r = 0; r < rows; ++r)
            {
                double reference = 0.0;
                for (unsigned c = 0; c < cols; ++c)
                    reference += (double)matrix[r * cols + c] * (double)vector[c];
                /* Compare bit patterns: -ffast-math may fold a NaN comparison. */
                float16_t expected = (float16_t)reference;
                if (bits_of(&plain[r + 1]) != bits_of(&expected))
                {
                    printf("FAIL: f16 padded-matrix result rows=%u cols=%u\n", rows, cols);
                    return 1;
                }
                if (bits_of(&guarded[r + 1]) != bits_of(&expected))
                {
                    printf("FAIL: f16 numerical result rows=%u cols=%u\n", rows, cols);
                    return 1;
                }
            }
            ++checks;
        }
    }
    printf("PASS: %u MVE f16 matrix-tail cases\n", checks);
    return 0;
}
