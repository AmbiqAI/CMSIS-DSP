/* Copyright (c) 2026 Ambiq Micro, Inc.
 * SPDX-License-Identifier: Apache-2.0
 */
/*
 * arm_rfft_fast_f32 built with ARM_MATH_RFFT_FAST_INDEXED_SPLIT against the
 * same source built without it (linked as arm_rfft_fast_f32_reference) and
 * against a float64 discrete Fourier transform, for every supported length.
 *
 * Per length:
 *  - the bin index table equals the permutation arm_bitreversal_32 applies
 *    to an identity array with the library's bit-reversal table;
 *  - the source buffer the indexed path leaves equals, bit for bit, the
 *    reference path's source buffer read through the table, so the complex
 *    FFT kernel ran and the bit reversal pass did not;
 *  - output bins 0 and N/2 (the packed real pair) are bit-identical to the
 *    reference path's;
 *  - forward error of both paths against float64, their mutual difference
 *    and the error of the inverse of the indexed output against the input
 *    are within TOLERANCE of the largest output (input for the inverse).
 */
#include "arm_math.h"
#include "arm_common_tables.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define MAX_LENGTH 4096U
#define SIGNALS 6U
#define TOLERANCE 2.0e-6

void arm_rfft_fast_f32_reference(const arm_rfft_fast_instance_f32 *S, float32_t *p,
                                 float32_t *pOut, uint8_t ifftFlag);
void arm_bitreversal_32(uint32_t *pSrc, const uint16_t bitRevLen,
                        const uint16_t *pBitRevTab);

static float32_t input[MAX_LENGTH], work_new[MAX_LENGTH], work_ref[MAX_LENGTH];
static float32_t out_new[MAX_LENGTH], out_ref[MAX_LENGTH], restored[MAX_LENGTH];
static float64_t dft[MAX_LENGTH], cosine[MAX_LENGTH], sine[MAX_LENGTH];
static uint32_t identity[MAX_LENGTH];
static uint32_t lcg_state = 0x2545F491U;

static float32_t uniform(void)
{
    lcg_state = lcg_state * 1664525U + 1013904223U;
    return (float32_t)((int32_t)(lcg_state >> 8) - 0x800000) / 8388608.0f;
}

static const uint16_t *table_for(uint32_t cfft_length)
{
    switch (cfft_length)
    {
        case 16U: return armCfftBinIndexTable16;
        case 32U: return armCfftBinIndexTable32;
        case 64U: return armCfftBinIndexTable64;
        case 128U: return armCfftBinIndexTable128;
        case 256U: return armCfftBinIndexTable256;
        case 512U: return armCfftBinIndexTable512;
        case 1024U: return armCfftBinIndexTable1024;
        default: return armCfftBinIndexTable2048;
    }
}

static void make_signal(uint32_t kind, uint32_t n)
{
    uint32_t i;
    for (i = 0U; i < n; i++)
    {
        switch (kind)
        {
            case 0U: input[i] = (i == 1U) ? 1.0f : 0.0f; break;
            case 1U: input[i] = 1.0f; break;
            case 2U: input[i] = (i & 1U) ? -1.0f : 1.0f; break;
            case 3U: input[i] = (float32_t)cos(2.0 * 3.14159265358979323846 * 3.0 * i / n + 0.5); break;
            default: input[i] = uniform(); break;
        }
    }
}

/* Packed as arm_rfft_fast_f32 packs it: [X0, X(N/2), Re X1, Im X1, ...] */
static void reference_dft(uint32_t n)
{
    uint32_t k, i;
    for (k = 0U; k <= n / 2U; k++)
    {
        float64_t re = 0.0, im = 0.0;
        for (i = 0U; i < n; i++)
        {
            uint32_t m = (k * i) & (n - 1U);
            re += (float64_t)input[i] * cosine[m];
            im -= (float64_t)input[i] * sine[m];
        }
        if (k == 0U) dft[0] = re;
        else if (k == n / 2U) dft[1] = re;
        else { dft[2U * k] = re; dft[2U * k + 1U] = im; }
    }
}

int main(void)
{
    uint32_t n, lengths = 0U;
    int failed = 0;

    for (n = 32U; n <= MAX_LENGTH; n <<= 1)
    {
        arm_rfft_fast_instance_f32 S;
        uint32_t half = n / 2U, i, l, kind;
        const uint16_t *table = table_for(half);
        float64_t err_new = 0.0, err_ref = 0.0, diff = 0.0, err_inverse = 0.0, peak = 0.0;
        int table_ok = 1, buffer_ok = 1, packed_ok = 1;

        if (arm_rfft_fast_init_f32(&S, (uint16_t)n) != ARM_MATH_SUCCESS)
        {
            printf("FAIL: init n=%u\n", (unsigned)n);
            return 1;
        }
        for (i = 0U; i < n; i++)
        {
            identity[i] = i;
            cosine[i] = cos(2.0 * 3.14159265358979323846 * i / n);
            sine[i] = sin(2.0 * 3.14159265358979323846 * i / n);
        }
        arm_bitreversal_32(identity, S.Sint.bitRevLength, S.Sint.pBitRevTable);
        for (l = 0U; l < half; l++)
        {
            if (identity[2U * l] != 2U * table[l] || identity[2U * l + 1U] != 2U * table[l] + 1U)
                table_ok = 0;
        }

        for (kind = 0U; kind < SIGNALS; kind++)
        {
            float64_t scale = 0.0, e_new = 0.0, e_ref = 0.0, d = 0.0, e_inv = 0.0;

            make_signal(kind, n);
            reference_dft(n);
            memcpy(work_new, input, n * sizeof(float32_t));
            memcpy(work_ref, input, n * sizeof(float32_t));
            arm_rfft_fast_f32(&S, work_new, out_new, 0U);
            arm_rfft_fast_f32_reference(&S, work_ref, out_ref, 0U);

            for (l = 0U; l < half; l++)
            {
                if (memcmp(&work_new[2U * table[l]], &work_ref[2U * l], 2U * sizeof(float32_t)) != 0)
                    buffer_ok = 0;
            }
            if (memcmp(out_new, out_ref, 2U * sizeof(float32_t)) != 0)
                packed_ok = 0;
            for (i = 0U; i < n; i++)
            {
                scale = fmax(scale, fabs(dft[i]));
                e_new = fmax(e_new, fabs((float64_t)out_new[i] - dft[i]));
                e_ref = fmax(e_ref, fabs((float64_t)out_ref[i] - dft[i]));
                d = fmax(d, fabs((float64_t)out_new[i] - (float64_t)out_ref[i]));
            }
            /* The inverse modifies its source: out_new is not read again. */
            arm_rfft_fast_f32(&S, out_new, restored, 1U);
            for (i = 0U; i < n; i++)
                e_inv = fmax(e_inv, fabs((float64_t)restored[i] - (float64_t)input[i]));

            peak = fmax(peak, scale);
            err_new = fmax(err_new, e_new / scale);
            err_ref = fmax(err_ref, e_ref / scale);
            diff = fmax(diff, d / scale);
            err_inverse = fmax(err_inverse, e_inv);   /* the inputs peak at 1 */
            if (kind == SIGNALS - 1U)
                printf("n=%u uniform: abs error indexed %.3e reference %.3e, difference %.3e, peak %.3e\n",
                       (unsigned)n, e_new, e_ref, d, scale);
        }
        printf("n=%u relative: indexed %.3e reference %.3e difference %.3e inverse %.3e table=%d buffer=%d packed=%d\n",
               (unsigned)n, err_new, err_ref, diff, err_inverse, table_ok, buffer_ok, packed_ok);
        if (!table_ok || !buffer_ok || !packed_ok || err_new > TOLERANCE || err_ref > TOLERANCE ||
            diff > TOLERANCE || err_inverse > TOLERANCE)
        {
            printf("FAIL: n=%u\n", (unsigned)n);
            failed = 1;
        }
        lengths++;
    }
    if (failed)
        return 1;
    printf("PASS: %u indexed split RFFT lengths\n", (unsigned)lengths);
    return 0;
}
