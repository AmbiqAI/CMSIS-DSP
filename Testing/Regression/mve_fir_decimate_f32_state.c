/* Copyright (c) 2026 Ambiq Micro, Inc.
 * SPDX-License-Identifier: Apache-2.0
 */
/* Cortex-M55 regression: the f32 FIR decimator's state buffer, of the
 * documented length numTaps + blockSize - 1, ends at an inaccessible MPU
 * region. Covers the tap remainder of both output loops and the final state
 * copy.
 */
#include "ARMCM55.h"
#include "dsp/filtering_functions.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(ARM_MATH_MVEF) || defined(ARM_MATH_AUTOVECTORIZE)
#error This regression requires the handwritten MVE floating-point kernel.
#endif

#define MAX_TAPS 20U
#define MAX_M 5U
#define MAX_OUT 9U
#define CALLS 2U
#define MAX_BLOCK (MAX_M * MAX_OUT)
#define STATE_LEN (MAX_TAPS + MAX_BLOCK - 1U)
#define GUARD_BYTES 32U
#define F32_INF 0x7f800000U

extern void regression_console_init(void);
static float32_t arena[STATE_LEN + 8U + GUARD_BYTES / 4U] __ALIGNED(32);
static float32_t padded[STATE_LEN + 8U];
static float32_t coeffs[MAX_TAPS];
static float32_t input[CALLS * MAX_BLOCK];
static float32_t plain[CALLS * MAX_OUT + 2U], guarded[CALLS * MAX_OUT + 2U];
static volatile unsigned current_taps, current_m, current_out;

void MemManage_Handler(void)
{
    ARM_MPU_Disable();
    printf("FAIL: decimator state access taps=%u M=%u outputs=%u\n", current_taps, current_m, current_out);
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

/* Bit pattern with the two zeros identified: under -ffast-math the reference
 * sum may come out as -0.0 where the kernel gives +0.0. */
static uint32_t bits_of(const float32_t *p)
{
    uint32_t bits;
    memcpy(&bits, p, sizeof(bits));
    return bits == 0x80000000U ? 0U : bits;
}

/* Each case runs two consecutive blocks twice: with a state buffer followed by
 * readable padding holding +Inf, and with the state buffer ending at the
 * guard. Inputs are multiples of 1/8 and coefficients multiples of 1/4 with
 * magnitude at most 1, so every partial sum is exact in f32; both outputs must
 * equal the double-precision reference bit for bit. A read of the padding
 * multiplies Inf by a zeroed coefficient lane and gives NaN.
 */
int main(void)
{
    regression_console_init();
    float32_t *end = (float32_t *)((unsigned char *)arena + sizeof(arena) - GUARD_BYTES);
    unsigned checks = 0;
    for (unsigned taps = 1; taps <= MAX_TAPS; ++taps)
    {
        for (unsigned m = 1; m <= MAX_M; ++m)
        {
            for (unsigned nout = 1; nout <= MAX_OUT; ++nout)
            {
                unsigned block = m * nout, state_len = taps + block - 1U;
                float32_t *state = end - state_len;
                for (unsigned i = 0; i < taps; ++i)
                    coeffs[i] = (float32_t)((int)(i % 9) - 4) * 0.25f;
                for (unsigned i = 0; i < CALLS * block; ++i)
                    input[i] = (float32_t)((int)((i * 5U) % 11U) - 5) * 0.125f;
                for (unsigned i = 0; i < CALLS * MAX_OUT + 2U; ++i)
                    plain[i] = guarded[i] = 42.0f;
                uint32_t inf = F32_INF;
                for (unsigned i = 0; i < STATE_LEN + 8U; ++i)
                    memcpy(&padded[i], &inf, sizeof(inf));
                current_taps = taps;
                current_m = m;
                current_out = nout;

                arm_fir_decimate_instance_f32 sp, sg;
                if (arm_fir_decimate_init_f32(&sp, (uint16_t)taps, (uint8_t)m, coeffs, padded, block) != ARM_MATH_SUCCESS ||
                    arm_fir_decimate_init_f32(&sg, (uint16_t)taps, (uint8_t)m, coeffs, state, block) != ARM_MATH_SUCCESS)
                {
                    printf("FAIL: decimator init taps=%u M=%u outputs=%u\n", taps, m, nout);
                    return 1;
                }
                for (unsigned k = 0; k < CALLS; ++k)
                    arm_fir_decimate_f32(&sp, input + k * block, plain + 1 + k * nout, block);
                protect_tail((uintptr_t)end);
                for (unsigned k = 0; k < CALLS; ++k)
                    arm_fir_decimate_f32(&sg, input + k * block, guarded + 1 + k * nout, block);
                ARM_MPU_Disable();

                if (guarded[0] != 42.0f || guarded[1 + CALLS * nout] != 42.0f)
                {
                    printf("FAIL: decimator output canary taps=%u M=%u outputs=%u\n", taps, m, nout);
                    return 1;
                }
                for (unsigned n = 0; n < CALLS * nout; ++n)
                {
                    /* y[n] = sum_k b[k] x[nM - k], with b stored time-reversed and x[<0] = 0 */
                    double reference = 0.0;
                    for (unsigned k = 0; k < taps; ++k)
                    {
                        int idx = (int)(n * m) - (int)(taps - 1U) + (int)k;
                        if (idx >= 0)
                            reference += (double)coeffs[k] * (double)input[idx];
                    }
                    float32_t expected = (float32_t)reference;
                    if (bits_of(&plain[n + 1]) != bits_of(&expected))
                    {
                        printf("FAIL: decimator padded-state result taps=%u M=%u outputs=%u\n", taps, m, nout);
                        return 1;
                    }
                    if (bits_of(&guarded[n + 1]) != bits_of(&expected))
                    {
                        printf("FAIL: decimator numerical result taps=%u M=%u outputs=%u\n", taps, m, nout);
                        return 1;
                    }
                }
                ++checks;
            }
        }
    }
    printf("PASS: %u MVE f32 decimator state cases\n", checks);
    return 0;
}
