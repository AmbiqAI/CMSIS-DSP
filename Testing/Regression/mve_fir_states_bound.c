/* Copyright (c) 2026 Ambiq Micro, Inc.
 * SPDX-License-Identifier: Apache-2.0
 */
/* Cortex-M55 regression: the handwritten MVE arm_fir_q7, arm_fir_q15 and
 * arm_fir_interpolate_f32/q15/q31 kernels must not read or store past Arm's
 * documented state length, nor read past their blockSize input samples:
 *   arm_fir_q7, arm_fir_q15       numTaps + blockSize - 1
 *                                 (arm_fir_q15 without ARM_MATH_LOOPUNROLL;
 *                                 numTaps + blockSize with it)
 *   arm_fir_interpolate_*         numTaps / L + blockSize - 1
 *
 * The state ends at an MPU-inaccessible guard, first with the documented
 * length and then with one extra element at a time until no access faults;
 * the input block ends at a second guard. A MemManage fault is recovered by
 * rewriting the stacked return address. Elements in the extra region are
 * canaries, so stores and loads past the documented length are told apart.
 * arm_fir_q7 runs at 1 to 64 taps and arm_fir_q15 at 2 to 32 taps (even, as
 * Arm requires), the fixed-tap kernels; the general kernels above those
 * counts have their own state regression. The interpolators run at L = 2, 3,
 * 4, 5 and 8 with phase lengths 1 to 12. Each case filters two consecutive blocks. Fixed-point outputs must equal
 * an integer reference that follows Arm's documented scaling (the
 * full-precision sum shifted right by 7 or 15 and saturated, or for Q31 the
 * 64-bit sum shifted right by 31); f32 outputs must lie within a relative
 * bound of a double-precision reference and be finite (regression_checks.h).
 */
#include "ARMCM55.h"
#include "dsp/filtering_functions.h"
#include <math.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>

#include "regression_checks.h"

#if !defined(ARM_MATH_MVEI) || !defined(ARM_MATH_MVEF) || defined(ARM_MATH_AUTOVECTORIZE)
#error This regression requires the handwritten MVE kernels.
#endif
#if defined(ARM_MATH_LOOPUNROLL)
#error Build without ARM_MATH_LOOPUNROLL: the q15 state length checked is numTaps + blockSize - 1.
#endif

extern void regression_console_init(void);

#define MAX_EXTRA 32U
#ifndef VERBOSE
#define VERBOSE 0
#endif
#define STATE_BYTES 1024U
#define INPUT_BYTES 1024U
#define GUARD_BYTES REGRESSION_GUARD_BYTES(STATE_BYTES > INPUT_BYTES ? STATE_BYTES : INPUT_BYTES)

static unsigned char state_arena[STATE_BYTES + GUARD_BYTES] __ALIGNED(32);
static unsigned char input_arena[INPUT_BYTES + GUARD_BYTES] __ALIGNED(32);
#define STATE_END ((uintptr_t)state_arena + STATE_BYTES)
#define INPUT_END ((uintptr_t)input_arena + INPUT_BYTES)

static const unsigned blocks[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17,
                                  18, 19, 20, 31, 32, 33, 64, 65};
#define NBLOCKS (sizeof(blocks) / sizeof(blocks[0]))

static jmp_buf recover_env;
static volatile unsigned faulted;

static void recover(void)
{
    __asm volatile("vmsr vpr, %0" : : "r"(0U));
    longjmp(recover_env, 1);
}

void memmanage_c(uint32_t *frame)
{
    ARM_MPU_Disable();
    SCB->CFSR = SCB->CFSR;
    faulted = 1;
    frame[6] = (uint32_t)recover & ~1U;
    frame[7] &= ~0x0600FC00U;
    frame[7] |= 1U << 24;
}

__attribute__((naked)) void MemManage_Handler(void)
{
    __asm volatile("tst lr, #4\n ite eq\n mrseq r0, msp\n mrsne r0, psp\n b memmanage_c\n");
}

static void protect_tails(void)
{
    ARM_MPU_Disable();
    for (uint32_t region = 0; region < ((MPU->TYPE >> 8) & 0xffU); ++region)
        ARM_MPU_ClrRegion(region);
    ARM_MPU_SetMemAttr(0, ARM_MPU_ATTR(ARM_MPU_ATTR_NON_CACHEABLE, ARM_MPU_ATTR_NON_CACHEABLE));
    uintptr_t lo = STATE_END < INPUT_END ? STATE_END : INPUT_END;
    uintptr_t hi = STATE_END < INPUT_END ? INPUT_END : STATE_END;
    ARM_MPU_SetRegion(0, ARM_MPU_RBAR(0, ARM_MPU_SH_NON, 0, 1, 0), ARM_MPU_RLAR(lo - 1U, 0));
    ARM_MPU_SetRegion(1, ARM_MPU_RBAR(lo + GUARD_BYTES, ARM_MPU_SH_NON, 0, 1, 0), ARM_MPU_RLAR(hi - 1U, 0));
    ARM_MPU_SetRegion(2, ARM_MPU_RBAR(hi + GUARD_BYTES, ARM_MPU_SH_NON, 0, 1, 0), ARM_MPU_RLAR(UINT32_MAX, 0));
    ARM_MPU_Enable(0);
}

static uint32_t lcg = 99U;
static uint32_t rnd(void) { lcg = lcg * 1664525U + 1013904223U; return lcg >> 8; }

/* Per kernel: cases, cases needing extra state, largest extra, stores past,
 * output failures. */
typedef struct { const char *name; unsigned cases, over, max_extra, stores, wrong; } tally;

/* One case: placement of the state with `extra` elements past the
 * documented length, two blocks guarded. Returns 1 on fault, 0 otherwise;
 * sets *stored and *wrong. CALL(S, in, out) filters one block; REF(o) is the
 * expected output o (0 .. 2 * outLen - 1); EQ(got, exp) compares. */
#define RUN_CASE(T, INST, INIT, CALL, DOC, OUTLEN, REF, EQ, CANARY)                          \
    {                                                                                       \
        T *state = (T *)STATE_END - (DOC) - extra;                                          \
        T *input = (T *)INPUT_END - 2U * b;                                                 \
        static T out[2U * 8U * 65U + 2U];                                                   \
        INST S;                                                                             \
        memcpy(input, src, 2U * b * sizeof(T));                                             \
        for (unsigned i = 0; i < (DOC) + extra; ++i) state[i] = (CANARY);                   \
        INIT;                                                                               \
        for (unsigned i = (DOC); i < (DOC) + extra; ++i) state[i] = (CANARY);               \
        faulted = 0;                                                                        \
        if (setjmp(recover_env) == 0)                                                       \
        {                                                                                   \
            for (unsigned blk = 0; blk < 2U; ++blk)                                         \
            {                                                                               \
                protect_tails();                                                            \
                CALL(&S, input + blk * b, out + blk * (OUTLEN), b);                         \
                REGRESSION_CONTROL_READ_ABOVE(STATE_END, GUARD_BYTES);                      \
                ARM_MPU_Disable();                                                          \
            }                                                                               \
        }                                                                                   \
        ARM_MPU_Disable();                                                                  \
        if (faulted) return 1;                                                              \
        *stored = 0;                                                                        \
        for (unsigned i = (DOC); i < (DOC) + extra; ++i)                                    \
            if (memcmp(&state[i], &(T){CANARY}, sizeof(T))) *stored = 1;                    \
        *wrong = 0;                                                                         \
        for (unsigned o = 0; o < 2U * (OUTLEN) && !*wrong; ++o)                             \
            if (!(EQ(out[o], (REF)))) *wrong = 1;                                           \
        return 0;                                                                           \
    }

/* ---- arm_fir_q7 / arm_fir_q15 ---- */
#define DEFINE_FIR(TAG, T, BITS, TMIN, TMAX, PAD)                                            \
static T src_##TAG[2U * 65U], coef_##TAG[64U + PAD];                                         \
static T fir_ref_##TAG(unsigned t, unsigned m)                                               \
{                                                                                           \
    int64_t acc = 0;                                                                        \
    for (unsigned j = 0; j < t && j <= m; ++j)                                              \
        acc += (int64_t)coef_##TAG[t - 1U - j] * src_##TAG[m - j];                          \
    acc >>= (BITS);                                                                         \
    return (T)(acc < (TMIN) ? (TMIN) : (acc > (TMAX) ? (TMAX) : acc));                      \
}                                                                                           \
static int fir_case_##TAG(unsigned t, unsigned b, unsigned extra, int *stored, int *wrong)   \
{                                                                                           \
    const T *src = src_##TAG;                                                               \
    RUN_CASE(T, arm_fir_instance_##TAG, arm_fir_init_##TAG(&S, (uint16_t)t, coef_##TAG, state, b), \
             arm_fir_##TAG, t + b - 1U, b, fir_ref_##TAG(t, o), FIX_EQ, (T)0x5A)            \
}
#define FIX_EQ(a, b) ((a) == (b))
DEFINE_FIR(q7, q7_t, 7, INT8_MIN, INT8_MAX, 16U)
DEFINE_FIR(q15, q15_t, 15, INT16_MIN, INT16_MAX, 8U)

/* ---- arm_fir_interpolate_f32/q15/q31 ----
 * Output n*L + p is sum_t pCoeffs[(L - 1 - p) + t*L] * s[n + t], where s is
 * the state of phaseLength - 1 history samples followed by the input. */
#define DEFINE_INTERP(TAG, T, ACC, SHIFT, TMIN, TMAX, CANARY, EQ)                           \
static T isrc_##TAG[2U * 65U], icoef_##TAG[8U * 12U + 8U];                                  \
static unsigned iL_##TAG, iP_##TAG;                                                         \
static T interp_ref_##TAG(unsigned o)                                                       \
{                                                                                           \
    unsigned L = iL_##TAG, P = iP_##TAG, n = o / L, p = o % L;                              \
    ACC acc = 0;                                                                            \
    for (unsigned t = 0; t < P; ++t)                                                        \
    {                                                                                       \
        int idx = (int)n + (int)t - (int)(P - 1U);                                          \
        if (idx >= 0)                                                                       \
            acc += (ACC)icoef_##TAG[(L - 1U - p) + t * L] * (ACC)isrc_##TAG[idx];           \
    }                                                                                       \
    return SHIFT;                                                                           \
}                                                                                           \
static int interp_case_##TAG(unsigned L, unsigned P, unsigned b, unsigned extra, int *stored, int *wrong) \
{                                                                                           \
    const T *src = isrc_##TAG;                                                              \
    iL_##TAG = L; iP_##TAG = P;                                                             \
    RUN_CASE(T, arm_fir_interpolate_instance_##TAG,                                         \
             arm_fir_interpolate_init_##TAG(&S, (uint8_t)L, (uint16_t)(L * P), icoef_##TAG, state, b), \
             arm_fir_interpolate_##TAG, P + b - 1U, L * b, interp_ref_##TAG(o), EQ, CANARY) \
}
#define SAT(x, lo, hi) ((x) < (lo) ? (lo) : ((x) > (hi) ? (hi) : (x)))
#define F32_EQ(a, b) (!regression_nonfinite_f32(&(a)) && fabs((double)(a) - (double)(b)) <= 1e-5 * (1.0 + fabs((double)(b))))
DEFINE_INTERP(f32, float32_t, double, (float32_t)acc, 0, 0, 1234.5f, F32_EQ)
DEFINE_INTERP(q15, q15_t, int64_t, (q15_t)SAT(acc >> 15, INT16_MIN, INT16_MAX), INT16_MIN, INT16_MAX, (q15_t)0x5A5A, FIX_EQ)
DEFINE_INTERP(q31, q31_t, int64_t, (q31_t)(acc >> 31), INT32_MIN, INT32_MAX, (q31_t)0x5A5A5A5A, FIX_EQ)

/* Grows the extra state until no fault; records the result. */
#define MEASURE(TALLY, CALLX)                                                               \
    {                                                                                       \
        unsigned extra = 0; int stored = 0, wrong = 0, r;                                   \
        while ((r = CALLX) != 0 && extra < MAX_EXTRA) ++extra;                              \
        (TALLY).cases++;                                                                    \
        if (VERBOSE && (r || extra)) printf("C %s %s extra=%u%s\n", (TALLY).name, where, extra, r ? "+" : ""); \
        if (r) { (TALLY).over++; (TALLY).max_extra = 999U; }                                \
        else if (extra) { (TALLY).over++; if (extra > (TALLY).max_extra) (TALLY).max_extra = extra; } \
        if (stored) (TALLY).stores++;                                                       \
        if (wrong) { if ((TALLY).wrong++ == 0) printf("first numerical failure: %s %s\n", (TALLY).name, where); } \
    }

int main(void)
{
    regression_console_init();
    SCB->SHCSR |= SCB_SHCSR_MEMFAULTENA_Msk;
    tally k[5] = {{"arm_fir_q7"}, {"arm_fir_q15"}, {"arm_fir_interpolate_f32"},
                  {"arm_fir_interpolate_q15"}, {"arm_fir_interpolate_q31"}};
    char where[64];
    for (unsigned i = 0; i < 2U * 65U; ++i)
    {
        src_q7[i] = (q7_t)((int)(rnd() % 255U) - 127);
        src_q15[i] = (q15_t)((int)(rnd() % 65535U) - 32767);
        isrc_f32[i] = (float32_t)((int)(rnd() % 2001U) - 1000) / 1000.0f;
        isrc_q15[i] = (q15_t)((int)(rnd() % 65535U) - 32767);
        isrc_q31[i] = (q31_t)(rnd() << 8) >> 5; /* headroom for the 64-bit sum */
    }
    for (unsigned t = 1; t <= 64U; ++t)
        for (unsigned kb = 0; kb < NBLOCKS; ++kb)
        {
            unsigned b = blocks[kb];
            memset(coef_q7, 0, sizeof coef_q7);
            for (unsigned i = 0; i < t; ++i) coef_q7[i] = (q7_t)((int)(rnd() % 255U) - 127);
            snprintf(where, sizeof where, "numTaps=%u blockSize=%u", t, b);
            MEASURE(k[0], fir_case_q7(t, b, extra, &stored, &wrong))
        }
    for (unsigned t = 2; t <= 32U; t += 2U)
        for (unsigned kb = 0; kb < NBLOCKS; ++kb)
        {
            unsigned b = blocks[kb];
            memset(coef_q15, 0, sizeof coef_q15);
            for (unsigned i = 0; i < t; ++i) coef_q15[i] = (q15_t)((int)(rnd() % 65535U) - 32767);
            snprintf(where, sizeof where, "numTaps=%u blockSize=%u", t, b);
            MEASURE(k[1], fir_case_q15(t, b, extra, &stored, &wrong))
        }
    static const unsigned Ls[] = {2, 3, 4, 5, 8};
    for (unsigned il = 0; il < 5U; ++il)
        for (unsigned P = 1; P <= 12U; ++P)
            for (unsigned kb = 0; kb < NBLOCKS; ++kb)
            {
                unsigned L = Ls[il], b = blocks[kb];
                memset(icoef_f32, 0, sizeof icoef_f32);
                memset(icoef_q15, 0, sizeof icoef_q15);
                memset(icoef_q31, 0, sizeof icoef_q31);
                for (unsigned i = 0; i < L * P; ++i)
                {
                    icoef_f32[i] = (float32_t)((int)(rnd() % 2001U) - 1000) / 1000.0f;
                    icoef_q15[i] = (q15_t)((int)(rnd() % 65535U) - 32767);
                    icoef_q31[i] = (q31_t)(rnd() << 8);
                }
                snprintf(where, sizeof where, "L=%u phaseLength=%u blockSize=%u", L, P, b);
                MEASURE(k[2], interp_case_f32(L, P, b, extra, &stored, &wrong))
                MEASURE(k[3], interp_case_q15(L, P, b, extra, &stored, &wrong))
                MEASURE(k[4], interp_case_q31(L, P, b, extra, &stored, &wrong))
            }
    unsigned bad = 0, cases = 0;
    for (unsigned i = 0; i < 5U; ++i)
    {
        printf("%s: %u cases, %u read or store past the documented state or input (largest extra %u), %u store past, %u wrong outputs\n",
               k[i].name, k[i].cases, k[i].over, k[i].max_extra, k[i].stores, k[i].wrong);
        cases += k[i].cases;
        if (k[i].over || k[i].stores || k[i].wrong) bad++;
    }
    if (bad)
        printf("FAIL: %u of 5 MVE FIR state-bound kernels exceed the contract or give wrong outputs\n", bad);
    else
        printf("PASS: %u MVE FIR state-bound cases\n", cases);
    return 0;
}
