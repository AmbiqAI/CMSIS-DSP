/* Copyright (c) 2026 Ambiq Micro, Inc.
 * SPDX-License-Identifier: Apache-2.0
 */
/* arm_mat_qr_f32, arm_mat_qr_f16 and arm_mat_qr_f64 with the m x m matrix Q
 * requested, for m x n inputs with m > n and for square inputs.
 *
 * The n entries of tau and the m x n entries of R are followed by memory
 * filled with 0.37; Arm documents tau as n long. Each case checks that Q is
 * orthogonal, max |Q^t Q - I|, and that Q times the upper triangle of R
 * reproduces A, within 1e-5 (f32), 2e-2 (f16) or 1e-12 (f64). The runner
 * builds this file with the Helium kernels and with the scalar code
 * (Cortex-M55 and Cortex-M55+nomve); f64 is scalar in both. SKIP_F16 leaves
 * out f16: Arm GNU 14.3.1 compiles the scalar f16 code for Cortex-M55+nomve
 * with Advanced SIMD lane loads and stores (vld1.16, vst1.16), which an
 * M-profile core does not implement.
 */
#include "ARMCM55.h"
#include "dsp/matrix_functions.h"
#include "dsp/matrix_functions_f16.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

extern void regression_console_init(void);

#define MAXM 20U
#define SLACK 32U
static const unsigned shapes[][2] = {{2, 2}, {3, 3}, {8, 8}, {16, 16}, {3, 2}, {4, 2}, {5, 3},
                                     {8, 4}, {9, 4}, {16, 8}, {17, 5}, {20, 3}};
#define NSHAPES (sizeof(shapes) / sizeof(shapes[0]))
static unsigned cases, failures;

#define DEFINE_TYPE(T, TAG, TOL)                                                           \
static T a0_##TAG[MAXM * MAXM], a_##TAG[MAXM * MAXM], r_##TAG[MAXM * MAXM + SLACK];        \
static T q_##TAG[MAXM * MAXM], tau_##TAG[MAXM + SLACK], ta_##TAG[MAXM], tb_##TAG[MAXM];    \
static void run_##TAG(void)                                                                \
{                                                                                          \
    for (unsigned s = 0; s < NSHAPES; ++s)                                                 \
    {                                                                                      \
        unsigned m = shapes[s][0], n = shapes[s][1];                                       \
        for (unsigned i = 0; i < m * n; ++i)                                               \
            a0_##TAG[i] = (T)(((int)((i * 37U + 11U) % 23U) - 11) / 8.0);                  \
        memcpy(a_##TAG, a0_##TAG, sizeof(T) * m * n);                                      \
        for (unsigned i = 0; i < MAXM * MAXM + SLACK; ++i) r_##TAG[i] = (T)0.37;           \
        for (unsigned i = 0; i < MAXM + SLACK; ++i) tau_##TAG[i] = (T)0.37;                \
        arm_matrix_instance_##TAG A = {(uint16_t)m, (uint16_t)n, a_##TAG};                 \
        arm_matrix_instance_##TAG R = {(uint16_t)m, (uint16_t)n, r_##TAG};                 \
        arm_matrix_instance_##TAG Q = {(uint16_t)m, (uint16_t)m, q_##TAG};                 \
        arm_status status = arm_mat_qr_##TAG(&A, (T)-1e-10, &R, &Q, tau_##TAG, ta_##TAG, tb_##TAG); \
        double eqr = 0.0, eqq = 0.0;                                                       \
        for (unsigned i = 0; i < m; ++i)                                                   \
            for (unsigned j = 0; j < n; ++j)                                               \
            {                                                                              \
                double v = 0.0;                                                            \
                for (unsigned k = 0; k <= j && k < m; ++k)                                 \
                    v += (double)q_##TAG[i * m + k] * (double)r_##TAG[k * n + j];          \
                if (fabs(v - (double)a0_##TAG[i * n + j]) > eqr) eqr = fabs(v - (double)a0_##TAG[i * n + j]); \
            }                                                                              \
        for (unsigned i = 0; i < m; ++i)                                                   \
            for (unsigned j = 0; j < m; ++j)                                               \
            {                                                                              \
                double v = 0.0;                                                            \
                for (unsigned k = 0; k < m; ++k)                                           \
                    v += (double)q_##TAG[k * m + i] * (double)q_##TAG[k * m + j];          \
                if (fabs(v - (i == j ? 1.0 : 0.0)) > eqq) eqq = fabs(v - (i == j ? 1.0 : 0.0)); \
            }                                                                              \
        ++cases;                                                                           \
        if (status != ARM_MATH_SUCCESS || eqr > (TOL) || eqq > (TOL))                      \
        {                                                                                  \
            ++failures;                                                                    \
            printf("FAIL: arm_mat_qr_" #TAG " %ux%u: status %d, max |QR - A| %.3g, max |Q^tQ - I| %.3g\n", \
                   m, n, (int)status, eqr, eqq);                                           \
        }                                                                                  \
    }                                                                                      \
}
DEFINE_TYPE(float32_t, f32, 1e-5)
DEFINE_TYPE(float16_t, f16, 2e-2)
DEFINE_TYPE(float64_t, f64, 1e-12)

int main(void)
{
    regression_console_init();
#if defined(ARM_MATH_MVEF) && !defined(ARM_MATH_AUTOVECTORIZE)
    printf("arm_mat_qr_f32/f16: Helium kernels\n");
#else
    printf("arm_mat_qr_f32/f16: scalar code\n");
#endif
    run_f32();
#if defined(SKIP_F16)
    printf("arm_mat_qr_f16: not built\n");
#else
    run_f16();
#endif
    run_f64();
    if (failures != 0U)
        printf("FAIL: %u of %u QR cases with Q\n", failures, cases);
    else
        printf("PASS: %u QR cases with Q\n", cases);
    return 0;
}
