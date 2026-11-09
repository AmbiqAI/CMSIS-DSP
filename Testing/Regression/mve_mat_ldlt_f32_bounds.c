/* Copyright (c) 2026 Ambiq Micro, Inc.
 * SPDX-License-Identifier: Apache-2.0
 */
/* Cortex-M55 regression: the MVE arm_mat_ldlt_f32 accesses nothing outside
 * its n x n operands. The input A, the outputs L and D and the permutation
 * vector each end at an MPU-inaccessible guard; a MemManage fault reports the
 * operand and the offset of the faulting address past its end.
 *
 * Sizes 1 to 16, 17, 20, 23, 24, 31, 32 and 33, each with a symmetric
 * positive definite input, a symmetric indefinite (diagonally dominant)
 * input and a rank-deficient input (a positive definite block of n/2 rows,
 * zeros elsewhere), 69 cases. Every result must be finite (checked by its
 * bits) with permutation entries below n. The positive definite and
 * indefinite results are checked against P A P^t = L D L^t in double
 * precision, with P built from the permutation vector as Arm's tests build
 * it. The rank-deficient inputs exercise the zeroing of the columns after
 * the last pivot; their reconstruction is not checked, because every
 * arm_mat_ldlt_f32 path (and arm_mat_ldlt_f64) leaves the last nonzero pivot
 * out of D when it stops early.
 */
#include "ARMCM55.h"
#include "dsp/matrix_functions.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "regression_checks.h"

#if !defined(ARM_MATH_MVEF) || defined(ARM_MATH_AUTOVECTORIZE)
#error This regression requires the handwritten MVE float kernels.
#endif

#define NMAX 33U
#define BLOCK_BYTES ((NMAX * NMAX * 4U + 31U) & ~31U)
/* Each guard covers a whole operand plus one vector access (regression_checks.h). */
#define GUARD_BYTES REGRESSION_GUARD_BYTES(BLOCK_BYTES)
#define SLOT_BYTES (BLOCK_BYTES + GUARD_BYTES)

extern void regression_console_init(void);

/* Four operand blocks (A, L, D, permutation), each followed by its guard. */
static unsigned char arena[4U * SLOT_BYTES] __ALIGNED(32);
#define GUARD(i) ((uintptr_t)arena + (i) * SLOT_BYTES + BLOCK_BYTES)
static const char *const operand_name[4] = {"A", "L", "D", "permutation"};

static volatile unsigned current_n;
static volatile const char *current_kind = "none";

void MemManage_Handler(void)
{
    uint32_t cfsr = SCB->CFSR, addr = SCB->MMFAR;
    ARM_MPU_Disable();
    if (cfsr & SCB_CFSR_MMARVALID_Msk)
    {
        for (unsigned i = 0; i < 4U; ++i)
            if (addr >= GUARD(i) && addr < GUARD(i) + GUARD_BYTES)
            {
                printf("FAIL: arm_mat_ldlt_f32 %s n=%u access %lu bytes past %s\n", current_kind,
                       current_n, (unsigned long)(addr - GUARD(i)), operand_name[i]);
                exit(1);
            }
    }
    printf("FAIL: arm_mat_ldlt_f32 %s n=%u guard access\n", current_kind, current_n);
    exit(1);
}

static void protect_guards(void)
{
    ARM_MPU_Disable();
    for (uint32_t region = 0; region < ((MPU->TYPE >> 8) & 0xffU); ++region)
    {
        ARM_MPU_ClrRegion(region);
    }
    ARM_MPU_SetMemAttr(0, ARM_MPU_ATTR(ARM_MPU_ATTR_NON_CACHEABLE,
                                     ARM_MPU_ATTR_NON_CACHEABLE));
    /* Map everything except the four aligned guards. No background map. */
    uintptr_t start = 0;
    for (uint32_t i = 0; i < 4U; ++i)
    {
        ARM_MPU_SetRegion(i, ARM_MPU_RBAR(start, ARM_MPU_SH_NON, 0, 1, 0),
                         ARM_MPU_RLAR(GUARD(i) - 1U, 0));
        start = GUARD(i) + GUARD_BYTES;
    }
    ARM_MPU_SetRegion(4, ARM_MPU_RBAR(start, ARM_MPU_SH_NON, 0, 1, 0),
                     ARM_MPU_RLAR(UINT32_MAX, 0));
    ARM_MPU_Enable(0);
}

/* Ends a guarded call; a read control (regression_checks.h) reads past L
 * before the guards are lifted. */
static void unguard_buffers(void)
{
    REGRESSION_CONTROL_READ_ABOVE(GUARD(1), GUARD_BYTES);
    ARM_MPU_Disable();
}

static uint32_t lcg = 1U;
static double rnd(void)
{
    lcg = lcg * 1664525U + 1013904223U;
    return (double)(lcg >> 8) / 16777216.0 - 0.5;
}

/* kind 0: B B^t + n I; kind 1: diagonal of alternating sign and magnitude
 * n + 2 + r, off-diagonal entries in [-0.5, 0.5); kind 2: B B^t + m I on the
 * leading m = n/2 rows and columns, zeros elsewhere. */
static void fill(float32_t *a, int kind, unsigned n)
{
    static double b[NMAX * NMAX];
    for (unsigned i = 0; i < n * n; ++i)
        b[i] = rnd();
    unsigned m = kind == 2 ? n / 2U : n;
    for (unsigned r = 0; r < n; ++r)
        for (unsigned c = 0; c <= r; ++c)
        {
            double v = 0.0;
            if (kind == 1)
                v = (r == c) ? (((r * 7U) % 3U) ? 1.0 : -1.0) * (double)(n + 2U + r) : b[r * n + c];
            else if (r < m && c < m)
            {
                for (unsigned k = 0; k < m; ++k)
                    v += b[r * n + k] * b[c * n + k];
                if (r == c)
                    v += (double)m;
            }
            a[r * n + c] = a[c * n + r] = (float32_t)v;
        }
}

/* max |P A P^t - L D L^t| relative to max |A|. P is the identity with rows
 * r and p[r] swapped for r = 0, 1, ..., as Arm's tests build it; row r of P
 * is then the unit vector e_q[r]. */
static double reconstruction_error(const float32_t *a, const float32_t *l, const float32_t *d,
                                   const uint16_t *p, unsigned n)
{
    unsigned q[NMAX];
    for (unsigned r = 0; r < n; ++r)
        q[r] = r;
    for (unsigned r = 0; r < n; ++r)
    {
        unsigned t = q[r];
        q[r] = q[p[r]];
        q[p[r]] = t;
    }
    double scale = 0.0, err = 0.0;
    for (unsigned i = 0; i < n * n; ++i)
        if (fabs((double)a[i]) > scale)
            scale = fabs((double)a[i]);
    for (unsigned r = 0; r < n; ++r)
        for (unsigned c = 0; c < n; ++c)
        {
            double t = 0.0;
            for (unsigned k = 0; k < n; ++k)
                t += (double)l[r * n + k] * (double)d[k * n + k] * (double)l[c * n + k];
            double e = fabs((double)a[q[r] * n + q[c]] - t);
            if (e > err)
                err = e;
        }
    return scale > 0.0 ? err / scale : err;
}

static const unsigned sizes[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16,
                                 17, 20, 23, 24, 31, 32, 33};
static const char *const kind_name[3] = {"positive-definite", "indefinite", "rank-deficient"};

int main(void)
{
    regression_console_init();
    SCB->SHCSR |= SCB_SHCSR_MEMFAULTENA_Msk;
    if (((MPU->TYPE >> 8) & 0xffU) < 5U)
    {
        printf("FAIL: the MPU has fewer than five regions\n");
        return 1;
    }
    unsigned cases = 0;
    for (int kind = 0; kind < 3; ++kind)
        for (unsigned s = 0; s < sizeof(sizes) / sizeof(sizes[0]); ++s)
        {
            unsigned n = sizes[s];
            float32_t *a = (float32_t *)GUARD(0) - n * n;
            float32_t *l = (float32_t *)GUARD(1) - n * n;
            float32_t *d = (float32_t *)GUARD(2) - n * n;
            uint16_t *p = (uint16_t *)GUARD(3) - n;
            arm_matrix_instance_f32 A = {(uint16_t)n, (uint16_t)n, a};
            arm_matrix_instance_f32 L = {(uint16_t)n, (uint16_t)n, l};
            arm_matrix_instance_f32 D = {(uint16_t)n, (uint16_t)n, d};
            fill(a, kind, n);
            memset(l, 0x5a, n * n * sizeof(float32_t));
            memset(d, 0x5a, n * n * sizeof(float32_t));
            memset(p, 0x5a, n * sizeof(uint16_t));
            current_kind = kind_name[kind];
            current_n = n;
            protect_guards();
            arm_status status = arm_mat_ldlt_f32(&A, &L, &D, p);
            unguard_buffers();
            REGRESSION_CONTROL_NAN_F32(l); /* NaN control; no-op otherwise */
            if (status != ARM_MATH_SUCCESS)
            {
                printf("FAIL: arm_mat_ldlt_f32 %s n=%u status %d\n", kind_name[kind], n, (int)status);
                return 1;
            }
            for (unsigned i = 0; i < n * n; ++i)
                if (regression_nonfinite_f32(&l[i]) || regression_nonfinite_f32(&d[i]))
                {
                    printf("FAIL: arm_mat_ldlt_f32 %s n=%u nonfinite output\n", kind_name[kind], n);
                    return 1;
                }
            for (unsigned i = 0; i < n; ++i)
                if (p[i] >= n)
                {
                    printf("FAIL: arm_mat_ldlt_f32 %s n=%u permutation entry %u\n", kind_name[kind], n, p[i]);
                    return 1;
                }
            double err = kind == 2 ? 0.0 : reconstruction_error(a, l, d, p, n);
            if (err > 1e-5 * (double)n)
            {
                printf("FAIL: arm_mat_ldlt_f32 %s n=%u reconstruction error %.3e\n", kind_name[kind], n, err);
                return 1;
            }
            ++cases;
        }
    printf("PASS: %u MVE LDL^t f32 bounds cases\n", cases);
    return 0;
}
