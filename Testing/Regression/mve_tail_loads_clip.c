/* Copyright (c) 2026 Ambiq Micro, Inc.
 * SPDX-License-Identifier: Apache-2.0
 */
/* Cortex-M55 regression: arm_clip_q7/q15/q31/f32/f16 with the source ending
 * at an MPU-inaccessible region. The kernels load one vector ahead; with the
 * look-ahead loads under the tail predicate, a block of n elements reads
 * exactly n elements of the source. Block sizes 1 to three full vectors and
 * 64, 100, 255 and 257 elements. Each case runs on a source followed by
 * readable nonzero padding and on the same source ending at the guard; the
 * outputs must be identical, equal bit for bit to a reference clip, and
 * leave the canaries around the output in place.
 */
#include "ARMCM55.h"
#include "dsp/basic_math_functions.h"
#include "dsp/basic_math_functions_f16.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "regression_checks.h"

#if !defined(ARM_MATH_MVEI) || !defined(ARM_MATH_MVEF) || defined(ARM_MATH_AUTOVECTORIZE)
#error This regression requires the handwritten MVE kernels.
#endif

#define BLOCK_BYTES 1056U
/* The guard covers the whole source block plus one vector access, so a read
 * anywhere within one block of the source end faults (regression_checks.h). */
#define GUARD_BYTES REGRESSION_GUARD_BYTES(BLOCK_BYTES)
#define PAD_BYTES 64U
#define MAX_N 257U

extern void regression_console_init(void);

static unsigned char arena[BLOCK_BYTES + GUARD_BYTES] __ALIGNED(32);
#define GUARD ((uintptr_t)arena + BLOCK_BYTES)
static unsigned char padded[BLOCK_BYTES + PAD_BYTES] __ALIGNED(16);
static unsigned char out_plain[(MAX_N + 2U) * 4U] __ALIGNED(16);
static unsigned char out_guarded[(MAX_N + 2U) * 4U] __ALIGNED(16);

static const char *current_name = "none";
static volatile unsigned current_n;
static unsigned cases;

void MemManage_Handler(void)
{
    ARM_MPU_Disable();
    printf("FAIL: %s tail access n=%u\n", current_name, current_n);
    exit(1);
}

static void protect_guard(void)
{
    ARM_MPU_Disable();
    for (uint32_t region = 0; region < ((MPU->TYPE >> 8) & 0xffU); ++region)
        ARM_MPU_ClrRegion(region);
    ARM_MPU_SetMemAttr(0, ARM_MPU_ATTR(ARM_MPU_ATTR_NON_CACHEABLE, ARM_MPU_ATTR_NON_CACHEABLE));
    /* Map everything except the aligned guard. No background map. */
    ARM_MPU_SetRegion(0, ARM_MPU_RBAR(0, ARM_MPU_SH_NON, 0, 1, 0), ARM_MPU_RLAR(GUARD - 1U, 0));
    ARM_MPU_SetRegion(1, ARM_MPU_RBAR(GUARD + GUARD_BYTES, ARM_MPU_SH_NON, 0, 1, 0),
                      ARM_MPU_RLAR(UINT32_MAX, 0));
    ARM_MPU_Enable(0);
}

/* Ends a guarded call; a read control (regression_checks.h) reads past the
 * source before the guard is lifted. */
static void unguard(void)
{
    REGRESSION_CONTROL_READ_ABOVE(GUARD, GUARD_BYTES);
    ARM_MPU_Disable();
}

/* T: element type; LANES: elements per vector; RANGE: source magnitude;
 * NAN_CONTROL: the nan control for float types (no-op for fixed point). */
#define DEFINE_TYPE(TAG, T, LANES, RANGE, NAN_CONTROL)                                   \
static int run_##TAG(void)                                                              \
{                                                                                       \
    static const unsigned extra[] = {64, 100, 255, 257};                                \
    const T low = (T)(-0.6 * (RANGE)), high = (T)(0.5 * (RANGE));                       \
    const unsigned count = 3U * (LANES) + 4U;                                           \
    for (unsigned c = 0; c < count; ++c)                                                \
    {                                                                                   \
        unsigned n = c < 3U * (LANES) ? c + 1U : extra[c - 3U * (LANES)];               \
        T *src = (T *)GUARD - n, *pad = (T *)padded;                                    \
        T *dp = (T *)out_plain + 1, *dg = (T *)out_guarded + 1;                         \
        memset(padded, 0x55, sizeof(padded));                                           \
        for (unsigned i = 0; i < n; ++i)                                                \
            src[i] = pad[i] = (T)((double)(RANGE) * ((double)((int)((i * 37U + 5U) % 41U) - 20) / 20.0)); \
        memset(out_plain, 0x2a, sizeof(out_plain));                                     \
        memset(out_guarded, 0x2a, sizeof(out_guarded));                                 \
        current_name = "arm_clip_" #TAG;                                                \
        current_n = n;                                                                  \
        arm_clip_##TAG(pad, dp, low, high, n);                                          \
        protect_guard();                                                                \
        arm_clip_##TAG(src, dg, low, high, n);                                          \
        unguard();                                                                      \
        NAN_CONTROL(dg);                                                                \
        if (memcmp(out_plain, out_guarded, (n + 2U) * sizeof(T)))                       \
        {                                                                               \
            printf("FAIL: %s placement mismatch n=%u\n", current_name, n);              \
            return 1;                                                                   \
        }                                                                               \
        for (unsigned i = 0; i < sizeof(T); ++i)                                        \
            if (out_guarded[i] != 0x2a || out_guarded[(n + 1U) * sizeof(T) + i] != 0x2a) \
            {                                                                           \
                printf("FAIL: %s output canary n=%u\n", current_name, n);               \
                return 1;                                                               \
            }                                                                           \
        for (unsigned i = 0; i < n; ++i)                                                \
        {                                                                               \
            T r = src[i] < low ? low : (src[i] > high ? high : src[i]);                 \
            if (memcmp(&dg[i], &r, sizeof(T)))                                          \
            {                                                                           \
                printf("FAIL: %s result n=%u index=%u\n", current_name, n, i);          \
                return 1;                                                               \
            }                                                                           \
        }                                                                               \
        ++cases;                                                                        \
    }                                                                                   \
    return 0;                                                                           \
}

#define NO_NAN_CONTROL(p) ((void)0)
DEFINE_TYPE(q7, q7_t, 16U, 120, NO_NAN_CONTROL)
DEFINE_TYPE(q15, q15_t, 8U, 30000, NO_NAN_CONTROL)
DEFINE_TYPE(q31, q31_t, 4U, 2000000000.0, NO_NAN_CONTROL)
DEFINE_TYPE(f32, float32_t, 4U, 3.0, REGRESSION_CONTROL_NAN_F32)
DEFINE_TYPE(f16, float16_t, 8U, 3.0, REGRESSION_CONTROL_NAN_F16)

int main(void)
{
    regression_console_init();
    SCB->SHCSR |= SCB_SHCSR_MEMFAULTENA_Msk;
    if (run_q7() || run_q15() || run_q31() || run_f32() || run_f16())
        return 1;
    printf("PASS: %u MVE clip tail-load cases\n", cases);
    return 0;
}
