/* Copyright (c) 2026 Ambiq Micro, Inc.
 * SPDX-License-Identifier: Apache-2.0
 */
/* Cortex-M55 regression: the MVE SVM (polynomial, rbf, sigmoid) and
 * Gaussian naive Bayes predictors, f32 and f16, with each operand in turn
 * ending at an MPU-inaccessible region: the support vectors, the dual
 * coefficients and the input of the SVMs; the input, theta, sigma and class
 * priors of the Bayes predictor. Every guarded call must give the same
 * result as a call on operands followed by readable padding (the SVM class;
 * the Bayes class index and every class probability, bit for bit). The
 * padded result is also checked against a double-precision reference: the
 * SVM class where the decision value is clear of zero, and the Bayes class
 * index where the largest log-probability is clear of the next.
 */
#include "ARMCM55.h"
#include "dsp/svm_functions.h"
#include "dsp/svm_functions_f16.h"
#include "dsp/bayes_functions.h"
#include "dsp/bayes_functions_f16.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "regression_checks.h"

#if !defined(ARM_MATH_MVEF) || defined(ARM_MATH_AUTOVECTORIZE)
#error This regression requires the handwritten MVE kernels.
#endif

#define BLOCK_BYTES 1024U
#define GUARD_BYTES REGRESSION_GUARD_BYTES(BLOCK_BYTES)
#define MAXV 9U   /* support vectors */
#define MAXD 17U  /* vector dimension */
#define MAXC 5U   /* classes */

extern void regression_console_init(void);

static unsigned char arena[BLOCK_BYTES + GUARD_BYTES] __ALIGNED(32);
#define GUARD ((uintptr_t)arena + BLOCK_BYTES)

static const char *current_name = "none";
static volatile unsigned current_a, current_b, current_op;
static unsigned cases;

void MemManage_Handler(void)
{
    ARM_MPU_Disable();
    printf("FAIL: %s tail access (sizes %u x %u, operand %u at the guard)\n",
           current_name, current_a, current_b, current_op);
    exit(1);
}

static void protect_guard(void)
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

static void unguard(void)
{
    REGRESSION_CONTROL_READ_ABOVE(GUARD, GUARD_BYTES);
    ARM_MPU_Disable();
}

static const unsigned nsv[] = {1, 2, 3, 4, 5, 6, 7, 9};
static const unsigned dims[] = {1, 2, 3, 4, 5, 7, 8, 9, 13, 16, 17};
static const unsigned ncls[] = {1, 2, 3, 5};
#define NN(a) (sizeof(a) / sizeof(a[0]))

/* Places the n elements of src (element size sz) either at the end of the
 * guarded block or in padded storage, and returns the copy. */
static unsigned char padded[4][BLOCK_BYTES + 64] __ALIGNED(16);
static void *place(int guarded, unsigned slot, const void *src, unsigned bytes)
{
    unsigned char *d = guarded ? (unsigned char *)GUARD - bytes : padded[slot];
    if (!guarded)
        memset(padded[slot] + bytes, 0x3c, 64); /* finite padding */
    memcpy(d, src, bytes);
    return d;
}

enum { K_POLY, K_RBF, K_SIGMOID };
static const char *kname[] = {"polynomial", "rbf", "sigmoid"};

static double kernel_ref(int k, const double *x, const double *sv, unsigned d)
{
    double dot = 0.0, dist = 0.0;
    for (unsigned i = 0; i < d; ++i)
    {
        dot += x[i] * sv[i];
        dist += (x[i] - sv[i]) * (x[i] - sv[i]);
    }
    switch (k)
    {
    case K_POLY: return pow(0.5 * dot + 0.25, 3);
    case K_RBF: return exp(-0.5 * dist);
    default: return tanh(0.5 * dot + 0.25);
    }
}

/* SVM: CALL(S, in, &cls) for kernel k on type T. */
#define DEFINE_SVM(T, TAG)                                                                   \
static int run_svm_##TAG(int k)                                                             \
{                                                                                            \
    static T sv0[MAXV * MAXD], dc0[MAXV], in0[MAXD];                                         \
    static const int32_t classes[2] = {7, 11};                                              \
    for (unsigned a = 0; a < NN(nsv); ++a)                                                   \
        for (unsigned b = 0; b < NN(dims); ++b)                                              \
        {                                                                                    \
            unsigned v = nsv[a], d = dims[b];                                                \
            double svd[MAXV * MAXD], dcd[MAXV], ind[MAXD];                                   \
            for (unsigned i = 0; i < v * d; ++i)                                             \
                svd[i] = 0.125 * (double)((int)((i * 7U + 3U) % 9U) - 4);                    \
            for (unsigned i = 0; i < v; ++i)                                                 \
                dcd[i] = (i & 1U) ? -0.75 + 0.125 * i : 0.5 + 0.0625 * i;                    \
            for (unsigned i = 0; i < d; ++i)                                                 \
                ind[i] = 0.125 * (double)((int)((i * 5U + 1U) % 7U) - 3);                    \
            for (unsigned i = 0; i < v * d; ++i) sv0[i] = (T)svd[i];                         \
            for (unsigned i = 0; i < v; ++i) dc0[i] = (T)dcd[i];                             \
            for (unsigned i = 0; i < d; ++i) in0[i] = (T)ind[i];                             \
            double sum = 0.1;                                                                \
            for (unsigned i = 0; i < v; ++i)                                                 \
                sum += (double)dc0[i] * kernel_ref(k, ind, svd + i * d, d);                  \
            int32_t ref = -1, got[4];                                                        \
            for (int op = -1; op < 3; ++op)                                                  \
            {                                                                                \
                T *svp = place(op == 0, 0, sv0, sizeof(T) * v * d);                          \
                T *dcp = place(op == 1, 1, dc0, sizeof(T) * v);                              \
                T *inp = place(op == 2, 2, in0, sizeof(T) * d);                              \
                current_name = "arm_svm_" #TAG; current_a = v; current_b = d; current_op = (unsigned)op; \
                int32_t cls = -1;                                                            \
                if (op >= 0) protect_guard();                                                \
                if (k == K_POLY)                                                             \
                {                                                                            \
                    arm_svm_polynomial_instance_##TAG S;                                     \
                    arm_svm_polynomial_init_##TAG(&S, v, d, (T)0.1, dcp, svp, classes, 3, (T)0.25, (T)0.5); \
                    arm_svm_polynomial_predict_##TAG(&S, inp, &cls);                         \
                }                                                                            \
                else if (k == K_RBF)                                                         \
                {                                                                            \
                    arm_svm_rbf_instance_##TAG S;                                            \
                    arm_svm_rbf_init_##TAG(&S, v, d, (T)0.1, dcp, svp, classes, (T)0.5);     \
                    arm_svm_rbf_predict_##TAG(&S, inp, &cls);                                \
                }                                                                            \
                else                                                                         \
                {                                                                            \
                    arm_svm_sigmoid_instance_##TAG S;                                        \
                    arm_svm_sigmoid_init_##TAG(&S, v, d, (T)0.1, dcp, svp, classes, (T)0.25, (T)0.5); \
                    arm_svm_sigmoid_predict_##TAG(&S, inp, &cls);                            \
                }                                                                            \
                if (op >= 0) unguard();                                                      \
                got[op + 1] = cls;                                                           \
            }                                                                                \
            ref = classes[sum < 0.0 ? 0 : 1];                                                \
            for (int op = 1; op < 4; ++op)                                                   \
                if (got[op] != got[0])                                                       \
                {                                                                            \
                    printf("FAIL: arm_svm_%s_predict_" #TAG " %ux%u: class %ld with operand %d guarded, %ld padded\n", \
                           kname[k], v, d, (long)got[op], op - 1, (long)got[0]);             \
                    return 1;                                                                \
                }                                                                            \
            if (fabs(sum) > 0.05 && got[0] != ref)                                           \
            {                                                                                \
                printf("FAIL: arm_svm_%s_predict_" #TAG " %ux%u: class %ld, reference %ld\n", \
                       kname[k], v, d, (long)got[0], (long)ref);                             \
                return 1;                                                                    \
            }                                                                                \
            ++cases;                                                                         \
        }                                                                                    \
    return 0;                                                                                \
}

#define DEFINE_BAYES(T, TAG)                                                                 \
static int run_bayes_##TAG(void)                                                            \
{                                                                                            \
    static T th0[MAXC * MAXD], sg0[MAXC * MAXD], pr0[MAXC], in0[MAXD], prob[MAXC], other[MAXC], buf[MAXC]; \
    for (unsigned a = 0; a < NN(ncls); ++a)                                                  \
        for (unsigned b = 0; b < NN(dims); ++b)                                              \
        {                                                                                    \
            unsigned c = ncls[a], d = dims[b];                                               \
            for (unsigned i = 0; i < c * d; ++i)                                             \
            {                                                                                \
                th0[i] = (T)(0.25 * (double)((int)((i * 7U + 2U) % 9U) - 4));                \
                sg0[i] = (T)(0.5 + 0.125 * (double)((i * 3U) % 5U));                         \
            }                                                                                \
            for (unsigned i = 0; i < c; ++i) pr0[i] = (T)(1.0 / c);                          \
            for (unsigned i = 0; i < d; ++i) in0[i] = (T)(0.25 * (double)((int)((i * 5U + 1U) % 7U) - 3)); \
            double best = -1e30, second = -1e30; unsigned refidx = 0;                        \
            for (unsigned j = 0; j < c; ++j)                                                 \
            {                                                                                \
                double l = log((double)pr0[j]);                                              \
                for (unsigned i = 0; i < d; ++i)                                             \
                {                                                                            \
                    double s = (double)sg0[j * d + i] + 1e-3, t = (double)in0[i] - (double)th0[j * d + i]; \
                    l -= 0.5 * log(2.0 * 3.14159265358979 * s) + 0.5 * t * t / s;            \
                }                                                                            \
                if (l > best) { second = best; best = l; refidx = j; } else if (l > second) second = l; \
            }                                                                                \
            uint32_t idx[5];                                                                 \
            for (int op = -1; op < 4; ++op)                                                  \
            {                                                                                \
                T *inp = place(op == 0, 0, in0, sizeof(T) * d);                              \
                T *thp = place(op == 1, 1, th0, sizeof(T) * c * d);                          \
                T *sgp = place(op == 2, 2, sg0, sizeof(T) * c * d);                          \
                T *prp = place(op == 3, 3, pr0, sizeof(T) * c);                              \
                arm_gaussian_naive_bayes_instance_##TAG S = {d, c, thp, sgp, prp, (T)1e-3};  \
                current_name = "arm_gaussian_naive_bayes_predict_" #TAG; current_a = c; current_b = d; current_op = (unsigned)op; \
                T *pp = op < 0 ? prob : other;                                               \
                if (op >= 0) protect_guard();                                                \
                idx[op + 1] = arm_gaussian_naive_bayes_predict_##TAG(&S, inp, pp, buf);      \
                if (op >= 0) unguard();                                                      \
                if (op >= 0 && memcmp(other, prob, sizeof(T) * c))                           \
                {                                                                            \
                    printf("FAIL: arm_gaussian_naive_bayes_predict_" #TAG " %ux%u: probabilities differ with operand %d guarded\n", c, d, op); \
                    return 1;                                                                \
                }                                                                            \
            }                                                                                \
            for (int op = 1; op < 5; ++op)                                                   \
                if (idx[op] != idx[0])                                                       \
                {                                                                            \
                    printf("FAIL: arm_gaussian_naive_bayes_predict_" #TAG " %ux%u: index differs with operand %d guarded\n", c, d, op - 1); \
                    return 1;                                                                \
                }                                                                            \
            if (best - second > 0.5 && idx[0] != refidx)                                     \
            {                                                                                \
                printf("FAIL: arm_gaussian_naive_bayes_predict_" #TAG " %ux%u: class %lu, reference %u\n", c, d, (unsigned long)idx[0], refidx); \
                return 1;                                                                    \
            }                                                                                \
            ++cases;                                                                         \
        }                                                                                    \
    return 0;                                                                                \
}

DEFINE_SVM(float32_t, f32)
DEFINE_SVM(float16_t, f16)
DEFINE_BAYES(float32_t, f32)
DEFINE_BAYES(float16_t, f16)

int main(void)
{
    regression_console_init();
    SCB->SHCSR |= SCB_SHCSR_MEMFAULTENA_Msk;
    for (int k = 0; k < 3; ++k)
        if (run_svm_f32(k) || run_svm_f16(k))
            return 1;
    if (run_bayes_f32() || run_bayes_f16())
        return 1;
    printf("PASS: %u MVE SVM and Bayes tail-load cases\n", cases);
    return 0;
}
