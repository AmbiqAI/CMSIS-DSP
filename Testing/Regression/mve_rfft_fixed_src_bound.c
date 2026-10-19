/* Copyright (c) 2026 Ambiq Micro, Inc.
 * SPDX-License-Identifier: Apache-2.0
 */
/* arm_rfft_q15 and arm_rfft_q31, forward, with the N input samples ending at
 * an MPU-inaccessible guard.
 *
 * Arm documents the input of a forward RFFT as N samples (the output of the
 * q15 and q31 Helium transforms as N + 2). Each case runs the transform once
 * on a copy of the input followed by readable padding and once on a copy that
 * ends at the guard; the second run must not fault and must give the same
 * output bit for bit. Lengths 32 to 4096 (q15 and q31), and 8192 (q31).
 */
#include "ARMCM55.h"
#include "dsp/transform_functions.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "regression_checks.h"

#if !defined(ARM_MATH_MVEI) || defined(ARM_MATH_AUTOVECTORIZE)
#error This regression requires the handwritten MVE integer kernels.
#endif

#define MAXN 8192U
#define GUARD_BYTES REGRESSION_GUARD_BYTES(64U)

extern void regression_console_init(void);

static q31_t arena[MAXN + GUARD_BYTES / sizeof(q31_t)] __ALIGNED(32);
#define GUARD ((uintptr_t)(arena + MAXN))
static q31_t padded[MAXN + 64], ref[MAXN + 2 + 16], out[MAXN + 2 + 16];
static const char *current = "none";
static volatile unsigned current_n;
static unsigned cases;

void MemManage_Handler(void)
{
    ARM_MPU_Disable();
    printf("FAIL: %s N=%u read past the input\n", current, current_n);
    exit(1);
}

static void protect(void)
{
    ARM_MPU_Disable();
    for (uint32_t region = 0; region < ((MPU->TYPE >> 8) & 0xffU); ++region)
        ARM_MPU_ClrRegion(region);
    ARM_MPU_SetMemAttr(0, ARM_MPU_ATTR(ARM_MPU_ATTR_NON_CACHEABLE, ARM_MPU_ATTR_NON_CACHEABLE));
    ARM_MPU_SetRegion(0, ARM_MPU_RBAR(0, ARM_MPU_SH_NON, 0, 1, 0), ARM_MPU_RLAR(GUARD - 1U, 0));
    ARM_MPU_SetRegion(1, ARM_MPU_RBAR(GUARD + GUARD_BYTES, ARM_MPU_SH_NON, 0, 1, 0),
                      ARM_MPU_RLAR(UINT32_MAX, 0));
    ARM_MPU_Enable(0);
}

static uint32_t lcg = 99U;
static uint32_t rnd(void) { lcg = lcg * 1664525U + 1013904223U; return lcg; }

#define DEFINE_TYPE(TAG, T, MAXLEN)                                                        \
static int run_##TAG(void)                                                                 \
{                                                                                          \
    current = "arm_rfft_" #TAG;                                                            \
    for (uint32_t n = 32; n <= (MAXLEN); n *= 2)                                           \
    {                                                                                      \
        arm_rfft_instance_##TAG S;                                                         \
        current_n = n;                                                                     \
        if (arm_rfft_init_##TAG(&S, n, 0, 1) != ARM_MATH_SUCCESS)                          \
        {                                                                                  \
            printf("FAIL: %s N=%u init\n", current, (unsigned)n);                          \
            return 1;                                                                      \
        }                                                                                  \
        T *pad = (T *)padded, *in = (T *)GUARD - n, *r = (T *)ref, *o = (T *)out;          \
        for (uint32_t i = 0; i < n; ++i)                                                   \
            pad[i] = (T)(rnd() >> (8 * (4 - sizeof(T)) + 3));                              \
        for (uint32_t i = n; i < n + 32U; ++i)                                             \
            pad[i] = (T)0x5a5a;                                                            \
        memcpy(in, pad, n * sizeof(T));                                                    \
        memset(ref, 0, sizeof(ref));                                                       \
        memset(out, 0, sizeof(out));                                                       \
        arm_rfft_##TAG(&S, pad, r);                                                        \
        protect();                                                                         \
        arm_rfft_##TAG(&S, in, o);                                                         \
        ARM_MPU_Disable();                                                                 \
        if (memcmp(r, o, (n + 2U) * sizeof(T)))                                            \
        {                                                                                  \
            printf("FAIL: %s N=%u output differs from the padded run\n", current, (unsigned)n); \
            return 1;                                                                      \
        }                                                                                  \
        ++cases;                                                                           \
    }                                                                                      \
    return 0;                                                                              \
}
DEFINE_TYPE(q15, q15_t, 4096U)
DEFINE_TYPE(q31, q31_t, 8192U)

int main(void)
{
    regression_console_init();
    SCB->SHCSR |= SCB_SHCSR_MEMFAULTENA_Msk;
    if (run_q15() || run_q31())
        return 1;
    printf("PASS: %u MVE RFFT q15/q31 input-bound cases\n", cases);
    return 0;
}
