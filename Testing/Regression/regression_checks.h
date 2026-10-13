/* Copyright (c) 2026 Ambiq Micro, Inc.
 * SPDX-License-Identifier: Apache-2.0
 */
/* Shared pieces of the guarded MVE regressions.
 *
 * REGRESSION_GUARD_BYTES(span): an MPU guard that covers span bytes past the
 * end of a buffer plus one 16-byte vector access, rounded up to the 32-byte
 * MPU granule. A test passes the extent it guards (its largest guarded
 * buffer, or the range it probes), so a read anywhere within that distance
 * of the buffer faults, however sparse.
 *
 * regression_nonfinite_f32/f16: an all-ones exponent (infinity or NaN), read
 * from the bits. Under -ffast-math the compiler may fold isnan()/isfinite()
 * to false, and fabs(NaN - reference) > tolerance is false.
 *
 * Negative controls, selected at compile time through each runner's
 * --control; each must make the run fail:
 *   REGRESSION_SPARSE_READ_BYTES=n  one volatile 32-bit read n bytes past the
 *                                   end of (below the start of, for a lower
 *                                   guard) the first guarded buffer
 *   REGRESSION_SPARSE_READ_FAR      the same at the far end of the guard
 *   REGRESSION_NAN_OUTPUT           a quiet NaN in the first checked output
 */
#ifndef REGRESSION_CHECKS_H
#define REGRESSION_CHECKS_H

#include <stdint.h>
#include <string.h>

#define REGRESSION_GUARD_BYTES(span) ((((span) + 16U) + 31U) & ~31U)

static inline int regression_nonfinite_f32(const void *p)
{
    uint32_t bits;
    memcpy(&bits, p, sizeof(bits));
    return (bits & 0x7f800000U) == 0x7f800000U;
}

static inline int regression_nonfinite_f16(const void *p)
{
    uint16_t bits;
    memcpy(&bits, p, sizeof(bits));
    return (bits & 0x7c00U) == 0x7c00U;
}

/* Offset of the control read from the buffer boundary, for a guard of
 * guard_bytes; 0 when no read control is built. */
#if defined(REGRESSION_SPARSE_READ_FAR)
#define REGRESSION_READ_OFFSET(guard_bytes) ((guard_bytes) - 4U)
#elif defined(REGRESSION_SPARSE_READ_BYTES)
#define REGRESSION_READ_OFFSET(guard_bytes) ((uint32_t)(REGRESSION_SPARSE_READ_BYTES))
#endif

/* The read controls; end is the guard address above a buffer, start the
 * first byte of a buffer above a lower guard. */
#if defined(REGRESSION_READ_OFFSET)
#define REGRESSION_CONTROL_READ_ABOVE(end, guard_bytes)                                    \
    ((void)*(volatile const uint32_t *)((uintptr_t)(end) + REGRESSION_READ_OFFSET(guard_bytes)))
#define REGRESSION_CONTROL_READ_BELOW(start, guard_bytes)                                  \
    ((void)*(volatile const uint32_t *)((uintptr_t)(start) - 4U - REGRESSION_READ_OFFSET(guard_bytes)))
#else
#define REGRESSION_CONTROL_READ_ABOVE(end, guard_bytes) ((void)0)
#define REGRESSION_CONTROL_READ_BELOW(start, guard_bytes) ((void)0)
#endif

#if defined(REGRESSION_NAN_OUTPUT)
#define REGRESSION_CONTROL_NAN_F32(p)                                                      \
    do                                                                                     \
    {                                                                                      \
        const uint32_t qnan_ = 0x7fc00000U;                                                \
        memcpy((p), &qnan_, sizeof(qnan_));                                                \
    } while (0)
#define REGRESSION_CONTROL_NAN_F16(p)                                                      \
    do                                                                                     \
    {                                                                                      \
        const uint16_t qnan_ = 0x7e00U;                                                    \
        memcpy((p), &qnan_, sizeof(qnan_));                                                \
    } while (0)
#else
#define REGRESSION_CONTROL_NAN_F32(p) ((void)0)
#define REGRESSION_CONTROL_NAN_F16(p) ((void)0)
#endif

#endif
