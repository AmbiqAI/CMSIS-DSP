/* ----------------------------------------------------------------------
 * Project:      CMSIS DSP Library
 * Title:        arm_cmplx_mult_cmplx_q31.c
 * Description:  Q31 complex-by-complex multiplication
 *
 * $Date:        23 April 2021
 * $Revision:    V1.9.0
 *
 * Target Processor: Cortex-M and Cortex-A cores
 * -------------------------------------------------------------------- */

/*
 * Copyright (C) 2010-2021 ARM Limited or its affiliates. All rights reserved.
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


#include "dsp/complex_math_functions.h"

/**
  @ingroup groupCmplxMath
 */

/**
  @addtogroup CmplxByCmplxMult
  @{
 */

/**
  @brief         Q31 complex-by-complex multiplication.
  @param[in]     pSrcA       points to first input vector
  @param[in]     pSrcB       points to second input vector
  @param[out]    pDst        points to output vector
  @param[in]     numSamples  number of samples in each vector

  @par           Scaling and Overflow Behavior
                   The function implements 1.31 by 1.31 multiplications and finally output is converted into 3.29 format.
                   Input down scaling is not required.
 */

#if defined(ARM_MATH_MVEI) && !defined(ARM_MATH_AUTOVECTORIZE)
/* Each output is formed as in the scalar code below, from two products
   shifted right by 33 (the high half, shifted right by 1), so that no product
   saturates and the result equals the scalar result bit for bit. The
   saturating doubling pair vqdmlsdhq/vqdmladhxq used before clipped a
   product component of magnitude 1 or more (ARM-software/CMSIS-DSP#47). */
ARM_DSP_ATTRIBUTE void arm_cmplx_mult_cmplx_q31(
  const q31_t * pSrcA,
  const q31_t * pSrcB,
        q31_t * pDst,
        uint32_t numSamples)
{
    int32_t         blkCnt;
    q31x4x2_t       vecA, vecB, vecDst;

    /* 4 complex samples at a time, real and imaginary parts de-interleaved */
    blkCnt = numSamples >> 2;
    while (blkCnt > 0) {
        vecA = vld2q(pSrcA);
        vecB = vld2q(pSrcB);

        /* C[2 * i] = A[2 * i] * B[2 * i] - A[2 * i + 1] * B[2 * i + 1].  */
        vecDst.val[0] = vsubq(vshrq(vmulhq(vecA.val[0], vecB.val[0]), 1),
                              vshrq(vmulhq(vecA.val[1], vecB.val[1]), 1));
        /* C[2 * i + 1] = A[2 * i] * B[2 * i + 1] + A[2 * i + 1] * B[2 * i].  */
        vecDst.val[1] = vaddq(vshrq(vmulhq(vecA.val[0], vecB.val[1]), 1),
                              vshrq(vmulhq(vecA.val[1], vecB.val[0]), 1));
        vst2q(pDst, vecDst);

        pSrcA += 8;
        pSrcB += 8;
        pDst += 8;
        blkCnt--;
    }

    /*
     * tail: interleaved, under the tail predicate
     */
    blkCnt = CMPLX_DIM * (numSamples & 3);
    while (blkCnt > 0) {
        mve_pred16_t    p = vctp32q(blkCnt);
        q31x4_t         vecSrcA = vldrwq_z_s32(pSrcA, p);
        q31x4_t         vecSrcB = vldrwq_z_s32(pSrcB, p);
        /* (a c, b d) and (a d, b c), each shifted as in the scalar code */
        q31x4_t         vecSame = vshrq(vmulhq(vecSrcA, vecSrcB), 1);
        q31x4_t         vecCross = vshrq(vmulhq(vecSrcA, vrev64q(vecSrcB)), 1);

        /* even lanes: a c - b d; odd lanes: a d + b c */
        vecSame = vsubq(vecSame, vrev64q(vecSame));
        vecCross = vaddq(vecCross, vrev64q(vecCross));
        vstrwq_p_s32(pDst, vpselq(vecSame, vecCross, (mve_pred16_t) 0x0F0F), p);

        pSrcA += 4;
        pSrcB += 4;
        pDst += 4;
        blkCnt -= 4;
    }
}
#else
ARM_DSP_ATTRIBUTE void arm_cmplx_mult_cmplx_q31(
  const q31_t * pSrcA,
  const q31_t * pSrcB,
        q31_t * pDst,
        uint32_t numSamples)
{
        uint32_t blkCnt;                               /* Loop counter */
        q31_t a, b, c, d;                              /* Temporary variables */

#if defined (ARM_MATH_LOOPUNROLL)

  /* Loop unrolling: Compute 4 outputs at a time */
  blkCnt = numSamples >> 2U;

  while (blkCnt > 0U)
  {
    /* C[2 * i    ] = A[2 * i] * B[2 * i    ] - A[2 * i + 1] * B[2 * i + 1]. */
    /* C[2 * i + 1] = A[2 * i] * B[2 * i + 1] + A[2 * i + 1] * B[2 * i    ]. */

    a = *pSrcA++;
    b = *pSrcA++;
    c = *pSrcB++;
    d = *pSrcB++;
    /* store result in 3.29 format in destination buffer. */
    *pDst++ = (q31_t) ( (((q63_t) a * c) >> 33) - (((q63_t) b * d) >> 33) );
    *pDst++ = (q31_t) ( (((q63_t) a * d) >> 33) + (((q63_t) b * c) >> 33) );

    a = *pSrcA++;
    b = *pSrcA++;
    c = *pSrcB++;
    d = *pSrcB++;
    *pDst++ = (q31_t) ( (((q63_t) a * c) >> 33) - (((q63_t) b * d) >> 33) );
    *pDst++ = (q31_t) ( (((q63_t) a * d) >> 33) + (((q63_t) b * c) >> 33) );

    a = *pSrcA++;
    b = *pSrcA++;
    c = *pSrcB++;
    d = *pSrcB++;
    *pDst++ = (q31_t) ( (((q63_t) a * c) >> 33) - (((q63_t) b * d) >> 33) );
    *pDst++ = (q31_t) ( (((q63_t) a * d) >> 33) + (((q63_t) b * c) >> 33) );

    a = *pSrcA++;
    b = *pSrcA++;
    c = *pSrcB++;
    d = *pSrcB++;
    *pDst++ = (q31_t) ( (((q63_t) a * c) >> 33) - (((q63_t) b * d) >> 33) );
    *pDst++ = (q31_t) ( (((q63_t) a * d) >> 33) + (((q63_t) b * c) >> 33) );

    /* Decrement loop counter */
    blkCnt--;
  }

  /* Loop unrolling: Compute remaining outputs */
  blkCnt = numSamples % 0x4U;

#else

  /* Initialize blkCnt with number of samples */
  blkCnt = numSamples;

#endif /* #if defined (ARM_MATH_LOOPUNROLL) */

  while (blkCnt > 0U)
  {
    /* C[2 * i    ] = A[2 * i] * B[2 * i    ] - A[2 * i + 1] * B[2 * i + 1]. */
    /* C[2 * i + 1] = A[2 * i] * B[2 * i + 1] + A[2 * i + 1] * B[2 * i    ]. */

    a = *pSrcA++;
    b = *pSrcA++;
    c = *pSrcB++;
    d = *pSrcB++;

    /* store result in 3.29 format in destination buffer. */
    *pDst++ = (q31_t) ( (((q63_t) a * c) >> 33) - (((q63_t) b * d) >> 33) );
    *pDst++ = (q31_t) ( (((q63_t) a * d) >> 33) + (((q63_t) b * c) >> 33) );

    /* Decrement loop counter */
    blkCnt--;
  }

}
#endif /* defined(ARM_MATH_MVEI) */

/**
  @} end of CmplxByCmplxMult group
 */
