/* Copyright (c) 2026 Ambiq Micro, Inc.
 * SPDX-License-Identifier: Apache-2.0
 */
/* Cortex-M55 regression: f16 vector kernels whose source buffers end at an
 * MPU-inaccessible region. Each kernel's final partial block is loaded with
 * the tail predicate, so a block of n elements reads exactly n elements from
 * every source. Covers the fifteen f16 kernels of the predicated tail-load
 * change: block sizes 1 to 16 (two full vectors), matrices of one and two
 * rows. Inputs are exact in half precision and small enough that sums and
 * products are exact; results are compared with double-precision references
 * and output canaries detect boundary writes.
 */
#include "ARMCM55.h"
#include "dsp/basic_math_functions_f16.h"
#include "dsp/complex_math_functions_f16.h"
#include "dsp/matrix_functions_f16.h"
#include "dsp/statistics_functions_f16.h"
#include "dsp/support_functions_f16.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(ARM_MATH_MVE_FLOAT16) || defined(ARM_MATH_AUTOVECTORIZE)
#error This regression requires the handwritten MVE float16 kernels.
#endif

#define MAX_N 16U
#define BLOCK_BYTES 512U
#define GUARD_BYTES 32U
#define CANARY 12345.0f16
#define CANARY_U 0x5a5a5a5aU

extern void regression_console_init(void);

/* Two source blocks, each followed by its own 32-byte guard. */
static unsigned char arena[2U * (BLOCK_BYTES + GUARD_BYTES)] __ALIGNED(32);
#define GUARD_A ((uintptr_t)arena + BLOCK_BYTES)
#define GUARD_B ((uintptr_t)arena + 2U * BLOCK_BYTES + GUARD_BYTES)

static const char *current_name = "none";
static volatile unsigned current_n;
static float16_t out[4U * MAX_N + 2U];
static uint32_t out_u[2];
static unsigned cases;

void MemManage_Handler(void)
{
    ARM_MPU_Disable();
    printf("FAIL: %s tail access n=%u\n", current_name, current_n);
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
    /* Map everything except the two aligned guards. No background map. */
    ARM_MPU_SetRegion(0, ARM_MPU_RBAR(0, ARM_MPU_SH_NON, 0, 1, 0),
                     ARM_MPU_RLAR(GUARD_A - 1U, 0));
    ARM_MPU_SetRegion(1, ARM_MPU_RBAR(GUARD_A + GUARD_BYTES, ARM_MPU_SH_NON, 0, 1, 0),
                     ARM_MPU_RLAR(GUARD_B - 1U, 0));
    ARM_MPU_SetRegion(2, ARM_MPU_RBAR(GUARD_B + GUARD_BYTES, ARM_MPU_SH_NON, 0, 1, 0),
                     ARM_MPU_RLAR(UINT32_MAX, 0));
    ARM_MPU_Enable(0);
}

/* Sources of n elements ending at the guards; |a| values are distinct within
 * a block of 16 so the absolute-minimum index is unambiguous. */
static float16_t *source_a(unsigned n)
{
    float16_t *a = (float16_t *)GUARD_A - n;
    for (unsigned i = 0; i < n; ++i)
        a[i] = (float16_t)(((i & 1U) ? -0.25 : 0.25) * (double)((i * 5U) % MAX_N + 1U));
    return a;
}

static float16_t *source_b(unsigned n)
{
    float16_t *b = (float16_t *)GUARD_B - n;
    for (unsigned i = 0; i < n; ++i)
        b[i] = (float16_t)(0.5 * (double)((int)(i % 5U) - 2));
    return b;
}

static void clear_out(void)
{
    for (unsigned i = 0; i < sizeof(out) / sizeof(out[0]); ++i)
        out[i] = CANARY;
    out_u[0] = out_u[1] = CANARY_U;
}

static int check_canaries(unsigned count)
{
    if ((double)out[0] != (double)CANARY || (double)out[count + 1U] != (double)CANARY)
    {
        printf("FAIL: %s output canary n=%u\n", current_name, current_n);
        return 1;
    }
    return 0;
}

/* Non-finite outputs are rejected by their binary16 bit pattern (exponent all
 * ones): under -ffast-math the compiler may fold isnan()/isfinite() to false,
 * and fabs(NaN - reference) > tolerance is false. */
static int is_finite_f16(float16_t x)
{
    uint16_t bits;
    memcpy(&bits, &x, sizeof(bits));
    return (bits & 0x7c00U) != 0x7c00U;
}

static int check_value(float16_t got, double reference, double tolerance, unsigned index)
{
    if (!is_finite_f16(got) || fabs((double)got - reference) > tolerance)
    {
        printf("FAIL: %s numerical result n=%u index=%u got=%g expected=%g\n",
               current_name, current_n, index, (double)got, reference);
        return 1;
    }
    return 0;
}

#define VECTOR_CASE(NAME, KERNEL_CALL, REF, TOL)                               \
    for (unsigned n = 1; n <= MAX_N; ++n)                                      \
    {                                                                          \
        float16_t *a = source_a(n), *b = source_b(n);                          \
        (void)b;                                                               \
        current_name = NAME;                                                   \
        current_n = n;                                                         \
        clear_out();                                                           \
        protect_guards();                                                      \
        KERNEL_CALL;                                                           \
        ARM_MPU_Disable();                                                     \
        if (check_canaries(n))                                                 \
            return 1;                                                          \
        for (unsigned i = 0; i < n; ++i)                                       \
            if (check_value(out[i + 1U], (REF), (TOL), i))                     \
                return 1;                                                      \
        ++cases;                                                               \
    }

static int run_vectors(void)
{
    VECTOR_CASE("arm_abs_f16", arm_abs_f16(a, out + 1, n), fabs((double)a[i]), 0.0)
    VECTOR_CASE("arm_negate_f16", arm_negate_f16(a, out + 1, n), -(double)a[i], 0.0)
    VECTOR_CASE("arm_offset_f16", arm_offset_f16(a, (float16_t)1.5, out + 1, n), (double)a[i] + 1.5, 0.0)
    VECTOR_CASE("arm_scale_f16", arm_scale_f16(a, (float16_t)-0.75, out + 1, n), (double)a[i] * -0.75, 0.0)
    VECTOR_CASE("arm_add_f16", arm_add_f16(a, b, out + 1, n), (double)a[i] + (double)b[i], 0.0)
    VECTOR_CASE("arm_sub_f16", arm_sub_f16(a, b, out + 1, n), (double)a[i] - (double)b[i], 0.0)
    VECTOR_CASE("arm_mult_f16", arm_mult_f16(a, b, out + 1, n), (double)a[i] * (double)b[i], 0.0)
    return 0;
}

static int run_reductions(void)
{
    for (unsigned n = 1; n <= MAX_N; ++n)
    {
        float16_t *a = source_a(n), *b = source_b(n);
        double dot = 0.0, mse = 0.0, absmin = 1e30;
        unsigned absmin_index = 0;
        for (unsigned i = 0; i < n; ++i)
        {
            dot += (double)a[i] * (double)b[i];
            mse += ((double)a[i] - (double)b[i]) * ((double)a[i] - (double)b[i]);
            if (fabs((double)a[i]) < absmin)
            {
                absmin = fabs((double)a[i]);
                absmin_index = i;
            }
        }
        mse /= (double)n;
        current_n = n;

        current_name = "arm_dot_prod_f16";
        clear_out();
        protect_guards();
        arm_dot_prod_f16(a, b, n, out + 1);
        ARM_MPU_Disable();
        if (check_canaries(1) || check_value(out[1], dot, 0.0, 0))
            return 1;
        ++cases;

        current_name = "arm_mse_f16";
        clear_out();
        protect_guards();
        arm_mse_f16(a, b, n, out + 1);
        ARM_MPU_Disable();
        if (check_canaries(1) || check_value(out[1], mse, 0.02 * mse + 0.002, 0))
            return 1;
        ++cases;

        current_name = "arm_absmin_f16";
        clear_out();
        protect_guards();
        arm_absmin_f16(a, n, out + 1, out_u + 1);
        ARM_MPU_Disable();
        if (check_canaries(1) || check_value(out[1], absmin, 0.0, 0))
            return 1;
        if (out_u[1] != absmin_index)
        {
            printf("FAIL: arm_absmin_f16 index n=%u\n", n);
            return 1;
        }
        ++cases;
    }
    return 0;
}

/* Complex times real: 2n complex interleaved values end at guard A, n real
 * values at guard B. */
static int run_cmplx_mult_real(void)
{
    for (unsigned n = 1; n <= MAX_N; ++n)
    {
        float16_t *c = source_a(2U * n), *r = source_b(n);
        current_name = "arm_cmplx_mult_real_f16";
        current_n = n;
        clear_out();
        protect_guards();
        arm_cmplx_mult_real_f16(c, r, out + 1, n);
        ARM_MPU_Disable();
        if (check_canaries(2U * n))
            return 1;
        for (unsigned i = 0; i < 2U * n; ++i)
            if (check_value(out[i + 1U], (double)c[i] * (double)r[i / 2U], 0.0, i))
                return 1;
        ++cases;
    }
    return 0;
}

/* Q15 to f16: n Q15 values end at guard A. */
static int run_q15_to_f16(void)
{
    for (unsigned n = 1; n <= MAX_N; ++n)
    {
        q15_t *q = (q15_t *)GUARD_A - n;
        for (unsigned i = 0; i < n; ++i)
            q[i] = (q15_t)(((int)((i * 7U) % 13U) - 6) * 2048);
        current_name = "arm_q15_to_f16";
        current_n = n;
        clear_out();
        protect_guards();
        arm_q15_to_f16(q, out + 1, n);
        ARM_MPU_Disable();
        if (check_canaries(n))
            return 1;
        for (unsigned i = 0; i < n; ++i)
            if (check_value(out[i + 1U], (double)q[i] / 32768.0, 0.0, i))
                return 1;
        ++cases;
    }
    return 0;
}

/* Matrix kernels: rows 1 and 2, columns 1 to 16, data ending at the guards. */
#define MATRIX_CASE(NAME, KERNEL_CALL, REF)                                    \
    for (unsigned rows = 1; rows <= 2U; ++rows)                                \
        for (unsigned cols = 1; cols <= MAX_N; ++cols)                         \
        {                                                                      \
            unsigned n = rows * cols;                                          \
            float16_t *a = source_a(n), *b = source_b(n);                      \
            (void)b;                                                           \
            arm_matrix_instance_f16 A = {(uint16_t)rows, (uint16_t)cols, a};   \
            arm_matrix_instance_f16 B = {(uint16_t)rows, (uint16_t)cols, b};   \
            arm_matrix_instance_f16 D = {(uint16_t)rows, (uint16_t)cols, out + 1}; \
            (void)B;                                                           \
            current_name = NAME;                                               \
            current_n = n;                                                     \
            clear_out();                                                       \
            protect_guards();                                                  \
            arm_status status = KERNEL_CALL;                                   \
            ARM_MPU_Disable();                                                 \
            if (status != ARM_MATH_SUCCESS)                                    \
            {                                                                  \
                printf("FAIL: %s status rows=%u cols=%u\n", NAME, rows, cols); \
                return 1;                                                      \
            }                                                                  \
            if (check_canaries(n))                                             \
                return 1;                                                      \
            for (unsigned i = 0; i < n; ++i)                                   \
                if (check_value(out[i + 1U], (REF), 0.0, i))                   \
                    return 1;                                                  \
            ++cases;                                                           \
        }

static int run_matrices(void)
{
    MATRIX_CASE("arm_mat_add_f16", arm_mat_add_f16(&A, &B, &D), (double)a[i] + (double)b[i])
    MATRIX_CASE("arm_mat_sub_f16", arm_mat_sub_f16(&A, &B, &D), (double)a[i] - (double)b[i])
    MATRIX_CASE("arm_mat_scale_f16", arm_mat_scale_f16(&A, (float16_t)2.5, &D), (double)a[i] * 2.5)
    return 0;
}

int main(void)
{
    regression_console_init();
    SCB->SHCSR |= SCB_SHCSR_MEMFAULTENA_Msk;
    if (run_vectors() || run_reductions() || run_cmplx_mult_real() || run_q15_to_f16() ||
        run_matrices())
        return 1;
    printf("PASS: %u MVE f16 tail-load cases\n", cases);
    return 0;
}
