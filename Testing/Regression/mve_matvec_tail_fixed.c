/* Copyright (c) 2026 Ambiq Micro, Inc.
 * SPDX-License-Identifier: Apache-2.0
 */
/* Cortex-M55 regression: Q7/Q15/Q31 matrix storage ends at an inaccessible
 * MPU region. Companion to mve_matvec_tail.c for the fixed-point kernels.
 */
#include "ARMCM55.h"
#include "dsp/matrix_functions.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(ARM_MATH_MVEI) || defined(ARM_MATH_AUTOVECTORIZE)
#error This regression requires the handwritten MVE integer kernels.
#endif

#define MAX_ROWS 7U
#define MAX_COLS 17U
#define GUARD_BYTES 32U

/* Datatypes under test, selectable per build so that a negative control can
 * link one unmodified upstream kernel while the others are absent. */
#ifndef MATVEC_TEST_Q7
#define MATVEC_TEST_Q7 1
#endif
#ifndef MATVEC_TEST_Q15
#define MATVEC_TEST_Q15 1
#endif
#ifndef MATVEC_TEST_Q31
#define MATVEC_TEST_Q31 1
#endif

extern void regression_console_init(void);
static unsigned char arena[512 + GUARD_BYTES] __ALIGNED(32);
static volatile unsigned current_bits, current_rows, current_cols;

void MemManage_Handler(void)
{
    ARM_MPU_Disable();
    printf("FAIL: Q%u matrix tail access rows=%u cols=%u\n",
           current_bits, current_rows, current_cols);
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

/* Each case runs the kernel twice: on a matrix followed by readable nonzero
 * padding, and on the same matrix ending at the guard. Outputs must be
 * identical, and within one LSB of a scalar accumulation (the MVE and scalar
 * kernels differ in rounding for some datatypes).
 */
#define DEFINE_CASES(T, BITS, SHIFT, SCALE)                                    \
static T##_t padded_##T[MAX_ROWS * MAX_COLS + 16];                                 \
static T##_t vector_##T[MAX_COLS + 16];                                            \
static T##_t plain_##T[MAX_ROWS + 2], guarded_##T[MAX_ROWS + 2];                   \
static int run_##T(unsigned *checks)                                           \
{                                                                              \
    T##_t *end = (T##_t *)(arena + sizeof(arena) - GUARD_BYTES);                       \
    current_bits = BITS;                                                       \
    for (unsigned rows = 1; rows <= MAX_ROWS; ++rows)                          \
    {                                                                          \
        for (unsigned cols = 1; cols <= MAX_COLS; ++cols)                      \
        {                                                                      \
            T##_t *matrix = end - rows * cols;                                     \
            for (unsigned i = 0; i < sizeof(padded_##T) / sizeof(T##_t); ++i)      \
                padded_##T[i] = (T##_t)0x55;                                          \
            for (unsigned i = 0; i < rows * cols; ++i)                         \
                padded_##T[i] = matrix[i] = (T##_t)(((int)(i % 7) - 3) * (SCALE)); \
            for (unsigned i = 0; i < MAX_COLS + 16; ++i)                       \
                vector_##T[i] = (T##_t)(((int)(i % 5) - 2) * (SCALE));             \
            for (unsigned i = 0; i < MAX_ROWS + 2; ++i)                        \
                plain_##T[i] = guarded_##T[i] = (T##_t)0x2a;                       \
            arm_matrix_instance_##T plain = {rows, cols, padded_##T};          \
            arm_matrix_instance_##T guarded = {rows, cols, matrix};            \
            current_rows = rows;                                               \
            current_cols = cols;                                               \
            arm_mat_vec_mult_##T(&plain, vector_##T, plain_##T + 1);           \
            protect_tail((uintptr_t)end);                                      \
            arm_mat_vec_mult_##T(&guarded, vector_##T, guarded_##T + 1);       \
            ARM_MPU_Disable();                                                 \
            if (guarded_##T[0] != (T##_t)0x2a || guarded_##T[rows + 1] != (T##_t)0x2a) \
            {                                                                  \
                printf("FAIL: Q%u output canary rows=%u cols=%u\n",            \
                       (unsigned)BITS, rows, cols);                            \
                return 1;                                                      \
            }                                                                  \
            if (memcmp(plain_##T, guarded_##T, sizeof(plain_##T)))             \
            {                                                                  \
                printf("FAIL: Q%u placement mismatch rows=%u cols=%u\n",       \
                       (unsigned)BITS, rows, cols);                            \
                return 1;                                                      \
            }                                                                  \
            for (unsigned r = 0; r < rows; ++r)                                \
            {                                                                  \
                long long reference = 0;                                       \
                for (unsigned c = 0; c < cols; ++c)                            \
                    reference += (long long)matrix[r * cols + c] * vector_##T[c]; \
                long long error = (long long)guarded_##T[r + 1] - (reference >> (SHIFT)); \
                if (error < -1 || error > 1)                                   \
                {                                                              \
                    printf("FAIL: Q%u numerical result rows=%u cols=%u\n",     \
                           (unsigned)BITS, rows, cols);                        \
                    return 1;                                                  \
                }                                                              \
            }                                                                  \
            ++*checks;                                                         \
        }                                                                      \
    }                                                                          \
    return 0;                                                                  \
}

#if MATVEC_TEST_Q7
DEFINE_CASES(q7, 7, 7, 9)
#endif
#if MATVEC_TEST_Q15
DEFINE_CASES(q15, 15, 15, 2311)
#endif
#if MATVEC_TEST_Q31
DEFINE_CASES(q31, 31, 31, 151500000)
#endif

int main(void)
{
    regression_console_init();
    unsigned checks = 0;
#if MATVEC_TEST_Q7
    if (run_q7(&checks)) return 1;
#endif
#if MATVEC_TEST_Q15
    if (run_q15(&checks)) return 1;
#endif
#if MATVEC_TEST_Q31
    if (run_q31(&checks)) return 1;
#endif
    printf("PASS: %u MVE fixed-point matrix-tail cases\n", checks);
    return 0;
}
