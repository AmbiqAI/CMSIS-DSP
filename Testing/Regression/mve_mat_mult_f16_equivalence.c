/* Copyright (c) 2026 Ambiq Micro, Inc.
 * SPDX-License-Identifier: Apache-2.0
 */
/* Cortex-M55 equivalence regression for arm_mat_mult_f16: the kernel of this
 * checkout and a reference copy of the kernel (another revision of
 * arm_mat_mult_f16.c compiled with -Darm_mat_mult_f16=arm_mat_mult_f16_ref)
 * run on the same pseudo-random data; their outputs must be identical bit
 * for bit. Square sizes 1 to 9 cover the special cases (2x2, 3x3, 4x4) and
 * their neighbours; rectangular products cover the general path.
 */
#include "ARMCM55.h"
#include "dsp/matrix_functions_f16.h"
#include <stdio.h>
#include <string.h>

#if !defined(ARM_MATH_MVE_FLOAT16) || defined(ARM_MATH_AUTOVECTORIZE)
#error This regression requires the handwritten MVE float16 kernel.
#endif

arm_status arm_mat_mult_f16_ref(const arm_matrix_instance_f16 *pSrcA, const arm_matrix_instance_f16 *pSrcB,
                                arm_matrix_instance_f16 *pDst);

#define MAX_DIM 9U
#define DRAWS 16U

extern void regression_console_init(void);
static float16_t a[MAX_DIM * MAX_DIM], b[MAX_DIM * MAX_DIM];
static float16_t out_a[MAX_DIM * MAX_DIM + 8U], out_b[MAX_DIM * MAX_DIM + 8U];

static uint32_t lcg = 0x2468ACE1U;
static float16_t next_value(void)
{
    lcg = lcg * 1664525U + 1013904223U;
    return (float16_t)((float32_t)((int32_t)lcg >> 1) / 2147483648.0f);
}

static int check(unsigned ra, unsigned ca, unsigned cb)
{
    for (unsigned d = 0; d < DRAWS; ++d)
    {
        for (unsigned i = 0; i < ra * ca; ++i)
            a[i] = next_value();
        for (unsigned i = 0; i < ca * cb; ++i)
            b[i] = next_value();
        memset(out_a, 0x5a, sizeof(out_a));
        memset(out_b, 0x5a, sizeof(out_b));
        arm_matrix_instance_f16 ma = {ra, ca, a}, mb = {ca, cb, b};
        arm_matrix_instance_f16 da = {ra, cb, out_a}, db = {ra, cb, out_b};
        arm_status sa = arm_mat_mult_f16(&ma, &mb, &da);
        arm_status sb = arm_mat_mult_f16_ref(&ma, &mb, &db);
        if (sa != sb || memcmp(out_a, out_b, sizeof(out_a)))
        {
            printf("FAIL: matrix product differs from the reference %ux%u by %ux%u\n", ra, ca, ca, cb);
            return 1;
        }
    }
    return 0;
}

int main(void)
{
    regression_console_init();
    unsigned checks = 0;
    for (unsigned n = 1; n <= MAX_DIM; ++n, ++checks)
        if (check(n, n, n)) return 1;
    for (unsigned ra = 1; ra <= MAX_DIM; ++ra)
        for (unsigned ca = 1; ca <= MAX_DIM; ca += 2)
            for (unsigned cb = 1; cb <= MAX_DIM; cb += 3, ++checks)
                if (check(ra, ca, cb)) return 1;
    printf("PASS: %u MVE f16 matrix-product equivalence shapes\n", checks);
    return 0;
}
