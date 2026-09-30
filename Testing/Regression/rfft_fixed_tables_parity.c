/* Copyright (c) 2026 Ambiq Micro, Inc.
 * SPDX-License-Identifier: Apache-2.0
 */
/*
 * Verify that every fixed-size Q15/Q31 RFFT initializer produces exactly the
 * same forward and inverse results as the generic initializer.  The generic
 * path uses the legacy 8192-point master tables; fixed-size paths use compact
 * strided copies with twidCoefRModifier == 1.
 */
#include "arm_math.h"
#include <stdio.h>
#include <string.h>

#define MAX_RFFT_LENGTH 8192U
static volatile unsigned current_length;
#ifdef __arm__
#define BIG __attribute__((section(".bss.tensor_arena")))
#else
#define BIG
#endif

static q15_t BIG q15_input_generic[MAX_RFFT_LENGTH + 2U];
static q15_t BIG q15_input_fixed[MAX_RFFT_LENGTH + 2U];
/* The legacy fixed-point RFFT writes the redundant conjugate half too. */
static q15_t BIG q15_output_generic[2U * MAX_RFFT_LENGTH];
static q15_t BIG q15_output_fixed[2U * MAX_RFFT_LENGTH];

static q31_t BIG q31_input_generic[MAX_RFFT_LENGTH + 2U];
static q31_t BIG q31_input_fixed[MAX_RFFT_LENGTH + 2U];
static q31_t BIG q31_output_generic[2U * MAX_RFFT_LENGTH];
static q31_t BIG q31_output_fixed[2U * MAX_RFFT_LENGTH];

static arm_status
init_fixed_q15(arm_rfft_instance_q15 *instance, uint32_t length,
               uint32_t inverse)
{
#define Q15_CASE(n) case n##U: return arm_rfft_init_##n##_q15(instance, inverse, 1U)
    switch (length)
    {
        Q15_CASE(32);
        Q15_CASE(64);
        Q15_CASE(128);
        Q15_CASE(256);
        Q15_CASE(512);
        Q15_CASE(1024);
        Q15_CASE(2048);
        Q15_CASE(4096);
        Q15_CASE(8192);
        default: return ARM_MATH_ARGUMENT_ERROR;
    }
#undef Q15_CASE
}

static arm_status
init_fixed_q31(arm_rfft_instance_q31 *instance, uint32_t length,
               uint32_t inverse)
{
#define Q31_CASE(n) case n##U: return arm_rfft_init_##n##_q31(instance, inverse, 1U)
    switch (length)
    {
        Q31_CASE(32);
        Q31_CASE(64);
        Q31_CASE(128);
        Q31_CASE(256);
        Q31_CASE(512);
        Q31_CASE(1024);
        Q31_CASE(2048);
        Q31_CASE(4096);
        Q31_CASE(8192);
        default: return ARM_MATH_ARGUMENT_ERROR;
    }
#undef Q31_CASE
}

static int
test_q15(uint32_t length)
{
    arm_rfft_instance_q15 generic;
    arm_rfft_instance_q15 fixed;

    for (uint32_t i = 0U; i < length; ++i)
    {
        /* Low amplitude prevents saturation while still exercising signs. */
        q15_input_generic[i] = (q15_t)(((i * 73U + 19U) & 0x7ffU) - 0x400);
    }
    memcpy(q15_input_fixed, q15_input_generic, length * sizeof(q15_t));

    if (arm_rfft_init_q15(&generic, length, 0U, 1U) != ARM_MATH_SUCCESS ||
        init_fixed_q15(&fixed, length, 0U) != ARM_MATH_SUCCESS ||
        fixed.twidCoefRModifier != 1U)
    {
        return 1;
    }
    for (uint32_t i = 0U; i < length; ++i)
    {
        const uint32_t master_index =
            (i & ~1U) * generic.twidCoefRModifier + (i & 1U);
        if (generic.pTwiddleAReal[master_index] != fixed.pTwiddleAReal[i] ||
            generic.pTwiddleBReal[master_index] != fixed.pTwiddleBReal[i])
        {
            fprintf(stderr, "Q15 coefficient length %u differs at %u\n",
                    (unsigned)length, (unsigned)i);
            return 5;
        }
    }
#ifdef HAVE_MASTER_GUARDS
    if (length == MAX_RFFT_LENGTH &&
        (generic.pTwiddleAReal[length] != 0 ||
         generic.pTwiddleAReal[length + 1U] != 0 ||
         generic.pTwiddleBReal[length] != 0 ||
         generic.pTwiddleBReal[length + 1U] != 0))
    {
        fprintf(stderr, "Q15 master-table MVE guards are missing\n");
        return 6;
    }
#endif
    arm_rfft_q15(&generic, q15_input_generic, q15_output_generic);
    arm_rfft_q15(&fixed, q15_input_fixed, q15_output_fixed);
    if (memcmp(q15_output_generic, q15_output_fixed,
               2U * length * sizeof(q15_t)) != 0)
    {
        for (uint32_t i = 0U; i < 2U * length; ++i)
        {
            if (q15_output_generic[i] != q15_output_fixed[i])
            {
                fprintf(stderr, "Q15 forward length %u differs at %u: %d != %d\n",
                        (unsigned)length, (unsigned)i,
                        (int)q15_output_generic[i], (int)q15_output_fixed[i]);
                break;
            }
        }
        return 2;
    }

    memcpy(q15_input_generic, q15_output_generic, (length + 2U) * sizeof(q15_t));
    memcpy(q15_input_fixed, q15_output_fixed, (length + 2U) * sizeof(q15_t));
    if (arm_rfft_init_q15(&generic, length, 1U, 1U) != ARM_MATH_SUCCESS ||
        init_fixed_q15(&fixed, length, 1U) != ARM_MATH_SUCCESS)
    {
        return 3;
    }
    arm_rfft_q15(&generic, q15_input_generic, q15_output_generic);
    arm_rfft_q15(&fixed, q15_input_fixed, q15_output_fixed);
    if (memcmp(q15_output_generic, q15_output_fixed,
               length * sizeof(q15_t)) != 0)
    {
        fprintf(stderr, "Q15 inverse length %u differs\n", (unsigned)length);
        return 4;
    }
    return 0;
}

static int
test_q31(uint32_t length)
{
    arm_rfft_instance_q31 generic;
    arm_rfft_instance_q31 fixed;

    for (uint32_t i = 0U; i < length; ++i)
    {
        q31_input_generic[i] = (q31_t)(((i * 65537U + 97U) & 0x7fffffU) - 0x400000);
    }
    memcpy(q31_input_fixed, q31_input_generic, length * sizeof(q31_t));

    if (arm_rfft_init_q31(&generic, length, 0U, 1U) != ARM_MATH_SUCCESS ||
        init_fixed_q31(&fixed, length, 0U) != ARM_MATH_SUCCESS ||
        fixed.twidCoefRModifier != 1U)
    {
        return 1;
    }
    for (uint32_t i = 0U; i < length; ++i)
    {
        const uint32_t master_index =
            (i & ~1U) * generic.twidCoefRModifier + (i & 1U);
        if (generic.pTwiddleAReal[master_index] != fixed.pTwiddleAReal[i] ||
            generic.pTwiddleBReal[master_index] != fixed.pTwiddleBReal[i])
        {
            fprintf(stderr, "Q31 coefficient length %u differs at %u\n",
                    (unsigned)length, (unsigned)i);
            return 5;
        }
    }
#ifdef HAVE_MASTER_GUARDS
    if (length == MAX_RFFT_LENGTH &&
        (generic.pTwiddleAReal[length] != 0 ||
         generic.pTwiddleAReal[length + 1U] != 0 ||
         generic.pTwiddleBReal[length] != 0 ||
         generic.pTwiddleBReal[length + 1U] != 0))
    {
        fprintf(stderr, "Q31 master-table MVE guards are missing\n");
        return 6;
    }
#endif
    arm_rfft_q31(&generic, q31_input_generic, q31_output_generic);
    arm_rfft_q31(&fixed, q31_input_fixed, q31_output_fixed);
    if (memcmp(q31_output_generic, q31_output_fixed,
               2U * length * sizeof(q31_t)) != 0)
    {
        for (uint32_t i = 0U; i < 2U * length; ++i)
        {
            if (q31_output_generic[i] != q31_output_fixed[i])
            {
                fprintf(stderr, "Q31 forward length %u differs at %u: %ld != %ld\n",
                        (unsigned)length, (unsigned)i,
                        (long)q31_output_generic[i], (long)q31_output_fixed[i]);
                break;
            }
        }
        return 2;
    }

    memcpy(q31_input_generic, q31_output_generic, (length + 2U) * sizeof(q31_t));
    memcpy(q31_input_fixed, q31_output_fixed, (length + 2U) * sizeof(q31_t));
    if (arm_rfft_init_q31(&generic, length, 1U, 1U) != ARM_MATH_SUCCESS ||
        init_fixed_q31(&fixed, length, 1U) != ARM_MATH_SUCCESS)
    {
        return 3;
    }
    arm_rfft_q31(&generic, q31_input_generic, q31_output_generic);
    arm_rfft_q31(&fixed, q31_input_fixed, q31_output_fixed);
    if (memcmp(q31_output_generic, q31_output_fixed,
               length * sizeof(q31_t)) != 0)
    {
        fprintf(stderr, "Q31 inverse length %u differs\n", (unsigned)length);
        return 4;
    }
    return 0;
}

#ifdef __arm__
#include <stdlib.h>
static void fault_report(const char *name)
{
    printf("FAIL: %s at length %u\n", name, (unsigned)current_length);
    exit(1);
}
void HardFault_Handler(void) { fault_report("HardFault"); }
void MemManage_Handler(void) { fault_report("MemManage"); }
void BusFault_Handler(void) { fault_report("BusFault"); }
void UsageFault_Handler(void) { fault_report("UsageFault"); }
#endif

int
main(void)
{
#ifdef __arm__
    extern void regression_console_init(void);
    regression_console_init();
#endif
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("START fixed-point RFFT parity\n");
    static const uint32_t lengths[] = {
        32U, 64U, 128U, 256U, 512U, 1024U, 2048U, 4096U, 8192U
    };
    for (uint32_t i = 0U; i < sizeof(lengths) / sizeof(lengths[0]); ++i)
    {
        current_length = lengths[i];
        int result = test_q15(lengths[i]);
        if (result != 0)
        {
            return 10 + result;
        }
        result = test_q31(lengths[i]);
        if (result != 0)
        {
            return 20 + result;
        }
        printf("PASS: length %u Q15 and Q31, forward and inverse\n", (unsigned)lengths[i]);
    }
    printf("PASS: 36 fixed-point RFFT parity cases\n");
    return 0;
}
