/* Copyright (c) 2026 Ambiq Micro, Inc.
 * SPDX-License-Identifier: Apache-2.0
 */
static unsigned configured_features(void)
{
    unsigned features = 0;
#ifdef ARM_MATH_AUTOVECTORIZE
    features |= 1;
#endif
#ifdef ARM_MATH_MVE_FLOAT16
    features |= 2;
#endif
#ifdef DISABLEFLOAT16
    features |= 4;
#endif
    return features;
}
