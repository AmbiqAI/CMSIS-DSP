/* Copyright (c) 2026 Ambiq Micro, Inc.
 * SPDX-License-Identifier: Apache-2.0
 */
/* Cortex-M55 regression: Q7/Q15/Q31 and u8/u16 vector kernels whose source
 * buffers end at an MPU-inaccessible region. Each kernel's final partial
 * block is loaded with the tail predicate, so a block of n elements reads
 * exactly n elements from every source. Covers the 45 fixed-point kernels of
 * the predicated tail-load change at block sizes 1 to two full vectors.
 *
 * Each case runs the kernel twice: on sources followed by readable nonzero
 * padding and on the same sources ending at the guards. The outputs must be
 * identical, and within a few LSB of a double-precision reference; output
 * canaries detect boundary writes.
 */
#include "ARMCM55.h"
#include "dsp/basic_math_functions.h"
#include "dsp/matrix_functions.h"
#include "dsp/statistics_functions.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(ARM_MATH_MVEI) || defined(ARM_MATH_AUTOVECTORIZE)
#error This regression requires the handwritten MVE integer kernels.
#endif

#define BLOCK_BYTES 512U
#define GUARD_BYTES 32U
#define PAD_BYTES 64U
#define OUT_BYTES 512U

extern void regression_console_init(void);

/* Two source blocks, each followed by its own 32-byte guard. */
static unsigned char arena[2U * (BLOCK_BYTES + GUARD_BYTES)] __ALIGNED(32);
#define GUARD_A ((uintptr_t)arena + BLOCK_BYTES)
#define GUARD_B ((uintptr_t)arena + 2U * BLOCK_BYTES + GUARD_BYTES)

static unsigned char pad_a[BLOCK_BYTES + PAD_BYTES] __ALIGNED(16);
static unsigned char pad_b[BLOCK_BYTES + PAD_BYTES] __ALIGNED(16);
static unsigned char out_plain[OUT_BYTES] __ALIGNED(16);
static unsigned char out_guarded[OUT_BYTES] __ALIGNED(16);

static const char *current_name = "none";
static volatile unsigned current_n;
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

/* Fill both copies of the sources: |a| values are distinct within a vector. */
#define FILL_SOURCES(T, UNIT, n)                                               \
    T *a = (T *)GUARD_A - (n), *b = (T *)GUARD_B - (n);                        \
    T *pa = (T *)pad_a, *pb = (T *)pad_b;                                      \
    (void)b; (void)pb;                                                         \
    memset(pad_a, 0x55, sizeof(pad_a));                                        \
    memset(pad_b, 0x55, sizeof(pad_b));                                        \
    for (unsigned i = 0; i < (n); ++i)                                         \
    {                                                                          \
        a[i] = pa[i] = (T)(((int)((i * 7U) % 13U) - 6) * (UNIT));              \
        b[i] = pb[i] = (T)(((int)((i * 5U) % 11U) - 5) * (UNIT));              \
    }

static void clear_outputs(void)
{
    memset(out_plain, 0x2a, sizeof(out_plain));
    memset(out_guarded, 0x2a, sizeof(out_guarded));
}

/* Output canaries sit one element before and after `count` elements of T. */
static int check_outputs(unsigned elem_bytes, unsigned count)
{
    unsigned used = elem_bytes * (count + 2U);
    if (memcmp(out_plain, out_guarded, used))
    {
        printf("FAIL: %s placement mismatch n=%u\n", current_name, current_n);
        return 1;
    }
    for (unsigned i = 0; i < elem_bytes; ++i)
        if (out_guarded[i] != 0x2a || out_guarded[elem_bytes * (count + 1U) + i] != 0x2a)
        {
            printf("FAIL: %s output canary n=%u\n", current_name, current_n);
            return 1;
        }
    return 0;
}

static int check_near(double got, double reference, double tolerance, unsigned index)
{
    if (fabs(got - reference) > tolerance)
    {
        printf("FAIL: %s numerical result n=%u index=%u got=%g expected=%g\n",
               current_name, current_n, index, got, reference);
        return 1;
    }
    return 0;
}

static double saturate(double x, double limit)
{
    if (x > limit) return limit;
    if (x < -limit - 1.0) return -limit - 1.0;
    return x;
}

/* Elementwise kernels: CALL(src_a, src_b, dst) runs the kernel; REF(i) is the
 * expected element i from a[] and b[]; TOL in LSB. */
#define ELEMENT_CASES(T, NAME, UNIT, NMAX, CALL, REF, TOL)                     \
    for (unsigned n = 1; n <= (NMAX); ++n)                                     \
    {                                                                          \
        FILL_SOURCES(T, UNIT, n)                                               \
        T *dp = (T *)out_plain + 1, *dg = (T *)out_guarded + 1;                \
        current_name = NAME;                                                   \
        current_n = n;                                                         \
        clear_outputs();                                                       \
        CALL(pa, pb, dp);                                                      \
        protect_guards();                                                      \
        CALL(a, b, dg);                                                        \
        ARM_MPU_Disable();                                                     \
        if (check_outputs(sizeof(T), n))                                       \
            return 1;                                                          \
        for (unsigned i = 0; i < n; ++i)                                       \
            if (check_near((double)dg[i], (REF), (TOL), i))                    \
                return 1;                                                      \
        ++cases;                                                               \
    }

/* Reductions: CALL(src_a, src_b, n, result pointer). */
#define REDUCTION_CASES(T, R, NAME, UNIT, NMAX, CALL, REF, TOL)                \
    for (unsigned n = 1; n <= (NMAX); ++n)                                     \
    {                                                                          \
        FILL_SOURCES(T, UNIT, n)                                               \
        R *rp = (R *)out_plain + 1, *rg = (R *)out_guarded + 1;                \
        current_name = NAME;                                                   \
        current_n = n;                                                         \
        clear_outputs();                                                       \
        CALL(pa, pb, n, rp);                                                   \
        protect_guards();                                                      \
        CALL(a, b, n, rg);                                                     \
        ARM_MPU_Disable();                                                     \
        if (check_outputs(sizeof(R), 1))                                       \
            return 1;                                                          \
        if (check_near((double)*rg, (REF), (TOL), 0))                          \
            return 1;                                                          \
        ++cases;                                                               \
    }

/* Matrices of ROWS_MAX rows by 1..COLS_MAX columns: CALL(A, B, D) on instances. */
#define MATRIX_CASES(T, TAG, NAME, UNIT, ROWS_MAX, COLS_MAX, CALL, REF, TOL)   \
    for (unsigned rows = 1; rows <= (ROWS_MAX); ++rows)                        \
        for (unsigned cols = 1; cols <= (COLS_MAX); ++cols)                    \
        {                                                                      \
            unsigned n = rows * cols;                                          \
            FILL_SOURCES(T, UNIT, n)                                           \
            T *dp = (T *)out_plain + 1, *dg = (T *)out_guarded + 1;            \
            arm_matrix_instance_##TAG PA = {(uint16_t)rows, (uint16_t)cols, pa}; \
            arm_matrix_instance_##TAG PB = {(uint16_t)rows, (uint16_t)cols, pb}; \
            arm_matrix_instance_##TAG DP = {(uint16_t)rows, (uint16_t)cols, dp}; \
            arm_matrix_instance_##TAG A = {(uint16_t)rows, (uint16_t)cols, a};   \
            arm_matrix_instance_##TAG B = {(uint16_t)rows, (uint16_t)cols, b};   \
            arm_matrix_instance_##TAG D = {(uint16_t)rows, (uint16_t)cols, dg};  \
            (void)PB; (void)B;                                                 \
            current_name = NAME;                                               \
            current_n = n;                                                     \
            clear_outputs();                                                   \
            if (CALL(PA, PB, DP) != ARM_MATH_SUCCESS)                          \
            {                                                                  \
                printf("FAIL: %s status rows=%u cols=%u\n", NAME, rows, cols); \
                return 1;                                                      \
            }                                                                  \
            protect_guards();                                                  \
            arm_status status = CALL(A, B, D);                                 \
            ARM_MPU_Disable();                                                 \
            if (status != ARM_MATH_SUCCESS)                                    \
            {                                                                  \
                printf("FAIL: %s status rows=%u cols=%u\n", NAME, rows, cols); \
                return 1;                                                      \
            }                                                                  \
            if (check_outputs(sizeof(T), n))                                   \
                return 1;                                                      \
            for (unsigned i = 0; i < n; ++i)                                   \
                if (check_near((double)dg[i], (REF), (TOL), i))                \
                    return 1;                                                  \
            ++cases;                                                           \
        }

/* Kernel adapters, so the case macros can take a uniform call shape. */
#define CALL_abs(T, TAG) static void call_abs_##T(const T *x, const T *y, T *d) { (void)y; arm_abs_##TAG(x, d, current_n); }
#define CALL_negate(T, TAG) static void call_negate_##T(const T *x, const T *y, T *d) { (void)y; arm_negate_##TAG(x, d, current_n); }
#define CALL_offset(T, TAG, OFF) static void call_offset_##T(const T *x, const T *y, T *d) { (void)y; arm_offset_##TAG(x, (T)(OFF), d, current_n); }
#define CALL_scale(T, TAG, FRACT) static void call_scale_##T(const T *x, const T *y, T *d) { (void)y; arm_scale_##TAG(x, (T)(FRACT), 1, d, current_n); }
#define CALL_shift(T, TAG) static void call_shift_##T(const T *x, const T *y, T *d) { (void)y; arm_shift_##TAG(x, 1, d, current_n); }
#define CALL_add(T, TAG) static void call_add_##T(const T *x, const T *y, T *d) { arm_add_##TAG(x, y, d, current_n); }
#define CALL_sub(T, TAG) static void call_sub_##T(const T *x, const T *y, T *d) { arm_sub_##TAG(x, y, d, current_n); }
#define CALL_mult(T, TAG) static void call_mult_##T(const T *x, const T *y, T *d) { arm_mult_##TAG(x, y, d, current_n); }
#define CALL_not(T, TAG) static void call_not_##T(const T *x, const T *y, T *d) { (void)y; arm_not_##TAG(x, d, current_n); }
#define CALL_and(T, TAG) static void call_and_##T(const T *x, const T *y, T *d) { arm_and_##TAG(x, y, d, current_n); }
#define CALL_or(T, TAG) static void call_or_##T(const T *x, const T *y, T *d) { arm_or_##TAG(x, y, d, current_n); }
#define CALL_xor(T, TAG) static void call_xor_##T(const T *x, const T *y, T *d) { arm_xor_##TAG(x, y, d, current_n); }

/* Double-precision references for the reductions. arm_mse_T halves both
 * inputs before differencing and returns the mean square scaled by 2^(2-BITS)
 * (the scalar kernels shift the mean by BITS - 2). */
#define DEFINE_REFS(T)                                                         \
static double dot_ref_##T(const T *x, const T *y, unsigned n)                  \
{                                                                              \
    double s = 0.0;                                                            \
    for (unsigned i = 0; i < n; ++i) s += (double)x[i] * (double)y[i];         \
    return s;                                                                  \
}                                                                              \
static double mse_ref_##T(const T *x, const T *y, unsigned n)                  \
{                                                                              \
    double s = 0.0;                                                            \
    for (unsigned i = 0; i < n; ++i)                                           \
    {                                                                          \
        double d = (double)(x[i] >> 1) - (double)(y[i] >> 1);                  \
        s += d * d;                                                            \
    }                                                                          \
    return s / (double)n;                                                      \
}
DEFINE_REFS(q7_t)
DEFINE_REFS(q15_t)
DEFINE_REFS(q31_t)

/* Signed types: LIMIT is the positive saturation value, UNIT the data step,
 * NMAX two full vectors, SCALE_DIV the divisor of arm_scale_T's product. */
#define DEFINE_SIGNED(T, TAG, LIMIT, UNIT, NMAX, SCALE_DIV, DOT_T, DOT_DIV)     \
CALL_abs(T, TAG) CALL_negate(T, TAG) CALL_offset(T, TAG, 3 * (UNIT)) CALL_scale(T, TAG, (LIMIT) / 2 + 1) \
CALL_shift(T, TAG) CALL_add(T, TAG) CALL_sub(T, TAG) CALL_mult(T, TAG)          \
static void call_dot_##T(const T *x, const T *y, unsigned n, DOT_T *r) { arm_dot_prod_##TAG(x, y, n, r); } \
static void call_mse_##T(const T *x, const T *y, unsigned n, T *r) { arm_mse_##TAG(x, y, n, r); } \
static int run_##T(void)                                                       \
{                                                                              \
    const double limit = (double)(LIMIT);                                      \
    ELEMENT_CASES(T, "arm_abs_" #TAG, UNIT, NMAX, call_abs_##T, fabs((double)a[i]), 0.0) \
    ELEMENT_CASES(T, "arm_negate_" #TAG, UNIT, NMAX, call_negate_##T, -(double)a[i], 0.0) \
    ELEMENT_CASES(T, "arm_offset_" #TAG, UNIT, NMAX, call_offset_##T, (double)a[i] + 3.0 * (UNIT), 0.0) \
    ELEMENT_CASES(T, "arm_scale_" #TAG, UNIT, NMAX, call_scale_##T,              \
                  saturate((double)a[i] * ((double)(LIMIT) / 2 + 1) / (SCALE_DIV) * 2.0, limit), 2.0) \
    ELEMENT_CASES(T, "arm_shift_" #TAG, UNIT, NMAX, call_shift_##T, saturate(2.0 * (double)a[i], limit), 0.0) \
    ELEMENT_CASES(T, "arm_add_" #TAG, UNIT, NMAX, call_add_##T, (double)a[i] + (double)b[i], 0.0) \
    ELEMENT_CASES(T, "arm_sub_" #TAG, UNIT, NMAX, call_sub_##T, (double)a[i] - (double)b[i], 0.0) \
    ELEMENT_CASES(T, "arm_mult_" #TAG, UNIT, NMAX, call_mult_##T,                \
                  (double)a[i] * (double)b[i] / (limit + 1.0), 2.0)            \
    REDUCTION_CASES(T, DOT_T, "arm_dot_prod_" #TAG, UNIT, NMAX, call_dot_##T,    \
                    dot_ref_##T(a, b, n) / (DOT_DIV), (double)n + 2.0)       \
    REDUCTION_CASES(T, T, "arm_mse_" #TAG, UNIT, NMAX, call_mse_##T,             \
                    mse_ref_##T(a, b, n) * 4.0 / (limit + 1.0), 4.0)               \
    return 0;                                                                  \
}


/* Element types: q7 16 lanes, q15 8, q31 4; two full vectors each.
 * arm_scale_T scales by fract / 2^BITS and shifts left by shift (q31: >> 32, << shift + 1).
 * arm_dot_prod_q7/q15 accumulate the raw products; arm_dot_prod_q31 shifts each by 14. */
DEFINE_SIGNED(q7_t, q7, 127, 8, 32, 128.0, q31_t, 1.0)
DEFINE_SIGNED(q15_t, q15, 32767, 2048, 16, 32768.0, q63_t, 1.0)
DEFINE_SIGNED(q31_t, q31, 2147483647, 134217728, 8, 2147483648.0, q63_t, 16384.0)

/* Bitwise kernels, exact comparison with the reference expression. */
#define DEFINE_BITWISE(T, TAG, NMAX)                                           \
CALL_and(T, TAG) CALL_or(T, TAG) CALL_xor(T, TAG) CALL_not(T, TAG)             \
static int run_##T(void)                                                       \
{                                                                              \
    ELEMENT_CASES(T, "arm_and_" #TAG, 37, NMAX, call_and_##T, (double)(T)(a[i] & b[i]), 0.0) \
    ELEMENT_CASES(T, "arm_or_" #TAG, 37, NMAX, call_or_##T, (double)(T)(a[i] | b[i]), 0.0) \
    ELEMENT_CASES(T, "arm_xor_" #TAG, 37, NMAX, call_xor_##T, (double)(T)(a[i] ^ b[i]), 0.0) \
    ELEMENT_CASES(T, "arm_not_" #TAG, 37, NMAX, call_not_##T, (double)(T)~a[i], 0.0) \
    return 0;                                                                  \
}
DEFINE_BITWISE(uint8_t, u8, 32)
DEFINE_BITWISE(uint16_t, u16, 16)

/* Matrix kernels. The q15 and q31 add/sub/scale kernels work on the flattened
 * matrix; rows 1 and 2 by 1 to two full vectors. arm_mat_scale_T applies
 * shift + 1 to the 2.30 (2.62) product. */
#define DEFINE_MATRIX(T, TAG, LIMIT, UNIT, COLS_MAX, SCALE_DIV)                \
static arm_status mat_add_##T(const arm_matrix_instance_##TAG *A, const arm_matrix_instance_##TAG *B, arm_matrix_instance_##TAG *D) { return arm_mat_add_##TAG(A, B, D); } \
static arm_status mat_sub_##T(const arm_matrix_instance_##TAG *A, const arm_matrix_instance_##TAG *B, arm_matrix_instance_##TAG *D) { return arm_mat_sub_##TAG(A, B, D); } \
static arm_status mat_scale_##T(const arm_matrix_instance_##TAG *A, const arm_matrix_instance_##TAG *B, arm_matrix_instance_##TAG *D) { (void)B; return arm_mat_scale_##TAG(A, (T)((LIMIT) / 2 + 1), 1, D); } \
static int run_mat_##T(void)                                                   \
{                                                                              \
    const double limit = (double)(LIMIT);                                      \
    MATRIX_CASES(T, TAG, "arm_mat_add_" #TAG, UNIT, 2, COLS_MAX, MAT_CALL(mat_add_##T), (double)a[i] + (double)b[i], 0.0) \
    MATRIX_CASES(T, TAG, "arm_mat_sub_" #TAG, UNIT, 2, COLS_MAX, MAT_CALL(mat_sub_##T), (double)a[i] - (double)b[i], 0.0) \
    MATRIX_CASES(T, TAG, "arm_mat_scale_" #TAG, UNIT, 2, COLS_MAX, MAT_CALL(mat_scale_##T), \
                 saturate((double)a[i] * ((double)(LIMIT) / 2 + 1) / (SCALE_DIV) * 2.0, limit), 2.0) \
    return 0;                                                                  \
}
#define MAT_CALL(F) F##_call
#define mat_add_q15_t_call(A, B, D) mat_add_q15_t(&A, &B, &D)
#define mat_sub_q15_t_call(A, B, D) mat_sub_q15_t(&A, &B, &D)
#define mat_scale_q15_t_call(A, B, D) mat_scale_q15_t(&A, &B, &D)
#define mat_add_q31_t_call(A, B, D) mat_add_q31_t(&A, &B, &D)
#define mat_sub_q31_t_call(A, B, D) mat_sub_q31_t(&A, &B, &D)
#define mat_scale_q31_t_call(A, B, D) mat_scale_q31_t(&A, &B, &D)
DEFINE_MATRIX(q15_t, q15, 32767, 2048, 16, 32768.0)
DEFINE_MATRIX(q31_t, q31, 2147483647, 134217728, 8, 2147483648.0)

/* arm_mat_trans_q7 gathers one column per output row; the tail handles
 * numRows % 8 rows with an unpredicated gather. Rows 1 to 9, columns 1 to 3. */
static int run_mat_trans_q7(void)
{
    for (unsigned rows = 1; rows <= 9U; ++rows)
        for (unsigned cols = 1; cols <= 3U; ++cols)
        {
            unsigned n = rows * cols;
            FILL_SOURCES(q7_t, 8, n)
            q7_t *dp = (q7_t *)out_plain + 1, *dg = (q7_t *)out_guarded + 1;
            arm_matrix_instance_q7 PA = {(uint16_t)rows, (uint16_t)cols, pa};
            arm_matrix_instance_q7 DP = {(uint16_t)cols, (uint16_t)rows, dp};
            arm_matrix_instance_q7 A = {(uint16_t)rows, (uint16_t)cols, a};
            arm_matrix_instance_q7 D = {(uint16_t)cols, (uint16_t)rows, dg};
            current_name = "arm_mat_trans_q7";
            current_n = n;
            clear_outputs();
            if (arm_mat_trans_q7(&PA, &DP) != ARM_MATH_SUCCESS)
            {
                printf("FAIL: arm_mat_trans_q7 status rows=%u cols=%u\n", rows, cols);
                return 1;
            }
            protect_guards();
            arm_status status = arm_mat_trans_q7(&A, &D);
            ARM_MPU_Disable();
            if (status != ARM_MATH_SUCCESS)
            {
                printf("FAIL: arm_mat_trans_q7 status rows=%u cols=%u\n", rows, cols);
                return 1;
            }
            if (check_outputs(sizeof(q7_t), n))
                return 1;
            for (unsigned r = 0; r < rows; ++r)
                for (unsigned c = 0; c < cols; ++c)
                    if (dg[c * rows + r] != a[r * cols + c])
                    {
                        printf("FAIL: arm_mat_trans_q7 result rows=%u cols=%u\n", rows, cols);
                        return 1;
                    }
            ++cases;
        }
    return 0;
}

int main(void)
{
    regression_console_init();
    SCB->SHCSR |= SCB_SHCSR_MEMFAULTENA_Msk;
    if (run_q7_t() || run_q15_t() || run_q31_t() || run_uint8_t() || run_uint16_t() ||
        run_mat_q15_t() || run_mat_q31_t() || run_mat_trans_q7())
        return 1;
    printf("PASS: %u MVE fixed-point tail-load cases\n", cases);
    return 0;
}
