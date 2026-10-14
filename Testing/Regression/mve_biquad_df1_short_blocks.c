/* Copyright (c) 2026 Ambiq Micro, Inc.
 * SPDX-License-Identifier: Apache-2.0
 */
/* arm_biquad_cascade_df1_q15 and arm_biquad_cascade_df1_q31 at small block
 * sizes.
 *
 * A 64-sample signal is filtered once in a single call and again in blocks of
 * b samples, b = 1 to 17 (the last block shorter when b does not divide 64),
 * through 1 to 3 stages, with separate and with in-place buffers. The blocked
 * output and the final state must equal those of the single call bit for bit.
 * Each block's output ends at a read-only MPU region, so a store past the
 * output faults while the kernels' reads remain allowed. A call with
 * blockSize 0 must store nothing and leave the state unchanged.
 */
#include "ARMCM55.h"
#include "dsp/filtering_functions.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(ARM_MATH_MVEI) || defined(ARM_MATH_AUTOVECTORIZE)
#error This regression requires the handwritten MVE integer kernels.
#endif

#define LENGTH 64U
#define MAX_STAGES 3U
#define GUARD_BYTES 64U

extern void regression_console_init(void);

/* The output of a block ends at GUARD, the start of a read-only region. */
static unsigned char arena[512 + GUARD_BYTES + 64] __ALIGNED(32);
#define GUARD ((uintptr_t)arena + 512U)

static const char *current_name = "none";
static volatile unsigned current_block, current_stages, current_inplace;
static unsigned cases;

void MemManage_Handler(void)
{
    ARM_MPU_Disable();
    printf("FAIL: %s store past the output, blockSize=%u stages=%u in-place=%u\n",
           current_name, current_block, current_stages, current_inplace);
    exit(1);
}

static void protect_guard(void)
{
    ARM_MPU_Disable();
    for (uint32_t region = 0; region < ((MPU->TYPE >> 8) & 0xffU); ++region)
        ARM_MPU_ClrRegion(region);
    ARM_MPU_SetMemAttr(0, ARM_MPU_ATTR(ARM_MPU_ATTR_NON_CACHEABLE, ARM_MPU_ATTR_NON_CACHEABLE));
    ARM_MPU_SetRegion(0, ARM_MPU_RBAR(0, ARM_MPU_SH_NON, 0, 1, 0), ARM_MPU_RLAR(GUARD - 1U, 0));
    ARM_MPU_SetRegion(1, ARM_MPU_RBAR(GUARD, ARM_MPU_SH_NON, 1, 1, 0),
                      ARM_MPU_RLAR(GUARD + GUARD_BYTES - 1U, 0));
    ARM_MPU_SetRegion(2, ARM_MPU_RBAR(GUARD + GUARD_BYTES, ARM_MPU_SH_NON, 0, 1, 0),
                      ARM_MPU_RLAR(UINT32_MAX, 0));
    ARM_MPU_Enable(0);
}

/* Butterworth low-pass (fc = 0.1 fs), a second-order section with
 * b = {0.25, -0.5, 0.25}, a = {0.5, -0.25}, and a gain of 0.75, each as
 * {b0, b1, b2, a1, a2} with postShift 1. */
static const double sections[MAX_STAGES][5] = {
    {0.0675, 0.1349, 0.0675, 1.1430, -0.4128},
    {0.25, -0.5, 0.25, 0.5, -0.25},
    {0.75, 0.0, 0.0, 0.0, 0.0},
};

#define DEFINE_TYPE(TAG, T, COEFFS_PER_STAGE, SCALE, SET_COEFFS)                       \
static T coeffs_##TAG[COEFFS_PER_STAGE * MAX_STAGES];                                 \
static T state_##TAG[4 * MAX_STAGES], state_ref_##TAG[4 * MAX_STAGES];                \
static T input_##TAG[LENGTH], out_ref_##TAG[LENGTH], out_blocked_##TAG[LENGTH];        \
static T in_block_##TAG[LENGTH];                                                      \
static int run_##TAG(void)                                                            \
{                                                                                     \
    for (unsigned s = 0; s < MAX_STAGES; ++s)                                         \
    {                                                                                 \
        T *c = coeffs_##TAG + COEFFS_PER_STAGE * s;                                   \
        const double *d = sections[s];                                                \
        SET_COEFFS                                                                    \
    }                                                                                 \
    for (unsigned i = 0; i < LENGTH; ++i)                                             \
        input_##TAG[i] = (T)((double)(SCALE) * 0.45 *                                 \
                             ((double)((int)((i * 37U + 5U) % 23U) - 11) / 11.0));    \
    current_name = "arm_biquad_cascade_df1_" #TAG;                                    \
    for (unsigned stages = 1; stages <= MAX_STAGES; ++stages)                         \
        for (unsigned inplace = 0; inplace <= 1; ++inplace)                           \
            for (unsigned b = 0; b <= 17; ++b)                                        \
            {                                                                         \
                arm_biquad_casd_df1_inst_##TAG S;                                     \
                current_block = b;                                                    \
                current_stages = stages;                                              \
                current_inplace = inplace;                                            \
                memset(state_ref_##TAG, 0, sizeof(state_ref_##TAG));                  \
                arm_biquad_cascade_df1_init_##TAG(&S, (uint8_t)stages, coeffs_##TAG,  \
                                                  state_ref_##TAG, 1);                \
                arm_biquad_cascade_df1_##TAG(&S, input_##TAG, out_ref_##TAG, LENGTH); \
                arm_biquad_cascade_df1_init_##TAG(&S, (uint8_t)stages, coeffs_##TAG,  \
                                                  state_##TAG, 1);                    \
                if (b == 0U)                                                          \
                {                                                                     \
                    /* State from a full pass; blockSize 0 must keep it. */            \
                    memcpy(state_##TAG, state_ref_##TAG, sizeof(state_##TAG));         \
                    T *dst = (T *)GUARD;                                              \
                    protect_guard();                                                  \
                    arm_biquad_cascade_df1_##TAG(&S, inplace ? dst : input_##TAG, dst, 0); \
                    ARM_MPU_Disable();                                                \
                    if (memcmp(state_##TAG, state_ref_##TAG, sizeof(state_##TAG)))     \
                    {                                                                 \
                        printf("FAIL: %s blockSize 0 changed the state, stages=%u\n",  \
                               current_name, stages);                                 \
                        return 1;                                                     \
                    }                                                                 \
                    ++cases;                                                          \
                    continue;                                                         \
                }                                                                     \
                for (unsigned start = 0; start < LENGTH; start += b)                  \
                {                                                                     \
                    unsigned len = LENGTH - start < b ? LENGTH - start : b;           \
                    T *dst = (T *)GUARD - len;                                        \
                    const T *src = input_##TAG + start;                               \
                    if (inplace)                                                      \
                    {                                                                 \
                        memcpy(dst, src, len * sizeof(T));                            \
                        src = dst;                                                    \
                    }                                                                 \
                    else                                                              \
                    {                                                                 \
                        memcpy(in_block_##TAG, src, len * sizeof(T));                 \
                        src = in_block_##TAG;                                         \
                    }                                                                 \
                    protect_guard();                                                  \
                    arm_biquad_cascade_df1_##TAG(&S, src, dst, len);                  \
                    ARM_MPU_Disable();                                                \
                    memcpy(out_blocked_##TAG + start, dst, len * sizeof(T));          \
                }                                                                     \
                if (memcmp(out_blocked_##TAG, out_ref_##TAG, sizeof(out_ref_##TAG)) ||  \
                    memcmp(state_##TAG, state_ref_##TAG, 4U * stages * sizeof(T)))     \
                {                                                                     \
                    printf("FAIL: %s blocks of %u differ from one call, stages=%u in-place=%u\n", \
                           current_name, b, stages, inplace);                         \
                    return 1;                                                         \
                }                                                                     \
                ++cases;                                                              \
            }                                                                         \
    return 0;                                                                         \
}

/* q15 layout {b0, 0, b1, b2, a1, a2}, q31 layout {b0, b1, b2, a1, a2}; with
 * postShift 1 each coefficient is stored halved. */
DEFINE_TYPE(q15, q15_t, 6, 32767.0,
            c[0] = (q15_t)(d[0] * 16384.0); c[1] = 0; c[2] = (q15_t)(d[1] * 16384.0);
            c[3] = (q15_t)(d[2] * 16384.0); c[4] = (q15_t)(d[3] * 16384.0);
            c[5] = (q15_t)(d[4] * 16384.0);)
DEFINE_TYPE(q31, q31_t, 5, 2147483647.0,
            for (unsigned k = 0; k < 5; ++k) c[k] = (q31_t)(d[k] * 1073741824.0);)

int main(void)
{
    regression_console_init();
    SCB->SHCSR |= SCB_SHCSR_MEMFAULTENA_Msk;
    if (run_q15() || run_q31())
        return 1;
    printf("PASS: %u MVE biquad df1 short-block cases\n", cases);
    return 0;
}
