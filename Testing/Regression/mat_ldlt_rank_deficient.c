/* Copyright (c) 2026 Ambiq Micro, Inc.
 * SPDX-License-Identifier: Apache-2.0
 */
/* arm_mat_ldlt_f32 and arm_mat_ldlt_f64 on rank-deficient and positive
 * definite inputs.
 *
 * Each case checks P A P^t = L D L^t, with P built from pp by the successive
 * row and column swaps Arm's tests use, within an absolute error of 1e-5
 * (f32) or 1e-10 (f64), relative to the largest element of A; D must be zero
 * off its diagonal and L unit lower triangular on its first rank columns.
 * Inputs:
 *   - exact rank-deficient matrices: [[4, 0], [0, 0]], a rank-2 3x3, the
 *     all-ones 3x3 and a 5x5 of rank 2;
 *   - Arm's semidefinite ("SDPO") and positive definite ("DPO") LDL^t test
 *     patterns from Testing/Patterns, which the runner turns into
 *     ldlt_patterns.h.
 * The runner builds this file for the Helium kernels and for the scalar code
 * (Cortex-M55 with and without MVE); f64 is scalar in both.
 */
#include "ARMCM55.h"
#include "dsp/matrix_functions.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "ldlt_patterns.h"

extern void regression_console_init(void);

#define MAXN 33
static unsigned cases, failures;

#define DEFINE_CHECK(T, TAG, TOL)                                                           \
static void check_##TAG(const char *name, unsigned n, const T *a)                           \
{                                                                                           \
    static T A[MAXN * MAXN], L[MAXN * MAXN], D[MAXN * MAXN], PA[MAXN * MAXN];               \
    uint16_t pp[MAXN];                                                                      \
    memcpy(A, a, sizeof(T) * n * n);                                                        \
    memset(L, 0, sizeof(L));                                                                \
    memset(D, 0, sizeof(D));                                                                \
    arm_matrix_instance_##TAG SA = {(uint16_t)n, (uint16_t)n, A};                           \
    arm_matrix_instance_##TAG SL = {(uint16_t)n, (uint16_t)n, L};                           \
    arm_matrix_instance_##TAG SD = {(uint16_t)n, (uint16_t)n, D};                           \
    arm_status status = arm_mat_ldlt_##TAG(&SA, &SL, &SD, pp);                              \
    memcpy(PA, a, sizeof(T) * n * n);                                                       \
    double scale = 0.0;                                                                     \
    for (unsigned i = 0; i < n * n; ++i)                                                    \
        if (fabs((double)a[i]) > scale)                                                     \
            scale = fabs((double)a[i]);                                                     \
    for (unsigned k = 0; k < n; ++k)                                                        \
    {                                                                                       \
        unsigned j = pp[k];                                                                 \
        if (j == k)                                                                         \
            continue;                                                                       \
        for (unsigned c = 0; c < n; ++c) { T t = PA[k * n + c]; PA[k * n + c] = PA[j * n + c]; PA[j * n + c] = t; } \
        for (unsigned r = 0; r < n; ++r) { T t = PA[r * n + k]; PA[r * n + k] = PA[r * n + j]; PA[r * n + j] = t; } \
    }                                                                                       \
    double err = 0.0;                                                                       \
    int shape = 0;                                                                          \
    for (unsigned i = 0; i < n; ++i)                                                        \
        for (unsigned j = 0; j < n; ++j)                                                    \
        {                                                                                   \
            double r = 0.0;                                                                 \
            for (unsigned k = 0; k < n; ++k)                                                \
                r += (double)L[i * n + k] * (double)D[k * n + k] * (double)L[j * n + k];    \
            if (fabs(r - (double)PA[i * n + j]) > err)                                      \
                err = fabs(r - (double)PA[i * n + j]);                                      \
            if ((i != j && D[i * n + j] != 0) || (j > i && L[i * n + j] != 0))              \
                shape = 1;                                                                  \
        }                                                                                   \
    ++cases;                                                                                \
    if (status != ARM_MATH_SUCCESS || shape || err > (TOL) * (scale > 1.0 ? scale : 1.0))   \
    {                                                                                       \
        ++failures;                                                                         \
        printf("FAIL: arm_mat_ldlt_" #TAG " %s n=%u: status %d, max |PAP^t - LDL^t| %.3g%s\n", \
               name, n, (int)status, err, shape ? ", D or L not of the documented form" : ""); \
    }                                                                                       \
}
DEFINE_CHECK(float32_t, f32, 1e-5)
DEFINE_CHECK(float64_t, f64, 1e-10)

/* Exact rank-deficient inputs, as double; each is checked in both types. */
static const double m2[] = {4, 0, 0, 0};
static const double m3a[] = {4, 2, 0, 2, 2, 0, 0, 0, 0};
static const double m3b[] = {1, 1, 1, 1, 1, 1, 1, 1, 1};
/* u u^t + v v^t with u = (1, 2, 0, 1, 1), v = (0, 1, 1, -1, 2): rank 2. */
static double m5[25];

static void exact_cases(void)
{
    static const double u[5] = {1, 2, 0, 1, 1}, v[5] = {0, 1, 1, -1, 2};
    for (unsigned i = 0; i < 5; ++i)
        for (unsigned j = 0; j < 5; ++j)
            m5[i * 5 + j] = u[i] * u[j] + v[i] * v[j];
    const struct { const char *name; unsigned n; const double *m; } list[] = {
        {"[[4,0],[0,0]]", 2, m2}, {"rank-2 3x3", 3, m3a}, {"all-ones 3x3", 3, m3b}, {"rank-2 5x5", 5, m5},
    };
    for (unsigned c = 0; c < sizeof(list) / sizeof(list[0]); ++c)
    {
        float32_t f[MAXN * MAXN];
        unsigned n = list[c].n;
        for (unsigned i = 0; i < n * n; ++i)
            f[i] = (float32_t)list[c].m[i];
        check_f32(list[c].name, n, f);
        check_f64(list[c].name, n, list[c].m);
    }
}

#define PATTERN_CASES(TAG, NAME, DIMS, NDIMS, DATA)                                          \
    do {                                                                                    \
        unsigned off = 0;                                                                   \
        for (unsigned k = 0; k < (NDIMS); ++k)                                              \
        {                                                                                   \
            check_##TAG(NAME, (DIMS)[k], &(DATA)[off]);                                     \
            off += (DIMS)[k] * (DIMS)[k];                                                   \
        }                                                                                   \
    } while (0)

int main(void)
{
    regression_console_init();
#if defined(ARM_MATH_MVEF) && !defined(ARM_MATH_AUTOVECTORIZE)
    printf("arm_mat_ldlt_f32: Helium kernel\n");
#else
    printf("arm_mat_ldlt_f32: scalar code\n");
#endif
    exact_cases();
    PATTERN_CASES(f32, "SDPO pattern", sdpo_dims, SDPO_N, sdpo_f32);
    PATTERN_CASES(f64, "SDPO pattern", sdpo_dims, SDPO_N, sdpo_f64);
    PATTERN_CASES(f32, "DPO pattern", dpo_dims, DPO_N, dpo_f32);
    PATTERN_CASES(f64, "DPO pattern", dpo_dims, DPO_N, dpo_f64);
    if (failures != 0U)
        printf("FAIL: %u of %u LDLt reconstruction cases\n", failures, cases);
    else
        printf("PASS: %u LDLt reconstruction cases\n", cases);
    return 0;
}
