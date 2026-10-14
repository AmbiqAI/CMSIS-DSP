/* Copyright (c) 2026 Ambiq Micro, Inc.
 * SPDX-License-Identifier: Apache-2.0
 */
/* arm_biquad_cascade_df1_q15, arm_biquad_cascade_df1_q31 and
 * arm_biquad_cas_df1_32x64_q31 with their source and destination against
 * MPU-inaccessible guards.
 *
 * Each case filters one block through 1 to 3 stages, with separate and with
 * in-place buffers, three times: with the buffers in padded memory (0x55
 * bytes around them), with the source and the destination each ending at a
 * guard, and with each starting right after a guard. A fault is a failure,
 * and the guarded outputs must equal the padded output bit for bit. Later
 * stages read the destination as their input, so both buffers are guarded.
 * Block sizes 1 to 17, 32, 64 and 100.
 *
 * The padded output must equal an integer model of each kernel's arithmetic,
 * and a 100-sample signal filtered in blocks of each size, with the state
 * carried between calls, must equal the model of the whole signal.
 */
#include "ARMCM55.h"
#include "dsp/filtering_functions.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "regression_checks.h"

#if !defined(ARM_MATH_MVEI) || defined(ARM_MATH_AUTOVECTORIZE)
#error This regression requires the handwritten MVE integer kernels.
#endif

#define MAX_BLOCK 100U
#define MAX_STAGES 3U
#define SPAN (MAX_BLOCK * 4U)
#define GUARD_BYTES REGRESSION_GUARD_BYTES(SPAN)
#define AREA (((SPAN + 31U) / 32U) * 32U)

extern void regression_console_init(void);

/* Two areas, each with a guard below and a guard above:
 * [guard][area A][guard][area B][guard] */
static unsigned char arena[3U * GUARD_BYTES + 2U * AREA] __ALIGNED(32);
#define G0 ((uintptr_t)arena)
#define A_START (G0 + GUARD_BYTES)
#define G1 (A_START + AREA)
#define B_START (G1 + GUARD_BYTES)
#define G2 (B_START + AREA)

static unsigned char pad_src[SPAN + 64U] __ALIGNED(16), pad_dst[SPAN + 64U] __ALIGNED(16);
static unsigned char out_ref[SPAN], out_g[SPAN], out_model[SPAN];
static const char *current_name = "none";
static volatile unsigned current_block, current_stages, current_inplace, current_place;
static unsigned cases, split_cases;

void MemManage_Handler(void)
{
    ARM_MPU_Disable();
    printf("FAIL: %s %s guard access, blockSize=%u stages=%u in-place=%u\n", current_name,
           current_place ? "below-start" : "past-end", current_block, current_stages, current_inplace);
    exit(1);
}

static void protect_guards(void)
{
    ARM_MPU_Disable();
    for (uint32_t region = 0; region < ((MPU->TYPE >> 8) & 0xffU); ++region)
        ARM_MPU_ClrRegion(region);
    ARM_MPU_SetMemAttr(0, ARM_MPU_ATTR(ARM_MPU_ATTR_NON_CACHEABLE, ARM_MPU_ATTR_NON_CACHEABLE));
    ARM_MPU_SetRegion(0, ARM_MPU_RBAR(0, ARM_MPU_SH_NON, 0, 1, 0), ARM_MPU_RLAR(G0 - 1U, 0));
    ARM_MPU_SetRegion(1, ARM_MPU_RBAR(A_START, ARM_MPU_SH_NON, 0, 1, 0), ARM_MPU_RLAR(G1 - 1U, 0));
    ARM_MPU_SetRegion(2, ARM_MPU_RBAR(B_START, ARM_MPU_SH_NON, 0, 1, 0), ARM_MPU_RLAR(G2 - 1U, 0));
    ARM_MPU_SetRegion(3, ARM_MPU_RBAR(G2 + GUARD_BYTES, ARM_MPU_SH_NON, 0, 1, 0), ARM_MPU_RLAR(UINT32_MAX, 0));
    ARM_MPU_Enable(0);
}

static void unguard(void)
{
    REGRESSION_CONTROL_READ_ABOVE(G1, GUARD_BYTES);
    ARM_MPU_Disable();
}

static const double sections[MAX_STAGES][5] = {
    {0.0675, 0.1349, 0.0675, 1.1430, -0.4128},
    {0.25, -0.5, 0.25, 0.5, -0.25},
    {0.75, 0.0, 0.0, 0.0, 0.0},
};
static const unsigned blocks[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 32, 64, 100};
#define NBLOCKS (sizeof(blocks) / sizeof(blocks[0]))

static q15_t c15[6 * MAX_STAGES], s15[4 * MAX_STAGES];
static q31_t c31[5 * MAX_STAGES], s31[4 * MAX_STAGES];
static q63_t s64[4 * MAX_STAGES];

static arm_biquad_casd_df1_inst_q15 S15;
static arm_biquad_casd_df1_inst_q31 S31;
static arm_biquad_cas_df1_32x64_ins_q31 S64;

/* kind 0: q15, 1: q31, 2: 32x64 */
static void init_kernel(int kind, unsigned stages)
{
    if (kind == 0)
        arm_biquad_cascade_df1_init_q15(&S15, (uint8_t)stages, c15, s15, 1);
    else if (kind == 1)
        arm_biquad_cascade_df1_init_q31(&S31, (uint8_t)stages, c31, s31, 1);
    else
        arm_biquad_cas_df1_32x64_init_q31(&S64, (uint8_t)stages, c31, s64, 1);
}

static void call_kernel(int kind, const void *src, void *dst, unsigned n)
{
    if (kind == 0)
        arm_biquad_cascade_df1_q15(&S15, (const q15_t *)src, (q15_t *)dst, n);
    else if (kind == 1)
        arm_biquad_cascade_df1_q31(&S31, (const q31_t *)src, (q31_t *)dst, n);
    else
        arm_biquad_cas_df1_32x64_q31(&S64, (const q31_t *)src, (q31_t *)dst, n);
}

static void run_kernel(int kind, unsigned stages, const void *src, void *dst, unsigned n)
{
    init_kernel(kind, stages);
    call_kernel(kind, src, dst, n);
}

/* 64-bit sum of the products rounded and shifted right by 8, as VRMLALDAVH does */
static int64_t round_sum8(const int64_t *p, unsigned n)
{
    int64_t hi = 0, lo = 0;
    for (unsigned i = 0; i < n; ++i)
    {
        hi += p[i] >> 8;
        lo += p[i] & 255;
    }
    return hi + ((lo + 128) >> 8);
}

/* Integer model of each kernel from zero state, postShift 1: q15 keeps the
 * low 15 - postShift bits of the 64-bit sum and saturates to 16 bits; q31
 * rounds the feedforward and feedback sums to 64 bits as VRMLALDAVH(A) does
 * and keeps bits 23 - postShift to 54 - postShift; 32x64 keeps the 64-bit
 * output state Y = (a1 Y1 + a2 Y2 (32x64 products) + b0 x0 + b1 x1 + b2 x2)
 * << (postShift + 1) and outputs its high 32 bits. */
static void model(int kind, unsigned stages, const void *src, void *dst, unsigned n)
{
    const int ps = 1;
    if (kind == 0)
    {
        const q15_t *in = (const q15_t *)src;
        q15_t *out = (q15_t *)dst;
        for (unsigned st = 0; st < stages; ++st, in = out)
        {
            const q15_t *c = &c15[6 * st];
            int32_t x1 = 0, x2 = 0, y1 = 0, y2 = 0;
            for (unsigned i = 0; i < n; ++i)
            {
                int32_t x0 = in[i];
                int64_t acc = (int64_t)c[0] * x0 + (int64_t)c[2] * x1 + (int64_t)c[3] * x2 +
                              (int64_t)c[4] * y1 + (int64_t)c[5] * y2;
                acc >>= 15 - ps;
                int32_t y = acc > 32767 ? 32767 : acc < -32768 ? -32768 : (int32_t)acc;
                out[i] = (q15_t)y;
                x2 = x1;
                x1 = x0;
                y2 = y1;
                y1 = y;
            }
        }
    }
    else
    {
        const q31_t *in = (const q31_t *)src;
        q31_t *out = (q31_t *)dst;
        for (unsigned st = 0; st < stages; ++st, in = out)
        {
            const q31_t *c = &c31[5 * st];
            int32_t x1 = 0, x2 = 0, y1 = 0, y2 = 0;
            int64_t Y1 = 0, Y2 = 0;
            for (unsigned i = 0; i < n; ++i)
            {
                int32_t x0 = in[i], y;
                int64_t pb[3] = {(int64_t)c[0] * x0, (int64_t)c[1] * x1, (int64_t)c[2] * x2};
                if (kind == 1)
                {
                    int64_t pa[2] = {(int64_t)c[3] * y1, (int64_t)c[4] * y2};
                    uint64_t acc = (uint64_t)(round_sum8(pb, 3) + round_sum8(pa, 2));
                    y = (int32_t)(uint32_t)((acc << (ps + 9)) >> 32);
                }
                else
                {
                    int64_t acc = (int64_t)((uint64_t)Y1 & 0xffffffffU) * c[3] >> 32;
                    acc += (Y1 >> 32) * c[3];
                    acc += (int64_t)((uint64_t)Y2 & 0xffffffffU) * c[4] >> 32;
                    acc += (Y2 >> 32) * c[4];
                    acc += pb[0] + pb[1] + pb[2];
                    Y2 = Y1;
                    Y1 = (int64_t)((uint64_t)acc << (ps + 1));
                    y = (int32_t)(Y1 >> 32);
                }
                out[i] = y;
                x2 = x1;
                x1 = x0;
                y2 = y1;
                y1 = y;
            }
        }
    }
}

static void fill_input(int kind, void *p, unsigned n)
{
    for (unsigned i = 0; i < n; ++i)
    {
        double v = 0.45 * ((double)((int)((i * 37U + 5U) % 23U) - 11) / 11.0);
        if (kind == 0)
            ((q15_t *)p)[i] = (q15_t)(v * 32767.0);
        else
            ((q31_t *)p)[i] = (q31_t)(v * 2147483647.0);
    }
}

static const char *const names[3] = {"arm_biquad_cascade_df1_q15", "arm_biquad_cascade_df1_q31",
                                     "arm_biquad_cas_df1_32x64_q31"};

static int run(int kind)
{
    unsigned size = kind == 0 ? 2U : 4U;
    current_name = names[kind];
    for (unsigned stages = 1; stages <= MAX_STAGES; ++stages)
        for (unsigned inplace = 0; inplace <= 1; ++inplace)
            for (unsigned k = 0; k < NBLOCKS; ++k)
            {
                unsigned n = blocks[k], bytes = n * size;
                current_block = n;
                current_stages = stages;
                current_inplace = inplace;
                /* padded reference */
                memset(pad_src, 0x55, sizeof(pad_src));
                memset(pad_dst, 0x55, sizeof(pad_dst));
                fill_input(kind, pad_src + 32, n);
                if (inplace)
                {
                    memcpy(pad_dst + 32, pad_src + 32, bytes);
                    run_kernel(kind, stages, pad_dst + 32, pad_dst + 32, n);
                }
                else
                    run_kernel(kind, stages, pad_src + 32, pad_dst + 32, n);
                memcpy(out_ref, pad_dst + 32, bytes);
                fill_input(kind, pad_src + 32, n);
                model(kind, stages, pad_src + 32, out_model, n);
                if (memcmp(out_ref, out_model, bytes))
                {
                    printf("FAIL: %s output differs from the integer model, blockSize=%u stages=%u in-place=%u\n",
                           names[kind], n, stages, inplace);
                    return 1;
                }
                for (unsigned place = 0; place < 2; ++place)
                {
                    /* place 0: buffers end at G1 and G2; place 1: start after G0 and G1 */
                    unsigned char *src = place ? (unsigned char *)A_START : (unsigned char *)G1 - bytes;
                    unsigned char *dst = place ? (unsigned char *)B_START : (unsigned char *)G2 - bytes;
                    current_place = place;
                    fill_input(kind, src, n);
                    if (inplace)
                        dst = src;
                    protect_guards();
                    run_kernel(kind, stages, src, dst, n);
                    unguard();
                    memcpy(out_g, dst, bytes);
                    if (memcmp(out_g, out_ref, bytes))
                    {
                        printf("FAIL: %s %s output differs from padded memory, blockSize=%u stages=%u in-place=%u\n",
                               names[kind], place ? "below-start" : "past-end", n, stages, inplace);
                        return 1;
                    }
                    ++cases;
                }
            }
    return 0;
}

/* A 100-sample signal in blocks of each size, the state carried between calls */
static int run_split(int kind)
{
    unsigned size = kind == 0 ? 2U : 4U;
    for (unsigned stages = 1; stages <= MAX_STAGES; ++stages)
        for (unsigned k = 0; k < NBLOCKS; ++k)
        {
            unsigned n = blocks[k];
            memset(pad_dst, 0x55, sizeof(pad_dst));
            fill_input(kind, pad_src + 32, MAX_BLOCK);
            model(kind, stages, pad_src + 32, out_model, MAX_BLOCK);
            init_kernel(kind, stages);
            for (unsigned i = 0; i < MAX_BLOCK; i += n)
            {
                unsigned m = MAX_BLOCK - i < n ? MAX_BLOCK - i : n;
                call_kernel(kind, pad_src + 32 + i * size, pad_dst + 32 + i * size, m);
            }
            if (memcmp(pad_dst + 32, out_model, MAX_BLOCK * size))
            {
                printf("FAIL: %s blocks of %u differ from the integer model, stages=%u\n", names[kind], n, stages);
                return 1;
            }
            ++split_cases;
        }
    return 0;
}

int main(void)
{
    regression_console_init();
    SCB->SHCSR |= SCB_SHCSR_MEMFAULTENA_Msk;
    for (unsigned s = 0; s < MAX_STAGES; ++s)
    {
        const double *d = sections[s];
        c15[6 * s + 0] = (q15_t)(d[0] * 16384.0);
        c15[6 * s + 1] = 0;
        c15[6 * s + 2] = (q15_t)(d[1] * 16384.0);
        c15[6 * s + 3] = (q15_t)(d[2] * 16384.0);
        c15[6 * s + 4] = (q15_t)(d[3] * 16384.0);
        c15[6 * s + 5] = (q15_t)(d[4] * 16384.0);
        for (unsigned k = 0; k < 5; ++k)
            c31[5 * s + k] = (q31_t)(d[k] * 1073741824.0);
    }
    if (run(0) || run(1) || run(2) || run_split(0) || run_split(1) || run_split(2))
        return 1;
    printf("PASS: %u MVE biquad df1 bounded-access cases, %u block-split cases\n", cases, split_cases);
    return 0;
}
