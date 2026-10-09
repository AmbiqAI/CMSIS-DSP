/* Copyright (c) 2026 Ambiq Micro, Inc.
 * SPDX-License-Identifier: Apache-2.0
 */
/* Cortex-M55 regression: the Q15 and Q31 FIR decimators with the state
 * buffer, of the documented length numTaps + blockSize - 1, or the input
 * block ending at an inaccessible MPU region. Companion to
 * mve_fir_decimate_f32_state.c.
 */
#include "ARMCM55.h"
#include "dsp/filtering_functions.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(ARM_MATH_MVEI) || defined(ARM_MATH_AUTOVECTORIZE)
#error This regression requires the handwritten MVE integer kernels.
#endif

#define MAX_TAPS 20U
#define MAX_M 5U
#define MAX_OUT 9U
#define CALLS 2U
#define MAX_BLOCK (MAX_M * MAX_OUT)
#define STATE_LEN (MAX_TAPS + MAX_BLOCK - 1U)
#define GUARD_BYTES 32U

/* Datatypes under test, selectable per build so that a negative control can
 * link one unmodified upstream kernel while the other is absent. */
#ifndef DECIM_TEST_Q15
#define DECIM_TEST_Q15 1
#endif
#ifndef DECIM_TEST_Q31
#define DECIM_TEST_Q31 1
#endif

extern void regression_console_init(void);
/* Data area rounded up to the 32-byte MPU granule, so that the guard begins
 * exactly at the end of the buffer under test. */
#define ARENA_DATA ((4U * (CALLS * MAX_BLOCK + STATE_LEN) + 31U) & ~31U)
static unsigned char arena[ARENA_DATA + GUARD_BYTES] __ALIGNED(32);
static volatile unsigned current_bits, current_taps, current_m, current_out;
static const char *volatile current_buffer;

void MemManage_Handler(void)
{
    ARM_MPU_Disable();
    printf("FAIL: Q%u decimator %s access taps=%u M=%u outputs=%u\n", current_bits, current_buffer,
           current_taps, current_m, current_out);
    exit(1);
}

static void protect_tail(uintptr_t guard)
{
    ARM_MPU_Disable();
    for (uint32_t region = 0; region < ((MPU->TYPE >> 8) & 0xffU); ++region)
    {
        ARM_MPU_ClrRegion(region);
    }
    ARM_MPU_SetMemAttr(0, ARM_MPU_ATTR(ARM_MPU_ATTR_NON_CACHEABLE,
                                     ARM_MPU_ATTR_NON_CACHEABLE));
    /* Map everything except the aligned 32-byte guard. No background map. */
    ARM_MPU_SetRegion(0, ARM_MPU_RBAR(0, ARM_MPU_SH_NON, 0, 1, 0),
                     ARM_MPU_RLAR(guard - 1U, 0));
    ARM_MPU_SetRegion(1, ARM_MPU_RBAR(guard + GUARD_BYTES, ARM_MPU_SH_NON, 0, 1, 0),
                     ARM_MPU_RLAR(UINT32_MAX, 0));
    ARM_MPU_Enable(0);
}

/* Each case runs two consecutive blocks three times: with every buffer in
 * readable memory followed by nonzero padding, with the state buffer ending at
 * the guard, and with the input ending at the guard. The three outputs must be
 * identical, and within one LSB of a 64-bit reference (Q15: exact shift and
 * saturation as documented; Q31: the MVE kernel accumulates with rounding
 * high-part instructions).
 */
#define DEFINE_CASES(T, BITS, XSCALE, CSCALE, PAD)                                       \
static T##_t coeffs_##T[MAX_TAPS], input_##T[CALLS * MAX_BLOCK + 16U];                     \
static T##_t state_##T[STATE_LEN + 16U];                                                   \
static T##_t out_##T[3][CALLS * MAX_OUT + 2U];                                             \
static void run_once_##T(unsigned taps, unsigned m, unsigned nout, T##_t *state, const T##_t *in, T##_t *out) \
{                                                                                          \
    unsigned block = m * nout;                                                             \
    arm_fir_decimate_instance_##T s;                                                       \
    if (arm_fir_decimate_init_##T(&s, (uint16_t)taps, (uint8_t)m, coeffs_##T, state, block) != ARM_MATH_SUCCESS) \
    {                                                                                      \
        printf("FAIL: Q%u decimator init taps=%u M=%u outputs=%u\n", (unsigned)BITS, taps, m, nout); \
        exit(1);                                                                           \
    }                                                                                      \
    for (unsigned k = 0; k < CALLS; ++k)                                                   \
        arm_fir_decimate_##T(&s, in + k * block, out + 1 + k * nout, block);               \
}                                                                                          \
static int run_##T(unsigned *checks)                                                       \
{                                                                                          \
    T##_t *end = (T##_t *)(arena + sizeof(arena) - GUARD_BYTES);                           \
    current_bits = BITS;                                                                   \
    for (unsigned taps = 1; taps <= MAX_TAPS; ++taps)                                      \
    for (unsigned m = 1; m <= MAX_M; ++m)                                                  \
    for (unsigned nout = 1; nout <= MAX_OUT; ++nout)                                       \
    {                                                                                      \
        unsigned block = m * nout, nin = CALLS * block, state_len = taps + block - 1U;     \
        for (unsigned i = 0; i < taps; ++i)                                                \
            coeffs_##T[i] = (T##_t)(((int)(i % 9) - 4) * (CSCALE));                        \
        for (unsigned i = 0; i < CALLS * MAX_BLOCK + 16U; ++i)                             \
            input_##T[i] = (T##_t)(PAD);                                                   \
        for (unsigned i = 0; i < nin; ++i)                                                 \
            input_##T[i] = (T##_t)(((int)((i * 5U) % 11U) - 5) * (XSCALE));                \
        for (unsigned i = 0; i < STATE_LEN + 16U; ++i)                                     \
            state_##T[i] = (T##_t)(PAD);                                                   \
        for (unsigned v = 0; v < 3; ++v)                                                   \
            for (unsigned i = 0; i < CALLS * MAX_OUT + 2U; ++i)                            \
                out_##T[v][i] = (T##_t)0x2a;                                               \
        current_taps = taps;                                                               \
        current_m = m;                                                                     \
        current_out = nout;                                                                \
        current_buffer = "padded";                                                         \
        run_once_##T(taps, m, nout, state_##T, input_##T, out_##T[0]);                     \
        current_buffer = "state";                                                          \
        protect_tail((uintptr_t)end);                                                      \
        run_once_##T(taps, m, nout, end - state_len, input_##T, out_##T[1]);               \
        ARM_MPU_Disable();                                                                 \
        T##_t *in = end - nin;                                                             \
        memcpy(in, input_##T, nin * sizeof(T##_t));                                        \
        current_buffer = "input";                                                          \
        protect_tail((uintptr_t)end);                                                      \
        run_once_##T(taps, m, nout, state_##T, in, out_##T[2]);                            \
        ARM_MPU_Disable();                                                                 \
        for (unsigned v = 0; v < 3; ++v)                                                   \
            if (out_##T[v][0] != (T##_t)0x2a || out_##T[v][1 + CALLS * nout] != (T##_t)0x2a) \
            {                                                                              \
                printf("FAIL: Q%u decimator output canary taps=%u M=%u outputs=%u\n",      \
                       (unsigned)BITS, taps, m, nout);                                     \
                return 1;                                                                  \
            }                                                                              \
        if (memcmp(out_##T[0], out_##T[1], sizeof(out_##T[0])) ||                          \
            memcmp(out_##T[0], out_##T[2], sizeof(out_##T[0])))                             \
        {                                                                                  \
            printf("FAIL: Q%u decimator placement mismatch taps=%u M=%u outputs=%u\n",     \
                   (unsigned)BITS, taps, m, nout);                                         \
            return 1;                                                                      \
        }                                                                                  \
        for (unsigned n = 0; n < CALLS * nout; ++n)                                        \
        {                                                                                  \
            long long sum = 0;                                                             \
            for (unsigned k = 0; k < taps; ++k)                                            \
            {                                                                              \
                int idx = (int)(n * m) - (int)(taps - 1U) + (int)k;                        \
                if (idx >= 0)                                                              \
                    sum += (long long)coeffs_##T[k] * input_##T[idx];                      \
            }                                                                              \
            long long ref = sum >> (BITS);                                                 \
            long long error = (long long)out_##T[0][n + 1] - ref;                          \
            if (error < -1 || error > 1)                                                   \
            {                                                                              \
                printf("FAIL: Q%u decimator numerical result taps=%u M=%u outputs=%u\n",   \
                       (unsigned)BITS, taps, m, nout);                                     \
                return 1;                                                                  \
            }                                                                              \
        }                                                                                  \
        ++*checks;                                                                         \
    }                                                                                      \
    return 0;                                                                              \
}

#if DECIM_TEST_Q15
DEFINE_CASES(q15, 15, 2000, 500, 0x5555)
#endif
#if DECIM_TEST_Q31
DEFINE_CASES(q31, 31, (1 << 26), (1 << 24), 0x55555555)
#endif

int main(void)
{
    regression_console_init();
    unsigned checks = 0;
#if DECIM_TEST_Q15
    if (run_q15(&checks)) return 1;
#endif
#if DECIM_TEST_Q31
    if (run_q31(&checks)) return 1;
#endif
    printf("PASS: %u MVE fixed-point decimator state cases\n", checks);
    return 0;
}
