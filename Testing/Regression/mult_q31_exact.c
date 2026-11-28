/* Copyright (c) 2026 Ambiq Micro, Inc.
 * SPDX-License-Identifier: Apache-2.0
 */
/*
 * arm_mult_q31 against VQDMULH, bit for bit: sat((2 * a * b) >> 32). The cases
 * and their expected values come from run_mult_q31_exact.py, which computes
 * them in Python integers rather than with the kernel's own formula.
 */
#include "arm_math.h"
#include <stdio.h>

#include "mult_q31_cases.h"

#define CANARY ((q31_t)0x5A5A5A5A)

int main(void)
{
    static q31_t dst[MULT_Q31_COUNT + 1];
    unsigned cases = 0;
    for (unsigned s = 0; s < sizeof(MULT_Q31_SIZES) / sizeof(MULT_Q31_SIZES[0]); s++) {
        const uint32_t n = MULT_Q31_SIZES[s];
        for (uint32_t i = 0; i <= MULT_Q31_COUNT; i++) {
            dst[i] = CANARY;
        }
        arm_mult_q31(MULT_Q31_A, MULT_Q31_B, dst, n);
        for (uint32_t i = 0; i < n; i++) {
            if (dst[i] != MULT_Q31_EXPECTED[i]) {
                printf("FAIL: n=%u i=%u a=%ld b=%ld got %ld expected %ld\n", (unsigned)n, (unsigned)i,
                       (long)MULT_Q31_A[i], (long)MULT_Q31_B[i], (long)dst[i], (long)MULT_Q31_EXPECTED[i]);
                return 1;
            }
        }
        if (dst[n] != CANARY) {
            printf("FAIL: n=%u wrote past the end\n", (unsigned)n);
            return 1;
        }
        cases++;
    }
    printf("PASS: %u arm_mult_q31 sizes bit-exact (%s)\n", cases, MULT_Q31_BUILD);
    return 0;
}
