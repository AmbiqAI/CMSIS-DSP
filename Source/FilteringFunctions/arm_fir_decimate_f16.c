/* ----------------------------------------------------------------------
 * Project:      CMSIS DSP Library
 * Title:        arm_fir_decimate_f16.c
 * Description:  FIR decimation for half-precision floating-point sequences
 *
 * $Date:        09 October 2026
 * $Revision:    V1.0.0
 *
 * Target Processor: Cortex-M and Cortex-A cores
 * -------------------------------------------------------------------- */

/*
 * Copyright (C) 2010-2021 ARM Limited or its affiliates. All rights reserved.
 * Copyright (c) 2026 Ambiq Micro, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the License); you may
 * not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an AS IS BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "arm_compiler_specific.h"


#include "dsp/filtering_functions_f16.h"

#if defined(ARM_FLOAT16_SUPPORTED)

/**
  @ingroup groupFilters
 */

/**
  @addtogroup FIR_decimate
  @{
 */

/**
  @brief         Processing function for the half-precision floating-point FIR decimator.
  @param[in]     S         points to an instance of the half-precision floating-point FIR decimator structure
  @param[in]     pSrc      points to the block of input data
  @param[out]    pDst      points to the block of output data
  @param[in]     blockSize number of input samples to process

  @par           Accuracy
                   Products and sums are formed in half precision, as in \ref arm_fir_f16:
                   the Helium implementation keeps eight partial sums per output and adds them at the end,
                   the scalar implementation keeps one sum per output.
                   With unit roundoff <code>u = 2^-11</code>, each output satisfies, in either implementation,
  <pre>
      |y[n] - y_exact[n]| <= gamma(numTaps + 1) * (|b[0] * x[nM]| + |b[1] * x[nM-1]| + ... + |b[numTaps-1] * x[nM-numTaps+1]|)
  </pre>
                   where <code>gamma(k) = k u / (1 - k u)</code> and <code>y_exact</code> is the exact result for the
                   half-precision inputs, provided that no partial sum exceeds the half-precision range (65504)
                   and that no product falls below the normal range (6.1e-5); with 32 taps the bound is
                   <code>0.0164</code> times the sum of the absolute products.
 */

#if defined(ARM_MATH_MVE_FLOAT16) && !defined(ARM_MATH_AUTOVECTORIZE)

#include "arm_helium_utils.h"

ARM_DSP_ATTRIBUTE void arm_fir_decimate_f16(
  const arm_fir_decimate_instance_f16 * S,
  const float16_t * pSrc,
  float16_t * pDst,
  uint32_t blockSize)
{
    float16_t *pState = S->pState;  /* State pointer */
    const float16_t *pCoeffs = S->pCoeffs;    /* Coefficient pointer */
    float16_t *pStateCurnt;     /* Points to the current sample of the state */
    const float16_t *px, *pb;         /* Temporary pointers for state and coefficient buffers */
    uint32_t  numTaps = S->numTaps; /* Number of filter coefficients in the filter */
    uint32_t  i, tapCnt, blkCnt, outBlockSize = blockSize / S->M;   /* Loop counters */
    uint32_t  blkCntN4;
    const float16_t *px0, *px1, *px2, *px3;
    f16x8_t acc0v, acc1v, acc2v, acc3v;
    f16x8_t x0v, x1v, x2v, x3v;
    f16x8_t c0v;

    /*
     * S->pState buffer contains previous frame (numTaps - 1) samples
     * pStateCurnt points to the location where the new input data should be written
     */
    pStateCurnt = S->pState + (numTaps - 1U);
    /*
     * Total number of output samples to be computed
     */
    blkCnt = outBlockSize / 4;
    blkCntN4 = outBlockSize - (4 * blkCnt);

    while (blkCnt > 0U)
    {
        /*
         * Copy 4 * decimation factor number of new input samples into the state buffer
         */
        i = (4 * S->M) >> 3;
        while (i > 0U)
        {
            vst1q(pStateCurnt, vld1q(pSrc));
            pSrc += 8;
            pStateCurnt += 8;
            i--;
        }
        i = (4 * S->M) & 7;
        if (i > 0U)
        {
            mve_pred16_t p0 = vctp16q(i);
            vstrhq_p_f16(pStateCurnt, vldrhq_z_f16(pSrc, p0), p0);
            pSrc += i;
            pStateCurnt += i;
        }

        /*
         * Set accumulators to zero
         */
        acc0v = vdupq_n_f16(0.0f16);
        acc1v = vdupq_n_f16(0.0f16);
        acc2v = vdupq_n_f16(0.0f16);
        acc3v = vdupq_n_f16(0.0f16);

        /*
         * Initialize state pointer for all the samples
         */
        px0 = pState;
        px1 = pState + S->M;
        px2 = pState + 2 * S->M;
        px3 = pState + 3 * S->M;
        /*
         * Initialize coeff pointer
         */
        pb = pCoeffs;
        /*
         * Process 8 taps at a time.
         */
        tapCnt = numTaps >> 3;
        while (tapCnt > 0U)
        {
            c0v = vld1q(pb);
            pb += 8;

            x0v = vld1q(px0);
            x1v = vld1q(px1);
            x2v = vld1q(px2);
            x3v = vld1q(px3);
            px0 += 8;
            px1 += 8;
            px2 += 8;
            px3 += 8;

            acc0v = vfmaq(acc0v, x0v, c0v);
            acc1v = vfmaq(acc1v, x1v, c0v);
            acc2v = vfmaq(acc2v, x2v, c0v);
            acc3v = vfmaq(acc3v, x3v, c0v);
            tapCnt--;
        }

        /*
         * Remaining taps, under the tail predicate: the window of the last
         * output ends with the state buffer.
         */
        tapCnt = numTaps & 7;
        if (tapCnt > 0U)
        {
            mve_pred16_t p0 = vctp16q(tapCnt);

            c0v = vldrhq_z_f16(pb, p0);
            x0v = vldrhq_z_f16(px0, p0);
            x1v = vldrhq_z_f16(px1, p0);
            x2v = vldrhq_z_f16(px2, p0);
            x3v = vldrhq_z_f16(px3, p0);

            acc0v = vfmaq(acc0v, x0v, c0v);
            acc1v = vfmaq(acc1v, x1v, c0v);
            acc2v = vfmaq(acc2v, x2v, c0v);
            acc3v = vfmaq(acc3v, x3v, c0v);
        }

        /*
         * Advance the state pointer by the decimation factor
         * to process the next group of decimation factor number samples
         */
        pState = pState + 4 * S->M;
        /*
         * The result is in the accumulator, store in the destination buffer.
         */
        *pDst++ = vecAddAcrossF16Mve(acc0v);
        *pDst++ = vecAddAcrossF16Mve(acc1v);
        *pDst++ = vecAddAcrossF16Mve(acc2v);
        *pDst++ = vecAddAcrossF16Mve(acc3v);

        blkCnt--;
    }

    while (blkCntN4 > 0U)
    {
        /*
         * Copy decimation factor number of new input samples into the state buffer
         */
        i = S->M;
        do
        {
            *pStateCurnt++ = *pSrc++;
        }
        while (--i);
        /*
         * Set accumulator to zero
         */
        acc0v = vdupq_n_f16(0.0f16);
        /*
         * Initialize state pointer
         */
        px = pState;
        /*
         * Initialize coeff pointer
         */
        pb = pCoeffs;

        tapCnt = numTaps >> 3;
        while (tapCnt > 0U)
        {
            c0v = vld1q(pb);
            x0v = vld1q(px);
            pb += 8;
            px += 8;
            acc0v = vfmaq(acc0v, x0v, c0v);
            tapCnt--;
        }
        tapCnt = numTaps & 7;
        if (tapCnt > 0U)
        {
            mve_pred16_t p0 = vctp16q(tapCnt);
            c0v = vldrhq_z_f16(pb, p0);
            x0v = vldrhq_z_f16(px, p0);
            acc0v = vfmaq(acc0v, x0v, c0v);
        }

        /*
         * Advance the state pointer by the decimation factor
         * to process the next group of decimation factor number samples
         */
        pState = pState + S->M;
        /*
         * The result is in the accumulator, store in the destination buffer.
         */
        *pDst++ = vecAddAcrossF16Mve(acc0v);

        blkCntN4--;
    }

    /*
     * Processing is complete.
     * Now copy the last numTaps - 1 samples to the start of the state buffer.
     * This prepares the state buffer for the next function call.
     */
    pStateCurnt = S->pState;
    blkCnt = (numTaps - 1) >> 3;
    while (blkCnt > 0U)
    {
        vst1q(pStateCurnt, vld1q(pState));
        pState += 8;
        pStateCurnt += 8;
        blkCnt--;
    }
    blkCnt = (numTaps - 1) & 7;
    if (blkCnt > 0U)
    {
        mve_pred16_t p0 = vctp16q(blkCnt);
        vstrhq_p_f16(pStateCurnt, vldrhq_z_f16(pState, p0), p0);
    }
}
#else
ARM_DSP_ATTRIBUTE void arm_fir_decimate_f16(
  const arm_fir_decimate_instance_f16 * S,
  const float16_t * pSrc,
        float16_t * pDst,
        uint32_t blockSize)
{
        float16_t *pState = S->pState;                 /* State pointer */
  const float16_t *pCoeffs = S->pCoeffs;               /* Coefficient pointer */
        float16_t *pStateCur;                          /* Points to the current sample of the state */
        float16_t *px0;                                /* Temporary pointer for state buffer */
  const float16_t *pb;                                 /* Temporary pointer for coefficient buffer */
        _Float16 x0, c0;                               /* Temporary variables to hold state and coefficient values */
        _Float16 acc0;                                 /* Accumulator */
        uint32_t numTaps = S->numTaps;                 /* Number of filter coefficients in the filter */
        uint32_t i, tapCnt, blkCnt, outBlockSize = blockSize / S->M;  /* Loop counters */

#if defined (ARM_MATH_LOOPUNROLL)
        float16_t *px1, *px2, *px3;
        _Float16 x1, x2, x3;
        _Float16 acc1, acc2, acc3;
#endif

  /* S->pState buffer contains previous frame (numTaps - 1) samples */
  /* pStateCur points to the location where the new input data should be written */
  pStateCur = S->pState + (numTaps - 1U);

#if defined (ARM_MATH_LOOPUNROLL)

    /* Loop unrolling: Compute 4 samples at a time */
  blkCnt = outBlockSize >> 2U;

  /* Samples loop unrolled by 4 */
  while (blkCnt > 0U)
  {
    /* Copy 4 * decimation factor number of new input samples into the state buffer */
    i = S->M * 4;

    do
    {
      *pStateCur++ = *pSrc++;

    } while (--i);

    /* Set accumulators to zero */
    acc0 = 0.0f16;
    acc1 = 0.0f16;
    acc2 = 0.0f16;
    acc3 = 0.0f16;

    /* Initialize state pointer for all the samples */
    px0 = pState;
    px1 = pState + S->M;
    px2 = pState + 2 * S->M;
    px3 = pState + 3 * S->M;

    /* Initialize coeff pointer */
    pb = pCoeffs;

    /* Loop unrolling: Compute 4 taps at a time */
    tapCnt = numTaps >> 2U;

    while (tapCnt > 0U)
    {
      /* Read the b[numTaps-1] coefficient */
      c0 = *(pb++);

      /* Read x[n-numTaps-1] sample for acc0, acc1, acc2, acc3 */
      x0 = *(px0++);
      x1 = *(px1++);
      x2 = *(px2++);
      x3 = *(px3++);

      /* Perform the multiply-accumulate */
      acc0 += x0 * c0;
      acc1 += x1 * c0;
      acc2 += x2 * c0;
      acc3 += x3 * c0;

      /* Read the b[numTaps-2] coefficient */
      c0 = *(pb++);

      /* Read x[n-numTaps-2] sample for acc0, acc1, acc2, acc3 */
      x0 = *(px0++);
      x1 = *(px1++);
      x2 = *(px2++);
      x3 = *(px3++);

      /* Perform the multiply-accumulate */
      acc0 += x0 * c0;
      acc1 += x1 * c0;
      acc2 += x2 * c0;
      acc3 += x3 * c0;

      /* Read the b[numTaps-3] coefficient */
      c0 = *(pb++);

      /* Read x[n-numTaps-3] sample acc0, acc1, acc2, acc3 */
      x0 = *(px0++);
      x1 = *(px1++);
      x2 = *(px2++);
      x3 = *(px3++);

      /* Perform the multiply-accumulate */
      acc0 += x0 * c0;
      acc1 += x1 * c0;
      acc2 += x2 * c0;
      acc3 += x3 * c0;

      /* Read the b[numTaps-4] coefficient */
      c0 = *(pb++);

      /* Read x[n-numTaps-4] sample acc0, acc1, acc2, acc3 */
      x0 = *(px0++);
      x1 = *(px1++);
      x2 = *(px2++);
      x3 = *(px3++);

      /* Perform the multiply-accumulate */
      acc0 += x0 * c0;
      acc1 += x1 * c0;
      acc2 += x2 * c0;
      acc3 += x3 * c0;

      /* Decrement loop counter */
      tapCnt--;
    }

    /* Loop unrolling: Compute remaining taps */
    tapCnt = numTaps % 0x4U;

    while (tapCnt > 0U)
    {
      /* Read coefficients */
      c0 = *(pb++);

      /* Fetch state variables for acc0, acc1, acc2, acc3 */
      x0 = *(px0++);
      x1 = *(px1++);
      x2 = *(px2++);
      x3 = *(px3++);

      /* Perform the multiply-accumulate */
      acc0 += x0 * c0;
      acc1 += x1 * c0;
      acc2 += x2 * c0;
      acc3 += x3 * c0;

      /* Decrement loop counter */
      tapCnt--;
    }

    /* Advance the state pointer by the decimation factor
     * to process the next group of decimation factor number samples */
    pState = pState + S->M * 4;

    /* The result is in the accumulator, store in the destination buffer. */
    *pDst++ = acc0;
    *pDst++ = acc1;
    *pDst++ = acc2;
    *pDst++ = acc3;

    /* Decrement loop counter */
    blkCnt--;
  }

  /* Loop unrolling: Compute remaining samples */
  blkCnt = outBlockSize % 0x4U;

#else

  /* Initialize blkCnt with number of samples */
  blkCnt = outBlockSize;

#endif /* #if defined (ARM_MATH_LOOPUNROLL) */

  while (blkCnt > 0U)
  {
    /* Copy decimation factor number of new input samples into the state buffer */
    i = S->M;

    do
    {
      *pStateCur++ = *pSrc++;

    } while (--i);

    /* Set accumulator to zero */
    acc0 = 0.0f16;

    /* Initialize state pointer */
    px0 = pState;

    /* Initialize coeff pointer */
    pb = pCoeffs;

#if defined (ARM_MATH_LOOPUNROLL)

    /* Loop unrolling: Compute 4 taps at a time */
    tapCnt = numTaps >> 2U;

    while (tapCnt > 0U)
    {
      /* Read the b[numTaps-1] coefficient */
      c0 = *pb++;

      /* Read x[n-numTaps-1] sample */
      x0 = *px0++;

      /* Perform the multiply-accumulate */
      acc0 += x0 * c0;

      /* Read the b[numTaps-2] coefficient */
      c0 = *pb++;

      /* Read x[n-numTaps-2] sample */
      x0 = *px0++;

      /* Perform the multiply-accumulate */
      acc0 += x0 * c0;

      /* Read the b[numTaps-3] coefficient */
      c0 = *pb++;

      /* Read x[n-numTaps-3] sample */
      x0 = *px0++;

      /* Perform the multiply-accumulate */
      acc0 += x0 * c0;

      /* Read the b[numTaps-4] coefficient */
      c0 = *pb++;

      /* Read x[n-numTaps-4] sample */
      x0 = *px0++;

      /* Perform the multiply-accumulate */
      acc0 += x0 * c0;

      /* Decrement loop counter */
      tapCnt--;
    }

    /* Loop unrolling: Compute remaining taps */
    tapCnt = numTaps % 0x4U;

#else

    /* Initialize tapCnt with number of taps */
    tapCnt = numTaps;

#endif /* #if defined (ARM_MATH_LOOPUNROLL) */

    while (tapCnt > 0U)
    {
      /* Read coefficients */
      c0 = *pb++;

      /* Fetch 1 state variable */
      x0 = *px0++;

      /* Perform the multiply-accumulate */
      acc0 += x0 * c0;

      /* Decrement loop counter */
      tapCnt--;
    }

    /* Advance the state pointer by the decimation factor
     * to process the next group of decimation factor number samples */
    pState = pState + S->M;

    /* The result is in the accumulator, store in the destination buffer. */
    *pDst++ = acc0;

    /* Decrement loop counter */
    blkCnt--;
  }

  /* Processing is complete.
     Now copy the last numTaps - 1 samples to the start of the state buffer.
     This prepares the state buffer for the next function call. */

  /* Points to the start of the state buffer */
  pStateCur = S->pState;

#if defined (ARM_MATH_LOOPUNROLL)

  /* Loop unrolling: Compute 4 taps at a time */
  tapCnt = (numTaps - 1U) >> 2U;

  /* Copy data */
  while (tapCnt > 0U)
  {
    *pStateCur++ = *pState++;
    *pStateCur++ = *pState++;
    *pStateCur++ = *pState++;
    *pStateCur++ = *pState++;

    /* Decrement loop counter */
    tapCnt--;
  }

  /* Loop unrolling: Compute remaining taps */
  tapCnt = (numTaps - 1U) % 0x04U;

#else

  /* Initialize tapCnt with number of taps */
  tapCnt = (numTaps - 1U);

#endif /* #if defined (ARM_MATH_LOOPUNROLL) */

  /* Copy data */
  while (tapCnt > 0U)
  {
    *pStateCur++ = *pState++;

    /* Decrement loop counter */
    tapCnt--;
  }

}
#endif /* defined(ARM_MATH_MVE_FLOAT16) && !defined(ARM_MATH_AUTOVECTORIZE) */

/**
  @} end of FIR_decimate group
 */

#endif /* #if defined(ARM_FLOAT16_SUPPORTED) */
