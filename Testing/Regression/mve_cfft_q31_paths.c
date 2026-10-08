/* Copyright (c) 2026 Ambiq Micro, Inc.
 * SPDX-License-Identifier: Apache-2.0
 */
/* Cortex-M55 regression: the GCC inline-assembly paths of the MVE Q31 CFFT
 * (final radix-4 stage, forward and inverse, lengths 16 to 1024) and of the
 * Q31 real inverse FFT split (lengths 64 to 2048; 32 keeps the intrinsic
 * split) produce bit-identical output to Arm's intrinsic paths, which the
 * runner builds from the same sources with ARM_MATH_MVE_FFT_REFERENCE and
 * renamed entry points. The buffers follow an MPU-inaccessible guard as in
 * mve_cfft_q31_guard.c, and the CFFT output is also checked against a
 * double-precision DFT. Under a compiler that does not take the assembly
 * path (ATfE) both builds are the intrinsic path and the comparison is
 * trivially equal.
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

extern void arm_cfft_q31_reference(const arm_cfft_instance_q31 *S, q31_t *p1,
                                   uint8_t ifftFlag, uint8_t bitReverseFlag);
extern void arm_rfft_q31_reference(const arm_rfft_instance_q31 *S, q31_t *pSrc, q31_t *pDst);

#define MAX_CFFT 1024U
#define MAX_RFFT 2048U
#define GUARD_BYTES 32U
#define PAD_WORDS 32U
#define CANARY ((q31_t)0x5a5a5a5a)

extern void regression_console_init(void);

/* Guard, buffer, canary padding: the kernels under test read a few words
 * past the buffer for small lengths (upstream behaviour), so the upper side
 * is readable padding whose writes are detected. */
static q31_t arena[GUARD_BYTES / sizeof(q31_t) + 2U * MAX_RFFT + PAD_WORDS] __ALIGNED(32);
static q31_t input[2U * MAX_RFFT];
static q31_t reference_out[2U * MAX_RFFT];
static q31_t rfft_dst[2U * MAX_RFFT + 2U];
static q31_t rfft_dst_reference[2U * MAX_RFFT + 2U];
static const char *current_name = "none";
static volatile unsigned current_len, current_inverse;

void MemManage_Handler(void)
{
    ARM_MPU_Disable();
    printf("FAIL: %s %s length %u touched the guard (MMFAR 0x%08lx)\n", current_name,
           current_inverse ? "inverse" : "forward", current_len, (unsigned long)SCB->MMFAR);
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

static void fill_input(uint32_t words)
{
    for (uint32_t i = 0; i < words; ++i)
        input[i] = ((q31_t)(((i * 2654435761U) >> 8) & 0xffffffU) - 0x800000) * 4;
}

static int check_pad(const q31_t *pad)
{
    for (uint32_t i = 0; i < PAD_WORDS; ++i)
        if (pad[i] != CANARY)
        {
            printf("FAIL: %s %s length %u wrote past the buffer at +%u\n", current_name,
                   current_inverse ? "inverse" : "forward", current_len, (unsigned)i);
            return 1;
        }
    return 0;
}

static int cfft_case(uint32_t len, unsigned inverse, unsigned *checks)
{
    q31_t *buf = arena + GUARD_BYTES / sizeof(q31_t);
    q31_t *pad = buf + 2U * len;
    arm_cfft_instance_q31 S;
    current_name = "CFFT Q31";
    current_len = (unsigned)len;
    current_inverse = inverse;
    if (arm_cfft_init_q31(&S, (uint16_t)len) != ARM_MATH_SUCCESS)
    {
        printf("FAIL: CFFT init length %u\n", (unsigned)len);
        return 1;
    }
    fill_input(2U * len);
    memcpy(reference_out, input, 2U * len * sizeof(q31_t));
    arm_cfft_q31_reference(&S, reference_out, (uint8_t)inverse, 1U);

    memcpy(buf, input, 2U * len * sizeof(q31_t));
    for (uint32_t i = 0; i < PAD_WORDS; ++i)
        pad[i] = CANARY;
    protect_guard((uintptr_t)arena);
    arm_cfft_q31(&S, buf, (uint8_t)inverse, 1U);
    ARM_MPU_Disable();
    if (check_pad(pad))
        return 1;
    if (memcmp(buf, reference_out, 2U * len * sizeof(q31_t)))
    {
        printf("FAIL: CFFT Q31 %s length %u differs from the intrinsic path\n",
               inverse ? "inverse" : "forward", (unsigned)len);
        return 1;
    }
    /* Scaled DFT in double: the Q31 CFFT divides by len in both directions. */
    double worst = 0.0;
    for (uint32_t k = 0; k < len; ++k)
    {
        double re = 0.0, im = 0.0;
        for (uint32_t n = 0; n < len; ++n)
        {
            double ang = (inverse ? 2.0 : -2.0) * M_PI * (double)((k * n) % len) / (double)len;
            double xr = (double)input[2 * n], xi = (double)input[2 * n + 1];
            re += xr * cos(ang) - xi * sin(ang);
            im += xr * sin(ang) + xi * cos(ang);
        }
        re /= (double)len;
        im /= (double)len;
        double er = fabs((double)buf[2 * k] - re), ei = fabs((double)buf[2 * k + 1] - im);
        if (er > worst) worst = er;
        if (ei > worst) worst = ei;
    }
    if (worst > 256.0)
    {
        printf("FAIL: CFFT Q31 %s length %u max error %.0f LSB\n",
               inverse ? "inverse" : "forward", (unsigned)len, worst);
        return 1;
    }
    ++*checks;
    return 0;
}

/* Real FFT: forward on len real samples (2*len words of output), then inverse
 * on that output. pSrc is modified by the kernel, so both paths start from a
 * fresh copy; pDst carries canaries one word before and after. */
static int rfft_case(uint32_t len, unsigned inverse, unsigned *checks)
{
    q31_t *buf = arena + GUARD_BYTES / sizeof(q31_t);
    uint32_t in_words = inverse ? 2U * len : len;
    uint32_t out_words = inverse ? len : 2U * len;
    q31_t *pad = buf + in_words;
    arm_rfft_instance_q31 S;
    current_name = "RFFT Q31";
    current_len = (unsigned)len;
    current_inverse = inverse;
    if (arm_rfft_init_q31(&S, len, (uint32_t)inverse, 1U) != ARM_MATH_SUCCESS)
    {
        printf("FAIL: RFFT init length %u\n", (unsigned)len);
        return 1;
    }
    if (inverse)
    {
        /* Spectrum of the forward transform as the inverse input. */
        arm_rfft_instance_q31 F;
        arm_rfft_init_q31(&F, len, 0U, 1U);
        fill_input(len);
        memcpy(reference_out, input, len * sizeof(q31_t));
        arm_rfft_q31_reference(&F, reference_out, input);
    }
    else
    {
        fill_input(len);
    }
    for (uint32_t i = 0; i < 2U * MAX_RFFT + 2U; ++i)
        rfft_dst[i] = rfft_dst_reference[i] = CANARY;
    memcpy(reference_out, input, in_words * sizeof(q31_t));
    arm_rfft_q31_reference(&S, reference_out, rfft_dst_reference + 1);

    memcpy(buf, input, in_words * sizeof(q31_t));
    for (uint32_t i = 0; i < PAD_WORDS; ++i)
        pad[i] = CANARY;
    protect_guard((uintptr_t)arena);
    arm_rfft_q31(&S, buf, rfft_dst + 1);
    ARM_MPU_Disable();
    if (check_pad(pad))
        return 1;
    if (rfft_dst[0] != CANARY || rfft_dst[out_words + 1U] != CANARY)
    {
        printf("FAIL: RFFT Q31 %s length %u output canary\n", inverse ? "inverse" : "forward",
               (unsigned)len);
        return 1;
    }
    if (memcmp(rfft_dst, rfft_dst_reference, (out_words + 2U) * sizeof(q31_t)))
    {
        printf("FAIL: RFFT Q31 %s length %u differs from the intrinsic path\n",
               inverse ? "inverse" : "forward", (unsigned)len);
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
    static const uint32_t cfft_lengths[] = {16, 32, 64, 128, 256, 512, 1024};
    static const uint32_t rfft_lengths[] = {32, 64, 128, 256, 512, 1024, 2048};
    for (unsigned i = 0; i < sizeof(cfft_lengths) / sizeof(cfft_lengths[0]); ++i)
        if (cfft_case(cfft_lengths[i], 0, &checks) || cfft_case(cfft_lengths[i], 1, &checks))
            return 1;
    for (unsigned i = 0; i < sizeof(rfft_lengths) / sizeof(rfft_lengths[0]); ++i)
        if (rfft_case(rfft_lengths[i], 0, &checks) || rfft_case(rfft_lengths[i], 1, &checks))
            return 1;
    printf("PASS: %u MVE Q31 FFT path-parity cases\n", checks);
    return 0;
}
