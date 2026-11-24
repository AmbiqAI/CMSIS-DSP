/* Negative control: a wrong arm_add_f32, which the control image's BasicTestsF32 calls instead of the
 * library's (CMakeLists.txt renames arm_add_f32 to planted_arm_add_f32 there), so its add tests must be
 * reported as failed. Only the last element is wrong, which also checks that the tests compare the tail
 * of the output. */

#include "arm_math.h"

void planted_arm_add_f32(const float32_t *pSrcA, const float32_t *pSrcB, float32_t *pDst, uint32_t blockSize)
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
