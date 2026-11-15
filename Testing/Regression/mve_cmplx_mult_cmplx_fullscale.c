/* Copyright (c) 2026 Ambiq Micro, Inc.
 * SPDX-License-Identifier: Apache-2.0
 */
/* arm_cmplx_mult_cmplx_q15/q31 at and near full scale.
 *
 * Arm documents the output as 3.13 (q15) or 3.29 (q31) and states, for q31,
 * that input down scaling is not required. The scalar code computes each output from two
 * products shifted right by 17 or 33, which cannot overflow. Every output is
 * compared bit for bit with that arithmetic:
 *   re = (a c >> s) - (b d >> s),  im = (a d >> s) + (b c >> s).
 *
 * Input kinds:
 *   rot      (0.75, 0.75) times exp(j theta), 64 angles: a product component
 *            reaches magnitude 1.06
 *   unit     0.999 exp(j phi) times 0.999 exp(j theta)
 *   extreme  every combination of six values, among them the most negative,
 *            the most negative plus one and the most positive, in a, b, c, d
 *   random   full-scale random values
 * Lengths 1 to 33, 63 to 65 and 100 (every tail of both types). The extreme
 * kind also runs 1296 samples (all 6^4 combinations in one call) and lengths
 * 1 to 7 starting at the all-most-negative sample, which puts the largest
 * products in the tail. Each case runs with a separate destination, in place
 * over the first source and in place over the second; the destination starts
 * filled with a canary, which the words after it must keep. The sources are
 * not placed at an MPU guard.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include "arm_math.h"

extern void regression_console_init(void);

#define MAXN 1296U
#define SLACK 16U
enum { K_ROT, K_UNIT, K_EXTREME, K_RANDOM, NKIND };
static const char *const kind_name[NKIND] = {"rot", "unit", "extreme", "random"};

static uint32_t lcg = 2026U;
static uint32_t rnd(void)
{
    lcg = lcg * 1664525U + 1013904223U;
    return lcg >> 8;
}

static unsigned cases, failures;
static unsigned long differing, clipped;
static long worst;

#define DEFINE_TYPE(S, T, W, TMIN, TMAX, SHIFT, CANARY)                                      \
    static T a_##S[2U * MAXN + SLACK], b_##S[2U * MAXN + SLACK];                             \
    static T out_##S[2U * MAXN + SLACK], ref_##S[2U * MAXN];                                 \
    static T fs_##S(double x)                                                                \
    {                                                                                        \
        double v = floor(x * -(double)TMIN + 0.5);                                           \
        return v > (double)TMAX ? TMAX : (v < (double)TMIN ? TMIN : (T)v);                   \
    }                                                                                        \
    static T rnd_##S(void)                                                                   \
    {                                                                                        \
        uint32_t r = rnd();                                                                  \
        return (T)((r << 16) ^ rnd());                                                       \
    }                                                                                        \
    static void fill_##S(int kind, uint32_t n, uint32_t salt)                                \
    {                                                                                        \
        static const T ext[6] = {TMIN, (T)(TMIN + 1), TMAX, 0,                               \
                                 (T)(TMAX / 2 + 1), (T)-(TMAX / 4 * 3)};                     \
        const double pi = 3.14159265358979323846;                                            \
        for (uint32_t i = 0; i < n; i++)                                                     \
        {                                                                                    \
            uint32_t k = i + salt;                                                           \
            T *a = &a_##S[2U * i], *b = &b_##S[2U * i];                                      \
            double th = 2.0 * pi * (double)(k % 64U) / 64.0;                                 \
            double ph = 2.0 * pi * (double)((k / 64U + 3U * k) % 61U) / 61.0;                \
            switch (kind)                                                                    \
            {                                                                                \
            case K_ROT:                                                                      \
                a[0] = fs_##S((k & 64U) ? -0.75 : 0.75);                                     \
                a[1] = fs_##S((k & 128U) ? -0.75 : 0.75);                                    \
                b[0] = fs_##S(cos(th));                                                      \
                b[1] = fs_##S(sin(th));                                                      \
                break;                                                                       \
            case K_UNIT:                                                                     \
                a[0] = fs_##S(0.999 * cos(ph));                                              \
                a[1] = fs_##S(0.999 * sin(ph));                                              \
                b[0] = fs_##S(0.999 * cos(th));                                              \
                b[1] = fs_##S(0.999 * sin(th));                                              \
                break;                                                                       \
            case K_EXTREME:                                                                  \
                a[0] = ext[k % 6U];                                                          \
                a[1] = ext[(k / 6U) % 6U];                                                   \
                b[0] = ext[(k / 36U) % 6U];                                                  \
                b[1] = ext[(k / 216U) % 6U];                                                 \
                break;                                                                       \
            default:                                                                         \
                a[0] = rnd_##S();                                                            \
                a[1] = rnd_##S();                                                            \
                b[0] = rnd_##S();                                                            \
                b[1] = rnd_##S();                                                            \
                break;                                                                       \
            }                                                                                \
            ref_##S[2U * i] = (T)((((W)a[0] * b[0]) >> SHIFT) - (((W)a[1] * b[1]) >> SHIFT)); \
            ref_##S[2U * i + 1U] = (T)((((W)a[0] * b[1]) >> SHIFT) + (((W)a[1] * b[0]) >> SHIFT)); \
        }                                                                                    \
        for (uint32_t i = 2U * n; i < 2U * n + SLACK; i++)                                   \
        {                                                                                    \
            a_##S[i] = CANARY;                                                               \
            b_##S[i] = CANARY;                                                               \
        }                                                                                    \
        for (uint32_t i = 0; i < 2U * n + SLACK; i++)                                        \
            out_##S[i] = CANARY;                                                             \
    }                                                                                        \
    static void case_##S(int kind, uint32_t n, uint32_t salt)                                \
    {                                                                                        \
        static const char *const place[3] = {"separate", "in place over A", "in place over B"}; \
        for (int mode = 0; mode < 3; mode++)                                                 \
        {                                                                                    \
            fill_##S(kind, n, salt);                                                         \
            T *dst = mode == 0 ? out_##S : (mode == 1 ? a_##S : b_##S);                      \
            arm_cmplx_mult_cmplx_##S(a_##S, b_##S, dst, n);                                  \
            int bad = 0;                                                                     \
            for (uint32_t i = 0; i < 2U * n; i++)                                            \
            {                                                                                \
                int64_t d = (int64_t)dst[i] - (int64_t)ref_##S[i];                           \
                if (d == 0)                                                                  \
                    continue;                                                                \
                d = d < 0 ? -d : d;                                                          \
                differing++;                                                                 \
                clipped += d > 1;                                                            \
                worst = d > worst ? (long)d : worst;                                         \
                if (!bad && failures == 0U)                                                  \
                    printf("first failure: arm_cmplx_mult_cmplx_" #S " %s n=%lu %s, element " \
                           "%lu: (%ld, %ld) x (%ld, %ld) returned %ld, expected %ld\n",      \
                           kind_name[kind], (unsigned long)n, place[mode], (unsigned long)i, \
                           (long)(mode == 1 ? 0 : a_##S[i & ~1U]),                           \
                           (long)(mode == 1 ? 0 : a_##S[i | 1U]),                            \
                           (long)(mode == 2 ? 0 : b_##S[i & ~1U]),                           \
                           (long)(mode == 2 ? 0 : b_##S[i | 1U]),                            \
                           (long)dst[i], (long)ref_##S[i]);                                  \
                bad = 1;                                                                     \
            }                                                                                \
            for (uint32_t i = 2U * n; i < 2U * n + SLACK; i++)                               \
                if (dst[i] != CANARY)                                                        \
                {                                                                            \
                    if (!bad && failures == 0U)                                              \
                        printf("first failure: arm_cmplx_mult_cmplx_" #S " %s n=%lu %s "     \
                               "wrote element %lu past the output\n", kind_name[kind],       \
                               (unsigned long)n, place[mode], (unsigned long)i);             \
                    bad = 1;                                                                 \
                }                                                                            \
            cases++;                                                                         \
            failures += (unsigned)bad;                                                       \
        }                                                                                    \
    }                                                                                        \
    static void run_##S(void)                                                                \
    {                                                                                        \
        for (int kind = 0; kind < NKIND; kind++)                                             \
        {                                                                                    \
            for (uint32_t n = 1; n <= 33U; n++)                                              \
                case_##S(kind, n, 37U * n);                                                  \
            for (uint32_t n = 63U; n <= 65U; n++)                                            \
                case_##S(kind, n, 0U);                                                       \
            case_##S(kind, 100U, 64U);                                                       \
        }                                                                                    \
        case_##S(K_EXTREME, MAXN, 0U);                                                       \
        for (uint32_t n = 1; n <= 7U; n++)                                                   \
            case_##S(K_EXTREME, n, 0U);                                                      \
    }

DEFINE_TYPE(q15, q15_t, int32_t, INT16_MIN, INT16_MAX, 17, (q15_t)0x5a5a)
DEFINE_TYPE(q31, q31_t, int64_t, INT32_MIN, INT32_MAX, 33, (q31_t)0x5a5a5a5a)

int main(void)
{
    regression_console_init();
#if defined(ARM_MATH_MVEI) && !defined(ARM_MATH_AUTOVECTORIZE)
    printf("arm_cmplx_mult_cmplx: Helium kernels\n");
#else
    printf("arm_cmplx_mult_cmplx: scalar code\n");
#endif
    run_q15();
    run_q31();
    if (failures != 0U)
        printf("FAIL: %u of %u complex multiply full-scale cases (%lu outputs differ, %lu by more "
               "than 1 LSB, largest difference %ld LSB)\n", failures, cases, differing, clipped, worst);
    else
        printf("PASS: %u complex multiply full-scale cases\n", cases);
    return 0;
}
