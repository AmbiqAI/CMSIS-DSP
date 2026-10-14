/* Copyright (c) 2026 Ambiq Micro, Inc.
 * SPDX-License-Identifier: Apache-2.0
 */
/* arm_fir_q15 above 32 taps and arm_fir_q7 above 64 taps, the general Helium
 * kernels, at block sizes with every remainder modulo 4.
 *
 * Each case filters two consecutive blocks and compares every output bit for
 * bit with an integer reference that follows Arm's documented scaling: the
 * full-precision sum of products, shifted right by 15 (q15) or 7 (q7) and
 * saturated. Tap counts: q15 34 to 48, even (Arm requires an even numTaps),
 * q7 65 to 80. Block sizes 1 to 24 and 61 to 67. Two coefficient sets: small
 * coefficients, and full-scale coefficients with random signs, whose outputs
 * also exercise the saturation. Coefficient arrays are zero-padded to a
 * multiple of 8 (q15) or 16 (q7), as Arm's Helium notes require. The state
 * buffers are larger than Arm's documented numTaps + blockSize - 1: these
 * general kernels copy input into the state ahead of use (8 or 16 samples per
 * 4 outputs), which this regression does not test.
 */
#include "ARMCM55.h"
#include "dsp/filtering_functions.h"
#include <stdio.h>
#include <string.h>

#if !defined(ARM_MATH_MVEI) || defined(ARM_MATH_AUTOVECTORIZE)
#error This regression requires the handwritten MVE integer kernels.
#endif

extern void regression_console_init(void);

#define MAX_TAPS 80U
#define MAX_BLOCK 67U
static const unsigned blocks[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16,
                                  17, 18, 19, 20, 21, 22, 23, 24, 61, 62, 63, 64, 65, 66, 67};
#define NBLOCKS (sizeof(blocks) / sizeof(blocks[0]))

static uint32_t lcg = 2024U;
static uint32_t rnd(void) { lcg = lcg * 1664525U + 1013904223U; return lcg >> 8; }

static unsigned cases, failures;

#define DEFINE_TYPE(TAG, T, BITS, TMIN, TMAX, TAP_FIRST, TAP_LAST, TAP_STEP, PAD)              \
static T coeffs_##TAG[MAX_TAPS + PAD], state_##TAG[MAX_TAPS + 4U * MAX_BLOCK + 64U];          \
static T input_##TAG[5U * MAX_BLOCK + 64U], output_##TAG[MAX_BLOCK + 16U];                     \
static void run_##TAG(void)                                                                   \
{                                                                                             \
    for (unsigned set = 0; set < 2; ++set)                                                    \
        for (unsigned t = (TAP_FIRST); t <= (TAP_LAST); t += (TAP_STEP))                      \
            for (unsigned k = 0; k < NBLOCKS; ++k)                                            \
            {                                                                                 \
                unsigned b = blocks[k];                                                       \
                arm_fir_instance_##TAG S;                                                     \
                memset(coeffs_##TAG, 0, sizeof(coeffs_##TAG));                                \
                for (unsigned i = 0; i < t; ++i)                                              \
                {                                                                             \
                    int32_t m = set ? (int32_t)(TMAX) - (int32_t)(rnd() % 8U)                 \
                                    : (int32_t)(rnd() % ((TMAX) / 64 + 1));                   \
                    coeffs_##TAG[i] = (T)((rnd() & 1U) ? -m : m);                             \
                }                                                                             \
                for (unsigned i = 0; i < 2U * b; ++i)                                         \
                    input_##TAG[i] = (T)((int32_t)(rnd() % ((uint32_t)(TMAX) * 2U + 1U)) - (TMAX)); \
                memset(state_##TAG, 0, sizeof(state_##TAG));                                  \
                arm_fir_init_##TAG(&S, (uint16_t)t, coeffs_##TAG, state_##TAG, b);            \
                int ok = 1;                                                                   \
                for (unsigned blk = 0; blk < 2U && ok; ++blk)                                 \
                {                                                                             \
                    arm_fir_##TAG(&S, input_##TAG + blk * b, output_##TAG, b);                \
                    for (unsigned n = 0; n < b && ok; ++n)                                    \
                    {                                                                         \
                        unsigned m = blk * b + n;                                             \
                        int64_t acc = 0;                                                      \
                        for (unsigned j = 0; j < t && j <= m; ++j)                            \
                            acc += (int64_t)coeffs_##TAG[t - 1U - j] * input_##TAG[m - j];    \
                        acc >>= (BITS);                                                       \
                        acc = acc < (TMIN) ? (TMIN) : (acc > (TMAX) ? (TMAX) : acc);          \
                        if (output_##TAG[n] != (T)acc)                                        \
                        {                                                                     \
                            ok = 0;                                                           \
                            if (failures == 0U)                                               \
                                printf("first failure: arm_fir_" #TAG " %s coefficients, numTaps=%u " \
                                       "blockSize=%u block %u output %u: got %d, expected %d\n", \
                                       set ? "full-scale" : "small", t, b, blk, n,            \
                                       (int)output_##TAG[n], (int)acc);                       \
                        }                                                                     \
                    }                                                                         \
                }                                                                             \
                ++cases;                                                                      \
                if (!ok)                                                                      \
                    ++failures;                                                               \
            }                                                                                 \
}

DEFINE_TYPE(q15, q15_t, 15, INT16_MIN, INT16_MAX, 34U, 48U, 2U, 8U)
DEFINE_TYPE(q7, q7_t, 7, INT8_MIN, INT8_MAX, 65U, 80U, 1U, 16U)

int main(void)
{
    regression_console_init();
    run_q15();
    run_q7();
    if (failures != 0U)
        printf("FAIL: %u of %u MVE FIR q15/q7 remainder cases\n", failures, cases);
    else
        printf("PASS: %u MVE FIR q15/q7 remainder cases\n", cases);
    return 0;
}
