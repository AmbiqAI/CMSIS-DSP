/* Copyright (c) 2026 Ambiq Micro, Inc.
 * SPDX-License-Identifier: Apache-2.0
 */
/* Cortex-M55 regression: the five kernels left out of the f32, fixed-point
 * and f16 tail-load groups. Three f16 kernels (arm_cmplx_mag_f16,
 * arm_cmplx_mag_squared_f16, arm_float_to_f16) loaded their final partial
 * block with vld2q, which has no predicated form, and now gather the
 * de-interleaved elements under the tail predicate; arm_mat_add_f32 and
 * arm_mat_sub_f32 load every block under the block predicate. Each source
 * buffer ends at an MPU-inaccessible region: block sizes 1 to 16 for the
 * vector kernels, matrices of one and two rows by 1 to 16 columns. Results
 * are compared with double-precision references and output canaries detect
 * boundary writes.
 */
#include "ARMCM55.h"
#include "dsp/complex_math_functions_f16.h"
#include "dsp/matrix_functions.h"
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
#define CANARY_F16 12345.0f16
#define CANARY_F32 12345.0f

extern void regression_console_init(void);

/* Two source blocks, each followed by its own 32-byte guard. */
static unsigned char arena[2U * (BLOCK_BYTES + GUARD_BYTES)] __ALIGNED(32);
#define GUARD_A ((uintptr_t)arena + BLOCK_BYTES)
#define GUARD_B ((uintptr_t)arena + 2U * BLOCK_BYTES + GUARD_BYTES)

static const char *current_name = "none";
static volatile unsigned current_n;
static float16_t out_h[2U * MAX_N + 2U];
static float32_t out_f[2U * MAX_N + 2U];
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

/* Values are quarter multiples with magnitude at most 4, exact in half
 * precision; index i of the sequence is the same in both widths. */
static double sample_a(unsigned i)
{
    return ((i & 1U) ? -0.25 : 0.25) * (double)((i * 5U) % MAX_N + 1U);
}

static double sample_b(unsigned i)
{
    return 0.5 * (double)((int)(i % 5U) - 2);
}

static float16_t *source_h(unsigned n, uintptr_t guard, double (*sample)(unsigned))
{
    float16_t *a = (float16_t *)guard - n;
    for (unsigned i = 0; i < n; ++i)
        a[i] = (float16_t)sample(i);
    return a;
}

static float32_t *source_f(unsigned n, uintptr_t guard, double (*sample)(unsigned))
{
    float32_t *a = (float32_t *)guard - n;
    for (unsigned i = 0; i < n; ++i)
        a[i] = (float32_t)sample(i);
    return a;
}

static void clear_out(void)
{
    for (unsigned i = 0; i < sizeof(out_h) / sizeof(out_h[0]); ++i)
        out_h[i] = CANARY_F16;
    for (unsigned i = 0; i < sizeof(out_f) / sizeof(out_f[0]); ++i)
        out_f[i] = CANARY_F32;
}

static int check_canaries_h(unsigned count)
{
    if ((double)out_h[0] != (double)CANARY_F16 || (double)out_h[count + 1U] != (double)CANARY_F16)
    {
        printf("FAIL: %s output canary n=%u\n", current_name, current_n);
        return 1;
    }
    return 0;
}

static int check_canaries_f(unsigned count)
{
    if (out_f[0] != CANARY_F32 || out_f[count + 1U] != CANARY_F32)
    {
        printf("FAIL: %s output canary n=%u\n", current_name, current_n);
        return 1;
    }
    return 0;
}

/* Non-finite outputs are rejected by their bit pattern (exponent all ones):
 * under -ffast-math the compiler may fold isnan()/isfinite() to false, and
 * fabs(NaN - reference) > tolerance is false. */
static int is_finite_f16(float16_t x)
{
    uint16_t bits;
    memcpy(&bits, &x, sizeof(bits));
    return (bits & 0x7c00U) != 0x7c00U;
}

static int is_finite_f32(float32_t x)
{
    uint32_t bits;
    memcpy(&bits, &x, sizeof(bits));
    return (bits & 0x7f800000U) != 0x7f800000U;
}

static int check_value_h(float16_t got, double reference, double tolerance, unsigned index)
{
    if (!is_finite_f16(got) || fabs((double)got - reference) > tolerance)
    {
        printf("FAIL: %s numerical result n=%u index=%u got=%g expected=%g\n",
               current_name, current_n, index, (double)got, reference);
        return 1;
    }
    return 0;
}

static int check_value_f(float32_t got, double reference, unsigned index)
{
    if (!is_finite_f32(got) || (double)got != reference)
    {
        printf("FAIL: %s numerical result n=%u index=%u got=%g expected=%g\n",
               current_name, current_n, index, (double)got, reference);
        return 1;
    }
    return 0;
}

/* Complex magnitude kernels: n interleaved complex values (2n halves) end at
 * guard A. The magnitude uses a fast inverse square root (two Newton steps in
 * the tail), so it is checked to 1 percent; the squared magnitude is a sum of
 * two exact products rounded once to half precision. */
static int run_complex(void)
{
    for (unsigned n = 1; n <= MAX_N; ++n)
    {
        float16_t *c = source_h(2U * n, GUARD_A, sample_a);
        current_n = n;

        current_name = "arm_cmplx_mag_f16";
        clear_out();
        protect_guards();
        arm_cmplx_mag_f16(c, out_h + 1, n);
        ARM_MPU_Disable();
        if (check_canaries_h(n))
            return 1;
        for (unsigned i = 0; i < n; ++i)
        {
            double re = (double)c[2U * i], im = (double)c[2U * i + 1U];
            double mag = sqrt(re * re + im * im);
            if (check_value_h(out_h[i + 1U], mag, 0.01 * mag + 0.004, i))
                return 1;
        }
        ++cases;

        current_name = "arm_cmplx_mag_squared_f16";
        clear_out();
        protect_guards();
        arm_cmplx_mag_squared_f16(c, out_h + 1, n);
        ARM_MPU_Disable();
        if (check_canaries_h(n))
            return 1;
        for (unsigned i = 0; i < n; ++i)
        {
            double re = (double)c[2U * i], im = (double)c[2U * i + 1U];
            double sq = re * re + im * im;
            if (check_value_h(out_h[i + 1U], sq, 0.001 * sq + 0.0005, i))
                return 1;
        }
        ++cases;
    }
    return 0;
}

/* Float to f16: n single-precision values, exact in half precision, end at
 * guard A. */
static int run_float_to_f16(void)
{
    for (unsigned n = 1; n <= MAX_N; ++n)
    {
        float32_t *a = source_f(n, GUARD_A, sample_a);
        current_name = "arm_float_to_f16";
        current_n = n;
        clear_out();
        protect_guards();
        arm_float_to_f16(a, out_h + 1, n);
        ARM_MPU_Disable();
        if (check_canaries_h(n))
            return 1;
        for (unsigned i = 0; i < n; ++i)
            if (check_value_h(out_h[i + 1U], (double)a[i], 0.0, i))
                return 1;
        ++cases;
    }
    return 0;
}

/* f32 matrix kernels: rows 1 and 2, columns 1 to 16, data ending at the
 * guards; sums and differences of the inputs are exact. */
#define MATRIX_CASE(NAME, KERNEL_CALL, REF)                                    \
    for (unsigned rows = 1; rows <= 2U; ++rows)                                \
        for (unsigned cols = 1; cols <= MAX_N; ++cols)                         \
        {                                                                      \
            unsigned n = rows * cols;                                          \
            float32_t *a = source_f(n, GUARD_A, sample_a);                     \
            float32_t *b = source_f(n, GUARD_B, sample_b);                     \
            arm_matrix_instance_f32 A = {(uint16_t)rows, (uint16_t)cols, a};   \
            arm_matrix_instance_f32 B = {(uint16_t)rows, (uint16_t)cols, b};   \
            arm_matrix_instance_f32 D = {(uint16_t)rows, (uint16_t)cols, out_f + 1}; \
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
            if (check_canaries_f(n))                                           \
                return 1;                                                      \
            for (unsigned i = 0; i < n; ++i)                                   \
                if (check_value_f(out_f[i + 1U], (REF), i))                    \
                    return 1;                                                  \
            ++cases;                                                           \
        }

static int run_matrices(void)
{
    MATRIX_CASE("arm_mat_add_f32", arm_mat_add_f32(&A, &B, &D), (double)a[i] + (double)b[i])
    MATRIX_CASE("arm_mat_sub_f32", arm_mat_sub_f32(&A, &B, &D), (double)a[i] - (double)b[i])
    return 0;
}

int main(void)
{
    regression_console_init();
    SCB->SHCSR |= SCB_SHCSR_MEMFAULTENA_Msk;
    if (run_complex() || run_float_to_f16() || run_matrices())
        return 1;
    printf("PASS: %u MVE remainder tail-load cases\n", cases);
    return 0;
}
