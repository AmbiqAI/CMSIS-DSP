/* Copyright (c) 2026 Ambiq Micro, Inc.
 * SPDX-License-Identifier: Apache-2.0
 */
/* arm_fir_q15 above 32 taps and arm_fir_q7 above 64 taps, the general Helium
 * kernels, with the state sized to Arm's documented numTaps + blockSize - 1
 * samples and the input block ending at MPU guards.
 *
 * Each case initializes the filter and filters two consecutive blocks. The
 * state buffer ends at one guard and each input block at another, so any
 * read or store past either faults; the fault handler returns into a
 * function that clears VPR and unwinds to the next case. Every output is
 * compared bit for bit with an integer reference that follows Arm's
 * documented scaling (the full-precision sum, shifted right by 15 or 7 and
 * saturated). Tap counts: q15 34 to 48, even (Arm requires an even numTaps),
 * q7 65 to 80; block sizes 1 to 24 and 61 to 67; small and full-scale
 * coefficients. Coefficient arrays are zero-padded to a multiple of 8 (q15)
 * or 16 (q7), as Arm's Helium notes require. Built without
 * ARM_MATH_LOOPUNROLL, for which arm_fir_init_q15 documents and clears
 * numTaps + blockSize - 1 state samples.
 */
#include "ARMCM55.h"
#include "dsp/filtering_functions.h"
#include <setjmp.h>
#include <stdio.h>
#include <string.h>

#include "regression_checks.h"

#if !defined(ARM_MATH_MVEI) || defined(ARM_MATH_AUTOVECTORIZE)
#error This regression requires the handwritten MVE integer kernels.
#endif
#if defined(ARM_MATH_LOOPUNROLL)
#error Build without ARM_MATH_LOOPUNROLL: the state length checked here is numTaps + blockSize - 1.
#endif

#define MAX_TAPS 80U
#define MAX_BLOCK 67U
/* The unmodified kernels write up to about 3 * blockSize samples past the
 * state; the guards cover 512 bytes past each buffer (regression_checks.h). */
#define GUARD_BYTES REGRESSION_GUARD_BYTES(512U)
#define AREA_BYTES 512U

extern void regression_console_init(void);

static unsigned char state_area[AREA_BYTES + GUARD_BYTES] __ALIGNED(32);
static unsigned char input_area[AREA_BYTES + GUARD_BYTES] __ALIGNED(32);
#define STATE_END ((uintptr_t)state_area + AREA_BYTES)
#define INPUT_END ((uintptr_t)input_area + AREA_BYTES)

static const unsigned blocks[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16,
                                  17, 18, 19, 20, 21, 22, 23, 24, 61, 62, 63, 64, 65, 66, 67};
#define NBLOCKS (sizeof(blocks) / sizeof(blocks[0]))

static jmp_buf recover_env;
static unsigned cases, failures;

static void recover(void)
{
    __asm volatile("vmsr vpr, %0" : : "r"(0U));
    longjmp(recover_env, 1);
}

/* Rewrite the stacked PC so that the exception returns into recover(). */
void memmanage_c(uint32_t *frame)
{
    ARM_MPU_Disable();
    SCB->CFSR = SCB->CFSR;
    frame[6] = (uint32_t)recover & ~1U;
    frame[7] &= ~0x0600FC00U;
    frame[7] |= 1U << 24;
}

__attribute__((naked)) void MemManage_Handler(void)
{
    __asm volatile(
        "tst lr, #4\n"
        "ite eq\n"
        "mrseq r0, msp\n"
        "mrsne r0, psp\n"
        "b memmanage_c\n");
}

static void protect_guards(void)
{
    uintptr_t lo = STATE_END < INPUT_END ? STATE_END : INPUT_END;
    uintptr_t hi = STATE_END < INPUT_END ? INPUT_END : STATE_END;
    ARM_MPU_Disable();
    for (uint32_t region = 0; region < ((MPU->TYPE >> 8) & 0xffU); ++region)
        ARM_MPU_ClrRegion(region);
    ARM_MPU_SetMemAttr(0, ARM_MPU_ATTR(ARM_MPU_ATTR_NON_CACHEABLE, ARM_MPU_ATTR_NON_CACHEABLE));
    ARM_MPU_SetRegion(0, ARM_MPU_RBAR(0, ARM_MPU_SH_NON, 0, 1, 0), ARM_MPU_RLAR(lo - 1U, 0));
    ARM_MPU_SetRegion(1, ARM_MPU_RBAR(lo + GUARD_BYTES, ARM_MPU_SH_NON, 0, 1, 0),
                      ARM_MPU_RLAR(hi - 1U, 0));
    ARM_MPU_SetRegion(2, ARM_MPU_RBAR(hi + GUARD_BYTES, ARM_MPU_SH_NON, 0, 1, 0),
                      ARM_MPU_RLAR(UINT32_MAX, 0));
    ARM_MPU_Enable(0);
}

static uint32_t lcg = 2024U;
static uint32_t rnd(void) { lcg = lcg * 1664525U + 1013904223U; return lcg >> 8; }

#define DEFINE_TYPE(TAG, T, BITS, TMIN, TMAX, TAP_FIRST, TAP_LAST, TAP_STEP, PAD)              \
static T coeffs_##TAG[MAX_TAPS + PAD];                                                        \
static T signal_##TAG[2U * MAX_BLOCK];                                                        \
static T output_##TAG[MAX_BLOCK];                                                             \
static void run_##TAG(void)                                                                   \
{                                                                                             \
    for (unsigned set = 0; set < 2; ++set)                                                    \
        for (unsigned t = (TAP_FIRST); t <= (TAP_LAST); t += (TAP_STEP))                      \
            for (unsigned k = 0; k < NBLOCKS; ++k)                                            \
            {                                                                                 \
                volatile unsigned b = blocks[k], blk = 0;                                     \
                arm_fir_instance_##TAG S;                                                     \
                T *state = (T *)STATE_END - (t + b - 1U);                                     \
                memset(coeffs_##TAG, 0, sizeof(coeffs_##TAG));                                \
                for (unsigned i = 0; i < t; ++i)                                              \
                {                                                                             \
                    int32_t m = set ? (int32_t)(TMAX) - (int32_t)(rnd() % 8U)                 \
                                    : (int32_t)(rnd() % ((TMAX) / 64 + 1));                   \
                    coeffs_##TAG[i] = (T)((rnd() & 1U) ? -m : m);                             \
                }                                                                             \
                for (unsigned i = 0; i < 2U * b; ++i)                                         \
                    signal_##TAG[i] = (T)((int32_t)(rnd() % ((uint32_t)(TMAX) * 2U + 1U)) - (TMAX)); \
                ++cases;                                                                      \
                if (setjmp(recover_env))                                                      \
                {                                                                             \
                    if (failures++ == 0U)                                                     \
                        printf("first failure: arm_fir_" #TAG " numTaps=%u blockSize=%u block %u " \
                               "touched a guard\n", t, b, blk);                               \
                    continue;                                                                 \
                }                                                                             \
                protect_guards();                                                             \
                arm_fir_init_##TAG(&S, (uint16_t)t, coeffs_##TAG, state, b);                  \
                int ok = 1;                                                                   \
                for (blk = 0; blk < 2U && ok; ++blk)                                          \
                {                                                                             \
                    T *in = (T *)INPUT_END - b;                                               \
                    ARM_MPU_Disable();                                                        \
                    memcpy(in, signal_##TAG + blk * b, b * sizeof(T));                        \
                    protect_guards();                                                         \
                    arm_fir_##TAG(&S, in, output_##TAG, b);                                   \
                    REGRESSION_CONTROL_READ_ABOVE(STATE_END, GUARD_BYTES);                    \
                    ARM_MPU_Disable();                                                        \
                    for (unsigned n = 0; n < b && ok; ++n)                                    \
                    {                                                                         \
                        unsigned m = blk * b + n;                                             \
                        int64_t acc = 0;                                                      \
                        for (unsigned j = 0; j < t && j <= m; ++j)                            \
                            acc += (int64_t)coeffs_##TAG[t - 1U - j] * signal_##TAG[m - j];   \
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
                    protect_guards();                                                         \
                }                                                                             \
                ARM_MPU_Disable();                                                            \
                if (!ok)                                                                      \
                    ++failures;                                                               \
            }                                                                                 \
}

DEFINE_TYPE(q15, q15_t, 15, INT16_MIN, INT16_MAX, 34U, 48U, 2U, 8U)
DEFINE_TYPE(q7, q7_t, 7, INT8_MIN, INT8_MAX, 65U, 80U, 1U, 16U)

int main(void)
{
    regression_console_init();
    SCB->SHCSR |= SCB_SHCSR_MEMFAULTENA_Msk;
    run_q15();
    run_q7();
    if (failures != 0U)
        printf("FAIL: %u of %u MVE FIR q15/q7 state-bound cases\n", failures, cases);
    else
        printf("PASS: %u MVE FIR q15/q7 state-bound cases\n", cases);
    return 0;
}
