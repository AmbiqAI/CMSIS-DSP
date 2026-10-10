/* Copyright (c) 2026 Ambiq Micro, Inc.
 * SPDX-License-Identifier: Apache-2.0
 */
/* Cortex-M55 regression: the MVE complex FFTs (f32, Q31, Q15, f16; lengths
 * 16 to 4096, forward and inverse) read nothing past the end of the in-place
 * buffer. Each transform runs on a buffer that ends at an MPU-inaccessible
 * guard; a guard access is a failure. The output is compared with a
 * double-precision radix-2 FFT of the same input. Before the guarded runs,
 * the test measures how far each transform reads above the buffer end, by
 * placing the buffer k words below the guard for k = 0..64 until no fault
 * occurs, and prints the extent (OVERREAD lines; 0 after the fix, 56 bytes
 * for upstream, 60 for Q15).
 */
#include "ARMCM55.h"
#include "dsp/transform_functions.h"
#include "dsp/transform_functions_f16.h"
#include <math.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(ARM_MATH_MVEF) || !defined(ARM_MATH_MVEI) || !defined(ARM_MATH_MVE_FLOAT16) || \
    defined(ARM_MATH_AUTOVECTORIZE)
#error This regression requires the handwritten MVE kernels with float16 enabled.
#endif

#define MAX_LEN 4096U
#define GUARD_BYTES 32U
#define PROBE_WORDS 64U

extern void regression_console_init(void);

/* All large arrays live in SRAM; DTCM is too small. */
#define LARGE __attribute__((section(".bss.tensor_arena")))
/* [probe slack | buffer up to 2*MAX_LEN words | guard] */
static uint32_t arena[PROBE_WORDS + 2U * MAX_LEN + GUARD_BYTES / 4U] __ALIGNED(32) LARGE;
static double ref_re[MAX_LEN] LARGE, ref_im[MAX_LEN] LARGE;
static double in_re[MAX_LEN] LARGE, in_im[MAX_LEN] LARGE;

static jmp_buf probe_env;
static volatile unsigned probing, probe_faulted;
static const char *current_name = "none";
static volatile unsigned current_len, current_inverse;

static void probe_recover(void)
{
    longjmp(probe_env, 1);
}

/* During the probe the fault returns into probe_recover() through the stacked
 * PC (ICI/IT and ECI bits cleared, the faulting instruction abandoned);
 * otherwise a fault is a failure. */
void memmanage_c(uint32_t *frame)
{
    ARM_MPU_Disable();
    SCB->CFSR = SCB->CFSR;
    if (probing)
    {
        probe_faulted = 1;
        frame[6] = (uint32_t)probe_recover & ~1U;
        frame[7] &= ~0x0600FC00U;
        frame[7] |= 1U << 24;
        return;
    }
    printf("FAIL: %s %s length %u touched the guard (MMFAR 0x%08lx)\n", current_name,
           current_inverse ? "inverse" : "forward", current_len, (unsigned long)SCB->MMFAR);
    exit(1);
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

static uintptr_t guard_address(void)
{
    return (uintptr_t)(arena + PROBE_WORDS + 2U * MAX_LEN);
}

static void protect_guard(void)
{
    uintptr_t guard = guard_address();
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

/* Pseudo-random input in (-0.5, 0.5) of full scale, the same sequence for
 * every type. */
static double sample(uint32_t i)
{
    return (double)((i * 2654435761U) >> 8 & 0xffffffU) / 16777216.0 - 0.5;
}

static void make_input(uint32_t len)
{
    for (uint32_t i = 0; i < len; ++i)
    {
        in_re[i] = sample(2U * i);
        in_im[i] = sample(2U * i + 1U);
    }
}

/* In-place iterative radix-2 FFT in double; inverse divides by len. */
static void reference_fft(uint32_t len, unsigned inverse)
{
    memcpy(ref_re, in_re, len * sizeof(double));
    memcpy(ref_im, in_im, len * sizeof(double));
    for (uint32_t i = 1, j = 0; i < len; ++i)
    {
        uint32_t bit = len >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j)
        {
            double t = ref_re[i]; ref_re[i] = ref_re[j]; ref_re[j] = t;
            t = ref_im[i]; ref_im[i] = ref_im[j]; ref_im[j] = t;
        }
    }
    for (uint32_t size = 2; size <= len; size <<= 1)
    {
        double angle = (inverse ? 2.0 : -2.0) * 3.14159265358979323846 / (double)size;
        for (uint32_t start = 0; start < len; start += size)
        {
            for (uint32_t k = 0; k < size / 2U; ++k)
            {
                double wr = cos(angle * (double)k), wi = sin(angle * (double)k);
                uint32_t a = start + k, b = a + size / 2U;
                double tr = ref_re[b] * wr - ref_im[b] * wi;
                double ti = ref_re[b] * wi + ref_im[b] * wr;
                ref_re[b] = ref_re[a] - tr; ref_im[b] = ref_im[a] - ti;
                ref_re[a] += tr; ref_im[a] += ti;
            }
        }
    }
    if (inverse)
        for (uint32_t i = 0; i < len; ++i)
        {
            ref_re[i] /= (double)len;
            ref_im[i] /= (double)len;
        }
}

/* One transform type: element size, fill and read in full-scale units, run,
 * output scale relative to the reference, and tolerance in full-scale units. */
typedef struct
{
    const char *name;
    size_t elem;
    int (*fill)(void *buf, uint32_t len);
    double (*get)(const void *buf, uint32_t i);
    int (*run)(void *buf, uint32_t len, unsigned inverse);
    double (*scale)(uint32_t len, unsigned inverse);
    double (*tolerance)(uint32_t len, double max_ref);
} transform_t;

static int fill_f32(void *buf, uint32_t len)
{
    float32_t *p = buf;
    for (uint32_t i = 0; i < len; ++i)
    {
        p[2U * i] = (float32_t)in_re[i];
        p[2U * i + 1U] = (float32_t)in_im[i];
    }
    return 0;
}
static double get_f32(const void *buf, uint32_t i) { return (double)((const float32_t *)buf)[i]; }
static int run_f32(void *buf, uint32_t len, unsigned inverse)
{
    arm_cfft_instance_f32 S;
    if (arm_cfft_init_f32(&S, (uint16_t)len) != ARM_MATH_SUCCESS)
        return 1;
    arm_cfft_f32(&S, buf, (uint8_t)inverse, 1U);
    return 0;
}
static double scale_float(uint32_t len, unsigned inverse) { (void)len; (void)inverse; return 1.0; }
static double tolerance_f32(uint32_t len, double max_ref) { (void)len; return 1e-5 * (max_ref + 1.0); }

static int fill_f16(void *buf, uint32_t len)
{
    float16_t *p = buf;
    for (uint32_t i = 0; i < len; ++i)
    {
        p[2U * i] = (float16_t)in_re[i];
        p[2U * i + 1U] = (float16_t)in_im[i];
    }
    return 0;
}
static double get_f16(const void *buf, uint32_t i) { return (double)((const float16_t *)buf)[i]; }
static int run_f16(void *buf, uint32_t len, unsigned inverse)
{
    arm_cfft_instance_f16 S;
    if (arm_cfft_init_f16(&S, (uint16_t)len) != ARM_MATH_SUCCESS)
        return 1;
    arm_cfft_f16(&S, buf, (uint8_t)inverse, 1U);
    return 0;
}
/* Half precision carries 11 significant bits; the input is rounded to it
 * before the reference is taken, so the remaining error is the transform's. */
static double tolerance_f16(uint32_t len, double max_ref)
{
    (void)len;
    return 1.5e-2 * (max_ref + 0.25);
}

static int fill_q31(void *buf, uint32_t len)
{
    q31_t *p = buf;
    for (uint32_t i = 0; i < len; ++i)
    {
        p[2U * i] = (q31_t)(in_re[i] * 2147483648.0);
        p[2U * i + 1U] = (q31_t)(in_im[i] * 2147483648.0);
    }
    return 0;
}
static double get_q31(const void *buf, uint32_t i) { return (double)((const q31_t *)buf)[i] / 2147483648.0; }
static int run_q31(void *buf, uint32_t len, unsigned inverse)
{
    arm_cfft_instance_q31 S;
    if (arm_cfft_init_q31(&S, (uint16_t)len) != ARM_MATH_SUCCESS)
        return 1;
    arm_cfft_q31(&S, buf, (uint8_t)inverse, 1U);
    return 0;
}
/* The fixed-point transforms divide by len in both directions. */
static double scale_fixed(uint32_t len, unsigned inverse) { return inverse ? 1.0 : 1.0 / (double)len; }
static double tolerance_q31(uint32_t len, double max_ref)
{
    (void)max_ref;
    return 4096.0 * (double)len / 2147483648.0; /* 4096 LSB of the 1/len-scaled output */
}

static int fill_q15(void *buf, uint32_t len)
{
    q15_t *p = buf;
    for (uint32_t i = 0; i < len; ++i)
    {
        p[2U * i] = (q15_t)(in_re[i] * 32768.0);
        p[2U * i + 1U] = (q15_t)(in_im[i] * 32768.0);
    }
    return 0;
}
static double get_q15(const void *buf, uint32_t i) { return (double)((const q15_t *)buf)[i] / 32768.0; }
static int run_q15(void *buf, uint32_t len, unsigned inverse)
{
    arm_cfft_instance_q15 S;
    if (arm_cfft_init_q15(&S, (uint16_t)len) != ARM_MATH_SUCCESS)
        return 1;
    arm_cfft_q15(&S, buf, (uint8_t)inverse, 1U);
    return 0;
}
static double tolerance_q15(uint32_t len, double max_ref)
{
    (void)max_ref;
    return 64.0 * (double)len / 32768.0; /* 64 LSB of the 1/len-scaled output */
}

static const transform_t transforms[] = {
    {"CFFT f32", sizeof(float32_t), fill_f32, get_f32, run_f32, scale_float, tolerance_f32},
    {"CFFT Q31", sizeof(q31_t), fill_q31, get_q31, run_q31, scale_fixed, tolerance_q31},
    {"CFFT Q15", sizeof(q15_t), fill_q15, get_q15, run_q15, scale_fixed, tolerance_q15},
    {"CFFT f16", sizeof(float16_t), fill_f16, get_f16, run_f16, scale_float, tolerance_f16},
};

/* Buffer of 2*len elements ending k words below the guard. */
static void *buffer_below_guard(const transform_t *t, uint32_t len, unsigned k)
{
    return (void *)(guard_address() - 4U * k - 2U * len * t->elem);
}

static unsigned probe_overread(const transform_t *t, uint32_t len, unsigned inverse)
{
    for (unsigned k = 0; k <= PROBE_WORDS; ++k)
    {
        void *buf = buffer_below_guard(t, len, k);
        t->fill(buf, len);
        probe_faulted = 0;
        probing = 1;
        if (setjmp(probe_env) == 0)
        {
            protect_guard();
            t->run(buf, len, inverse);
        }
        ARM_MPU_Disable();
        probing = 0;
        if (!probe_faulted)
            return 4U * k;
    }
    return 0xffffffffU;
}

static int guarded_case(const transform_t *t, uint32_t len, unsigned inverse)
{
    void *buf = buffer_below_guard(t, len, 0);
    current_name = t->name;
    current_len = (unsigned)len;
    current_inverse = inverse;
    make_input(len);
    if (t->fill(buf, len))
        return 1;
    /* The reference sees the input as the transform does, after rounding
       to the type's precision. */
    for (uint32_t i = 0; i < len; ++i)
    {
        in_re[i] = t->get(buf, 2U * i);
        in_im[i] = t->get(buf, 2U * i + 1U);
    }
    reference_fft(len, inverse);
    protect_guard();
    int status = t->run(buf, len, inverse);
    ARM_MPU_Disable();
    if (status)
    {
        printf("FAIL: %s init length %u\n", t->name, (unsigned)len);
        return 1;
    }
    double scale = t->scale(len, inverse), max_ref = 0.0, max_err = 0.0;
    for (uint32_t i = 0; i < len; ++i)
    {
        double r = fabs(ref_re[i] * scale), m = fabs(ref_im[i] * scale);
        if (r > max_ref) max_ref = r;
        if (m > max_ref) max_ref = m;
    }
    double tol = t->tolerance(len, max_ref);
    for (uint32_t i = 0; i < len; ++i)
    {
        double er = fabs(t->get(buf, 2U * i) - ref_re[i] * scale);
        double ei = fabs(t->get(buf, 2U * i + 1U) - ref_im[i] * scale);
        if (er > max_err) max_err = er;
        if (ei > max_err) max_err = ei;
    }
    if (!(max_err <= tol))
    {
        printf("FAIL: %s %s length %u differs from the reference: max error %g, tolerance %g\n",
               t->name, inverse ? "inverse" : "forward", (unsigned)len, max_err, tol);
        return 1;
    }
    return 0;
}

int main(void)
{
    regression_console_init();
    SCB->SHCSR |= SCB_SHCSR_MEMFAULTENA_Msk;
    static const uint32_t lengths[] = {16, 32, 64, 128, 256, 512, 1024, 2048, 4096};
    const unsigned n_lengths = sizeof(lengths) / sizeof(lengths[0]);
    const unsigned n_types = sizeof(transforms) / sizeof(transforms[0]);
    unsigned cases = 0;

    for (unsigned ti = 0; ti < n_types; ++ti)
    {
        const transform_t *t = &transforms[ti];
        current_name = t->name;
        for (unsigned li = 0; li < n_lengths; ++li)
        {
            current_len = lengths[li];
            make_input(lengths[li]);
            printf("OVERREAD %s length %u: forward %u inverse %u bytes\n", t->name,
                   (unsigned)lengths[li], probe_overread(t, lengths[li], 0),
                   probe_overread(t, lengths[li], 1));
        }
    }
    for (unsigned ti = 0; ti < n_types; ++ti)
        for (unsigned li = 0; li < n_lengths; ++li)
            for (unsigned inverse = 0; inverse < 2U; ++inverse)
            {
                if (guarded_case(&transforms[ti], lengths[li], inverse))
                    return 1;
                ++cases;
            }
    printf("PASS: %u MVE CFFT last-stage cases\n", cases);
    return 0;
}
