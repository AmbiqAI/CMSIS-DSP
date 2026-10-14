/* Copyright (c) 2026 Ambiq Micro, Inc.
 * SPDX-License-Identifier: Apache-2.0
 */
/* Cortex-M55 regression: the MVE convolution and correlation kernels
 * (arm_conv q7/q15/q31/f32, arm_correlate q7/q15/q31/f32/f16) with source
 * buffers that end at, or start right after, an MPU-inaccessible region.
 * Their final partial blocks are loaded with the tail predicate, so a call
 * reads only srcA[0..srcALen) and srcB[0..srcBLen).
 *
 * Both lengths run from 1 to two full vectors plus two, and at three full
 * vectors plus one and at 61, in every combination. Each pair runs three
 * times: with the sources followed and preceded by readable padding, with
 * both sources ending at a guard, and with both starting right after one.
 * The guarded outputs must equal the padded output bit for bit, and agree
 * with a double-precision reference (fixed point within 1 LSB, floating
 * point within a relative bound; infinite or NaN outputs fail by their
 * bits); output canaries detect boundary writes.
 */
#include "ARMCM55.h"
#include "dsp/filtering_functions.h"
#include "dsp/filtering_functions_f16.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "regression_checks.h"

#if !defined(ARM_MATH_MVEF) || !defined(ARM_MATH_MVEI) || defined(ARM_MATH_AUTOVECTORIZE)
#error This regression requires the handwritten MVE kernels.
#endif

#define MAX_LEN 61U
#define BLOCK_BYTES 512U
/* Each guard covers a whole source block plus one vector access, so a read
 * anywhere within one block of a source end or start faults
 * (regression_checks.h). */
#define GUARD_BYTES REGRESSION_GUARD_BYTES(BLOCK_BYTES)
#define PAD_BYTES 64U
#define OUT_ELEMS (2U * MAX_LEN + 2U)

extern void regression_console_init(void);

/* Guard, source block A, guard, source block B, guard. */
static unsigned char arena[2U * BLOCK_BYTES + 3U * GUARD_BYTES] __ALIGNED(32);
#define GUARD_0 ((uintptr_t)arena)
#define BLOCK_A (GUARD_0 + GUARD_BYTES)
#define GUARD_1 (BLOCK_A + BLOCK_BYTES)
#define BLOCK_B (GUARD_1 + GUARD_BYTES)
#define GUARD_2 (BLOCK_B + BLOCK_BYTES)

static unsigned char pad_a[PAD_BYTES + BLOCK_BYTES + PAD_BYTES] __ALIGNED(16);
static unsigned char pad_b[PAD_BYTES + BLOCK_BYTES + PAD_BYTES] __ALIGNED(16);
static unsigned char out_plain[OUT_ELEMS * 4U] __ALIGNED(16);
static unsigned char out_guarded[OUT_ELEMS * 4U] __ALIGNED(16);
static double reference[OUT_ELEMS], magnitude[OUT_ELEMS];

static const char *current_name = "none";
static const char *current_place = "none";
static volatile unsigned current_a, current_b;
static unsigned cases;

void MemManage_Handler(void)
{
    ARM_MPU_Disable();
    printf("FAIL: %s tail access srcALen=%u srcBLen=%u (%s)\n", current_name, current_a,
           current_b, current_place);
    exit(1);
}

static void protect_guards(void)
{
    ARM_MPU_Disable();
    for (uint32_t region = 0; region < ((MPU->TYPE >> 8) & 0xffU); ++region)
    {
        ARM_MPU_ClrRegion(region);
    }
    ARM_MPU_SetMemAttr(0, ARM_MPU_ATTR(ARM_MPU_ATTR_NON_CACHEABLE,
                                     ARM_MPU_ATTR_NON_CACHEABLE));
    /* Map everything except the three aligned guards. No background map. */
    ARM_MPU_SetRegion(0, ARM_MPU_RBAR(0, ARM_MPU_SH_NON, 0, 1, 0),
                     ARM_MPU_RLAR(GUARD_0 - 1U, 0));
    ARM_MPU_SetRegion(1, ARM_MPU_RBAR(BLOCK_A, ARM_MPU_SH_NON, 0, 1, 0),
                     ARM_MPU_RLAR(GUARD_1 - 1U, 0));
    ARM_MPU_SetRegion(2, ARM_MPU_RBAR(BLOCK_B, ARM_MPU_SH_NON, 0, 1, 0),
                     ARM_MPU_RLAR(GUARD_2 - 1U, 0));
    ARM_MPU_SetRegion(3, ARM_MPU_RBAR(GUARD_2 + GUARD_BYTES, ARM_MPU_SH_NON, 0, 1, 0),
                     ARM_MPU_RLAR(UINT32_MAX, 0));
    ARM_MPU_Enable(0);
}

/* Ends a guarded call; a read control (regression_checks.h) reads past the
 * end of source block A before the guards are lifted. */
static void unguard_buffers(void)
{
    REGRESSION_CONTROL_READ_ABOVE(GUARD_1, GUARD_BYTES);
    ARM_MPU_Disable();
}

/* Reference for n outputs from the double copies da/db. Convolution:
 * c[n] = sum a[k] b[n-k]. Correlation (Arm's layout): with
 * R[t] = sum a[k] b[k + srcBLen - 1 - t], t = 0 .. srcALen + srcBLen - 2, the
 * output is R preceded by 2 max(srcALen, srcBLen) - srcALen - srcBLen zeros
 * when srcALen >= srcBLen and followed by them otherwise. */
static double da[MAX_LEN], db[MAX_LEN];
static unsigned make_reference(int correlate, unsigned la, unsigned lb)
{
    unsigned full = la + lb - 1U;
    unsigned n = correlate ? 2U * (la > lb ? la : lb) - 1U : full;
    unsigned offset = (correlate && la >= lb) ? n - full : 0U;
    for (unsigned i = 0; i < n; ++i)
    {
        reference[i] = 0.0;
        magnitude[i] = 0.0;
    }
    for (unsigned t = 0; t < full; ++t)
        for (unsigned k = 0; k < la; ++k)
        {
            long j = correlate ? (long)k + (long)lb - 1L - (long)t : (long)t - (long)k;
            if (j >= 0 && j < (long)lb)
            {
                reference[offset + t] += da[k] * db[j];
                magnitude[offset + t] += fabs(da[k] * db[j]);
            }
        }
    return n;
}

static int length_list(unsigned lanes, unsigned *list)
{
    unsigned count = 0;
    for (unsigned n = 1; n <= 2U * lanes + 2U; ++n)
        list[count++] = n;
    list[count++] = 3U * lanes + 1U;
    list[count++] = MAX_LEN;
    return (int)count;
}

/* One kernel: TAG names the case, T the element type, CALL(a, la, b, lb, dst)
 * runs it, VALUE(i) gives the double value of element i, EXPECT(i) the
 * expected output from reference[i], TOL(i) the tolerance, NAN_CONTROL the
 * float NaN control (or nothing), NONFINITE(i) the bit test for an infinite or
 * NaN float output (regression_checks.h; 0 for fixed point). */
#define DEFINE_KERNEL(TAG, T, LANES, CORRELATE, CALL, FILL_A, FILL_B, EXPECT, TOL, NAN_CONTROL, NONFINITE) \
static int run_##TAG(void)                                                     \
{                                                                              \
    unsigned lengths[2U * LANES + 4U];                                         \
    int nl = length_list(LANES, lengths);                                      \
    current_name = #TAG;                                                       \
    for (int ia = 0; ia < nl; ++ia)                                            \
        for (int ib = 0; ib < nl; ++ib)                                        \
        {                                                                      \
            unsigned la = lengths[ia], lb = lengths[ib];                       \
            T *pa = (T *)(pad_a + PAD_BYTES), *pb = (T *)(pad_b + PAD_BYTES);  \
            memset(pad_a, 0x55, sizeof(pad_a));                                \
            memset(pad_b, 0x3c, sizeof(pad_b));                                \
            for (unsigned i = 0; i < la; ++i)                                  \
            {                                                                  \
                pa[i] = (FILL_A);                                              \
                da[i] = (double)pa[i];                                         \
            }                                                                  \
            for (unsigned i = 0; i < lb; ++i)                                  \
            {                                                                  \
                pb[i] = (FILL_B);                                              \
                db[i] = (double)pb[i];                                         \
            }                                                                  \
            unsigned n = make_reference(CORRELATE, la, lb);                    \
            T *dp = (T *)out_plain + 1, *dg = (T *)out_guarded + 1;            \
            memset(out_plain, 0x2a, sizeof(out_plain));                        \
            memset(dp, 0, n * sizeof(T));                                      \
            CALL(pa, la, pb, lb, dp);                                          \
            current_a = la;                                                    \
            current_b = lb;                                                    \
            for (int place = 0; place < 2; ++place)                            \
            {                                                                  \
                T *a = place ? (T *)BLOCK_A : (T *)GUARD_1 - la;               \
                T *b = place ? (T *)BLOCK_B : (T *)GUARD_2 - lb;               \
                current_place = place ? "sources start after a guard"          \
                                      : "sources end at a guard";              \
                memcpy(a, pa, la * sizeof(T));                                 \
                memcpy(b, pb, lb * sizeof(T));                                 \
                memset(out_guarded, 0x2a, sizeof(out_guarded));                \
                memset(dg, 0, n * sizeof(T));                                  \
                protect_guards();                                              \
                CALL(a, la, b, lb, dg);                                        \
                unguard_buffers();                                             \
                NAN_CONTROL;                                                   \
                for (unsigned i = 0; i < n; ++i)                               \
                    if ((NONFINITE) || fabs((double)dg[i] - (EXPECT)) > (TOL)) \
                    {                                                          \
                        printf("FAIL: %s numerical result srcALen=%u srcBLen=%u index=%u got=%g expected=%g\n", \
                               current_name, la, lb, i, (double)dg[i], (double)(EXPECT)); \
                        return 1;                                              \
                    }                                                          \
                if (memcmp(out_plain, out_guarded, (n + 2U) * sizeof(T)))      \
                {                                                              \
                    printf("FAIL: %s placement mismatch srcALen=%u srcBLen=%u (%s)\n", \
                           current_name, la, lb, current_place);               \
                    return 1;                                                  \
                }                                                              \
                ++cases;                                                       \
            }                                                                  \
        }                                                                      \
    return 0;                                                                  \
}

#define CALL_conv_q7(a, la, b, lb, d) arm_conv_q7(a, la, b, lb, d)
#define CALL_conv_q15(a, la, b, lb, d) arm_conv_q15(a, la, b, lb, d)
#define CALL_conv_q31(a, la, b, lb, d) arm_conv_q31(a, la, b, lb, d)
#define CALL_conv_f32(a, la, b, lb, d) arm_conv_f32(a, la, b, lb, d)
#define CALL_correlate_q7(a, la, b, lb, d) arm_correlate_q7(a, la, b, lb, d)
#define CALL_correlate_q15(a, la, b, lb, d) arm_correlate_q15(a, la, b, lb, d)
#define CALL_correlate_q31(a, la, b, lb, d) arm_correlate_q31(a, la, b, lb, d)
#define CALL_correlate_f32(a, la, b, lb, d) arm_correlate_f32(a, la, b, lb, d)
#define CALL_correlate_f16(a, la, b, lb, d) arm_correlate_f16(a, la, b, lb, d)

/* Sources: small integers (scaled), so no sum saturates. */
#define FILL_INT_A(SCALE) (((int)((i * 7U) % 13U) - 6) * (SCALE))
#define FILL_INT_B(SCALE) (((int)((i * 5U) % 11U) - 5) * (SCALE))
/* Fixed point: the kernels shift the sum of products right by 7, 15 or 31
 * bits. */
#define FIXED_EXPECT(SHIFT) floor(reference[i] / (double)(1ULL << (SHIFT)))
#define FLOAT_TOL_F32 (1e-5 * magnitude[i] + 1e-6)
#define FLOAT_TOL_F16 (4e-3 * magnitude[i] + 2e-3)
#define NAN_F32 REGRESSION_CONTROL_NAN_F32(&dg[0])
#define NAN_F16 REGRESSION_CONTROL_NAN_F16(&dg[0])

DEFINE_KERNEL(arm_conv_q7, q7_t, 16U, 0, CALL_conv_q7, (q7_t)FILL_INT_A(2), (q7_t)FILL_INT_B(2),
              FIXED_EXPECT(7), 1.0, (void)0, 0)
DEFINE_KERNEL(arm_conv_q15, q15_t, 8U, 0, CALL_conv_q15, (q15_t)FILL_INT_A(300), (q15_t)FILL_INT_B(300),
              FIXED_EXPECT(15), 1.0, (void)0, 0)
DEFINE_KERNEL(arm_conv_q31, q31_t, 4U, 0, CALL_conv_q31, (q31_t)FILL_INT_A(1 << 24), (q31_t)FILL_INT_B(1 << 24),
              FIXED_EXPECT(31), 1.0, (void)0, 0)
DEFINE_KERNEL(arm_conv_f32, float32_t, 4U, 0, CALL_conv_f32, (float32_t)FILL_INT_A(0.125), (float32_t)FILL_INT_B(0.125),
              reference[i], FLOAT_TOL_F32, NAN_F32, regression_nonfinite_f32(&dg[i]))
DEFINE_KERNEL(arm_correlate_q7, q7_t, 16U, 1, CALL_correlate_q7, (q7_t)FILL_INT_A(2), (q7_t)FILL_INT_B(2),
              FIXED_EXPECT(7), 1.0, (void)0, 0)
DEFINE_KERNEL(arm_correlate_q15, q15_t, 8U, 1, CALL_correlate_q15, (q15_t)FILL_INT_A(300), (q15_t)FILL_INT_B(300),
              FIXED_EXPECT(15), 1.0, (void)0, 0)
DEFINE_KERNEL(arm_correlate_q31, q31_t, 4U, 1, CALL_correlate_q31, (q31_t)FILL_INT_A(1 << 24), (q31_t)FILL_INT_B(1 << 24),
              FIXED_EXPECT(31), 1.0, (void)0, 0)
DEFINE_KERNEL(arm_correlate_f32, float32_t, 4U, 1, CALL_correlate_f32, (float32_t)FILL_INT_A(0.125), (float32_t)FILL_INT_B(0.125),
              reference[i], FLOAT_TOL_F32, NAN_F32, regression_nonfinite_f32(&dg[i]))
DEFINE_KERNEL(arm_correlate_f16, float16_t, 8U, 1, CALL_correlate_f16, (float16_t)FILL_INT_A(0.125), (float16_t)FILL_INT_B(0.125),
              reference[i], FLOAT_TOL_F16, NAN_F16, regression_nonfinite_f16(&dg[i]))

int main(void)
{
    regression_console_init();
    SCB->SHCSR |= SCB_SHCSR_MEMFAULTENA_Msk;
    if (run_arm_conv_q7() || run_arm_conv_q15() || run_arm_conv_q31() || run_arm_conv_f32() ||
        run_arm_correlate_q7() || run_arm_correlate_q15() || run_arm_correlate_q31() ||
        run_arm_correlate_f32() || run_arm_correlate_f16())
        return 1;
    printf("PASS: %u MVE convolution and correlation tail-load cases\n", cases);
    return 0;
}
