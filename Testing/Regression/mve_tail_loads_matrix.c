/* Copyright (c) 2026 Ambiq Micro, Inc.
 * SPDX-License-Identifier: Apache-2.0
 */
/* Cortex-M55 regression: MVE matrix kernels whose operands end at an
 * MPU-inaccessible region. Covers arm_mat_trans and arm_mat_cmplx_trans
 * (f16, f32, q15, q31), arm_mat_cmplx_mult (f16, f32, q15, q31),
 * arm_mat_mult_q31, arm_mat_mult_opt_q31, arm_mat_mult_q15, arm_mat_mult_q7
 * and arm_mat_qr (f32, f16).
 *
 * Each case first runs the kernel with every operand followed by readable
 * padding. It then runs once per operand with that operand ending at the
 * guard and the others padded: every operand, inputs, outputs and scratch
 * alike, must equal the padded run byte for byte. The padded run is checked
 * against a double-precision reference (exact for the transposes, within a
 * few LSB for the fixed-point products, and through the reconstruction Q R of
 * the input for QR).
 *
 * Operand lengths are Arm's documented sizes; the pState scratch of
 * arm_mat_mult_q31/q15/q7 (opt_q31) is numRowsB * numColsB elements and the
 * unused pScratch of arm_mat_cmplx_mult_q15 is 2 * numRowsB * numColsB.
 * Shapes: transposes of 1 to 17 rows by 1 to 17 columns; products of 1 to 5
 * rows of A by 1 to 17 columns of A by 1 to 5 columns of B, and the square
 * 2, 3 and 4 sizes of the specialized kernels; QR of n x n matrices, n = 1
 * to 12.
 */
#include "ARMCM55.h"
#include "dsp/matrix_functions.h"
#include "dsp/matrix_functions_f16.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "regression_checks.h"

#if !defined(ARM_MATH_MVEF) || !defined(ARM_MATH_MVEI) || defined(ARM_MATH_AUTOVECTORIZE)
#error This regression requires the handwritten MVE kernels.
#endif

#define SLOT_BYTES 4096U
/* The guard covers a whole operand slot plus one vector access, so a read
 * anywhere within one slot of an operand end faults (regression_checks.h). */
#define GUARD_BYTES REGRESSION_GUARD_BYTES(SLOT_BYTES)
#define PAD_BYTES 64U
#define MAX_OPS 6U

extern void regression_console_init(void);

static unsigned char arena[SLOT_BYTES + GUARD_BYTES] __ALIGNED(32);
#define GUARD ((uintptr_t)arena + SLOT_BYTES)

static unsigned char init[MAX_OPS][SLOT_BYTES] __ALIGNED(16);
static unsigned char padded[MAX_OPS][SLOT_BYTES + PAD_BYTES] __ALIGNED(16);
static unsigned char result[MAX_OPS][SLOT_BYTES] __ALIGNED(16);

static const char *current_name = "none";
static volatile unsigned current_r, current_c, current_q, current_op;
static unsigned cases;
/* Shape of the current case: A is R x C, B is C x Q. */
static unsigned R, C, Q;

void MemManage_Handler(void)
{
    ARM_MPU_Disable();
    printf("FAIL: %s tail access %ux%ux%u operand %u\n", current_name, current_r, current_c,
           current_q, current_op);
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
    /* Map everything except the aligned guard. No background map. */
    ARM_MPU_SetRegion(0, ARM_MPU_RBAR(0, ARM_MPU_SH_NON, 0, 1, 0),
                     ARM_MPU_RLAR(GUARD - 1U, 0));
    ARM_MPU_SetRegion(1, ARM_MPU_RBAR(GUARD + GUARD_BYTES, ARM_MPU_SH_NON, 0, 1, 0),
                     ARM_MPU_RLAR(UINT32_MAX, 0));
    ARM_MPU_Enable(0);
}

/* Ends a guarded call; a read control (regression_checks.h) reads past the
 * guarded operand before the guard is lifted. */
static void unguard(void)
{
    REGRESSION_CONTROL_READ_ABOVE(GUARD, GUARD_BYTES);
    ARM_MPU_Disable();
}

typedef void (*call_fn)(void **p);

/* Runs one case: init[i] holds bytes[i] initial bytes of operand i. Leaves the
 * padded run's operands in result[]. */
static int run_case(const char *name, unsigned nops, const unsigned *bytes, call_fn call)
{
    void *p[MAX_OPS];
    current_name = name;
    current_r = R;
    current_c = C;
    current_q = Q;
    for (unsigned i = 0; i < nops; ++i)
    {
        if (bytes[i] > SLOT_BYTES)
        {
            printf("FAIL: %s operand %u of %u bytes exceeds the slot\n", name, i, bytes[i]);
            return 1;
        }
        memcpy(padded[i], init[i], bytes[i]);
        memset(padded[i] + bytes[i], 0x3c, PAD_BYTES);
        p[i] = padded[i];
    }
    call(p);
    for (unsigned i = 0; i < nops; ++i)
        memcpy(result[i], padded[i], bytes[i]);

    for (unsigned g = 0; g < nops; ++g)
    {
        for (unsigned i = 0; i < nops; ++i)
        {
            memcpy(padded[i], init[i], bytes[i]);
            memset(padded[i] + bytes[i], 0x3c, PAD_BYTES);
            p[i] = padded[i];
        }
        p[g] = (unsigned char *)GUARD - bytes[g];
        memcpy(p[g], init[g], bytes[g]);
        current_op = g;
        protect_guard();
        call(p);
        unguard();
        for (unsigned i = 0; i < nops; ++i)
            if (memcmp(p[i], result[i], bytes[i]))
            {
                printf("FAIL: %s placement mismatch %ux%ux%u guarded operand %u, operand %u\n",
                       name, R, C, Q, g, i);
                return 1;
            }
    }
    ++cases;
    return 0;
}

static int check_near(const char *name, unsigned index, double got, double reference, double tolerance)
{
    if (!(fabs(got - reference) <= tolerance))
    {
        printf("FAIL: %s numerical result %ux%ux%u index=%u got=%g expected=%g\n",
               name, R, C, Q, index, got, reference);
        return 1;
    }
    return 0;
}

/* Deterministic values in [-1, 1) with distinct magnitudes in each row. */
static double value(unsigned op, unsigned i)
{
    unsigned h = (i * 2654435761u) ^ (op * 40503u + 17u);
    double v = (double)((h >> 8) % 2001U) / 1000.0 - 1.0;
    return v;
}

/* Element access by type. */
enum { T_F16, T_F32, T_Q7, T_Q15, T_Q31 };
static const unsigned type_bytes[] = {2, 4, 1, 2, 4};
/* Fixed-point inputs use a fraction of full scale so the products neither
 * saturate nor overflow: q31 2^27, q15 2^11, q7 2^4. */
static void put(int t, unsigned char *b, unsigned i, double v)
{
    switch (t)
    {
    case T_F16: ((float16_t *)b)[i] = (float16_t)v; break;
    case T_F32: ((float32_t *)b)[i] = (float32_t)v; break;
    case T_Q7: ((q7_t *)b)[i] = (q7_t)(v * 16.0); break;
    case T_Q15: ((q15_t *)b)[i] = (q15_t)(v * 2048.0); break;
    default: ((q31_t *)b)[i] = (q31_t)(v * 134217728.0); break;
    }
}
static double get(int t, const unsigned char *b, unsigned i)
{
    switch (t)
    {
    case T_F16: return (double)((const float16_t *)b)[i];
    case T_F32: return (double)((const float32_t *)b)[i];
    case T_Q7: return (double)((const q7_t *)b)[i];
    case T_Q15: return (double)((const q15_t *)b)[i];
    default: return (double)((const q31_t *)b)[i];
    }
}
static void fill(int t, unsigned op, unsigned count)
{
    for (unsigned i = 0; i < count; ++i)
        put(t, init[op], i, value(op, i));
}

/* ---------------- Transposes ---------------- */
#define TRANS_CALLS(TAG, T)                                                                        \
static void call_trans_##TAG(void **p)                                                             \
{                                                                                                  \
    arm_matrix_instance_##TAG A = {(uint16_t)R, (uint16_t)C, (T *)p[0]};                            \
    arm_matrix_instance_##TAG D = {(uint16_t)C, (uint16_t)R, (T *)p[1]};                            \
    (void)arm_mat_trans_##TAG(&A, &D);                                                             \
}                                                                                                  \
static void call_cmplx_trans_##TAG(void **p)                                                       \
{                                                                                                  \
    arm_matrix_instance_##TAG A = {(uint16_t)R, (uint16_t)C, (T *)p[0]};                            \
    arm_matrix_instance_##TAG D = {(uint16_t)C, (uint16_t)R, (T *)p[1]};                            \
    (void)arm_mat_cmplx_trans_##TAG(&A, &D);                                                       \
}
TRANS_CALLS(f16, float16_t)
TRANS_CALLS(f32, float32_t)
TRANS_CALLS(q15, q15_t)
TRANS_CALLS(q31, q31_t)

static int run_transposes(void)
{
    static const struct { const char *name, *cname; int t; call_fn call, ccall; } k[] = {
        {"arm_mat_trans_f16", "arm_mat_cmplx_trans_f16", T_F16, call_trans_f16, call_cmplx_trans_f16},
        {"arm_mat_trans_f32", "arm_mat_cmplx_trans_f32", T_F32, call_trans_f32, call_cmplx_trans_f32},
        {"arm_mat_trans_q15", "arm_mat_cmplx_trans_q15", T_Q15, call_trans_q15, call_cmplx_trans_q15},
        {"arm_mat_trans_q31", "arm_mat_cmplx_trans_q31", T_Q31, call_trans_q31, call_cmplx_trans_q31},
    };
    for (unsigned j = 0; j < sizeof(k) / sizeof(k[0]); ++j)
        for (R = 1; R <= 17U; ++R)
            for (C = 1; C <= 17U; ++C)
            {
                unsigned sz = type_bytes[k[j].t];
                Q = 0;
                for (int cplx = 0; cplx <= 1; ++cplx)
                {
                    unsigned d = cplx ? 2U : 1U;
                    unsigned bytes[2] = {d * R * C * sz, d * R * C * sz};
                    fill(k[j].t, 0, d * R * C);
                    memset(init[1], 0, bytes[1]);
                    if (run_case(cplx ? k[j].cname : k[j].name, 2, bytes, cplx ? k[j].ccall : k[j].call))
                        return 1;
                    REGRESSION_CONTROL_NAN_F32(result[1]); /* NaN control; no-op otherwise */
                    for (unsigned r = 0; r < R; ++r)
                        for (unsigned c = 0; c < C; ++c)
                            for (unsigned e = 0; e < d; ++e)
                            {
                                unsigned src = d * (r * C + c) + e, dst = d * (c * R + r) + e;
                                if (memcmp(result[1] + dst * sz, init[0] + src * sz, sz) ||
                                    (k[j].t == T_F32 && regression_nonfinite_f32(result[1] + dst * sz)))
                                {
                                    printf("FAIL: %s result %ux%u index=%u\n",
                                           cplx ? k[j].cname : k[j].name, R, C, dst);
                                    return 1;
                                }
                            }
                }
            }
    return 0;
}

/* ---------------- Products ---------------- */
/* Shapes: index 0..84 gives R 1..5, C 1..17, Q 1..5; 85..87 the squares 2, 3, 4. */
static void product_shape(unsigned s)
{
    if (s >= 85U)
    {
        R = C = Q = s - 83U;
        return;
    }
    R = 1U + s / 17U;
    C = 1U + s % 17U;
    Q = 1U + ((s + 1U) * 3U) % 5U;
}
#define PRODUCT_SHAPES 88U

#define REAL_CALL(NAME, TAG, T, STATE)                                                             \
static void call_##NAME(void **p)                                                                  \
{                                                                                                  \
    arm_matrix_instance_##TAG A = {(uint16_t)R, (uint16_t)C, (T *)p[0]};                            \
    arm_matrix_instance_##TAG B = {(uint16_t)C, (uint16_t)Q, (T *)p[1]};                            \
    arm_matrix_instance_##TAG D = {(uint16_t)R, (uint16_t)Q, (T *)p[2]};                            \
    (void)arm_##NAME(&A, &B, &D STATE);                                                            \
}
#define NO_STATE
#define WITH_STATE(T) , (T *)p[3]
REAL_CALL(mat_mult_q31, q31, q31_t, NO_STATE)
REAL_CALL(mat_mult_opt_q31, q31, q31_t, WITH_STATE(q31_t))
REAL_CALL(mat_mult_q15, q15, q15_t, WITH_STATE(q15_t))
REAL_CALL(mat_mult_q7, q7, q7_t, WITH_STATE(q7_t))
REAL_CALL(mat_cmplx_mult_f16, f16, float16_t, NO_STATE)
REAL_CALL(mat_cmplx_mult_f32, f32, float32_t, NO_STATE)
REAL_CALL(mat_cmplx_mult_q31, q31, q31_t, NO_STATE)
REAL_CALL(mat_cmplx_mult_q15, q15, q15_t, WITH_STATE(q15_t))

static int run_products(void)
{
    /* scale: the product of two inputs is divided by it to give output units;
     * tol: tolerance in output units per accumulated term. */
    static const struct { const char *name; int t, cplx, state; call_fn call; double scale, tol; } k[] = {
        {"arm_mat_mult_q31", T_Q31, 0, 0, call_mat_mult_q31, 2147483648.0, 1.0},
        {"arm_mat_mult_opt_q31", T_Q31, 0, 1, call_mat_mult_opt_q31, 2147483648.0, 1.0},
        {"arm_mat_mult_q15", T_Q15, 0, 1, call_mat_mult_q15, 32768.0, 1.0},
        {"arm_mat_mult_q7", T_Q7, 0, 1, call_mat_mult_q7, 128.0, 1.0},
        {"arm_mat_cmplx_mult_f16", T_F16, 1, 0, call_mat_cmplx_mult_f16, 1.0, 0.004},
        {"arm_mat_cmplx_mult_f32", T_F32, 1, 0, call_mat_cmplx_mult_f32, 1.0, 1e-6},
        {"arm_mat_cmplx_mult_q31", T_Q31, 1, 0, call_mat_cmplx_mult_q31, 2147483648.0, 1.0},
        {"arm_mat_cmplx_mult_q15", T_Q15, 1, 1, call_mat_cmplx_mult_q15, 32768.0, 1.0},
    };
    for (unsigned j = 0; j < sizeof(k) / sizeof(k[0]); ++j)
        for (unsigned s = 0; s < PRODUCT_SHAPES; ++s)
        {
            product_shape(s);
            unsigned sz = type_bytes[k[j].t], d = k[j].cplx ? 2U : 1U;
            unsigned bytes[4] = {d * R * C * sz, d * C * Q * sz, d * R * Q * sz, d * C * Q * sz};
            unsigned nops = k[j].state ? 4U : 3U;
            fill(k[j].t, 0, d * R * C);
            fill(k[j].t, 1, d * C * Q);
            memset(init[2], 0, bytes[2]);
            memset(init[3], 0, bytes[3]);
            if (run_case(k[j].name, nops, bytes, k[j].call))
                return 1;
            if (k[j].t == T_F32)
                REGRESSION_CONTROL_NAN_F32(result[2]); /* NaN control; no-op otherwise */
            for (unsigned r = 0; r < R; ++r)
                for (unsigned q = 0; q < Q; ++q)
                {
                    double re = 0.0, im = 0.0;
                    for (unsigned c = 0; c < C; ++c)
                    {
                        double ar = get(k[j].t, init[0], d * (r * C + c));
                        double br = get(k[j].t, init[1], d * (c * Q + q));
                        if (k[j].cplx)
                        {
                            double ai = get(k[j].t, init[0], d * (r * C + c) + 1U);
                            double bi = get(k[j].t, init[1], d * (c * Q + q) + 1U);
                            re += ar * br - ai * bi;
                            im += ar * bi + ai * br;
                        }
                        else
                            re += ar * br;
                    }
                    double tol = k[j].tol * (double)(C + 1U) * (k[j].cplx ? 2.0 : 1.0);
                    unsigned o = d * (r * Q + q);
                    if (k[j].t == T_F32 && regression_nonfinite_f32(result[2] + o * sz))
                    {
                        printf("FAIL: %s nonfinite result %ux%ux%u index=%u\n", k[j].name, R, C, Q, o);
                        return 1;
                    }
                    if (check_near(k[j].name, o, get(k[j].t, result[2], o), re / k[j].scale, tol))
                        return 1;
                    if (k[j].cplx && check_near(k[j].name, o + 1U, get(k[j].t, result[2], o + 1U),
                                                im / k[j].scale, tol))
                        return 1;
                }
        }
    return 0;
}

/* ---------------- QR ---------------- */
/* Operands: src (m x n, modified), R (m x n), Q (m x m), tau (n), tmpA (m), tmpB (m);
 * here m = n. */
static void call_qr_f32(void **p)
{
    arm_matrix_instance_f32 A = {(uint16_t)R, (uint16_t)C, (float32_t *)p[0]};
    arm_matrix_instance_f32 Rm = {(uint16_t)R, (uint16_t)C, (float32_t *)p[1]};
    arm_matrix_instance_f32 Qm = {(uint16_t)R, (uint16_t)R, (float32_t *)p[2]};
    (void)arm_mat_qr_f32(&A, 1e-6f, &Rm, &Qm, (float32_t *)p[3], (float32_t *)p[4], (float32_t *)p[5]);
}
static void call_qr_f16(void **p)
{
    arm_matrix_instance_f16 A = {(uint16_t)R, (uint16_t)C, (float16_t *)p[0]};
    arm_matrix_instance_f16 Rm = {(uint16_t)R, (uint16_t)C, (float16_t *)p[1]};
    arm_matrix_instance_f16 Qm = {(uint16_t)R, (uint16_t)R, (float16_t *)p[2]};
    (void)arm_mat_qr_f16(&A, (float16_t)1e-3f, &Rm, &Qm, (float16_t *)p[3], (float16_t *)p[4], (float16_t *)p[5]);
}

static int run_qr(void)
{
    static const struct { const char *name; int t; call_fn call; double tol; } k[] = {
        {"arm_mat_qr_f32", T_F32, call_qr_f32, 1e-5},
        {"arm_mat_qr_f16", T_F16, call_qr_f16, 0.02},
    };
    for (unsigned j = 0; j < sizeof(k) / sizeof(k[0]); ++j)
        for (C = 1; C <= 12U; ++C)
            {
                R = C;
                Q = R;
                unsigned sz = type_bytes[k[j].t];
                unsigned bytes[6] = {R * C * sz, R * C * sz, R * R * sz, C * sz, R * sz, R * sz};
                /* Diagonally dominant input: well conditioned at every size. */
                for (unsigned i = 0; i < R * C; ++i)
                    put(k[j].t, init[0], i, (i / C == i % C) ? 2.0 + 0.1 * (double)(i % C)
                                                              : 0.3 * value(0, i));
                for (unsigned i = 1; i < 6U; ++i)
                    memset(init[i], 0, bytes[i]);
                if (run_case(k[j].name, 6, bytes, k[j].call))
                    return 1;
                if (k[j].t == T_F32)
                    REGRESSION_CONTROL_NAN_F32(result[1]); /* NaN control; no-op otherwise */
                /* Q times the upper triangle of R must reconstruct the input. */
                for (unsigned r = 0; r < R; ++r)
                    for (unsigned c = 0; c < C; ++c)
                    {
                        double s = 0.0;
                        for (unsigned i = 0; i <= c && i < R; ++i)
                            s += get(k[j].t, result[2], r * R + i) * get(k[j].t, result[1], i * C + c);
                        if (k[j].t == T_F32 && regression_nonfinite_f32(result[1] + (r * C + c) * sz))
                        {
                            printf("FAIL: %s nonfinite R %ux%u index=%u\n", k[j].name, R, C, r * C + c);
                            return 1;
                        }
                        if (check_near(k[j].name, r * C + c, s, get(k[j].t, init[0], r * C + c),
                                       k[j].tol * 4.0 * (double)R))
                            return 1;
                    }
            }
    return 0;
}

int main(void)
{
    regression_console_init();
    SCB->SHCSR |= SCB_SHCSR_MEMFAULTENA_Msk;
    if (run_transposes() || run_products() || run_qr())
        return 1;
    printf("PASS: %u MVE matrix tail-load cases\n", cases);
    return 0;
}
