/* Copyright (c) 2026 Ambiq Micro, Inc.
 * SPDX-License-Identifier: Apache-2.0
 */
/* Cortex-M55 equivalence regression for arm_mat_vec_mult_f32: the kernel of
 * this checkout and a reference copy of the kernel (another revision of
 * arm_mat_vec_mult_f32.c compiled with -Darm_mat_vec_mult_f32=
 * arm_mat_vec_mult_f32_ref) run on the same pseudo-random data; their outputs
 * must be identical bit for bit. Used for changes meant to preserve output.
 */
#include "ARMCM55.h"
#include "dsp/matrix_functions.h"
#include <stdio.h>
#include <string.h>

void arm_mat_vec_mult_f32_ref(const arm_matrix_instance_f32 *pSrcMat, const float32_t *pVec, float32_t *pDst);

#define MAX_ROWS 13U
#define MAX_COLS 40U

extern void regression_console_init(void);
static float32_t matrix[MAX_ROWS * MAX_COLS], vector[MAX_COLS];
static float32_t out_a[MAX_ROWS], out_b[MAX_ROWS];

static uint32_t lcg = 0x2468ACE1U;
static float32_t next_value(void)
{
    lcg = lcg * 1664525U + 1013904223U;
    return (float32_t)((int32_t)lcg >> 1) / 2147483648.0f;
}

int main(void)
{
    regression_console_init();
    unsigned checks = 0;
    for (unsigned rows = 1; rows <= MAX_ROWS; ++rows)
    for (unsigned cols = 1; cols <= MAX_COLS; ++cols)
    for (unsigned rep = 0; rep < 4; ++rep)
    {
        for (unsigned i = 0; i < rows * cols; ++i)
            matrix[i] = next_value();
        for (unsigned i = 0; i < cols; ++i)
            vector[i] = next_value();
        arm_matrix_instance_f32 m = {rows, cols, matrix};
        arm_mat_vec_mult_f32(&m, vector, out_a);
        arm_mat_vec_mult_f32_ref(&m, vector, out_b);
        if (memcmp(out_a, out_b, rows * sizeof(float32_t)))
        {
            printf("FAIL: matrix-vector output differs from the reference rows=%u cols=%u\n", rows, cols);
            return 1;
        }
        ++checks;
    }
    printf("PASS: %u MVE f32 matrix-vector equivalence cases\n", checks);
    return 0;
}
