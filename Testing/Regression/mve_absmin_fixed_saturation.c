/* Copyright (c) 2026 Ambiq Micro, Inc.
 * SPDX-License-Identifier: Apache-2.0
 */
/* arm_absmin_q7/q15/q31 with the most negative input value.
 *
 * The scalar paths saturate the absolute value of the most negative value
 * (0x80, 0x8000, 0x80000000) to the most positive value. Each case checks
 * that the returned value is the minimum of the saturated absolute values and
 * that the returned index is below blockSize and points to an element whose
 * saturated absolute value equals it. Which of several equal minima the index
 * names is not checked.
 *
 * Inputs: random magnitudes in [max/8, max/2] with random signs, and
 *   one   the most negative value at one position (every position up to 40
 *         elements, every 37th beyond)
 *   all   every element the most negative value
 *   small the most negative value at one position and +-3 at another
 *   max   the most negative value at one position and +-max at another
 * for 22 lengths from 1 to 513, which cover the vector tails and the q7 path
 * for blocks above 256 elements.
 */
#include <stdint.h>
#include <stdio.h>
#include "arm_math.h"

extern void regression_console_init(void);

static const uint32_t lengths[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 15, 16, 17, 31, 32, 33,
                                   37, 255, 256, 257, 300, 512, 513};
#define NLEN (sizeof(lengths) / sizeof(lengths[0]))
#define MAXN 513U
enum { K_ONE, K_ALL, K_SMALL, K_MAX, NKIND };
static const char *const kind_name[NKIND] = {"one", "all", "small", "max"};

static uint32_t lcg = 12345U;
static uint32_t rnd(void)
{
    lcg = lcg * 1664525U + 1013904223U;
    return lcg >> 8;
}

static unsigned cases, failures;

/* The kernels load whole vectors in the tail, so each input has one vector of
   slack after MAXN elements. */
#define DEFINE_TYPE(S, T, TMIN, TMAX)                                                        \
    static T in_##S[MAXN + 16U];                                                             \
    static T sat_##S(T x) { return x == TMIN ? TMAX : (x < 0 ? (T)-x : x); }                 \
    static void fill_##S(int kind, uint32_t n, uint32_t pos)                                 \
    {                                                                                        \
        for (uint32_t i = 0; i < n; i++)                                                     \
        {                                                                                    \
            T v = (T)(TMAX / 8 + (T)(rnd() % (uint32_t)(TMAX / 2 - TMAX / 8)));              \
            in_##S[i] = (rnd() & 1U) ? (T)-v : v;                                            \
        }                                                                                    \
        if (kind == K_ALL)                                                                   \
        {                                                                                    \
            for (uint32_t i = 0; i < n; i++)                                                 \
            {                                                                                \
                in_##S[i] = TMIN;                                                            \
            }                                                                                \
            return;                                                                          \
        }                                                                                    \
        in_##S[pos] = TMIN;                                                                  \
        if (n > 1U && kind == K_SMALL)                                                       \
        {                                                                                    \
            in_##S[(pos + 1U + rnd() % (n - 1U)) % n] = (T)((rnd() & 1U) ? 3 : -3);          \
        }                                                                                    \
        if (n > 1U && kind == K_MAX)                                                         \
        {                                                                                    \
            in_##S[(pos + 1U + rnd() % (n - 1U)) % n] = (T)((rnd() & 1U) ? TMAX : -TMAX);    \
        }                                                                                    \
    }                                                                                        \
    static void run_##S(void)                                                                \
    {                                                                                        \
        for (int kind = 0; kind < NKIND; kind++)                                             \
        {                                                                                    \
            for (uint32_t l = 0; l < NLEN; l++)                                              \
            {                                                                                \
                uint32_t n = lengths[l];                                                     \
                for (uint32_t pos = 0; pos < n; pos += (n > 40U ? 37U : 1U))                 \
                {                                                                            \
                    if (kind == K_ALL && pos > 0U)                                           \
                    {                                                                        \
                        break;                                                               \
                    }                                                                        \
                    fill_##S(kind, n, pos);                                                  \
                    T expected = sat_##S(in_##S[0]);                                         \
                    for (uint32_t i = 1; i < n; i++)                                         \
                    {                                                                        \
                        if (sat_##S(in_##S[i]) < expected)                                   \
                        {                                                                    \
                            expected = sat_##S(in_##S[i]);                                   \
                        }                                                                    \
                    }                                                                        \
                    T value;                                                                 \
                    uint32_t index;                                                          \
                    arm_absmin_##S(in_##S, n, &value, &index);                               \
                    cases++;                                                                 \
                    if (value != expected || index >= n || sat_##S(in_##S[index]) != value)  \
                    {                                                                        \
                        if (failures++ == 0U)                                                \
                        {                                                                    \
                            printf("first failure: arm_absmin_" #S " %s n=%lu pos=%lu "      \
                                   "returned %ld at index %lu, expected %ld\n",              \
                                   kind_name[kind], (unsigned long)n, (unsigned long)pos,    \
                                   (long)value, (unsigned long)index, (long)expected);       \
                        }                                                                    \
                    }                                                                        \
                }                                                                            \
            }                                                                                \
        }                                                                                    \
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
    {
        printf("FAIL: %u of %u MVE absmin fixed-point saturation cases\n", failures, cases);
    }
    else
    {
        printf("PASS: %u MVE absmin fixed-point saturation cases\n", cases);
    }
    return 0;
}
