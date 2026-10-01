/* Copyright (c) 2026 Ambiq Micro, Inc.
 * SPDX-License-Identifier: Apache-2.0
 */
/* Cortex-M55 regression: the MVE Q31 complex FFT, forward and inverse, for
 * lengths 16 to 1024, on a buffer whose first element follows an
 * MPU-inaccessible guard, checked against a double-precision DFT.
 *
 * The final radix-4 stage of the MVE kernel uses vldrwq_gather_base_wb_s32
 * and then gathers and scatters relative to the written-back base. A
 * compiler that issues those accesses from the pre-writeback base touches
 * 64 bytes below each block, which here is the guard, and corrupts the
 * output. Arm GNU Toolchain 15.2.Rel1 does so at every optimization level;
 * 14.x does not.
 */
#include "ARMCM55.h"
#include "dsp/transform_functions.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(ARM_MATH_MVEI) || defined(ARM_MATH_AUTOVECTORIZE)
#error This regression requires the handwritten MVE integer kernels.
#endif

#define MAX_LEN 1024U
#define GUARD_BYTES 32U

extern void regression_console_init(void);
/* Guard, then the buffer, then readable canary padding: the miscompiled
 * kernel reads and writes below pSrc. The upstream kernel reads one element
 * past the buffer for small lengths under a correct compiler, so the upper
 * side is padding with canaries (writes detected, reads tolerated) rather
 * than a second guard. */
#define PAD_WORDS 32U
#define CANARY ((q31_t)0x5a5a5a5a)
static q31_t arena[GUARD_BYTES / sizeof(q31_t) + 2U * MAX_LEN + PAD_WORDS] __ALIGNED(32);
static q31_t reference_in[2U * MAX_LEN];
static volatile unsigned current_len, current_inverse;

void MemManage_Handler(void)
{
    ARM_MPU_Disable();
    printf("FAIL: CFFT Q31 %s length %u touched the guard (MMFAR 0x%08lx, arena 0x%08lx)\n",
           current_inverse ? "inverse" : "forward", current_len,
           (unsigned long)SCB->MMFAR, (unsigned long)(uintptr_t)arena);
    exit(1);
}

static void protect_guard(uintptr_t guard)
{
    ARM_MPU_Disable();
    for (uint32_t region = 0; region < ((MPU->TYPE >> 8) & 0xffU); ++region)
    {
        ARM_MPU_ClrRegion(region);
    }
    ARM_MPU_SetMemAttr(0, ARM_MPU_ATTR(ARM_MPU_ATTR_NON_CACHEABLE,
                                     ARM_MPU_ATTR_NON_CACHEABLE));
    ARM_MPU_SetRegion(0, ARM_MPU_RBAR(0, ARM_MPU_SH_NON, 0, 1, 0), ARM_MPU_RLAR(guard - 1U, 0));
    ARM_MPU_SetRegion(1, ARM_MPU_RBAR(guard + GUARD_BYTES, ARM_MPU_SH_NON, 0, 1, 0),
                     ARM_MPU_RLAR(UINT32_MAX, 0));
    ARM_MPU_Enable(0);
}

static int run_case(uint32_t len, unsigned inverse, unsigned *checks)
{
    q31_t *lo_guard = arena;
    q31_t *buf = arena + GUARD_BYTES / sizeof(q31_t);
    q31_t *pad = buf + 2U * len;
    arm_cfft_instance_q31 S;

    if (arm_cfft_init_q31(&S, (uint16_t)len) != ARM_MATH_SUCCESS)
    {
        printf("FAIL: init length %u\n", (unsigned)len);
        return 1;
    }
    for (uint32_t i = 0; i < 2U * len; ++i)
    {
        /* Low amplitude keeps the forward transform away from saturation. */
        reference_in[i] = (q31_t)(((int32_t)((i * 2654435761U) >> 8) & 0xffffff) - 0x800000) << 2;
        buf[i] = reference_in[i];
    }
    for (uint32_t i = 0; i < PAD_WORDS; ++i)
        pad[i] = CANARY;
    current_len = (unsigned)len;
    current_inverse = inverse;
    protect_guard((uintptr_t)lo_guard);
    arm_cfft_q31(&S, buf, (uint8_t)inverse, 1U);
    ARM_MPU_Disable();
    for (uint32_t i = 0; i < PAD_WORDS; ++i)
    {
        if (pad[i] != CANARY)
        {
            printf("FAIL: CFFT Q31 %s length %u wrote past the buffer at +%u\n",
                   inverse ? "inverse" : "forward", (unsigned)len, (unsigned)i);
            return 1;
        }
    }

    /* Reference: scaled DFT in double. The Q31 CFFT divides by len in both
     * directions; the inverse uses +j, the forward -j. */
    double worst = 0.0;
    for (uint32_t k = 0; k < len; ++k)
    {
        double re = 0.0, im = 0.0;
        for (uint32_t n = 0; n < len; ++n)
        {
            double ang = (inverse ? 2.0 : -2.0) * M_PI * (double)((k * n) % len) / (double)len;
            double xr = (double)reference_in[2 * n], xi = (double)reference_in[2 * n + 1];
            re += xr * cos(ang) - xi * sin(ang);
            im += xr * sin(ang) + xi * cos(ang);
        }
        re /= (double)len;
        im /= (double)len;
        double er = fabs((double)buf[2 * k] - re), ei = fabs((double)buf[2 * k + 1] - im);
        if (er > worst) worst = er;
        if (ei > worst) worst = ei;
    }
    /* Q31 rounding over log4(len) stages: tens of LSB for 1024 points. */
    if (worst > 256.0)
    {
        printf("FAIL: CFFT Q31 %s length %u max error %.0f LSB\n",
               inverse ? "inverse" : "forward", (unsigned)len, worst);
        return 1;
    }
    ++*checks;
    return 0;
}

int main(void)
{
    regression_console_init();
    SCB->SHCSR |= SCB_SHCSR_MEMFAULTENA_Msk;
    unsigned checks = 0;
    static const uint32_t lengths[] = {16, 32, 64, 128, 256, 512, 1024};
    for (unsigned i = 0; i < sizeof(lengths) / sizeof(lengths[0]); ++i)
    {
        if (run_case(lengths[i], 0, &checks) || run_case(lengths[i], 1, &checks))
            return 1;
    }
    printf("PASS: %u MVE CFFT Q31 guard cases\n", checks);
    return 0;
}
