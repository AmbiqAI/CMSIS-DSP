/* Negative control: a wrong arm_add_f32, linked into the control image ahead of the library's, so the
 * BasicTestsF32 add tests must be reported as failed. Only the last element is wrong, which also checks
 * that the tests compare the tail of the output. */

#include "arm_math.h"

void arm_add_f32(const float32_t *pSrcA, const float32_t *pSrcB, float32_t *pDst, uint32_t blockSize)
{
    for (uint32_t i = 0; i < blockSize; i++)
    {
        pDst[i] = pSrcA[i] + pSrcB[i];
    }
    if (blockSize > 0U)
    {
        pDst[blockSize - 1U] += 1.0f;
    }
}
