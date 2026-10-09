/* Copyright (c) 2026 Ambiq Micro, Inc.
 * SPDX-License-Identifier: Apache-2.0
 */
/* Cortex-M55 equivalence regression for arm_fir_decimate_f32: the kernel of
 * this checkout and a reference copy of the kernel (another revision of
 * arm_fir_decimate_f32.c compiled with -Darm_fir_decimate_f32=
 * arm_fir_decimate_f32_ref) run on the same pseudo-random data; their outputs
 * must be identical bit for bit. Used for changes meant to preserve output.
 */
#include "ARMCM55.h"
#include "dsp/filtering_functions.h"
#include <stdio.h>
#include <string.h>

void arm_fir_decimate_f32_ref(const arm_fir_decimate_instance_f32 *S, const float32_t *pSrc,
                              float32_t *pDst, uint32_t blockSize);

#define MAX_TAPS 40U
#define MAX_M 8U
#define MAX_OUT 12U
#define CALLS 2U
#define MAX_BLOCK (MAX_M * MAX_OUT)

extern void regression_console_init(void);
static float32_t coeffs[MAX_TAPS], input[CALLS * MAX_BLOCK];
static float32_t state_a[MAX_TAPS + MAX_BLOCK - 1U], state_b[MAX_TAPS + MAX_BLOCK - 1U];
static float32_t out_a[CALLS * MAX_OUT], out_b[CALLS * MAX_OUT];

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
    for (unsigned taps = 1; taps <= MAX_TAPS; ++taps)
    for (unsigned m = 1; m <= MAX_M; ++m)
    for (unsigned nout = 1; nout <= MAX_OUT; ++nout)
    {
        unsigned block = m * nout;
        for (unsigned i = 0; i < taps; ++i)
            coeffs[i] = next_value();
        for (unsigned i = 0; i < CALLS * block; ++i)
            input[i] = next_value();
        arm_fir_decimate_instance_f32 sa, sb;
        if (arm_fir_decimate_init_f32(&sa, (uint16_t)taps, (uint8_t)m, coeffs, state_a, block) != ARM_MATH_SUCCESS ||
            arm_fir_decimate_init_f32(&sb, (uint16_t)taps, (uint8_t)m, coeffs, state_b, block) != ARM_MATH_SUCCESS)
        {
            printf("FAIL: decimator init taps=%u M=%u outputs=%u\n", taps, m, nout);
            return 1;
        }
        for (unsigned k = 0; k < CALLS; ++k)
        {
            arm_fir_decimate_f32(&sa, input + k * block, out_a + k * nout, block);
            arm_fir_decimate_f32_ref(&sb, input + k * block, out_b + k * nout, block);
        }
        if (memcmp(out_a, out_b, CALLS * nout * sizeof(float32_t)) ||
            memcmp(state_a, state_b, (taps - 1U) * sizeof(float32_t)))
        {
            printf("FAIL: decimator output differs from the reference taps=%u M=%u outputs=%u\n", taps, m, nout);
            return 1;
        }
        ++checks;
    }
    printf("PASS: %u MVE f32 decimator equivalence cases\n", checks);
    return 0;
}
