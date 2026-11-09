/* Copyright (c) 2026 Ambiq Micro, Inc.
 * SPDX-License-Identifier: Apache-2.0
 */
/* arm_absmin_q7/q15/q31 on inputs whose minimum absolute value can occur
 * more than once.
 *
 * The scalar paths return the first element with the minimum saturated
 * absolute value (they replace the minimum only on a strictly smaller value).
 * Each case checks that the returned value is that minimum and that the
 * returned index is the first element with it.
 *
 * Inputs, for 52 lengths from 1 to 513 (every tail remainder of each type and
 * the q7 path above 256 elements), 12 draws each:
 *   dup   random magnitudes from max/8 (below max/2; below max/8 + 2^24 for
 *         q31) with random signs, and a smaller magnitude at 2 to 4 random
 *         positions, which can coincide
 *   sat   every element the most negative value or +-max, so the saturated
 *         absolute values all tie
 *   const one random magnitude at every element, with random signs
 */
#include <stdint.h>
#include <stdio.h>
#include "arm_math.h"

extern void regression_console_init(void);

static const uint32_t lengths[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17,
                                   18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33,
                                   63, 64, 65, 127, 128, 129, 255, 256, 257, 258, 300,
                                   383, 384, 385, 400, 511, 512, 513, 509};
#define NLEN (sizeof(lengths) / sizeof(lengths[0]))
#define MAXN 513U
#define DRAWS 12U
enum { K_DUP, K_SAT, K_CONST, NKIND };
static const char *const kind_name[NKIND] = {"dup", "sat", "const"};

static uint32_t lcg = 2026U;
static uint32_t rnd(void)
{
    lcg = lcg * 1664525U + 1013904223U;
    return lcg >> 8;
}

static unsigned cases, failures;

/* One vector of slack after MAXN elements, as in the saturation regression. */
#define DEFINE_TYPE(S, T, TMIN, TMAX)                                                        \
    static T in_##S[MAXN + 16U];                                                             \
    static T sat_##S(T x) { return x == TMIN ? TMAX : (x < 0 ? (T)-x : x); }                 \
    static T sign_##S(T v) { return (rnd() & 1U) ? (T)-v : v; }                              \
    static void fill_##S(int kind, uint32_t n)                                               \
    {                                                                                        \
        if (kind == K_SAT)                                                                   \
        {                                                                                    \
            for (uint32_t i = 0; i < n; i++)                                                 \
                in_##S[i] = (rnd() % 3U == 0U) ? TMIN : sign_##S(TMAX);                      \
            return;                                                                          \
        }                                                                                    \
        if (kind == K_CONST)                                                                 \
        {                                                                                    \
            T v = (T)(1 + (T)(rnd() % (uint32_t)(TMAX - 1)));                                \
            for (uint32_t i = 0; i < n; i++)                                                 \
                in_##S[i] = sign_##S(v);                                                     \
            return;                                                                          \
        }                                                                                    \
        for (uint32_t i = 0; i < n; i++)                                                     \
            in_##S[i] = sign_##S((T)(TMAX / 8 + (T)(rnd() % (uint32_t)(TMAX / 2 - TMAX / 8)))); \
        T small = (T)(rnd() % (uint32_t)(TMAX / 8));                                         \
        uint32_t copies = 2U + rnd() % 3U;                                                   \
        for (uint32_t c = 0; c < copies; c++)                                                \
        {                                                                                    \
            uint32_t at = rnd() % n;                                                         \
            in_##S[at] = sign_##S(small);                                                    \
        }                                                                                    \
    }                                                                                        \
    static void run_##S(void)                                                                \
    {                                                                                        \
        for (int kind = 0; kind < NKIND; kind++)                                             \
            for (uint32_t l = 0; l < NLEN; l++)                                              \
                for (uint32_t d = 0; d < DRAWS; d++)                                         \
                {                                                                            \
                    uint32_t n = lengths[l];                                                 \
                    fill_##S(kind, n);                                                       \
                    T expected = sat_##S(in_##S[0]);                                         \
                    uint32_t first = 0;                                                      \
                    for (uint32_t i = 1; i < n; i++)                                         \
                        if (sat_##S(in_##S[i]) < expected)                                   \
                        {                                                                    \
                            expected = sat_##S(in_##S[i]);                                   \
                            first = i;                                                       \
                        }                                                                    \
                    T value;                                                                 \
                    uint32_t index;                                                          \
                    arm_absmin_##S(in_##S, n, &value, &index);                               \
                    cases++;                                                                 \
                    if (value != expected || index != first)                                 \
                    {                                                                        \
                        if (failures++ == 0U)                                                \
                            printf("first failure: arm_absmin_" #S " %s n=%lu draw %lu "     \
                                   "returned %ld at index %lu, expected %ld at index %lu\n", \
                                   kind_name[kind], (unsigned long)n, (unsigned long)d,      \
                                   (long)value, (unsigned long)index, (long)expected,        \
                                   (unsigned long)first);                                    \
                    }                                                                        \
                }                                                                            \
    }

DEFINE_TYPE(q7, q7_t, INT8_MIN, INT8_MAX)
DEFINE_TYPE(q15, q15_t, INT16_MIN, INT16_MAX)
DEFINE_TYPE(q31, q31_t, INT32_MIN, INT32_MAX)

int main(void)
{
    regression_console_init();
    run_q7();
    run_q15();
    run_q31();
    if (failures != 0U)
        printf("FAIL: %u of %u MVE absmin fixed-point tie-index cases\n", failures, cases);
    else
        printf("PASS: %u MVE absmin fixed-point tie-index cases\n", cases);
    return 0;
}
