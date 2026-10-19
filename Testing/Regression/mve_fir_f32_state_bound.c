/* Copyright (c) 2026 Ambiq Micro, Inc.
 * SPDX-License-Identifier: Apache-2.0
 */
/* Cortex-M55 regression: the handwritten MVE arm_fir_f32 kernel must not
 * read or store past Arm's documented state length numTaps + 2 * blockSize - 1
 * (including through the zero-padded taps of the coefficient array), the
 * initializer must clear that whole length and write nothing past it, and the
 * kernel must not read past the blockSize input samples, for 1..MAX_TAPS taps
 * and 1..MAX_BLOCK samples per block. Each case also reports how many state
 * elements beyond the documented length were touched.
 *
 * The state buffer ends at an MPU-inaccessible guard, first with the
 * documented length and then with one extra element at a time until no
 * access faults; the input block ends at a second guard. A MemManage fault is recovered by rewriting the stacked
 * return address, so one run measures every case. Elements in the extra
 * region are canaries, so stores and loads past the documented length are
 * distinguished. Each guard covers the largest state (documented length plus
 * MAX_EXTRA) or input plus one vector access, so a read anywhere within that
 * distance of the buffer end faults; nonfinite outputs fail by their bits
 * (regression_checks.h).
 */
#include "ARMCM55.h"
#include "dsp/filtering_functions.h"
#include <math.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "regression_checks.h"

#if !defined(ARM_MATH_MVEF) || defined(ARM_MATH_AUTOVECTORIZE)
#error This measurement requires the handwritten MVE float kernels.
#endif

#ifndef MAX_TAPS
#define MAX_TAPS 20U
#endif
#ifndef MAX_BLOCK
#define MAX_BLOCK 12U
#endif
#define MAX_EXTRA 16U
#define STATE_SPAN ((MAX_TAPS + 2U * MAX_BLOCK - 1U + MAX_EXTRA) * sizeof(float32_t))
#define INPUT_SPAN (2U * MAX_BLOCK * sizeof(float32_t))
#define GUARD_BYTES REGRESSION_GUARD_BYTES(STATE_SPAN > INPUT_SPAN ? STATE_SPAN : INPUT_SPAN)
#define CANARY 12345.0f

extern void regression_console_init(void);

static float32_t arena[256 + GUARD_BYTES / sizeof(float32_t)] __ALIGNED(32);
static float32_t coeffs[MAX_TAPS + 4];
static float32_t input_arena[64 + GUARD_BYTES / sizeof(float32_t)] __ALIGNED(32);
static float32_t output[MAX_BLOCK + 2];
static jmp_buf recover_env;
static volatile unsigned faulted;

/* The exception return restores VPR from the interrupted kernel; a fault
 * inside a VPT block would otherwise predicate the vector instructions that
 * follow (for example a vectorized canary fill), so clear it. */
static void recover(void)
{
    __asm volatile("vmsr vpr, %0" : : "r"(0U));
    longjmp(recover_env, 1);
}

/* Rewrite the stacked PC so that the exception returns into recover(), which
 * unwinds to the current case. ICI/IT and ECI bits in the stacked xPSR are
 * cleared because the interrupted (partially executed) instruction is
 * abandoned. */
void memmanage_c(uint32_t *frame)
{
    ARM_MPU_Disable();
    SCB->CFSR = SCB->CFSR;
    faulted = 1;
    frame[6] = (uint32_t)recover & ~1U;
    frame[7] &= ~0x0600FC00U;
    frame[7] |= 1U << 24; /* Thumb state */
}

__attribute__((naked)) void MemManage_Handler(void)
{
    __asm volatile(
        "tst lr, #4\n"
        "ite eq\n"
        "mrseq r0, msp\n"
        "mrsne r0, psp\n"
        "b memmanage_c\n");
}

static void protect_tails(uintptr_t guard_a, uintptr_t guard_b)
{
    uintptr_t lo = guard_a < guard_b ? guard_a : guard_b;
    uintptr_t hi = guard_a < guard_b ? guard_b : guard_a;
    ARM_MPU_Disable();
    for (uint32_t region = 0; region < ((MPU->TYPE >> 8) & 0xffU); ++region)
    {
        ARM_MPU_ClrRegion(region);
    }
    ARM_MPU_SetMemAttr(0, ARM_MPU_ATTR(ARM_MPU_ATTR_NON_CACHEABLE,
                                     ARM_MPU_ATTR_NON_CACHEABLE));
    /* Map everything except the two aligned guards. No background map. */
    ARM_MPU_SetRegion(0, ARM_MPU_RBAR(0, ARM_MPU_SH_NON, 0, 1, 0),
                     ARM_MPU_RLAR(lo - 1U, 0));
    ARM_MPU_SetRegion(1, ARM_MPU_RBAR(lo + GUARD_BYTES, ARM_MPU_SH_NON, 0, 1, 0),
                     ARM_MPU_RLAR(hi - 1U, 0));
    ARM_MPU_SetRegion(2, ARM_MPU_RBAR(hi + GUARD_BYTES, ARM_MPU_SH_NON, 0, 1, 0),
                     ARM_MPU_RLAR(UINT32_MAX, 0));
    ARM_MPU_Enable(0);
}

/* Returns 0 on success, 1 on fault, 2 on canary/numerical/clearing failure. */
static int run_case(unsigned numTaps, unsigned blockSize, unsigned extra, int *stored)
{
    const unsigned documented = numTaps + 2U * blockSize - 1U;
    float32_t *end = arena + 256;
    float32_t *state = end - documented - extra;
    float32_t *input_end = input_arena + 64;
    float32_t *input = input_end - 2U * blockSize; /* two blocks, the second ends at the guard */
    arm_fir_instance_f32 S;

    for (unsigned i = 0; i < MAX_TAPS + 4; ++i)
        coeffs[i] = (i < numTaps) ? (float32_t)((int)(i % 5) - 2) * 0.25f : 0.0f;
    for (unsigned i = 0; i < 2U * blockSize; ++i)
        input[i] = (float32_t)((int)(i % 7) + 1) * 0.5f; /* never zero */
    /* The canaries continue for four elements into the guard, which the
     * initializer reaches with the MPU disabled. */
    for (unsigned i = 0; i < documented + extra + 4U; ++i)
        state[i] = CANARY;

    arm_fir_init_f32(&S, (uint16_t)numTaps, coeffs, state, blockSize);
    /* The initializer must clear the documented length and nothing past it. */
    for (unsigned i = 0; i < documented + extra + 4U; ++i)
    {
        if ((i < documented) != (state[i] == 0.0f))
        {
            printf("FAIL: initializer %s state[%u] numTaps=%u blockSize=%u\n",
                   i < documented ? "left uncleared" : "wrote past the documented length at",
                   i, numTaps, blockSize);
            return 2;
        }
    }

    faulted = 0;
    if (setjmp(recover_env) == 0)
    {
        protect_tails((uintptr_t)end, (uintptr_t)input_end);
        for (unsigned block = 0; block < 2; ++block)
        {
            for (unsigned i = 0; i < MAX_BLOCK + 2; ++i)
                output[i] = CANARY;
            arm_fir_f32(&S, input + block * blockSize, output + 1, blockSize);
            REGRESSION_CONTROL_READ_ABOVE(end, GUARD_BYTES);
            ARM_MPU_Disable();
            REGRESSION_CONTROL_NAN_F32(&output[1]);
            if (output[0] != CANARY || output[blockSize + 1] != CANARY)
            {
                printf("FAIL: output canary numTaps=%u blockSize=%u\n", numTaps, blockSize);
                return 2;
            }
            for (unsigned n = 0; n < blockSize; ++n)
            {
                double reference = 0.0;
                int t = (int)(block * blockSize + n);
                for (unsigned k = 0; k < numTaps; ++k)
                {
                    int idx = t - (int)k;
                    if (idx >= 0)
                        reference += (double)coeffs[numTaps - 1U - k] * (double)input[idx];
                }
                if (regression_nonfinite_f32(&output[n + 1]) ||
                    fabs((double)output[n + 1] - reference) > 1e-5)
                {
                    printf("FAIL: numerical result numTaps=%u blockSize=%u extra=%u n=%u\n",
                           numTaps, blockSize, extra, n);
                    return 2;
                }
            }
            protect_tails((uintptr_t)end, (uintptr_t)input_end);
        }
        ARM_MPU_Disable();
    }
    ARM_MPU_Disable();
    if (faulted)
        return 1;
    /* Elements past the documented length hold the canary; any stored value
     * differs from it. */
    *stored = 0;
    for (unsigned i = documented; i < documented + extra; ++i)
        if (state[i] != CANARY)
            *stored = 1;
    return 0;
}

int main(void)
{
    regression_console_init();
    SCB->SHCSR |= SCB_SHCSR_MEMFAULTENA_Msk;
    unsigned cases = 0, violations = 0;
    printf("numTaps blockSize documented extra_needed stores_past_documented\n");
    for (unsigned numTaps = 1; numTaps <= MAX_TAPS; ++numTaps)
    {
        for (unsigned blockSize = 1; blockSize <= MAX_BLOCK; ++blockSize)
        {
            unsigned extra;
            int stored = 0, result = 1;
            for (extra = 0; extra <= MAX_EXTRA && result == 1; ++extra)
                result = run_case(numTaps, blockSize, extra, &stored);
            if (result == 2)
                return 1;
            if (result == 1)
            {
                printf("FAIL: numTaps=%u blockSize=%u still faults with %u extra elements\n",
                       numTaps, blockSize, MAX_EXTRA);
                return 1;
            }
            extra--; /* loop incremented past the passing value */
            printf("%u %u %u %u %s\n", numTaps, blockSize, numTaps + 2U * blockSize - 1U,
                   extra, stored ? "yes" : "no");
            if (stored || extra != 0U)
                ++violations;
            ++cases;
        }
    }
    if (violations)
    {
        printf("FAIL: %u of %u MVE FIR f32 state-bound cases exceed the contract\n",
               violations, cases);
        return 1;
    }
    printf("PASS: %u MVE FIR f32 state-bound cases\n", cases);
    return 0;
}
