/* Copyright (c) 2026 Ambiq Micro, Inc.
 * SPDX-License-Identifier: Apache-2.0
 */
/* Cortex-M55 regression: the MVE arm_min, arm_max, arm_absmin and arm_absmax
 * kernels for f32, Q31, Q15, Q7 and f16 return the extreme value and its
 * index. For every length from 1 to 48 (three full Q7 vectors, six of Q15
 * and f16, twelve of f32 and Q31) and around 64, 128, 256 and 512 (the Q7
 * kernels process blocks of 255 elements above 255), a unique extreme is
 * placed at every position, so the vector loop, the tail after it and the
 * Q7 block split are all exercised. The other elements lie in [10, 106];
 * for absmin and absmax the extreme alternates in sign with its position.
 * Value and index must equal the extreme's, compared by their bits (exact
 * under -ffast-math).
 *
 * The source ends at an MPU-inaccessible guard, so a read past it faults.
 * Sixteen sentinels that would win over the extreme sit directly before the
 * source, so a kernel whose result uses anything before the data fails too.
 * The five kernels whose tail load PR #32 predicates (arm_min_f16,
 * arm_absmax_f16, arm_absmin_q7/q15/q31) still read up to one vector past
 * the source; until #32 merges they run with one vector of sentinels between
 * the source and the guard (SLACK_* below), which must not change their
 * result either.
 *
 * The Helium arm_min_f32 scanned the first blockSize % 4 elements again in
 * its tail instead of the last ones, so a minimum among the last
 * blockSize % 4 elements of a block longer than four was missed: the
 * unmodified kernel reports FAIL at length 5, extreme at position 4.
 *
 * A failing kernel is reported and the run continues with the next one, so a
 * negative control shows every kernel that fails; a guard access ends the
 * run. Negative controls of the shared kind (regression_checks.h, selected
 * by run_mve_min_max_index.py --control) must make the run fail.
 */
#include "ARMCM55.h"
#include "dsp/statistics_functions.h"
#include "dsp/statistics_functions_f16.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "regression_checks.h"

#if !defined(ARM_MATH_MVEF) || !defined(ARM_MATH_MVEI) || !defined(ARM_MATH_MVE_FLOAT16) || \
    defined(ARM_MATH_AUTOVECTORIZE)
#error This regression requires the handwritten MVE kernels with float16 enabled.
#endif

#define MAX_LEN 513U
#define SENTINELS 16U
/* One vector of slack for the kernels listed below (16 bytes). */
#define SLACK_BYTES 16U
/* The guard covers the largest source plus one vector access. */
#define GUARD_BYTES REGRESSION_GUARD_BYTES(MAX_LEN * 4U)
#define BELOW_BYTES (((SENTINELS + MAX_LEN) * 4U + SLACK_BYTES + 31U) & ~31U)

#define BACKGROUND(i) (10 + (int)(((i) * 37U) % 97U))
#define MIN_EXTREME (-50)
#define MIN_SENTINEL (-100)
#define MAX_EXTREME 120
#define MAX_SENTINEL 127
#define ABSMIN_EXTREME 3
#define ABSMIN_SENTINEL 0
#define ABSMAX_EXTREME 120
#define ABSMAX_SENTINEL (-127)

extern void regression_console_init(void);

/* [sentinels | source | slack] ending at the guard. */
static unsigned char arena[BELOW_BYTES + GUARD_BYTES] __ALIGNED(32);
#define GUARD ((uintptr_t)arena + BELOW_BYTES)

static const uint32_t lengths[] = {
    1,   2,   3,   4,   5,   6,   7,   8,   9,   10,  11,  12,  13,  14,  15,  16,  17,
    18,  19,  20,  21,  22,  23,  24,  25,  26,  27,  28,  29,  30,  31,  32,  33,  34,
    35,  36,  37,  38,  39,  40,  41,  42,  43,  44,  45,  46,  47,  48,  63,  64,  65,
    127, 128, 129, 254, 255, 256, 257, 300, 510, 511, 512, 513,
};

static const char *current_name = "none";
static volatile unsigned current_len, current_pos;
static unsigned cases, failed_kernels;

void MemManage_Handler(void)
{
    ARM_MPU_Disable();
    printf("FAIL: %s source access length %u position %u (MMFAR 0x%08lx)\n", current_name,
           current_len, current_pos, (unsigned long)SCB->MMFAR);
    exit(1);
}

static void protect_guard(void)
{
    ARM_MPU_Disable();
    for (uint32_t region = 0; region < ((MPU->TYPE >> 8) & 0xffU); ++region)
    {
        ARM_MPU_ClrRegion(region);
    }
    ARM_MPU_SetMemAttr(0, ARM_MPU_ATTR(ARM_MPU_ATTR_NON_CACHEABLE,
                                     ARM_MPU_ATTR_NON_CACHEABLE));
    ARM_MPU_SetRegion(0, ARM_MPU_RBAR(0, ARM_MPU_SH_NON, 0, 1, 0), ARM_MPU_RLAR(GUARD - 1U, 0));
    ARM_MPU_SetRegion(1, ARM_MPU_RBAR(GUARD + GUARD_BYTES, ARM_MPU_SH_NON, 0, 1, 0),
                     ARM_MPU_RLAR(UINT32_MAX, 0));
    ARM_MPU_Enable(0);
}

/* Ends a guarded call; a read control reads past the source first. */
static void unguard(void)
{
    REGRESSION_CONTROL_READ_ABOVE(GUARD, GUARD_BYTES);
    ARM_MPU_Disable();
}

/*
 * Defines check_<suffix>(kernel, name, extreme, alternate, expected,
 * sentinel, slack): for each length and each position, the kernel must
 * return the expected value and the position as index. With alternate set
 * the extreme is negated at odd positions. slack is the number of bytes of
 * sentinels between the source and the guard.
 */
#define DEFINE_CHECK(suffix, type, scale, NAN_CONTROL)                                             \
    static void check_##suffix(void (*kernel)(const type *, uint32_t, type *, uint32_t *),         \
                               const char *name, int extreme, int alternate, int expected,         \
                               int sentinel, uint32_t slack)                                       \
    {                                                                                              \
        const type want = (type)(expected * (scale));                                              \
        current_name = name;                                                                       \
        for (unsigned l = 0; l < sizeof(lengths) / sizeof(lengths[0]); ++l)                        \
        {                                                                                          \
            const uint32_t len = lengths[l];                                                       \
            type *const data = (type *)(GUARD - slack) - len;                                      \
            for (uint32_t i = 0; i < SENTINELS; ++i)                                               \
                data[(int32_t)i - (int32_t)SENTINELS] = (type)(sentinel * (scale));                \
            for (uint32_t i = 0; i < slack / sizeof(type); ++i)                                    \
                data[len + i] = (type)(sentinel * (scale));                                        \
            for (uint32_t i = 0; i < len; ++i)                                                     \
                data[i] = (type)(BACKGROUND(i) * (scale));                                         \
            current_len = len;                                                                     \
            for (uint32_t pos = 0; pos < len; ++pos)                                               \
            {                                                                                      \
                const int x = (alternate && (pos & 1U)) ? -extreme : extreme;                      \
                type result = (type)0;                                                             \
                uint32_t index = UINT32_MAX;                                                       \
                data[pos] = (type)(x * (scale));                                                   \
                current_pos = pos;                                                                 \
                protect_guard();                                                                   \
                kernel(data, len, &result, &index);                                                \
                unguard();                                                                         \
                data[pos] = (type)(BACKGROUND(pos) * (scale));                                     \
                NAN_CONTROL(&result);                                                              \
                if (index != pos || memcmp(&result, &want, sizeof(type)) != 0)                     \
                {                                                                                  \
                    printf("FAIL: %s length %u extreme at %u: index %lu, value %s\n", name,        \
                           (unsigned)len, (unsigned)pos, (unsigned long)index,                     \
                           memcmp(&result, &want, sizeof(type)) ? "wrong" : "right");              \
                    ++failed_kernels;                                                              \
                    return;                                                                        \
                }                                                                                  \
                ++cases;                                                                           \
            }                                                                                      \
        }                                                                                          \
    }

#define NO_CONTROL(p) ((void)0)
DEFINE_CHECK(f32, float32_t, 1.0f, REGRESSION_CONTROL_NAN_F32)
DEFINE_CHECK(q31, q31_t, 16777216, NO_CONTROL)
DEFINE_CHECK(q15, q15_t, 256, NO_CONTROL)
DEFINE_CHECK(q7, q7_t, 1, NO_CONTROL)
DEFINE_CHECK(f16, float16_t, 1.0f, REGRESSION_CONTROL_NAN_F16)

/* Kernels whose tail load reads past the source until PR #32 merges. */
#define SLACK_min_f16 SLACK_BYTES
#define SLACK_absmax_f16 SLACK_BYTES
#define SLACK_absmin_q7 SLACK_BYTES
#define SLACK_absmin_q15 SLACK_BYTES
#define SLACK_absmin_q31 SLACK_BYTES

#define SLACK_min_f32 0U
#define SLACK_max_f32 0U
#define SLACK_absmin_f32 0U
#define SLACK_absmax_f32 0U
#define SLACK_min_q31 0U
#define SLACK_max_q31 0U
#define SLACK_absmax_q31 0U
#define SLACK_min_q15 0U
#define SLACK_max_q15 0U
#define SLACK_absmax_q15 0U
#define SLACK_min_q7 0U
#define SLACK_max_q7 0U
#define SLACK_absmax_q7 0U
#define SLACK_max_f16 0U
#define SLACK_absmin_f16 0U

#define CHECK_FAMILY(suffix)                                                                       \
    do                                                                                             \
    {                                                                                              \
        check_##suffix(arm_min_##suffix, "arm_min_" #suffix, MIN_EXTREME, 0, MIN_EXTREME,          \
                       MIN_SENTINEL, SLACK_min_##suffix);                                          \
        check_##suffix(arm_max_##suffix, "arm_max_" #suffix, MAX_EXTREME, 0, MAX_EXTREME,          \
                       MAX_SENTINEL, SLACK_max_##suffix);                                          \
        check_##suffix(arm_absmin_##suffix, "arm_absmin_" #suffix, ABSMIN_EXTREME, 1,              \
                       ABSMIN_EXTREME, ABSMIN_SENTINEL, SLACK_absmin_##suffix);                    \
        check_##suffix(arm_absmax_##suffix, "arm_absmax_" #suffix, ABSMAX_EXTREME, 1,              \
                       ABSMAX_EXTREME, ABSMAX_SENTINEL, SLACK_absmax_##suffix);                    \
    } while (0)

int main(void)
{
    regression_console_init();
    SCB->SHCSR |= SCB_SHCSR_MEMFAULTENA_Msk;
    CHECK_FAMILY(f32);
    CHECK_FAMILY(q31);
    CHECK_FAMILY(q15);
    CHECK_FAMILY(q7);
    CHECK_FAMILY(f16);
    if (failed_kernels)
    {
        printf("FAIL: %u of 20 kernels\n", failed_kernels);
        return 1;
    }
    printf("PASS: %u MVE min/max index cases\n", cases);
    return 0;
}
