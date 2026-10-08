/* Copyright (c) 2026 Ambiq Micro, Inc.
 * SPDX-License-Identifier: Apache-2.0
 */
/* Cortex-M55 regression: f32 and u32 vector kernels whose source buffers end
 * at an MPU-inaccessible region. Each kernel's final partial block is loaded
 * with the tail predicate, so a block of n elements reads exactly n elements
 * from every source. Covers the sixteen f32/u32 kernels of the predicated
 * tail-load change plus arm_mat_add_f32 and arm_mat_sub_f32, whose do/while
 * bodies the compilers already turn into tail-predicated loops: block sizes
 * 1 to 8 (two full vectors), matrices of one and two rows. Results are
 * compared with double-precision references; output canaries detect boundary
 * writes.
 */
#include "ARMCM55.h"
#include "dsp/basic_math_functions.h"
#include "dsp/matrix_functions.h"
#include "dsp/statistics_functions.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(ARM_MATH_MVEF) || !defined(ARM_MATH_MVEI) || defined(ARM_MATH_AUTOVECTORIZE)
#error This regression requires the handwritten MVE kernels.
#endif

#define MAX_N 8U
#define BLOCK_BYTES 512U
#define GUARD_BYTES 32U
#define CANARY 12345.0f
#define CANARY_U 0x5a5a5a5aU

extern void regression_console_init(void);

/* Two source blocks, each followed by its own 32-byte guard. */
static unsigned char arena[2U * (BLOCK_BYTES + GUARD_BYTES)] __ALIGNED(32);
#define GUARD_A ((uintptr_t)arena + BLOCK_BYTES)
#define GUARD_B ((uintptr_t)arena + 2U * BLOCK_BYTES + GUARD_BYTES)

static const char *current_name = "none";
static volatile unsigned current_n;
static float32_t out[MAX_N * 2U + 2U];
static uint32_t out_u[MAX_N + 2U];

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

/* Sources of n elements ending at the guards. |a| values are distinct so the
 * absolute-minimum index is unambiguous. */
static float32_t *source_a(unsigned n)
{
    float32_t *a = (float32_t *)GUARD_A - n;
    for (unsigned i = 0; i < n; ++i)
        a[i] = ((i & 1U) ? -0.5f : 0.5f) * (float32_t)((i * 3U) % MAX_N + 1U);
    return a;
}

static float32_t *source_b(unsigned n)
{
    float32_t *b = (float32_t *)GUARD_B - n;
    for (unsigned i = 0; i < n; ++i)
        b[i] = 0.25f * (float32_t)((int)(i % 5U) - 2);
    return b;
}

static uint32_t *source_ua(unsigned n)
{
    uint32_t *a = (uint32_t *)GUARD_A - n;
    for (unsigned i = 0; i < n; ++i)
        a[i] = 0x9e3779b9U * (i + 1U);
    return a;
}

static uint32_t *source_ub(unsigned n)
{
    uint32_t *b = (uint32_t *)GUARD_B - n;
    for (unsigned i = 0; i < n; ++i)
        b[i] = 0x7f4a7c15U ^ (i * 0x01000193U);
    return b;
}

static void clear_out(void)
{
    for (unsigned i = 0; i < sizeof(out) / sizeof(out[0]); ++i)
        out[i] = CANARY;
    for (unsigned i = 0; i < sizeof(out_u) / sizeof(out_u[0]); ++i)
        out_u[i] = CANARY_U;
}

static int check_canaries(unsigned count)
{
    if (out[0] != CANARY || out[count + 1U] != CANARY ||
        out_u[0] != CANARY_U || (count < MAX_N + 1U && out_u[count + 1U] != CANARY_U))
    {
        printf("FAIL: %s output canary n=%u\n", current_name, current_n);
        return 1;
    }
    return 0;
}

static int check_value(float32_t got, double reference, unsigned index)
{
    if (fabs((double)got - reference) > 1e-5)
    {
        printf("FAIL: %s numerical result n=%u index=%u\n", current_name, current_n, index);
        return 1;
    }
    return 0;
}

static unsigned cases;

/* Elementwise f32 kernels: KERNEL_CALL runs the kernel into out + 1, REF(i)
 * gives element i of the expected output from a[] and b[]. */
#define VECTOR_CASE(NAME, KERNEL_CALL, REF)                                    \
    for (unsigned n = 1; n <= MAX_N; ++n)                                      \
    {                                                                          \
        float32_t *a = source_a(n), *b = source_b(n);                          \
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
            if (check_value(out[i + 1U], (REF), i))                            \
                return 1;                                                      \
        ++cases;                                                               \
    }

/* Bitwise u32 kernels, exact comparison. */
#define BITWISE_CASE(NAME, KERNEL_CALL, REF)                                   \
    for (unsigned n = 1; n <= MAX_N; ++n)                                      \
    {                                                                          \
        uint32_t *a = source_ua(n), *b = source_ub(n);                         \
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
            if (out_u[i + 1U] != (REF))                                        \
            {                                                                  \
                printf("FAIL: %s result n=%u index=%u\n", NAME, n, i);         \
                return 1;                                                      \
            }                                                                  \
        ++cases;                                                               \
    }

static int run_vectors(void)
{
    VECTOR_CASE("arm_abs_f32", arm_abs_f32(a, out + 1, n), fabs((double)a[i]))
    VECTOR_CASE("arm_negate_f32", arm_negate_f32(a, out + 1, n), -(double)a[i])
    VECTOR_CASE("arm_offset_f32", arm_offset_f32(a, 1.5f, out + 1, n), (double)a[i] + 1.5)
    VECTOR_CASE("arm_scale_f32", arm_scale_f32(a, -0.75f, out + 1, n), (double)a[i] * -0.75)
    VECTOR_CASE("arm_add_f32", arm_add_f32(a, b, out + 1, n), (double)a[i] + (double)b[i])
    VECTOR_CASE("arm_sub_f32", arm_sub_f32(a, b, out + 1, n), (double)a[i] - (double)b[i])
    VECTOR_CASE("arm_mult_f32", arm_mult_f32(a, b, out + 1, n), (double)a[i] * (double)b[i])
    BITWISE_CASE("arm_and_u32", arm_and_u32(a, b, out_u + 1, n), (a[i] & b[i]))
    BITWISE_CASE("arm_or_u32", arm_or_u32(a, b, out_u + 1, n), (a[i] | b[i]))
    BITWISE_CASE("arm_xor_u32", arm_xor_u32(a, b, out_u + 1, n), (a[i] ^ b[i]))
    BITWISE_CASE("arm_not_u32", arm_not_u32(a, out_u + 1, n), ~a[i])
    return 0;
}

static int run_reductions(void)
{
    for (unsigned n = 1; n <= MAX_N; ++n)
    {
        float32_t *a = source_a(n), *b = source_b(n);
        double dot = 0.0, sum = 0.0, mse = 0.0, absmin = 1e30;
        unsigned absmin_index = 0;
        for (unsigned i = 0; i < n; ++i)
        {
            dot += (double)a[i] * (double)b[i];
            sum += (double)a[i];
            mse += ((double)a[i] - (double)b[i]) * ((double)a[i] - (double)b[i]);
            if (fabs((double)a[i]) < absmin)
            {
                absmin = fabs((double)a[i]);
                absmin_index = i;
            }
        }
        mse /= (double)n;
        current_n = n;

        current_name = "arm_dot_prod_f32";
        clear_out();
        protect_guards();
        arm_dot_prod_f32(a, b, n, out + 1);
        ARM_MPU_Disable();
        if (check_canaries(1) || check_value(out[1], dot, 0))
            return 1;
        ++cases;

        current_name = "arm_accumulate_f32";
        clear_out();
        protect_guards();
        arm_accumulate_f32(a, n, out + 1);
        ARM_MPU_Disable();
        if (check_canaries(1) || check_value(out[1], sum, 0))
            return 1;
        ++cases;

        current_name = "arm_mse_f32";
        clear_out();
        protect_guards();
        arm_mse_f32(a, b, n, out + 1);
        ARM_MPU_Disable();
        if (check_canaries(1) || check_value(out[1], mse, 0))
            return 1;
        ++cases;

        current_name = "arm_absmin_f32";
        clear_out();
        protect_guards();
        arm_absmin_f32(a, n, out + 1, out_u + 1);
        ARM_MPU_Disable();
        if (check_canaries(1) || check_value(out[1], absmin, 0))
            return 1;
        if (out_u[1] != absmin_index)
        {
            printf("FAIL: arm_absmin_f32 index n=%u\n", n);
            return 1;
        }
        ++cases;
    }
    return 0;
}

/* Matrix kernels: rows 1 and 2, columns 1 to 8, data ending at the guards. */
#define MATRIX_CASE(NAME, KERNEL_CALL, REF)                                    \
    for (unsigned rows = 1; rows <= 2U; ++rows)                                \
        for (unsigned cols = 1; cols <= MAX_N; ++cols)                         \
        {                                                                      \
            unsigned n = rows * cols;                                          \
            float32_t *a = source_a(n), *b = source_b(n);                      \
            (void)b;                                                           \
            arm_matrix_instance_f32 A = {(uint16_t)rows, (uint16_t)cols, a};   \
            arm_matrix_instance_f32 B = {(uint16_t)rows, (uint16_t)cols, b};   \
            arm_matrix_instance_f32 D = {(uint16_t)rows, (uint16_t)cols, out + 1}; \
            (void)B;                                                           \
            current_name = NAME;                                               \
            current_n = n;                                                     \
            clear_out();                                                       \
            protect_guards();                                                  \
            if ((KERNEL_CALL) != ARM_MATH_SUCCESS)                             \
            {                                                                  \
                ARM_MPU_Disable();                                             \
                printf("FAIL: %s status rows=%u cols=%u\n", NAME, rows, cols); \
                return 1;                                                      \
            }                                                                  \
            ARM_MPU_Disable();                                                 \
            if (check_canaries(n))                                             \
                return 1;                                                      \
            for (unsigned i = 0; i < n; ++i)                                   \
                if (check_value(out[i + 1U], (REF), i))                        \
                    return 1;                                                  \
            ++cases;                                                           \
        }

static int run_matrices(void)
{
    MATRIX_CASE("arm_mat_add_f32", arm_mat_add_f32(&A, &B, &D), (double)a[i] + (double)b[i])
    MATRIX_CASE("arm_mat_sub_f32", arm_mat_sub_f32(&A, &B, &D), (double)a[i] - (double)b[i])
    MATRIX_CASE("arm_mat_scale_f32", arm_mat_scale_f32(&A, 2.5f, &D), (double)a[i] * 2.5)
    return 0;
}

int main(void)
{
    regression_console_init();
    SCB->SHCSR |= SCB_SHCSR_MEMFAULTENA_Msk;
    if (run_vectors() || run_reductions() || run_matrices())
        return 1;
    printf("PASS: %u MVE f32/u32 tail-load cases\n", cases);
    return 0;
}
