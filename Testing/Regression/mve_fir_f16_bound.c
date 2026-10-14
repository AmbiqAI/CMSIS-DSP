/* Copyright (c) 2026 Ambiq Micro, Inc.
 * SPDX-License-Identifier: Apache-2.0
 */
/* Cortex-M55 regression: the handwritten MVE arm_fir_f16 kernel must read and
 * write only within Arm's documented Helium state length
 * numTaps + 8 * ceil(blockSize / 8) + blockSize - 1 and within the blockSize
 * input samples, and its outputs must not depend on the memory around them.
 *
 * For 1..MAX_TAPS taps and the block sizes in blocks[], each case filters two
 * consecutive blocks
 *   - with the state sized exactly to the documented length and ending at an
 *     MPU-inaccessible guard, and each input block ending at a second guard;
 *     a MemManage fault is recovered and counted as a failure;
 *   - unguarded, three times, with the memory before and after the state and
 *     the input filled with zero, +Inf and quiet NaN.
 * The unguarded outputs must be finite, identical bit for bit across the
 * three paddings and close to a double-precision reference; the guarded run,
 * when it completes, must give the same outputs. Each check is counted
 * separately.
 */
#include "ARMCM55.h"
#include "dsp/filtering_functions_f16.h"
#include <math.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>

#include "regression_checks.h"

#if !defined(ARM_MATH_MVE_FLOAT16) || defined(ARM_MATH_AUTOVECTORIZE)
#error This regression requires the handwritten MVE f16 kernels.
#endif

#define MAX_TAPS 40U
static const unsigned blocks[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16,
                                  17, 23, 24, 25, 31, 32, 33, 40};
#define NBLOCKS (sizeof(blocks) / sizeof(blocks[0]))
#define MAX_BLOCK 40U
#define DOCUMENTED(numTaps, blockSize) ((numTaps) + 8U * (((blockSize) + 7U) / 8U) + (blockSize) - 1U)
#define STATE_ELEMS 128U   /* >= DOCUMENTED(MAX_TAPS, MAX_BLOCK) = 119, multiple of 16 */
#define INPUT_ELEMS 64U
#define PAD 64U
#define GUARD_BYTES REGRESSION_GUARD_BYTES(STATE_ELEMS * sizeof(float16_t))

extern void regression_console_init(void);

/* Guarded placement: each buffer ends at its guard. */
static float16_t state_arena[STATE_ELEMS + GUARD_BYTES / sizeof(float16_t)] __ALIGNED(32);
static float16_t input_arena[INPUT_ELEMS + GUARD_BYTES / sizeof(float16_t)] __ALIGNED(32);
#define STATE_GUARD ((uintptr_t)(state_arena + STATE_ELEMS))
#define INPUT_GUARD ((uintptr_t)(input_arena + INPUT_ELEMS))
/* Unguarded placement with padding on both sides. */
static float16_t state_pad[PAD + STATE_ELEMS + PAD];
static float16_t input_pad[PAD + MAX_BLOCK + PAD];

static float16_t coeffs[MAX_TAPS + 8];
static float16_t signal[2U * MAX_BLOCK];
static float16_t out[4][2U * MAX_BLOCK];
static jmp_buf recover_env;
static volatile unsigned faulted;

/* The exception return restores VPR from the interrupted kernel; clear it
 * before unwinding to the case. */
static void recover(void)
{
    __asm volatile("vmsr vpr, %0" : : "r"(0U));
    longjmp(recover_env, 1);
}

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

static void protect_guards(void)
{
    uintptr_t lo = STATE_GUARD < INPUT_GUARD ? STATE_GUARD : INPUT_GUARD;
    uintptr_t hi = STATE_GUARD < INPUT_GUARD ? INPUT_GUARD : STATE_GUARD;
    ARM_MPU_Disable();
    for (uint32_t region = 0; region < ((MPU->TYPE >> 8) & 0xffU); ++region)
        ARM_MPU_ClrRegion(region);
    ARM_MPU_SetMemAttr(0, ARM_MPU_ATTR(ARM_MPU_ATTR_NON_CACHEABLE, ARM_MPU_ATTR_NON_CACHEABLE));
    ARM_MPU_SetRegion(0, ARM_MPU_RBAR(0, ARM_MPU_SH_NON, 0, 1, 0), ARM_MPU_RLAR(lo - 1U, 0));
    ARM_MPU_SetRegion(1, ARM_MPU_RBAR(lo + GUARD_BYTES, ARM_MPU_SH_NON, 0, 1, 0), ARM_MPU_RLAR(hi - 1U, 0));
    ARM_MPU_SetRegion(2, ARM_MPU_RBAR(hi + GUARD_BYTES, ARM_MPU_SH_NON, 0, 1, 0), ARM_MPU_RLAR(UINT32_MAX, 0));
    ARM_MPU_Enable(0);
}

static void fill_pad(float16_t *p, unsigned n, int kind)
{
    uint16_t w = kind == 0 ? 0x0000U : (kind == 1 ? 0x7c00U : 0x7e00U);
    for (unsigned i = 0; i < n; ++i)
        memcpy(&p[i], &w, sizeof(w));
}

/* Returns 1 on a fault. */
static int run_guarded(unsigned numTaps, unsigned blockSize)
{
    float16_t *state = (float16_t *)STATE_GUARD - DOCUMENTED(numTaps, blockSize);
    float16_t *input = (float16_t *)INPUT_GUARD - blockSize;
    arm_fir_instance_f16 S;
    faulted = 0;
    arm_fir_init_f16(&S, (uint16_t)numTaps, coeffs, state, blockSize);
    if (setjmp(recover_env) == 0)
    {
        for (unsigned blk = 0; blk < 2U; ++blk)
        {
            memcpy(input, &signal[blk * blockSize], blockSize * sizeof(float16_t));
            protect_guards();
            arm_fir_f16(&S, input, &out[0][blk * blockSize], blockSize);
            REGRESSION_CONTROL_READ_ABOVE(STATE_GUARD, GUARD_BYTES);
            ARM_MPU_Disable();
        }
    }
    ARM_MPU_Disable();
    return faulted != 0U;
}

static void run_padded(unsigned numTaps, unsigned blockSize, int kind)
{
    unsigned documented = DOCUMENTED(numTaps, blockSize);
    float16_t *state = state_pad + PAD;
    float16_t *input = input_pad + PAD;
    arm_fir_instance_f16 S;
    fill_pad(state_pad, PAD + STATE_ELEMS + PAD, kind);
    arm_fir_init_f16(&S, (uint16_t)numTaps, coeffs, state, blockSize);
    (void)documented;
    for (unsigned blk = 0; blk < 2U; ++blk)
    {
        fill_pad(input_pad, PAD + MAX_BLOCK + PAD, kind);
        memcpy(input, &signal[blk * blockSize], blockSize * sizeof(float16_t));
        arm_fir_f16(&S, input, &out[1 + kind][blk * blockSize], blockSize);
    }
}

int main(void)
{
    unsigned cases = 0, failures = 0, faults = 0, padding = 0, nonfinite = 0, numerical = 0, mismatch = 0;
    regression_console_init();
    SCB->SHCSR |= SCB_SHCSR_MEMFAULTENA_Msk;
    for (unsigned i = 0; i < 2U * MAX_BLOCK; ++i)
        signal[i] = (float16_t)(0.125f * (float)((int)((i * 7U) % 13U) - 6));
    for (unsigned numTaps = 1; numTaps <= MAX_TAPS; ++numTaps)
    {
        memset(coeffs, 0, sizeof(coeffs));
        for (unsigned i = 0; i < numTaps; ++i)
            coeffs[i] = (float16_t)(0.0625f * (float)((int)((i * 5U) % 9U) - 4));
        for (unsigned k = 0; k < NBLOCKS; ++k)
        {
            unsigned b = blocks[k];
            ++cases;
            int bad = 0, fault = 0, nf = 0;
            if (run_guarded(numTaps, b))
            {
                if (faults++ == 0U)
                    printf("first fault: numTaps=%u blockSize=%u\n", numTaps, b);
                bad = fault = 1;
            }
            for (int kind = 0; kind < 3; ++kind)
                run_padded(numTaps, b, kind);
            REGRESSION_CONTROL_NAN_F16(&out[1][0]);
            /* Unguarded runs: finite and independent of the padding. */
            for (int r = 1; r < 4 && !nf; ++r)
                for (unsigned i = 0; i < 2U * b; ++i)
                    if (regression_nonfinite_f16(&out[r][i]))
                    {
                        if (nonfinite++ == 0U)
                            printf("first nonfinite output: numTaps=%u blockSize=%u padding %d output %u\n", numTaps, b, r - 1, i);
                        bad = nf = 1;
                        break;
                    }
            if (memcmp(out[1], out[2], 2U * b * sizeof(float16_t)) ||
                memcmp(out[1], out[3], 2U * b * sizeof(float16_t)))
            {
                if (padding++ == 0U)
                    printf("first padding-dependent output: numTaps=%u blockSize=%u\n", numTaps, b);
                bad = 1;
            }
            /* Guarded run, when it completed: identical to the unguarded one. */
            if (!fault && memcmp(out[0], out[1], 2U * b * sizeof(float16_t)))
            {
                if (mismatch++ == 0U)
                    printf("first guarded/unguarded mismatch: numTaps=%u blockSize=%u\n", numTaps, b);
                bad = 1;
            }
            for (unsigned i = 0; i < 2U * b && !nf; ++i)
            {
                double r = 0.0, mag = 0.0;
                for (unsigned t = 0; t < numTaps && t <= i; ++t)
                {
                    double term = (double)coeffs[numTaps - 1U - t] * (double)signal[i - t];
                    r += term;
                    mag += fabs(term);
                }
                if (fabs((double)out[1][i] - r) > 4e-3 * mag + 1e-3)
                {
                    if (numerical++ == 0U)
                        printf("first numerical failure: numTaps=%u blockSize=%u output %u\n", numTaps, b, i);
                    bad = 1;
                    break;
                }
            }
            failures += (unsigned)bad;
        }
    }
    if (failures)
        printf("FAIL: %u of %u MVE FIR f16 bound cases (faults %u, nonfinite %u, padding-dependent %u, "
               "guarded mismatch %u, numerical %u)\n", failures, cases, faults, nonfinite, padding, mismatch, numerical);
    else
        printf("PASS: %u MVE FIR f16 bound cases\n", cases);
    return 0;
}
