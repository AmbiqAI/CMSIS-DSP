/* Copyright (c) 2026 Ambiq Micro, Inc.
 * SPDX-License-Identifier: Apache-2.0
 */
/* arm_fir_q31 at 29 to 32 taps and blockSize 32 or more, the case that uses
 * the Helium kernel arm_fir_q31_29_32_mve, at block sizes with every
 * remainder modulo 4.
 *
 * Each case filters three consecutive blocks and compares every output bit
 * for bit with an integer reference that follows the scalar kernel: the
 * 64-bit sum of products shifted right by 31. Inputs are scaled down by
 * log2(numTaps) bits, as Arm requires to avoid accumulator overflow, so the
 * reference equals the scalar kernel's result. Output buffers start filled
 * with a canary, so an output that is not written fails. Taps 29 to 32,
 * block sizes 32 to 48 and 61 to 67; small and full-scale coefficients.
 */
#include "ARMCM55.h"
#include "dsp/filtering_functions.h"
#include <stdio.h>
#include <string.h>

#if !defined(ARM_MATH_MVEI) || defined(ARM_MATH_AUTOVECTORIZE)
#error This regression requires the handwritten MVE integer kernels.
#endif

extern void regression_console_init(void);

#define MAX_BLOCK 67U
#define CANARY ((q31_t)0x5a5a5a5a)
static const unsigned blocks[] = {32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44,
                                  45, 46, 47, 48, 61, 62, 63, 64, 65, 66, 67};
#define NBLOCKS (sizeof(blocks) / sizeof(blocks[0]))

/* Coefficients padded to a multiple of 4; state of the Helium length
 * numTaps + 8 * ceil(blockSize / 4) + blockSize - 1, with slack. */
static q31_t coeffs[32 + 4];
static q31_t state[32 + 8 * ((MAX_BLOCK + 3) / 4) + MAX_BLOCK + 16];
static q31_t input[3 * MAX_BLOCK];
static q31_t output[MAX_BLOCK + 4];

static uint32_t lcg = 77U;
static uint32_t rnd(void) { lcg = lcg * 1664525U + 1013904223U; return lcg; }

int main(void)
{
    unsigned cases = 0, failures = 0;
    regression_console_init();
    for (unsigned set = 0; set < 2; ++set)
        for (unsigned t = 29; t <= 32; ++t)
            for (unsigned k = 0; k < NBLOCKS; ++k)
            {
                unsigned b = blocks[k];
                arm_fir_instance_q31 S;
                memset(coeffs, 0, sizeof(coeffs));
                for (unsigned i = 0; i < t; ++i)
                    coeffs[i] = set ? (q31_t)rnd() : (q31_t)rnd() >> 12;
                for (unsigned i = 0; i < 3U * b; ++i)
                    input[i] = (q31_t)rnd() >> 5; /* log2(32) bits of headroom */
                arm_fir_init_q31(&S, (uint16_t)t, coeffs, state, b);
                int ok = 1;
                for (unsigned blk = 0; blk < 3U && ok; ++blk)
                {
                    for (unsigned i = 0; i < MAX_BLOCK + 4; ++i)
                        output[i] = CANARY;
                    arm_fir_q31(&S, input + blk * b, output, b);
                    for (unsigned n = 0; n < b && ok; ++n)
                    {
                        unsigned m = blk * b + n;
                        int64_t acc = 0;
                        for (unsigned j = 0; j < t && j <= m; ++j)
                            acc += (int64_t)coeffs[t - 1U - j] * input[m - j];
                        q31_t expected = (q31_t)(acc >> 31);
                        if (output[n] != expected)
                        {
                            ok = 0;
                            if (failures == 0U)
                                printf("first failure: arm_fir_q31 %s coefficients, numTaps=%u blockSize=%u "
                                       "block %u output %u: got 0x%08lx, expected 0x%08lx%s\n",
                                       set ? "full-scale" : "small", t, b, blk, n,
                                       (unsigned long)output[n], (unsigned long)expected,
                                       output[n] == CANARY ? " (not written)" : "");
                        }
                    }
                }
                ++cases;
                if (!ok)
                    ++failures;
            }
    if (failures != 0U)
        printf("FAIL: %u of %u MVE FIR q31 29-32 tap remainder cases\n", failures, cases);
    else
        printf("PASS: %u MVE FIR q31 29-32 tap remainder cases\n", cases);
    return 0;
}
